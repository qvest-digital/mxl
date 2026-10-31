// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include "RCInitiator.hpp"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <exception>
#include <ranges>
#include <uuid.h>
#include <mxl-internal/Logging.hpp>
#include <rdma/fabric.h>
#include "mxl-internal/Flow.hpp"
#include "Domain.hpp"
#include "Exception.hpp"
#include "FabricInfo.hpp"
#include "FabricInfoHelpers.hpp"
#include "Protocol.hpp"
#include "Region.hpp"
#include "SliceRange.hpp"
#include "VariantUtils.hpp"

namespace mxl::lib::fabrics::ofi
{
    namespace
    {
        // Rank of an endpoint's state when several endpoints carry the same target: a target removed and added again leaves the old
        // endpoint behind until it is done and evicted, and the most advanced live endpoint is the target's.
        int liveRank(mxlFabricsTargetState state) noexcept
        {
            switch (state)
            {
                case MXL_FABRICS_TARGET_STATE_CONNECTED:  return 3;
                case MXL_FABRICS_TARGET_STATE_CONNECTING: return 2;
                case MXL_FABRICS_TARGET_STATE_CLOSING:    return 1;
            }
            return 0;
        }

        // The endpoint that stands for the target with the given id: of the endpoints for it that are not done, the one in the most
        // advanced state. The targets map is keyed by a random token, so its order says nothing about which endpoint is the old one.
        template<typename Targets>
        auto liveEndpointFor(Targets& targets, Endpoint::Id id)
        {
            auto best = targets.end();
            auto bestRank = 0;
            for (auto it = targets.begin(); it != targets.end(); ++it)
            {
                if ((it->second.info().id != id) || it->second.canEvict())
                {
                    continue;
                }
                if (auto const rank = liveRank(it->second.state()); rank > bestRank)
                {
                    best = it;
                    bestRank = rank;
                }
            }
            return best;
        }
    }

    RCInitiatorEndpoint::RCInitiatorEndpoint(Endpoint ep, std::unique_ptr<EgressProtocol> proto, TargetInfo info)
        : _state(Idle{.ep = std::move(ep), .idleSince = std::chrono::steady_clock::time_point{}})
        , _info(std::move(info))
        , _proto(std::move(proto))
    {}

    TargetInfo const& RCInitiatorEndpoint::info() const noexcept
    {
        return _info;
    }

    bool RCInitiatorEndpoint::isIdle() const noexcept
    {
        return std::holds_alternative<Idle>(_state);
    }

    bool RCInitiatorEndpoint::canEvict() const noexcept
    {
        return std::holds_alternative<Done>(_state);
    }

    mxlFabricsTargetState RCInitiatorEndpoint::state() const noexcept
    {
        if (std::holds_alternative<Connected>(_state))
        {
            return MXL_FABRICS_TARGET_STATE_CONNECTED;
        }
        if (std::holds_alternative<Flushing>(_state))
        {
            return MXL_FABRICS_TARGET_STATE_CLOSING;
        }
        return MXL_FABRICS_TARGET_STATE_CONNECTING;
    }

    bool RCInitiatorEndpoint::hasPendingWork() const noexcept
    {
        return std::visit(
            overloaded{
                [](std::monostate) { return false; },   // Something went wrong with this target, but there is probably no work to do.
                [](Idle const&) { return true; },       // An idle target means there is no work to do right now.
                [](Connecting const&) { return true; }, // In the connecting state, the target is waiting for a connected event.
                [&](Connected const&) { return _proto->hasPendingWork(); }, // While connected, a target has pending work when
                                                                            // there are transfers that have not yet completed.
                [](Flushing const&) { return true; },                       // In the shutdown state, the target is waiting for a FI_SHUTDOWN event.
                [](Done const&) { return false; },                          // In the done state, there is no pending work.
            },
            _state);
    }

    void RCInitiatorEndpoint::shutdown()
    {
        _state = std::visit(
            overloaded{
                [](Idle) -> State
                {
                    MXL_INFO("Shutdown requested while waiting to activate, aborting.");
                    return Done{};
                },
                [](Connecting) -> State
                {
                    MXL_INFO("Shutdown requested while trying to connect, aborting.");
                    return Done{};
                },
                [&](Connected state) -> State
                {
                    MXL_INFO("Shutting down");
                    auto pending = _proto->reset();
                    state.ep.shutdown();

                    return Flushing{.ep = std::move(state.ep), .pending = pending};
                },
                [](Flushing state) -> State
                {
                    MXL_WARN("Another shutdown was requested while trying to flush the completion queue, ignoring.");
                    return state;
                },
                [](Done state) -> State { return state; },
            },
            std::move(_state));
    }

    void RCInitiatorEndpoint::terminate() noexcept
    {
        if (auto state = std::get_if<Flushing>(&_state); state)
        {
            MXL_INFO("Flushing complete, transitioning to done state.");
            _state = Done{};
        }
    }

    void RCInitiatorEndpoint::activate(std::shared_ptr<CompletionQueue> const& cq, std::shared_ptr<EventQueue> const& eq)
    {
        _state = std::visit(
            overloaded{
                [&](Idle state) -> State
                {
                    // A fresh endpoint connects at once. One restarted after a failure waits, longer after each consecutive failure.
                    auto idleDuration = std::chrono::steady_clock::now() - state.idleSince;
                    if (idleDuration < state.delay)
                    {
                        return state;
                    }

                    MXL_INFO("Endpoint has been idle for {}ms, activating",
                        std::chrono::duration_cast<std::chrono::milliseconds>(idleDuration).count());

                    // The endpoint in an idle target is always fresh and thus needs to be bound the the queues. Only the writes that end a
                    // transfer ask for a completion; the provider still signals every write and flushes every write of a failed connection to
                    // the queue, which is what it is sized for.
                    state.ep.bind(eq);
                    state.ep.bind(cq, FI_TRANSMIT | FI_SELECTIVE_COMPLETION);

                    // Transition into the connecting state
                    auto const faddr = _info.fabricAddress.decode();
                    MXL_INFO("Connecting to {}", faddr.toString());
                    state.ep.connect(_info.fabricAddress);
                    return Connecting{.ep = std::move(state.ep)};
                },
                [](Connecting state) -> State { return state; },
                [](Connected state) -> State { return state; },
                [](Flushing state) -> State { return state; },
                [](Done state) -> State { return state; },
            },
            std::move(_state));
    }

    void RCInitiatorEndpoint::consume(Event ev)
    {
        _state = std::visit(
            overloaded{
                [](Idle state) -> State { return state; }, // Nothing to do when idle.
                [&](Connecting state) -> State
                {
                    if (ev.isError())
                    {
                        MXL_ERROR("Failed to connect endpoint: {}", ev.error().toString());

                        // Go into the idle state with a new endpoint
                        return restart(state.ep);
                    }
                    else if (ev.isConnected())
                    {
                        MXL_INFO("Endpoint is now connected");
                        _reconnectDelay = {};
                        return Connected{.ep = std::move(state.ep)};
                    }
                    else if (ev.isShutdown())
                    {
                        MXL_WARN("Received a shutdown event while connecting, going idle");

                        // Go to idle state with a new endpoint
                        return restart(state.ep);
                    }

                    MXL_WARN("Received an unexpected event while establishing a connection");
                    return state;
                },
                [&](Connected state) -> State
                {
                    if (ev.isError())
                    {
                        MXL_WARN("Received an error event in connected state, going idle. Error: {}", ev.error().toString());
                        // The old endpoint's writes will not complete on the new one.
                        static_cast<void>(_proto->reset());
                        return restart(state.ep);
                    }
                    else if (ev.isShutdown())
                    {
                        MXL_INFO("Remote endpoint has closed the connection");
                        return Flushing{.ep = std::move(state.ep), .pending = _proto->reset()};
                    }

                    return state;
                },
                [&](Flushing state) -> State
                {
                    if (ev.isShutdown())
                    {
                        MXL_INFO("Received a Shutdown Event while flushing the completion queue");
                    }
                    else if (ev.isError())
                    {
                        MXL_ERROR("Received an error while shutting down: {}", ev.error().toString());
                        return Done{};
                    }
                    else
                    {
                        MXL_ERROR("Received an unexpected event while shutting down");
                    }
                    return state;
                },
                [](Done state) -> State { return state; },
            },
            std::move(_state));
    }

    void RCInitiatorEndpoint::consume(Completion completion)
    {
        if (auto error = completion.tryErr(); error)
        {
            handleCompletionError(*error);
        }
        else if (auto data = completion.tryData(); data)
        {
            handleCompletionData(*data);
        }
    }

    void RCInitiatorEndpoint::transferGrain(std::uint64_t localIndex, std::uint64_t remoteIndex, std::uint64_t remotePayloadOffset,
        SliceRange const& sliceRange)
    {
        if (auto const state = std::get_if<Connected>(&_state); state != nullptr)
        {
            _proto->transferGrain(state->ep, localIndex, remoteIndex, remotePayloadOffset, sliceRange);
        }
        else if (sliceRange.start() == 0)
        {
            _proto->skip();
        }
    }

    void RCInitiatorEndpoint::transferSamples(std::uint64_t headIndex, std::size_t count)
    {
        if (auto const state = std::get_if<Connected>(&_state); state != nullptr)
        {
            _proto->transferSamples(state->ep, headIndex, count);
        }
        else
        {
            _proto->skip();
        }
    }

    mxlFabricsTargetStats RCInitiatorEndpoint::stats() const noexcept
    {
        return _proto->stats();
    }

    void RCInitiatorEndpoint::handleCompletionData(Completion::Data completion)
    {
        _state = std::visit(
            overloaded{[](Idle state) -> State
                {
                    MXL_WARN("Received a completion event while idle, ignoring.");
                    return state;
                },
                [](Connecting state) -> State
                {
                    MXL_WARN("Received a completion event while connecting, ignoring.");
                    return state;
                },
                [&](Connected state) -> State
                {
                    _proto->processCompletion(completion);
                    return state;
                },
                [](Flushing state) -> State
                {
                    state.pending--; // Decrease the counter of completions that must still be flushed.
                    return state;
                },
                [](Done state) -> State
                {
                    MXL_DEBUG("Ignoring completion after shutdown.");
                    return state;
                }},
            std::move(_state));
    }

    void RCInitiatorEndpoint::handleCompletionError(Completion::Error err)
    {
        MXL_ERROR("Received a completion error: {}", err.toString());

        // A failed write is finished all the same; left counted, the endpoint
        // reported pending work for the rest of its life.
        if (auto* flushing = std::get_if<Flushing>(&_state); flushing != nullptr)
        {
            if (flushing->pending > 0)
            {
                flushing->pending--;
            }
        }
        else if (std::holds_alternative<Connected>(_state))
        {
            _proto->processCompletionError(err);
        }
    }

    RCInitiatorEndpoint::Idle RCInitiatorEndpoint::restart(Endpoint const& old)
    {
        _reconnectDelay = (_reconnectDelay == std::chrono::steady_clock::duration{})
                            ? std::chrono::steady_clock::duration{MinReconnectDelay}
                            : std::min<std::chrono::steady_clock::duration>(_reconnectDelay * 2, MaxReconnectDelay);
        return Idle{
            .ep = Endpoint::create(old.domain(), old.id(), old.info()), .idleSince = std::chrono::steady_clock::now(), .delay = _reconnectDelay};
    }

    std::unique_ptr<RCInitiator> RCInitiator::setup(mxlFabricsInitiatorConfig const& config, FabricInfoView info,
        InitiatorSetupOptions const& options)
    {
        requireCapability(info, FI_WRITE, "Interface is missing required write capability");
        MXL_DEBUG("{}", fi_tostr(info.raw(), FI_TYPE_INFO));

        auto fabric = Fabric::open(info);
        auto domain = Domain::open(fabric);

        auto eq = EventQueue::open(fabric);

        auto regions = MxlRegions::forReader(config.reader);
        auto const egressConfig = EgressConfig{
            .grainsInFlight = options.grainsInFlight,
            .chunksPerGrain = options.chunksPerGrain,
            .sendQueueDepth = info.txSize(),
            .selectiveCompletion = true,
            .sourceHead = sourceHeadOf(config.reader),
        };
        auto proto = selectEgressProtocol(regions.dataLayout(), regions.regions(), egressConfig);
        proto->registerMemory(domain);

        // One completion queue serves every endpoint, and every endpoint may hold as many writes as its window allows. The provider signals
        // each write to the queue whether or not a completion was asked for, and a connection that fails flushes all of them there at once,
        // so the queue holds the windows of maxTargets endpoints. Sized for a single send queue, it overran as soon as two endpoints filled
        // theirs. An explicit cqDepth option still takes precedence.
        auto const maxTargets = std::max<std::size_t>(options.maxTargets, 1);
        auto cqAttr = CompletionQueue::Attributes::defaults();
        cqAttr.size = options.cqDepth.value_or(std::max(cqAttr.size, maxTargets * proto->maxWritesInFlight()));
        auto cq = CompletionQueue::open(domain, cqAttr);

        struct MakeUniqueEnabler : RCInitiator
        {
            MakeUniqueEnabler(std::shared_ptr<Domain> domain, std::shared_ptr<CompletionQueue> cq, std::shared_ptr<EventQueue> eq,
                std::unique_ptr<EgressProtocolTemplate> proto, std::size_t maxTargets)
                : RCInitiator(std::move(domain), std::move(cq), std::move(eq), std::move(proto), maxTargets)
            {}
        };

        return std::make_unique<MakeUniqueEnabler>(std::move(domain), std::move(cq), std::move(eq), std::move(proto), maxTargets);
    }

    RCInitiator::RCInitiator(std::shared_ptr<Domain> domain, std::shared_ptr<CompletionQueue> cq, std::shared_ptr<EventQueue> eq,
        std::unique_ptr<EgressProtocolTemplate> proto, std::size_t maxTargets)
        : _domain(std::move(domain))
        , _cq(std::move(cq))
        , _eq(std::move(eq))
        , _proto(std::move(proto))
        , _maxTargets(maxTargets)
    {}

    void RCInitiator::addTarget(TargetInfo const& targetInfo)
    {
        // The completion queue holds the writes of _maxTargets endpoints. An endpoint that is done holds none; every other one, including
        // one still shutting down, may.
        auto const held = std::ranges::count_if(_targets, [](auto const& item) { return !item.second.canEvict(); });
        if (static_cast<std::size_t>(held) >= _maxTargets)
        {
            throw Exception::invalidState("The initiator already holds {} targets, the most its completion queue is sized for.", _maxTargets);
        }

        auto endpoint = Endpoint::create(_domain);
        auto id = endpoint.id();
        auto proto = _proto->createInstance(Endpoint::tokenFromId(id), targetInfo);
        proto->registerMemory(_domain);

        _targets.emplace(id, RCInitiatorEndpoint{std::move(endpoint), std::move(proto), targetInfo});
    }

    void RCInitiator::removeTarget(TargetInfo const& targetInfo)
    {
        // The endpoint that stands for the target, not whichever the map holds first: shutting down a done endpoint left behind by an
        // earlier removal does nothing and would leave the target attached. A target held only by such an endpoint is removed already.
        auto target = liveEndpointFor(_targets, targetInfo.id);
        if (target == _targets.end())
        {
            target = std::ranges::find_if(_targets, [&targetInfo](auto const& item) { return item.second.info().id == targetInfo.id; });
        }
        if (target == _targets.end())
        {
            throw Exception::notFound("Target with id {} not found", targetInfo.id);
        }
        target->second.shutdown();
    }

    std::optional<mxlFabricsTargetState> RCInitiator::targetState(TargetInfo const& targetInfo) const
    {
        // An endpoint that is done is evicted by the next progress call and never connects again, so it is reported as gone already: a
        // caller can then tell a target it has to add again from one that is still connecting.
        if (auto const target = liveEndpointFor(_targets, targetInfo.id); target != _targets.end())
        {
            return target->second.state();
        }

        return std::nullopt;
    }

    std::optional<mxlFabricsTargetStats> RCInitiator::targetStats(TargetInfo const& targetInfo) const
    {
        if (auto const target = liveEndpointFor(_targets, targetInfo.id); target != _targets.end())
        {
            return target->second.stats();
        }

        return std::nullopt;
    }

    void RCInitiator::transferGrain(std::uint64_t grainIndex, std::uint16_t startSlice, std::uint16_t endSlice)
    {
        // Post a transfer work item to all targets. A target that is not connected skips the grain; one whose window or send queue is
        // full skips it too and reports not-ready. Either way the skip is counted for that target and the others still get the grain.
        // The first failure is reported to the caller once every target has been offered the transfer.
        auto failure = std::exception_ptr{};
        for (auto& [_, target] : _targets)
        {
            try
            {
                target.transferGrain(grainIndex, grainIndex, MXL_GRAIN_PAYLOAD_OFFSET, SliceRange::make(startSlice, endSlice));
            }
            catch (...)
            {
                if (!failure)
                {
                    failure = std::current_exception();
                }
            }
        }
        if (failure)
        {
            std::rethrow_exception(failure);
        }
    }

    void RCInitiator::transferGrainToTarget(Endpoint::Id targetId, std::uint64_t localIndex, std::uint64_t remoteIndex, std::uint64_t payloadOffset,
        std::uint16_t startSlice, std::uint16_t endSlice)
    {
        auto it = _targets.find(targetId);
        if (it == _targets.end())
        {
            throw Exception::notFound("Target with id {} not found", targetId);
        }

        it->second.transferGrain(localIndex, remoteIndex, payloadOffset, SliceRange::make(startSlice, endSlice));
    }

    void RCInitiator::transferSamples(std::uint64_t headIndex, std::size_t count)
    {
        // As for grains: a target that cannot take the batch skips it, and the first failure is reported after every target was offered it.
        auto failure = std::exception_ptr{};
        for (auto& [_, target] : _targets)
        {
            try
            {
                target.transferSamples(headIndex, count);
            }
            catch (...)
            {
                if (!failure)
                {
                    failure = std::current_exception();
                }
            }
        }
        if (failure)
        {
            std::rethrow_exception(failure);
        }
    }

    Initiator::MakeProgressResult RCInitiator::afterProgressResult() const noexcept
    {
        // Check if any of the targets have pending work.
        for (auto& [_, target] : _targets)
        {
            if (target.hasPendingWork())
            {
                return Initiator::NotReady{};
            }
        }

        return Initiator::Ready{};
    }

    bool RCInitiator::hasTarget() const noexcept
    {
        return _targets.size() > 0;
    }

    void RCInitiator::activateIdleEndpoints()
    {
        // Call the activate function on all endpoints. This is a no-op when the endpoint is not idle
        // and there is probably not that many of them.
        for (auto& [_, target] : _targets)
        {
            target.activate(_cq, _eq);
        }
    }

    void RCInitiator::evictDeadEndpoints()
    {
        std::erase_if(_targets, [](auto const& item) { return item.second.canEvict(); });
    }

    Initiator::MakeProgressResult RCInitiator::blockOnCQ(std::chrono::steady_clock::duration timeout)
    {
        // A zero timeout would cause the queue to block indefinetly, which
        // is not our documented behaviour.
        if (timeout == std::chrono::milliseconds::zero())
        {
            // So just behave exactly like the non-blocking variant.
            makeProgress();
            return afterProgressResult();
        }

        for (;;)
        {
            try
            {
                auto completion = _cq->readBlocking(timeout);
                if (!completion)
                {
                    // No completion available, if we were flushing any endpoint, transition their state to done.
                    for (auto& [_, target] : _targets)
                    {
                        target.terminate();
                    }

                    return afterProgressResult();
                }

                // Find the endpoint that this completion was generated from
                auto ep = _targets.find(Endpoint::idFromToken(completion->token()));
                if (ep == _targets.end())
                {
                    MXL_WARN("Received completion for an unknown endpoint");

                    return afterProgressResult();
                }

                ep->second.consume(*completion);

                return afterProgressResult();
            }
            catch (FabricException const& ex)
            {
                if (ex.isInterrupted())
                {
                    return Initiator::Interrupted{};
                }

                throw;
            }
        }
    }

    void RCInitiator::pollCQ()
    {
        for (;;)
        {
            auto completion = _cq->read();
            if (!completion)
            {
                // No completion available, if we were flushing any endpoint, transition their state to done.
                for (auto& [_, target] : _targets)
                {
                    target.terminate();
                }
                break;
            }

            // Find the endpoint this completion was generated from.
            auto ep = _targets.find(Endpoint::idFromToken(completion->token()));
            if (ep == _targets.end())
            {
                MXL_WARN("Received completion for an unknown endpoint");

                continue;
            }

            ep->second.consume(*completion);
        }
    }

    void RCInitiator::pollEQ()
    {
        for (;;)
        {
            auto event = _eq->read();
            if (!event)
            {
                break;
            }

            // Find the endpoint this event was generated from.
            auto ep = _targets.find(Endpoint::idFromFID(event->fid()));
            if (ep == _targets.end())
            {
                MXL_WARN("Received event for an unknown endpoint");

                continue;
            }

            ep->second.consume(*event);
        }
    }

    Initiator::MakeProgressResult RCInitiator::makeProgress()
    {
        if (!hasTarget())
        {
            throw Exception::interrupted("No more targets available while calling makeProgress.");
        }

        // Activate any peers that might be idle and waiting for activation.
        activateIdleEndpoints();

        // Poll the completion and event queue once and process pending events.
        pollCQ();
        pollEQ();

        // Evict any peers that are dead and no longer will make progress.
        evictDeadEndpoints();

        return afterProgressResult();
    }

    Initiator::MakeProgressResult RCInitiator::makeProgressBlocking(std::chrono::steady_clock::duration timeout)
    {
        // If the timeout is less than our maintainance interval, just check all the queues once, execute all maintainance tasks once
        // and block on the completion queue for the rest of the time.
        if (timeout < EQPollInterval)
        {
            makeProgress();
            return blockOnCQ(timeout);
        }

        auto deadline = std::chrono::steady_clock::now() + timeout;

        for (;;)
        {
            // Poll all queues, execute all maintainance actions
            if (std::holds_alternative<Initiator::Ready>(makeProgress()))
            {
                return Initiator::Ready{};
            }

            // Calculate the remaining time until the user wants the blocking function to return. If there is no time left
            // (millisecond precision) return right away.
            auto timeUntilDeadline = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (timeUntilDeadline <= decltype(timeUntilDeadline){0})
            {
                return afterProgressResult();
            }

            // Block on the completion queue until a completion arrives, or the interval timeout occurs.
            auto const res = blockOnCQ(std::min(EQPollInterval, timeUntilDeadline));
            if (std::holds_alternative<Initiator::NotReady>(res))
            {
                continue;
            }

            return res;
        }
    }

    void RCInitiator::shutdown()
    {
        for (auto& target : _targets)
        {
            target.second.shutdown();
        }
    }
}
