// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <rdma/fabric.h>
#include "AudioBounceBuffer.hpp"
#include "DataLayout.hpp"
#include "Endpoint.hpp"
#include "Protocol.hpp"
#include "RegisteredRegion.hpp"
#include "SampleTransferQueue.hpp"
#include "SampleWritePlan.hpp"

namespace mxl::lib::fabrics::ofi
{
    class RMAGrainEgressProtocol /*final*/ : public EgressProtocol
    {
    public:
        /** \copydoc EgressProtocol::registerMemory()
         * \note This is a NOP for this protocol
         */
        virtual void registerMemory(std::shared_ptr<Domain> domain) final;

        /** \copydoc EgressProtocol::transferGrain()
         */
        virtual void transferGrain(Endpoint const& ep, std::uint64_t localIndex, std::uint64_t remoteIndex, std::uint32_t payloadOffset,
            SliceRange const& sliceRange, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::transferSamples()
         * \note This is unsupported in this protocol since it is designed for grain data. Calling this function will result in an
         * Exception::invalidState being thrown.
         */
        virtual void transferSamples(Endpoint const&, std::uint64_t, std::size_t, ::fi_addr_t) final;

        /** \copydoc EgressProtocol::processCompletion()
         */
        virtual void processCompletion(Endpoint const& ep, Completion::Data const& completion, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::processCompletionError()
         */
        virtual void processCompletionError(Endpoint const& ep, Completion::Error const& error, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::hasPendingWork()
         */
        [[nodiscard]]
        virtual bool hasPendingWork() const final;

        /** \copydoc EgressProtocol::reset()
         */
        virtual std::size_t reset() final;

    private:
        friend class RMAGrainEgressProtocolTemplate;

        RMAGrainEgressProtocol(Completion::Token token, TargetInfo info, DataLayout::Discrete dataLayout, std::vector<LocalRegion> _localRegions);

    private:
        Completion::Token _token;
        TargetInfo _remoteInfo;
        DataLayout::Discrete _layout;
        std::vector<LocalRegion> _localRegions;
        std::size_t _pending = 0;
    };

    /** \brief Template for creating an Egress protocol for RMA writer endpoint to handle transferring grains to remote targets using remote write
     * operations.
     */
    class RMAGrainEgressProtocolTemplate final : public EgressProtocolTemplate
    {
    public:
        RMAGrainEgressProtocolTemplate(DataLayout::Discrete layout, std::vector<Region> regions);

        virtual void registerMemory(std::shared_ptr<Domain> domain) override;
        virtual std::unique_ptr<EgressProtocol> createInstance(Completion::Token, TargetInfo remoteInfo) override;

    private:
        DataLayout::Discrete _layout;
        std::vector<Region> _regions;
        std::optional<std::vector<LocalRegion>> _localRegions{};
    };

    //
    // RMASampleEgressProtocol below

    /** \brief Egress protocol that writes audio samples straight from the source ring into a target's ring.
     *
     * A batch goes out as the data writes planSampleWrites() plans, followed by a descriptor write into one of the target's descriptor slots.
     * SampleTransferQueue decides when each is posted. The target's layout comes from TargetInfoContinuous.
     */
    class RMASampleEgressProtocol /*final*/ : public EgressProtocol
    {
    public:
        /** \copydoc EgressProtocol::registerMemory()
         * \note With this protocol, the local copy of the descriptor slots gets registered.
         */
        virtual void registerMemory(std::shared_ptr<Domain> domain) final;

        /** \copydoc EgressProtocol::transferGrain()
         *\note This is unsupported in this protocol since it is designed for audio data. Calling this function will result in an
         * Exception::invalidState being thrown.
         */
        virtual void transferGrain(Endpoint const& ep, std::uint64_t localIndex, std::uint64_t remoteIndex, std::uint32_t payloadOffset,
            SliceRange const& sliceRange, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::transferSamples()
         * \throws Exception::invalidArgument if count is 0, larger than headIndex, or larger than half the target ring or the source ring.
         * \throws FabricException if the provider refused a write. The batch stays queued and goes out with the next call or completion.
         */
        virtual void transferSamples(Endpoint const& ep, std::uint64_t headIndex, std::size_t count, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::processCompletion()
         */
        virtual void processCompletion(Endpoint const& ep, Completion::Data const& completion, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::processCompletionError()
         */
        virtual void processCompletionError(Endpoint const& ep, Completion::Error const& error, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::hasPendingWork()
         */
        [[nodiscard]]
        virtual bool hasPendingWork() const final;

        /** \copydoc EgressProtocol::reset()
         */
        virtual std::size_t reset() final;

    private:
        friend class RMASampleEgressProtocolTemplate;

        /** \brief Plans and posts the queue's writes on one endpoint.
         */
        class EndpointSink;

    private:
        /** \brief Construct the protocol for one target.
         * \param token Completion token of the target's writes.
         * \param info The target's info. It must carry TargetInfoContinuous, which the template checks.
         * \param layout Layout of the source ring.
         * \param localRegion The registered source ring.
         * \param limits Limits of one write on the endpoint.
         * \param ordering How the endpoint orders writes.
         */
        RMASampleEgressProtocol(Completion::Token token, TargetInfo info, DataLayout::Continuous layout, LocalRegion localRegion, WriteLimits limits,
            SampleTransferQueue::Ordering ordering);

        /** \brief Post what the queue allows after a completion.
         *
         * A write the provider refuses here is logged and stays queued for the next transfer or completion.
         */
        void pumpAfterCompletion(Endpoint const& ep, ::fi_addr_t destAddr);

    private:
        Completion::Token _token;                                /**< Completion token of the target's writes. */
        TargetInfo _remoteInfo;                                  /**< The target's info. */
        DataLayout::Continuous _layout;                          /**< Layout of the source ring. */
        DataLayout::Continuous _remoteLayout;                    /**< Layout of the target's ring. */
        LocalRegion _localRegion;                                /**< The registered source ring. */
        RemoteRegion _remoteRing;                                /**< The target's ring. */
        RemoteRegion _remoteDescriptors;                         /**< The target's descriptor slots. */
        WriteLimits _limits;                                     /**< Limits of one write on the endpoint. */
        std::vector<AudioEntryHeader> _descriptors;              /**< Local copy of the descriptor slots, written from here. */
        std::optional<RegisteredRegion> _descriptorRegistration; /**< Registration of _descriptors, owned here so it goes with the target. */
        std::optional<LocalRegion> _descriptorRegion;            /**< The registered region backing _descriptors. */
        SampleTransferQueue _queue;                              /**< Decides when the writes are posted. */
    };

    /** \brief Egress protocol for targets that receive audio through a bounce buffer.
     *
     * Targets that predate TargetInfoContinuous publish TargetInfoBounceBufferInfo and unpack every transfer from a bounce buffer entry. This
     * protocol copies the samples into a registered staging copy of the entry and sends it as one write.
     */
    class RMASampleBounceEgressProtocol /*final*/ : public EgressProtocol
    {
    public:
        /** \copydoc EgressProtocol::registerMemory()
         * \note With this protocol, the staging copy of the bounce buffer entries will get registered.
         */
        virtual void registerMemory(std::shared_ptr<Domain> domain) final;

        /** \copydoc EgressProtocol::transferGrain()
         *\note This is unsupported in this protocol since it is designed for audio data. Calling this function will result in an
         * Exception::invalidState being thrown.
         */
        virtual void transferGrain(Endpoint const& ep, std::uint64_t localIndex, std::uint64_t remoteIndex, std::uint32_t payloadOffset,
            SliceRange const& sliceRange, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::transferSamples()
         */
        virtual void transferSamples(Endpoint const& ep, std::uint64_t headIndex, std::size_t count, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::processCompletion()
         */
        virtual void processCompletion(Endpoint const& ep, Completion::Data const& completion, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::processCompletionError()
         */
        virtual void processCompletionError(Endpoint const& ep, Completion::Error const& error, ::fi_addr_t destAddr = FI_ADDR_UNSPEC) final;

        /** \copydoc EgressProtocol::hasPendingWork()
         */
        [[nodiscard]]
        virtual bool hasPendingWork() const final;

        /** \copydoc EgressProtocol::reset()
         */
        virtual std::size_t reset() final;

    private:
        friend class RMASampleEgressProtocolTemplate;

    private:
        RMASampleBounceEgressProtocol(Completion::Token token, TargetInfo info, DataLayout::Continuous dataLayout, LocalRegion _localRegion,
            std::size_t bounceBufferEntryCount);

        /** \brief Copy the samples of a transfer into a staging entry, channel after channel, the order the target unpacks them in.
         * \param layout The audio data layout.
         * \param headIndex The head index of the audio samples to transfer.
         * \param count The number of samples per channel to transfer.
         * \param region The local region corresponding to the user provided audio region.
         * \param dst Where the samples of the first channel go.
         */
        static void copySamples(DataLayout::Continuous const& layout, std::uint64_t headIndex, std::size_t count, LocalRegion const& region,
            std::uint8_t* dst);

        /** \brief Size of the staging copy of a target's bounce buffer, validated against what the target reported.
         * \throws Exception::invalidArgument if the geometry is empty, inconsistent or unreasonably large.
         */
        static std::size_t stagingSize(TargetInfo const& info, std::size_t entryCount);

    private:
        Completion::Token _token;
        TargetInfo _remoteInfo;
        DataLayout::Continuous _layout;
        LocalRegion _localRegion;           /**< Registered local region corresponding to the user provided audio region.  */
        std::vector<std::uint8_t> _staging; /**< One registered copy of each remote bounce buffer entry, header and samples, so a transfer is a
                                               single write. */
        std::optional<RegisteredRegion> _stagingRegistration; /**< Registration of _staging, owned here so it goes with the target. */
        std::optional<LocalRegion> _stagingRegion;            /**< The registered region backing _staging. */
        bool _saturated{false};                               /**< Every staging entry was in flight at the last transfer, which was skipped. */
        std::size_t _pending = 0;
        std::uint32_t _bounceBufferEntryIndex{0};             /**< The index of the bounce buffer entry to use for the next transfer. */
        std::size_t _bounceBufferEntryCount; /**< The total number of bounce buffer entries. Used to wrap around the bounce buffer entry index. */
    };

    /** \brief Template for creating an Egress protocol for RMA writer endpoint to handles transferring audio data to remote targets using remote
     * write operations.
     *
     * A target that publishes TargetInfoContinuous gets an RMASampleEgressProtocol, one that publishes only TargetInfoBounceBufferInfo an
     * RMASampleBounceEgressProtocol.
     */
    class RMASampleEgressProtocolTemplate final : public EgressProtocolTemplate
    {
    public:
        RMASampleEgressProtocolTemplate(DataLayout::Continuous layout, Region region);

        /** \copydoc EgressProtocolTemplate::registerMemory()
         * \note This also reads the write limits and the ordering of the endpoints the domain creates.
         */
        virtual void registerMemory(std::shared_ptr<Domain> domain) override;

        /** \copydoc EgressProtocolTemplate::createInstance()
         * \throws Exception::invalidArgument if the target publishes no audio layout, a protocol version this initiator does not speak, or a
         * ring that does not match the source flow.
         */
        virtual std::unique_ptr<EgressProtocol> createInstance(Completion::Token, TargetInfo remoteInfo) override;

    private:
        DataLayout::Continuous _layout;
        Region _region;                          /**< Region provided by the user. */
        std::optional<LocalRegion> _localRegion; /**< Registered local region corresponding to the user provided region. This is what will actually be
                                                    used for remote writes. */
        WriteLimits _limits;                     /**< Limits of one write on the domain's endpoints. */
        SampleTransferQueue::Ordering _ordering; /**< How the domain's endpoints order writes. */
    };

}
