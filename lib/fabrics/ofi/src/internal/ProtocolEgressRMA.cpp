// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include "ProtocolEgressRMA.hpp"
#include <cstring>
#include <algorithm>
#include <mxl-internal/Logging.hpp>
#include <rdma/fi_errno.h>
#include "AudioBounceBuffer.hpp"
#include "DataLayout.hpp"
#include "Exception.hpp"
#include "ImmData.hpp"
#include "LocalRegion.hpp"

namespace mxl::lib::fabrics::ofi
{

    RMAGrainEgressProtocol::RMAGrainEgressProtocol(Completion::Token token, TargetInfo info, DataLayout::Discrete layout,
        std::vector<LocalRegion> localRegions)
        : _token{token}
        , _remoteInfo{std::move(info)}
        , _layout{layout}
        , _localRegions{std::move(localRegions)}
    {}

    void RMAGrainEgressProtocol::registerMemory(std::shared_ptr<Domain>)
    {
        // For this protocol, memory registration is completely handled at the protocol template level, so this function doesn't need to do anything.
    }

    void RMAGrainEgressProtocol::transferGrain(Endpoint const& ep, std::uint64_t localIndex, std::uint64_t remoteIndex, std::uint32_t payloadOffset,
        SliceRange const& sliceRange, ::fi_addr_t destAddr)
    {
        auto const localGrain = _localRegions[localIndex % _localRegions.size()];
        auto const remoteGrain = _remoteInfo.remoteRegions[remoteIndex % _remoteInfo.remoteRegions.size()];
        auto const remoteSlot = remoteIndex % _remoteInfo.remoteRegions.size();

        // Fast path: a full grain is transferred, can do one write no matter how many planes
        if ((sliceRange.start() == 0) && (sliceRange.end() == _layout.totalSlices))
        {
            _pending += ep.write(_token, localGrain, remoteGrain, destAddr, std::make_optional(ImmDataGrain{remoteSlot, _layout.totalSlices}.data()));
            return;
        }

        auto const planeCount = _layout.activePlaneCount();
        for (std::size_t plane = 0; plane < planeCount; ++plane)
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

            auto const localRegion = localGrain.sub(offset, size);
            auto const remoteRegion = remoteGrain.sub(offset, size);

            auto const isLastPlane = (plane == planeCount - 1);
            auto const immData = isLastPlane ? std::make_optional(ImmDataGrain{remoteSlot, sliceRange.end()}.data()) : std::nullopt;

            _pending += ep.write(_token, localRegion, remoteRegion, destAddr, immData);
        }
    }

    void RMAGrainEgressProtocol::transferSamples(Endpoint const&, std::uint64_t, std::size_t, ::fi_addr_t)
    {
        throw Exception::invalidState("transferSamples is not supported in RMAGrainEgressProtocol.");
    }

    void RMAGrainEgressProtocol::processCompletion(Endpoint const&, Completion::Data const&, ::fi_addr_t)
    {
        if (_pending > 0)
        {
            --_pending;
        }
    }

    void RMAGrainEgressProtocol::processCompletionError(Endpoint const&, Completion::Error const&, ::fi_addr_t)
    {
        if (_pending > 0)
        {
            --_pending;
        }
    }

    bool RMAGrainEgressProtocol::hasPendingWork() const
    {
        return _pending > 0;
    }

    std::size_t RMAGrainEgressProtocol::reset()
    {
        return std::exchange(_pending, 0);
    }

    RMAGrainEgressProtocolTemplate::RMAGrainEgressProtocolTemplate(DataLayout::Discrete layout, std::vector<Region> regions)
        : _layout{layout}
        , _regions{std::move(regions)}
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

    std::unique_ptr<EgressProtocol> RMAGrainEgressProtocolTemplate::createInstance(Completion::Token token, TargetInfo remoteInfo)
    {
        if (!_localRegions)
        {
            throw Exception::invalidState("Cannot create protocol before memory is registered.");
        }

        struct MakeUniqueEnabler : RMAGrainEgressProtocol
        {
            MakeUniqueEnabler(Completion::Token token, TargetInfo info, DataLayout::Discrete layout, std::vector<LocalRegion> localRegion)
                : RMAGrainEgressProtocol{token, std::move(info), layout, std::move(localRegion)}
            {}
        };

        return std::make_unique<MakeUniqueEnabler>(token, std::move(remoteInfo), _layout, *_localRegions);
    }

    class RMASampleEgressProtocol::EndpointSink final : public SampleTransferQueue::Sink
    {
    public:
        EndpointSink(RMASampleEgressProtocol& protocol, Endpoint const& ep, ::fi_addr_t destAddr)
            : _protocol{protocol}
            , _ep{ep}
            , _destAddr{destAddr}
        {}

        std::vector<SampleWrite> plan(std::uint64_t headIndex, std::size_t count) override
        {
            return planSampleWrites(
                _protocol._layout, _protocol._localRegion, _protocol._remoteLayout, _protocol._remoteRing, headIndex, count, _protocol._limits);
        }

        std::pair<SampleWrite, std::uint32_t> describe(std::uint64_t headIndex, std::size_t count, std::size_t slot) override
        {
            // The queue reuses a slot only once the write that last sent it has completed.
            _protocol._descriptors[slot] = AudioEntryHeader{.headIndex = headIndex, .count = count};

            auto const offset = slot * sizeof(AudioEntryHeader);
            auto write = SampleWrite{
                .local = {_protocol._descriptorRegion->sub(offset, sizeof(AudioEntryHeader))},
                .remote = {_protocol._remoteDescriptors.sub(offset, sizeof(AudioEntryHeader))},
            };
            auto const immData = ImmDataSample{slot, headIndex}.data();
            return {std::move(write), immData};
        }

        void post(SampleWrite const& write, std::optional<std::uint32_t> immData) override
        {
            _ep.write(_protocol._token, write.local, write.remote, _destAddr, immData);
        }

    private:
        RMASampleEgressProtocol& _protocol; /**< The protocol whose layouts, regions and descriptors the writes use. */
        Endpoint const& _ep;                /**< The endpoint the writes are posted on. */
        ::fi_addr_t _destAddr;              /**< The target's address. Ignored on connected endpoints. */
    };

    RMASampleEgressProtocol::RMASampleEgressProtocol(Completion::Token token, TargetInfo info, DataLayout::Continuous layout, LocalRegion localRegion,
        WriteLimits limits, SampleTransferQueue::Ordering ordering)
        : _token{token}
        , _remoteInfo{std::move(info)}
        , _layout{layout}
        , _remoteLayout{.sampleSize = _remoteInfo.continuous->sampleSize,
              .channelCount = _remoteInfo.continuous->channelCount,
              .bufferLength = _remoteInfo.continuous->bufferLength}
        , _localRegion{localRegion}
        , _remoteRing{_remoteInfo.continuous->ring}
        , _remoteDescriptors{_remoteInfo.continuous->descriptors}
        , _limits{limits}
        , _descriptors(_remoteInfo.continuous->descriptorCount)
        , _descriptorRegistration{}
        , _descriptorRegion{}
        , _queue{ordering, std::min(_remoteLayout.bufferLength / 2, _layout.bufferLength), _remoteInfo.continuous->descriptorCount}
    {}

    void RMASampleEgressProtocol::registerMemory(std::shared_ptr<Domain> domain)
    {
        if (_descriptorRegistration)
        {
            throw Exception::invalidState("Memory already registered.");
        }

        // Registered here rather than on the domain, which keeps what it registers until it closes: the descriptors go with the target.
        auto const region = Region{reinterpret_cast<std::uintptr_t>(_descriptors.data()),
            _descriptors.size() * sizeof(AudioEntryHeader),
            nullptr,
            nullptr,
            Region::Location::host()};
        _descriptorRegistration.emplace(MemoryRegion::reg(*domain, region, FI_WRITE), region);
        _descriptorRegion = _descriptorRegistration->toLocal();
    }

    void RMASampleEgressProtocol::transferGrain(Endpoint const&, std::uint64_t, std::uint64_t, std::uint32_t, SliceRange const&, ::fi_addr_t)
    {
        throw Exception::invalidState("transferGrain is not supported in RMASampleEgressProtocol.");
    }

    void RMASampleEgressProtocol::transferSamples(Endpoint const& ep, std::uint64_t headIndex, std::size_t count, ::fi_addr_t destAddr)
    {
        if (!_descriptorRegion)
        {
            throw Exception::invalidState("Cannot transfer before memory is registered.");
        }

        _queue.push(headIndex, count);
        auto sink = EndpointSink{*this, ep, destAddr};
        _queue.pump(sink);
    }

    void RMASampleEgressProtocol::processCompletion(Endpoint const& ep, Completion::Data const&, ::fi_addr_t destAddr)
    {
        _queue.complete();
        pumpAfterCompletion(ep, destAddr);
    }

    void RMASampleEgressProtocol::processCompletionError(Endpoint const& ep, Completion::Error const&, ::fi_addr_t destAddr)
    {
        _queue.fail();
        pumpAfterCompletion(ep, destAddr);
    }

    void RMASampleEgressProtocol::pumpAfterCompletion(Endpoint const& ep, ::fi_addr_t destAddr)
    {
        try
        {
            auto sink = EndpointSink{*this, ep, destAddr};
            _queue.pump(sink);
        }
        // The writes stay queued, and the next transfer or completion posts them. Nothing is thrown from here: the initiators call this from
        // inside their endpoint state machines, which an exception would leave without a state. A failure that persists reaches the
        // application through the next transfer.
        catch (FabricException const& e)
        {
            if (e.fiErrno() == -FI_EAGAIN)
            {
                MXL_DEBUG("Audio writes deferred after a completion: {}", e.what());
            }
            else
            {
                MXL_ERROR("Failed to post audio writes after a completion: {}", e.what());
            }
        }
        catch (Exception const& e)
        {
            MXL_ERROR("Failed to post audio writes after a completion: {}", e.what());
        }
    }

    bool RMASampleEgressProtocol::hasPendingWork() const
    {
        return _queue.hasPendingWork();
    }

    std::size_t RMASampleEgressProtocol::reset()
    {
        return _queue.reset();
    }

    RMASampleBounceEgressProtocol::RMASampleBounceEgressProtocol(Completion::Token token, TargetInfo info, DataLayout::Continuous layout,
        LocalRegion localRegion, std::size_t bounceBufferEntryCount)
        : _token{token}
        , _remoteInfo{std::move(info)}
        , _layout{layout}
        , _localRegion{localRegion}
        , _staging(stagingSize(_remoteInfo, bounceBufferEntryCount))
        , _bounceBufferEntryCount{bounceBufferEntryCount}
    {}

    std::size_t RMASampleBounceEgressProtocol::stagingSize(TargetInfo const& info, std::size_t entryCount)
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

    void RMASampleBounceEgressProtocol::registerMemory(std::shared_ptr<Domain> domain)
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

    void RMASampleBounceEgressProtocol::transferGrain(Endpoint const&, std::uint64_t, std::uint64_t, std::uint32_t, SliceRange const&, ::fi_addr_t)
    {
        throw Exception::invalidState("transferGrain is not supported in RMASampleBounceEgressProtocol.");
    }

    void RMASampleBounceEgressProtocol::transferSamples(Endpoint const& ep, std::uint64_t headIndex, std::size_t count, ::fi_addr_t destAddr)
    {
        if ((count == 0) || (count > _layout.bufferLength))
        {
            throw Exception::invalidArgument("Count must be greater than 0 and at most the ring length {}, got {}.", _layout.bufferLength, count);
        }

        // An entry is rewritten only once the write that last sent it has completed. With as many writes in flight as there are entries, the next
        // one would overwrite a staging entry the provider may still be reading, so this target misses the transfer. Only this target: failing
        // the call would have the caller retry it for every target of the flow, and a peer that stopped completing -- one that died -- would
        // hold back all of them until its writes time out.
        if (_pending >= _bounceBufferEntryCount)
        {
            if (!_saturated)
            {
                MXL_WARN("All {} bounce buffer entries of a target are in flight, skipping its transfers until one completes.",
                    _bounceBufferEntryCount);
                _saturated = true;
            }
            return;
        }
        _saturated = false;

        auto const entrySize = _remoteInfo.bounceBufferInfo->entrySize;
        auto const entrySizeRequired = (_layout.sampleSize * _layout.channelCount * count) + sizeof(AudioEntryHeader);
        if (entrySizeRequired > entrySize)
        {
            throw Exception::invalidArgument("Count is too large for the bounce buffer entry size. Count {}, entry size {}, required entry size {}.",
                count,
                entrySize,
                entrySizeRequired);
        }

        // One write per transfer, from a staging copy of the entry. The target reads the header when the write carrying the immediate data
        // completes, and a provider without write-after-write ordering -- EFA -- delivers the writes of a scatter-gather list split over several
        // messages in any order. Split, a 12-channel transfer read a header the first message had not delivered yet.
        auto const offset = static_cast<std::size_t>(_bounceBufferEntryIndex) * entrySize;
        auto* entry = _staging.data() + offset;
        auto const header = AudioEntryHeader{.headIndex = headIndex, .count = count};
        std::memcpy(entry, &header, sizeof(header));
        copySamples(_layout, headIndex, count, _localRegion, entry + sizeof(header));

        auto const remoteRegion = _remoteInfo.remoteRegions[_bounceBufferEntryIndex % _remoteInfo.remoteRegions.size()];

        _pending += ep.write(
            _token, _stagingRegion->sub(offset, entrySizeRequired), remoteRegion.sub(0, entrySizeRequired), destAddr, _bounceBufferEntryIndex);

        _bounceBufferEntryIndex = (_bounceBufferEntryIndex + 1) % _bounceBufferEntryCount;
    }

    void RMASampleBounceEgressProtocol::processCompletion(Endpoint const&, Completion::Data const&, ::fi_addr_t)
    {
        if (_pending > 0)
        {
            --_pending;
        }
    }

    void RMASampleBounceEgressProtocol::processCompletionError(Endpoint const&, Completion::Error const&, ::fi_addr_t)
    {
        if (_pending > 0)
        {
            --_pending;
        }
    }

    bool RMASampleBounceEgressProtocol::hasPendingWork() const
    {
        return _pending > 0;
    }

    std::size_t RMASampleBounceEgressProtocol::reset()
    {
        return std::exchange(_pending, 0);
    }

    void RMASampleBounceEgressProtocol::copySamples(DataLayout::Continuous const& layout, std::uint64_t headIndex, std::size_t count,
        LocalRegion const& region, std::uint8_t* dst)
    {
        auto slice = mxlMutableWrappedMultiBufferSlice{};
        getMutableMultiBufferSlices(headIndex,
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

    RMASampleEgressProtocolTemplate::RMASampleEgressProtocolTemplate(DataLayout::Continuous layout, Region region)
        : _layout{layout}
        , _region{region}
        , _localRegion{}
        , _limits{.iovLimit = 1, .rmaIovLimit = 1, .maxMsgSize = 0}
        , _ordering{SampleTransferQueue::Ordering::Gated}
    {}

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

        // Register the source ring. Every target's writes read straight from it.
        domain->registerRegion(_region, FI_WRITE);
        _localRegion = domain->localRegions().front();

        // Connected endpoints place writes in the order they are posted; connectionless ones, EFA among them, do not.
        auto const info = domain->fabric()->info();
        _limits = WriteLimits{.iovLimit = info.txIovLimit(), .rmaIovLimit = info.rmaIovLimit(), .maxMsgSize = info.maxMsgSize()};
        _ordering = (info.endpointType() == FI_EP_MSG) ? SampleTransferQueue::Ordering::Ordered : SampleTransferQueue::Ordering::Gated;
    }

    std::unique_ptr<EgressProtocol> RMASampleEgressProtocolTemplate::createInstance(Completion::Token token, TargetInfo remoteInfo)
    {
        if (!_localRegion)
        {
            throw Exception::invalidState("Cannot create protocol before memory is registered.");
        }

        if (remoteInfo.continuous)
        {
            auto const& remote = *remoteInfo.continuous;
            if (remote.version != TargetInfoContinuous::ProtocolVersion)
            {
                throw Exception::invalidArgument("The target speaks audio write protocol version {}, this initiator speaks {}.",
                    remote.version,
                    TargetInfoContinuous::ProtocolVersion);
            }
            if ((remote.sampleSize != _layout.sampleSize) || (remote.channelCount != _layout.channelCount) || (remote.bufferLength < 2) ||
                (remote.descriptorCount == 0) || (remote.descriptorCount > 65536U) ||
                (remote.ring.len < (remote.sampleSize * remote.channelCount * remote.bufferLength)) ||
                (remote.descriptors.len < (remote.descriptorCount * sizeof(AudioEntryHeader))))
            {
                throw Exception::invalidArgument("The target's audio ring does not match the source flow: {} channels of {} samples of {} bytes "
                                                 "against {} channels of {} bytes, {} descriptor slots, a ring of {} bytes, {} bytes of slots.",
                    remote.channelCount,
                    remote.bufferLength,
                    remote.sampleSize,
                    _layout.channelCount,
                    _layout.sampleSize,
                    remote.descriptorCount,
                    remote.ring.len,
                    remote.descriptors.len);
            }

            struct MakeUniqueEnabler : RMASampleEgressProtocol
            {
                MakeUniqueEnabler(Completion::Token token, TargetInfo info, DataLayout::Continuous layout, LocalRegion localRegion,
                    WriteLimits limits, SampleTransferQueue::Ordering ordering)
                    : RMASampleEgressProtocol{token, std::move(info), layout, localRegion, limits, ordering}
                {}
            };

            return std::make_unique<MakeUniqueEnabler>(token, std::move(remoteInfo), _layout, *_localRegion, _limits, _ordering);
        }

        if (remoteInfo.bounceBufferInfo)
        {
            struct MakeUniqueEnabler : RMASampleBounceEgressProtocol
            {
                MakeUniqueEnabler(Completion::Token token, TargetInfo info, DataLayout::Continuous layout, LocalRegion localRegion,
                    std::size_t bounceBufferEntryCount)
                    : RMASampleBounceEgressProtocol{token, std::move(info), layout, localRegion, bounceBufferEntryCount}
                {}
            };

            auto const entryCount = remoteInfo.bounceBufferInfo->entryCount;
            return std::make_unique<MakeUniqueEnabler>(token, std::move(remoteInfo), _layout, *_localRegion, entryCount);
        }

        throw Exception::invalidArgument("The target publishes no audio layout, it cannot receive samples.");
    }
}
