// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include "ProtocolIngressRMA.hpp"
#include <cstring>
#include <rdma/fi_errno.h>
#include "mxl-internal/Logging.hpp"
#include "AudioBounceBuffer.hpp"
#include "DataLayout.hpp"
#include "Exception.hpp"
#include "ImmData.hpp"
#include "Region.hpp"

namespace mxl::lib::fabrics::ofi
{
    //
    // RMAGrainIngressProtocol implementations below
    ImmediateDataPool::ImmediateDataPool(std::size_t depth)
        : _buffers(depth == 0 ? 1 : depth)
    {}

    LocalRegion ImmediateDataPool::next() noexcept
    {
        auto const& buf = _buffers[_cursor];
        _cursor = (_cursor + 1) % _buffers.size();
        return buf.toLocalRegion();
    }

    std::size_t ImmediateDataPool::depth() const noexcept
    {
        return _buffers.size();
    }

    RMAGrainIngressProtocol::RMAGrainIngressProtocol(std::vector<Region> regions)
        : _regions{std::move(regions)}
    {}

    std::vector<RemoteRegion> RMAGrainIngressProtocol::registerMemory(std::shared_ptr<Domain> domain)
    {
        if (_isMemoryRegistered)
        {
            throw Exception::invalidState("Memory is already registered.");
        }

        domain->registerRegions(_regions, FI_REMOTE_WRITE);
        _isMemoryRegistered = true;

        return domain->remoteRegions();
    }

    std::optional<TargetInfoContinuous> RMAGrainIngressProtocol::continuousInfo() const
    {
        return std::nullopt;
    }

    void RMAGrainIngressProtocol::start(Endpoint const& endpoint)
    {
        if (endpoint.domain()->usingRecvBufForCqData())
        {
            postReceives(endpoint, DefaultReceiveDepth);
        }
    }

    std::optional<Target::ReadResult> RMAGrainIngressProtocol::read(Endpoint const& endpoint, Completion const& completion)
    {
        auto completionData = completion.tryData();
        if (!completionData)
        {
            // A receive that failed is gone from the window all the same.
            // Not replacing it shrank the window with every peer failure,
            // until writes carrying immediate data found none posted and
            // retried without end.
            // A flushed receive is not replaced: its queue is being torn
            // down, and a receive posted there would only be flushed again.
            auto const error = completion.tryErr();
            if (error && (error->code() == FI_ECANCELED))
            {
                return {};
            }
            if (error)
            {
                MXL_WARN("Completion error on target: {}", error->toString());
            }
            if (_immData)
            {
                static_cast<void>(tryPostOne(endpoint));
            }
            return {};
        }

        // One receive back for the one this completion consumed, so the
        // window stays open for as long as the connection lives.
        if (_immData)
        {
            static_cast<void>(tryPostOne(endpoint));
        }

        auto immData = completionData->data();
        if (!immData)
        {
            return {};
        }

        auto [slot, slice] = ImmDataGrain{static_cast<std::uint32_t>(*immData)}.unpack();

        // Set the number of valid slices in the grain header. This information is received through the immediate data and must be updated
        // in the local shared memory in the case of partial writes.
        setValidSlicesForGrain(_regions, slot, slice);

        // Get the actual grain index from the grain header in share memory. This was written in the first RMA write.
        auto grainIndex = getGrainIndexInRingSlot(_regions, slot);

        return std::make_optional<Target::GrainReadResult>(grainIndex);
    }

    bool RMAGrainIngressProtocol::canReadGrains() const noexcept
    {
        return true;
    }

    bool RMAGrainIngressProtocol::canReadSamples() const noexcept
    {
        return false;
    }

    void RMAGrainIngressProtocol::reset()
    {}

    void RMAGrainIngressProtocol::postReceives(Endpoint const& endpoint, std::size_t count)
    {
        if (!_immData)
        {
            _immData.emplace(count);
        }

        // Post as many as the queue will take, not as many as were asked
        // for. How deep it is was negotiated with the peer and can be
        // shallower than the window; a refusal is that limit being
        // reported, not a failure.
        //
        // Letting it throw would be worse than shallow. The caller runs
        // inside a visit over a state that has already been moved from,
        // so an exception leaves the state machine holding a moved-from
        // endpoint, and the next read dereferences a null one.
        auto posted = std::size_t{0};
        for (auto i = std::size_t{0}; i < _immData->depth(); ++i)
        {
            if (!tryPostOne(endpoint))
            {
                break;
            }
            ++posted;
        }

        if (posted < _immData->depth())
        {
            MXL_INFO("Receive queue accepted {} of {} receives for immediate data", posted, _immData->depth());
        }
    }

    bool RMAGrainIngressProtocol::tryPostOne(Endpoint const& endpoint)
    {
        try
        {
            endpoint.recv(_immData->next());
            return true;
        }
        catch (FabricException const& e)
        {
            MXL_DEBUG("Could not post a receive for immediate data: {}", e.what());
            return false;
        }
    }

    //
    // RMASampleIngressProtocol implementations below
    RMASampleIngressProtocol::RMASampleIngressProtocol(Region region, DataLayout::Continuous const& layout)
        : _region{region}
        , _layout{layout}
        , _descriptors(DefaultDescriptorCount)
        , _isMemoryRegistered{false}
    {}

    std::vector<RemoteRegion> RMASampleIngressProtocol::registerMemory(std::shared_ptr<Domain> domain)
    {
        if (_isMemoryRegistered)
        {
            throw Exception::invalidState("Memory is already registered.");
        }

        auto const descriptors = Region{reinterpret_cast<std::uintptr_t>(_descriptors.data()),
            _descriptors.size() * sizeof(AudioEntryHeader),
            nullptr,
            nullptr,
            Region::Location::host()};
        domain->registerRegions({_region, descriptors}, FI_REMOTE_WRITE);
        _isMemoryRegistered = true;

        return domain->remoteRegions();
    }

    std::optional<TargetInfoContinuous> RMASampleIngressProtocol::continuousInfo() const
    {
        return TargetInfoContinuous{
            .version = TargetInfoContinuous::ProtocolVersion,
            .sampleSize = _layout.sampleSize,
            .channelCount = _layout.channelCount,
            .bufferLength = _layout.bufferLength,
            .descriptorCount = _descriptors.size(),
        };
    }

    void RMASampleIngressProtocol::start(Endpoint const& endpoint)
    {
        if (endpoint.domain()->usingRecvBufForCqData())
        {
            postReceives(endpoint, DefaultReceiveDepth);
        }
    }

    std::optional<Target::ReadResult> RMASampleIngressProtocol::read(Endpoint const& endpoint, Completion const& completion)
    {
        auto completionData = completion.tryData();
        if (!completionData)
        {
            // A receive that failed is gone from the window all the same.
            // Not replacing it shrank the window with every peer failure,
            // until writes carrying immediate data found none posted and
            // retried without end.
            // A flushed receive is not replaced: its queue is being torn
            // down, and a receive posted there would only be flushed again.
            auto const error = completion.tryErr();
            if (error && (error->code() == FI_ECANCELED))
            {
                return {};
            }
            if (error)
            {
                MXL_WARN("Completion error on target: {}", error->toString());
            }
            if (_immData)
            {
                static_cast<void>(tryPostOne(endpoint));
            }
            return {};
        }

        // One receive back for the one this completion consumed, so the
        // window stays open for as long as the connection lives.
        if (_immData)
        {
            static_cast<void>(tryPostOne(endpoint));
        }

        auto const immData = completionData->data();
        if (!immData)
        {
            throw Exception::invalidState("Received a completion without immediate data.");
        }

        auto const [slot, headTag] = ImmDataSample{static_cast<std::uint32_t>(*immData)}.unpack();
        if (slot >= _descriptors.size())
        {
            throw Exception::invalidArgument("Audio descriptor slot {} does not exist, there are {}.", slot, _descriptors.size());
        }

        // The slot was written before the completion was generated. Copy it out once, so the checks and the result see the same values.
        auto descriptor = AudioEntryHeader{};
        std::memcpy(&descriptor, &_descriptors[slot], sizeof(descriptor));

        if (ImmDataSample::headTag(descriptor.headIndex) != headTag)
        {
            throw Exception::invalidState("Audio descriptor slot {} was overwritten before it was read: it holds head index {}, the write that "
                                          "filled it carried tag {}.",
                slot,
                descriptor.headIndex,
                headTag);
        }

        // The writer of the flow commits at most half its ring at once; anything else did not come from an initiator that read this layout.
        if ((descriptor.count == 0) || (descriptor.count > (_layout.bufferLength / 2)) || (descriptor.count > descriptor.headIndex))
        {
            throw Exception::invalidArgument("Invalid audio descriptor in slot {}: {} samples ending at index {} for a ring of {} samples.",
                slot,
                descriptor.count,
                descriptor.headIndex,
                _layout.bufferLength);
        }

        return std::make_optional<Target::SampleReadResult>(descriptor.headIndex, descriptor.count);
    }

    bool RMASampleIngressProtocol::canReadGrains() const noexcept
    {
        return false;
    }

    bool RMASampleIngressProtocol::canReadSamples() const noexcept
    {
        return true;
    }

    void RMASampleIngressProtocol::reset()
    {}

    void RMASampleIngressProtocol::postReceives(Endpoint const& endpoint, std::size_t count)
    {
        if (!_immData)
        {
            _immData.emplace(count);
        }

        // Post as many as the queue will take, not as many as were asked
        // for. How deep it is was negotiated with the peer and can be
        // shallower than the window; a refusal is that limit being
        // reported, not a failure.
        //
        // Letting it throw would be worse than shallow. The caller runs
        // inside a visit over a state that has already been moved from,
        // so an exception leaves the state machine holding a moved-from
        // endpoint, and the next read dereferences a null one.
        auto posted = std::size_t{0};
        for (auto i = std::size_t{0}; i < _immData->depth(); ++i)
        {
            if (!tryPostOne(endpoint))
            {
                break;
            }
            ++posted;
        }

        if (posted < _immData->depth())
        {
            MXL_INFO("Receive queue accepted {} of {} receives for immediate data", posted, _immData->depth());
        }
    }

    bool RMASampleIngressProtocol::tryPostOne(Endpoint const& endpoint)
    {
        try
        {
            endpoint.recv(_immData->next());
            return true;
        }
        catch (FabricException const& e)
        {
            MXL_DEBUG("Could not post a receive for immediate data: {}", e.what());
            return false;
        }
    }

}
