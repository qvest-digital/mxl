// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include "ProtocolEgressRMA.hpp"
#include <cstring>
#include <algorithm>
#include <mxl-internal/Logging.hpp>
#include "AudioBounceBuffer.hpp"
#include "DataLayout.hpp"
#include "Exception.hpp"
#include "ImmData.hpp"
#include "LocalRegion.hpp"

namespace mxl::lib::fabrics::ofi
{

    namespace
    {
        /** \brief Whether an exception reports a full send queue rather than a failure.
         */
        bool isQueueFull(Exception const& e) noexcept
        {
            return e.status() == MXL_ERR_NOT_READY;
        }
    }

    RMAGrainEgressProtocol::RMAGrainEgressProtocol(Completion::Token token, TargetInfo info, DataLayout::Discrete layout,
        std::vector<LocalRegion> localRegions, EgressConfig const& config, EgressWindow window)
        : _token{token}
        , _remoteInfo{std::move(info)}
        , _layout{layout}
        , _localRegions{std::move(localRegions)}
        , _config{config}
        , _window{window}
    {}

    void RMAGrainEgressProtocol::registerMemory(std::shared_ptr<Domain>)
    {
        // For this protocol, memory registration is completely handled at the protocol template level, so this function doesn't need to do anything.
    }

    void RMAGrainEgressProtocol::write(Endpoint const& ep, LocalRegion const& local, RemoteRegion const& remote, ::fi_addr_t destAddr,
        std::optional<std::uint32_t> immData, bool last, bool endsTransfer, std::uint64_t index)
    {
        // Every write reports a completion on an endpoint without selective completion, so every write is accounted for as one.
        auto const reportsCompletion = (last && endsTransfer) || !_config.selectiveCompletion;
        static_cast<void>(ep.write(_token, local, remote, destAddr, immData, reportsCompletion));
        _window.wrote(reportsCompletion, last && endsTransfer, index, local.len);
    }

    void RMAGrainEgressProtocol::transferGrain(Endpoint const& ep, std::uint64_t localIndex, std::uint64_t remoteIndex, std::uint32_t payloadOffset,
        SliceRange const& sliceRange, ::fi_addr_t destAddr)
    {
        auto const chunked = _config.chunksPerGrain > 1;
        auto const endsGrain = sliceRange.end() == _layout.totalSlices;

        // Sent in chunks, a grain is one transfer that its first chunk opens and its last chunk ends. Otherwise every call is a transfer of
        // its own. A grain whose last chunk never came is given up when the next one starts; its writes are settled by a later completion.
        if (!chunked || (sliceRange.start() == 0) || (_grain != localIndex))
        {
            if (_grainOpen)
            {
                _window.abandon();
            }
            _grain = localIndex;
            _grainOpen = _window.open();
        }
        if (!_grainOpen)
        {
            return;
        }

        // Without chunks every call ends its transfer. In chunks only the last chunk does, and only it carries immediate data: the
        // target then sees the grain once, after all of it has arrived, and needs one receive per grain.
        auto const endsTransfer = !chunked || endsGrain;
        auto const fullGrain = (sliceRange.start() == 0) && endsGrain;
        auto const writes = fullGrain ? std::size_t{1} : _layout.activePlaneCount();
        if (!_window.hasRoomFor(writes))
        {
            _window.abandon();
            _grainOpen = false;
            return;
        }

        auto const localGrain = _localRegions[localIndex % _localRegions.size()];
        auto const remoteGrain = _remoteInfo.remoteRegions[remoteIndex % _remoteInfo.remoteRegions.size()];
        auto const remoteSlot = remoteIndex % _remoteInfo.remoteRegions.size();

        try
        {
            // Fast path: a full grain is transferred, can do one write no matter how many planes
            if (fullGrain)
            {
                write(ep, localGrain, remoteGrain, destAddr, ImmDataGrain{remoteSlot, _layout.totalSlices}.data(), true, endsTransfer, localIndex);
            }
            else
            {
                for (std::size_t plane = 0; plane < writes; ++plane)
                {
                    auto const sliceSize = _layout.sliceSizes[plane];
                    auto offset = std::uint32_t{0};
                    auto size = std::uint32_t{0};

                    if (plane == 0)
                    {
                        // need to include header with slice 0-x
                        if (sliceRange.start() == 0)
                        {
                            offset = sliceRange.transferOffset(0, sliceSize);
                            size = sliceRange.transferSize(sliceSize) + payloadOffset;
                        }
                        else
                        {
                            offset = sliceRange.transferOffset(payloadOffset, sliceSize);
                            size = sliceRange.transferSize(sliceSize);
                        }
                    }
                    else
                    {
                        auto const planeBase = _layout.planePayloadOffset(plane, payloadOffset);
                        offset = sliceRange.transferOffset(planeBase, sliceSize);
                        size = sliceRange.transferSize(sliceSize);
                    }

                    auto const isLastPlane = (plane == writes - 1);
                    auto const immData =
                        (isLastPlane && endsTransfer) ? std::make_optional(ImmDataGrain{remoteSlot, sliceRange.end()}.data()) : std::nullopt;

                    write(ep, localGrain.sub(offset, size), remoteGrain.sub(offset, size), destAddr, immData, isLastPlane, endsTransfer, localIndex);
                }
            }
        }
        catch (Exception const& e)
        {
            // A full send queue costs this target the grain, not the caller the call: the other targets of the initiator still get it.
            if (!isQueueFull(e))
            {
                throw;
            }
            _window.abandon();
            _grainOpen = false;
            return;
        }

        if (endsTransfer)
        {
            _grainOpen = false;
        }
    }

    void RMAGrainEgressProtocol::transferSamples(Endpoint const&, std::uint64_t, std::size_t, ::fi_addr_t)
    {
        throw Exception::invalidState("transferSamples is not supported in RMAGrainEgressProtocol.");
    }

    void RMAGrainEgressProtocol::processCompletion(Completion::Data const&)
    {
        _window.complete(true);
    }

    void RMAGrainEgressProtocol::processCompletionError(Completion::Error const&)
    {
        _window.complete(false);
    }

    bool RMAGrainEgressProtocol::hasPendingWork() const
    {
        return _window.expectsCompletion();
    }

    std::size_t RMAGrainEgressProtocol::reset()
    {
        _grainOpen = false;
        return _window.reset();
    }

    void RMAGrainEgressProtocol::skip()
    {
        _window.skip();
    }

    mxlFabricsTargetStats RMAGrainEgressProtocol::stats() const
    {
        return _window.stats();
    }

    RMAGrainEgressProtocolTemplate::RMAGrainEgressProtocolTemplate(DataLayout::Discrete layout, std::vector<Region> regions,
        EgressConfig const& config)
        : _layout{layout}
        , _regions{std::move(regions)}
        , _config{config}
    {}

    void RMAGrainEgressProtocolTemplate::registerMemory(std::shared_ptr<Domain> domain)
    {
        if (_localRegions)
        {
            throw Exception::invalidState("Memory already registered.");
        }

        domain->registerRegions(_regions, FI_WRITE);
        _localRegions = domain->localRegions();
    }

    EgressWindow RMAGrainEgressProtocolTemplate::window() const noexcept
    {
        // A call that is not a full grain takes one write per plane, and a grain takes chunksPerGrain calls.
        auto const writesPerGrain = std::max<std::size_t>(_config.chunksPerGrain, 1) * std::max<std::size_t>(_layout.activePlaneCount(), 1);
        auto grains = std::clamp<std::size_t>(_config.grainsInFlight, 1, MXL_FABRICS_MAX_TRANSFERS_IN_FLIGHT);
        // The send queue has to hold every write in flight, or the provider refuses work the window allowed.
        if ((_config.sendQueueDepth > 0) && ((grains * writesPerGrain) > _config.sendQueueDepth))
        {
            grains = std::max<std::size_t>(_config.sendQueueDepth / writesPerGrain, 1);
        }
        auto writes = grains * writesPerGrain;
        if (_config.sendQueueDepth > 0)
        {
            writes = std::min(writes, _config.sendQueueDepth);
        }
        return EgressWindow{grains, writes};
    }

    std::size_t RMAGrainEgressProtocolTemplate::maxWritesInFlight() const noexcept
    {
        return window().maxWrites();
    }

    std::unique_ptr<EgressProtocol> RMAGrainEgressProtocolTemplate::createInstance(Completion::Token token, TargetInfo remoteInfo)
    {
        if (!_localRegions)
        {
            throw Exception::invalidState("Cannot create protocol before memory is registered.");
        }

        struct MakeUniqueEnabler : RMAGrainEgressProtocol
        {
            MakeUniqueEnabler(Completion::Token token, TargetInfo info, DataLayout::Discrete layout, std::vector<LocalRegion> localRegion,
                EgressConfig const& config, EgressWindow window)
                : RMAGrainEgressProtocol{token, std::move(info), layout, std::move(localRegion), config, window}
            {}
        };

        return std::make_unique<MakeUniqueEnabler>(token, std::move(remoteInfo), _layout, *_localRegions, _config, window());
    }

    RMASampleEgressProtocol::RMASampleEgressProtocol(Completion::Token token, TargetInfo info, DataLayout::Continuous layout, LocalRegion localRegion,
        std::size_t bounceBufferEntryCount)
        : _token{token}
        , _remoteInfo{std::move(info)}
        , _layout{layout}
        , _localRegion{localRegion}
        , _staging(stagingSize(_remoteInfo, bounceBufferEntryCount))
        , _window{std::min<std::size_t>(bounceBufferEntryCount, MXL_FABRICS_MAX_TRANSFERS_IN_FLIGHT),
              std::min<std::size_t>(bounceBufferEntryCount, MXL_FABRICS_MAX_TRANSFERS_IN_FLIGHT)}
        , _bounceBufferEntryCount{bounceBufferEntryCount}
    {}

    std::size_t RMASampleEgressProtocol::stagingSize(TargetInfo const& info, std::size_t entryCount)
    {
        // The geometry comes from the target, and it is now an allocation and a pinned registration per target rather than a header per entry.
        constexpr auto maxStagingSize = std::size_t{256} * 1024 * 1024;
        auto const entrySize = info.bounceBufferInfo->entrySize;
        if ((entryCount == 0) || (entrySize == 0) || (entryCount != info.remoteRegions.size()) || (entrySize > (maxStagingSize / entryCount)))
        {
            throw Exception::invalidArgument("Unusable bounce buffer geometry from the target: {} entries of {} bytes, {} remote regions.",
                entryCount,
                entrySize,
                info.remoteRegions.size());
        }
        return entryCount * entrySize;
    }

    void RMASampleEgressProtocol::registerMemory(std::shared_ptr<Domain> domain)
    {
        if (_staging.empty())
        {
            throw Exception::invalidState("Staging buffer is not initialized.");
        }

        // Registered here rather than on the domain, which keeps what it registers until it closes: the staging copy goes with the target, and a
        // target re-added for every peer restart would otherwise leave a whole pinned copy of its bounce buffer behind each time.
        auto const region = Region{reinterpret_cast<std::uintptr_t>(_staging.data()), _staging.size(), nullptr, nullptr, Region::Location::host()};
        _stagingRegistration.emplace(MemoryRegion::reg(*domain, region, FI_WRITE), region);
        _stagingRegion = _stagingRegistration->toLocal();
    }

    void RMASampleEgressProtocol::transferGrain(Endpoint const&, std::uint64_t, std::uint64_t, std::uint32_t, SliceRange const&, ::fi_addr_t)
    {
        throw Exception::invalidState("transferGrain is not supported in RMASampleEgressProtocol.");
    }

    void RMASampleEgressProtocol::transferSamples(Endpoint const& ep, std::uint64_t headIndex, std::size_t count, ::fi_addr_t destAddr)
    {
        if ((count == 0) || (count > _layout.bufferLength))
        {
            throw Exception::invalidArgument("Count must be greater than 0 and at most the ring length {}, got {}.", _layout.bufferLength, count);
        }

        auto const entrySize = _remoteInfo.bounceBufferInfo->entrySize;
        auto const entrySizeRequired = (_layout.sampleSize * _layout.channelCount * count) + sizeof(AudioEntryHeader);
        if (entrySizeRequired > entrySize)
        {
            throw Exception::invalidArgument("Count is too large for the bounce buffer entry size. Count {}, entry size {}, required entry size {}.",
                count,
                entrySize,
                entrySizeRequired);
        }

        // An entry is rewritten only once the write that last sent it has completed, and every write consumes a receive at the target. The
        // window holds no more transfers than there are entries, nor more than half the receives the target keeps posted. A transfer that
        // does not fit costs this target the batch, not the caller the call: failing it would have the caller retry it for every target of
        // the flow, and a peer that stopped completing would hold back all of them.
        if (!_window.open())
        {
            if (!_saturated)
            {
                MXL_WARN("{} transfers to a target are in flight, skipping its transfers until one completes.", _window.maxTransfers());
                _saturated = true;
            }
            return;
        }
        _saturated = false;

        // One write per transfer, from a staging copy of the entry. The target reads the header when the write carrying the immediate data
        // completes, and a provider without write-after-write ordering -- EFA -- delivers the writes of a scatter-gather list split over several
        // messages in any order. Split, a 12-channel transfer read a header the first message had not delivered yet.
        auto const offset = static_cast<std::size_t>(_bounceBufferEntryIndex) * entrySize;
        auto* entry = _staging.data() + offset;
        auto const header = AudioEntryHeader{.headIndex = headIndex, .count = count};
        std::memcpy(entry, &header, sizeof(header));
        copySamples(_layout, headIndex, count, _localRegion, entry + sizeof(header));

        auto const remoteRegion = _remoteInfo.remoteRegions[_bounceBufferEntryIndex % _remoteInfo.remoteRegions.size()];

        try
        {
            static_cast<void>(ep.write(
                _token, _stagingRegion->sub(offset, entrySizeRequired), remoteRegion.sub(0, entrySizeRequired), destAddr, _bounceBufferEntryIndex));
        }
        catch (Exception const& e)
        {
            if (!isQueueFull(e))
            {
                throw;
            }
            _window.abandon();
            return;
        }
        _window.wrote(true, true, headIndex, entrySizeRequired - sizeof(AudioEntryHeader));

        _bounceBufferEntryIndex = (_bounceBufferEntryIndex + 1) % _bounceBufferEntryCount;
    }

    void RMASampleEgressProtocol::processCompletion(Completion::Data const&)
    {
        _window.complete(true);
    }

    void RMASampleEgressProtocol::processCompletionError(Completion::Error const&)
    {
        _window.complete(false);
    }

    bool RMASampleEgressProtocol::hasPendingWork() const
    {
        return _window.expectsCompletion();
    }

    std::size_t RMASampleEgressProtocol::reset()
    {
        return _window.reset();
    }

    void RMASampleEgressProtocol::skip()
    {
        _window.skip();
    }

    mxlFabricsTargetStats RMASampleEgressProtocol::stats() const
    {
        return _window.stats();
    }

    void RMASampleEgressProtocol::copySamples(DataLayout::Continuous const& layout, std::uint64_t headIndex, std::size_t count,
        LocalRegion const& region, std::uint8_t* dst)
    {
        auto slice = mxlMutableWrappedMultiBufferSlice{};
        AudioBounceBuffer::getMutableMultiBufferSlices(headIndex,
            count,
            layout.bufferLength,
            layout.sampleSize,
            layout.channelCount,
            reinterpret_cast<std::uint8_t*>(region.addr), // NOLINT
            slice);

        // All fragments of one channel before the next channel. Fragment first only works when source and target wrapped slices split at the
        // same fragment sizes; with different ring positions the target would stitch the channel data back together incorrectly.
        for (auto chan = std::size_t{0}; chan < slice.count; chan++)
        {
            for (auto const& fragment : slice.base.fragments)
            {
                if (fragment.size > 0)
                {
                    std::memcpy(dst, static_cast<std::uint8_t const*>(fragment.pointer) + (slice.stride * chan), fragment.size);
                    dst += fragment.size;
                }
            }
        }
    }

    RMASampleEgressProtocolTemplate::RMASampleEgressProtocolTemplate(DataLayout::Continuous layout, Region region, EgressConfig const&)
        : _layout{layout}
        , _region{region}
    {}

    std::size_t RMASampleEgressProtocolTemplate::maxWritesInFlight() const noexcept
    {
        // One write per transfer, and the bounce buffer of a target may allow fewer.
        return MXL_FABRICS_MAX_TRANSFERS_IN_FLIGHT;
    }

    void RMASampleEgressProtocolTemplate::registerMemory(std::shared_ptr<Domain> domain)
    {
        // This function should be called once during setup and be the first memory to register, otherwise it's a bug.
        if (_localRegion)
        {
            throw Exception::invalidState("Memory already registered.");
        }
        if (!domain->localRegions().empty())
        {
            throw Exception::invalidState("No memory should be previously registered.");
        }

        // Register the audio region provided by the user. When the actual protocol instance is created, the protocol will register additional
        // regions for the bounce buffer entry headers, but we can only do that once we have the actual protocol instance since the bounce buffer
        // entry count is a parameter of the protocol instance.
        domain->registerRegion(_region, FI_WRITE);

        _localRegion = domain->localRegions().front();
    }

    std::unique_ptr<EgressProtocol> RMASampleEgressProtocolTemplate::createInstance(Completion::Token token, TargetInfo remoteInfo)
    {
        if (!_localRegion)
        {
            throw Exception::invalidState("Cannot create protocol before memory is registered.");
        }

        if (!remoteInfo.bounceBufferInfo)
        {
            throw Exception::invalidArgument("Remote target does not have bounce buffer info required for sample egress protocol.");
        }

        struct MakeUniqueEnabler : RMASampleEgressProtocol
        {
            MakeUniqueEnabler(Completion::Token token, TargetInfo info, DataLayout::Continuous layout, LocalRegion localRegion,
                std::uint32_t bounceBufferEntryCount)
                : RMASampleEgressProtocol{token, std::move(info), layout, localRegion, bounceBufferEntryCount}
            {}
        };

        return std::make_unique<MakeUniqueEnabler>(token, std::move(remoteInfo), _layout, *_localRegion, remoteInfo.bounceBufferInfo->entryCount);
    };
}
