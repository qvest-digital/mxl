// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <vector>
#include "AudioBounceBuffer.hpp"
#include "DataLayout.hpp"
#include "Protocol.hpp"

namespace mxl::lib::fabrics::ofi
{
    /** \brief A fixed pool of landing buffers for immediate data, with a
     * round-robin cursor over them.
     *
     * A provider that reports FI_RX_CQ_DATA delivers the immediate data of
     * an RDMA write with immediate only into a posted receive, and the
     * write consumes that receive. Keeping a single one posted therefore
     * lets an initiator have exactly one write in flight: the second write
     * of any burst finds the receive queue empty and the transport falls
     * back on RNR retry. No credit is exchanged that would tell the
     * initiator to stop, so it keeps retrying -- on providers that leave
     * the RNR retry count at its infinite sentinel, forever.
     *
     * Keeping a window posted is what lets a writer committing several
     * batches between two transfer periods be mirrored at all. The buffers
     * themselves are never read: the immediate data is taken from the
     * completion, and they exist only because the provider requires
     * somewhere to put it.
     *
     * The storage is sized once and never resized, because a posted
     * receive holds the address of its buffer.
     */
    class ImmediateDataPool
    {
    public:
        explicit ImmediateDataPool(std::size_t depth);

        /** \brief The next buffer in the pool, as a region to post.
         */
        [[nodiscard]]
        LocalRegion next() noexcept;

        /** \brief How many receives are meant to be kept posted against it.
         */
        [[nodiscard]]
        std::size_t depth() const noexcept;

    private:
        std::vector<Target::ImmediateDataLocation> _buffers;
        std::size_t _cursor{0};
    };

    /** \brief Receives kept posted for immediate data.
     *
     * Large enough that a producer committing several batches between two
     * transfer periods is absorbed without an RNR round trip, and far below
     * the receive-queue size every provider we build against reports.
     */
    constexpr std::size_t DefaultReceiveDepth = 32;

    /** \brief Ingress protocol for RMA writer endpoint.
     *
     * Handles processing of completions when paired with an endpoint that does remote write to our buffers without bounce buffering.
     */
    class RMAGrainIngressProtocol final : public IngressProtocol
    {
    public:
        RMAGrainIngressProtocol(std::vector<Region> regions);

        /** \copydoc IngressProtocol::registerMemory()
         */
        [[nodiscard]]
        virtual std::vector<RemoteRegion> registerMemory(std::shared_ptr<Domain> domain) override;

        /** \copydoc IngressProtocol::bounceBufferInfo()
         * \note This protocol receives grains, so this function returns an empty optional.
         */
        [[nodiscard]]
        virtual std::optional<TargetInfoBounceBufferInfo> bounceBufferInfo() const override;

        /** \copydoc IngressProtocol::continuousInfo()
         * \note This protocol receives grains, so this function returns an empty optional.
         */
        [[nodiscard]]
        virtual std::optional<TargetInfoContinuous> continuousInfo() const override;

        /** \copydoc IngressProtocol::start()
         */
        virtual void start(Endpoint const& endpoint) override;

        /** \copydoc IngressProtocol::processCompletion()
         */
        [[nodiscard]]
        virtual std::optional<Target::ReadResult> read(Endpoint const& endpoint, Completion const& completion) override;

        /**\brief This protocol can read grains, but not samples, since it is designed for remote writes of grain buffers.
         */
        [[nodiscard]]
        virtual bool canReadGrains() const noexcept override;

        /** \brief This protocol cannot read samples, since it is designed for remote writes of grain buffers.
         */
        [[nodiscard]]
        virtual bool canReadSamples() const noexcept override;

        /** \copydoc IngressProtocol::destroy()
         */
        virtual void reset() override;

    private:
        void postReceives(Endpoint const& endpoint, std::size_t count);
        bool tryPostOne(Endpoint const& endpoint);

    private:
        std::vector<Region> _regions;
        bool _isMemoryRegistered{false};
        std::optional<ImmediateDataPool> _immData{};
    };

    /** \brief Descriptor slots a target offers for audio batches.
     *
     * A slot is reused after this many batches, so a target that lags further behind its initiator reads a slot that was overwritten, which
     * ImmDataSample detects.
     */
    constexpr std::size_t DefaultDescriptorCount = 256;

    /** \brief Ingress protocol for RMA writer endpoint for audio samples.
     *
     * It serves two kinds of initiator on the same endpoint. One that reads TargetInfoContinuous writes samples straight into the flow's ring,
     * then writes {headIndex, count} into one of the descriptor slots registered here, with immediate data built by ImmDataSample. One that
     * predates it writes a batch into an entry of the bounce buffer registered here, with the entry index as immediate data, and the samples
     * are copied into the ring when the completion is read. Bit 31 of the immediate data tells the two apart. Reading a completion yields the
     * batch either way; the application commits it.
     */
    class RMASampleIngressProtocol final : public IngressProtocol
    {
    public:
        /** Construct an RMASampleIngressProtocol with the given region and data layout.
         * \param region The memory region holding the flow's ring. Initiators write samples into it.
         * \param dataLayout The layout of the ring.
         * \param maxSyncBatchSize The most samples per channel a bounce buffer entry holds.
         * \throws Exception::invalidArgument if the bounce buffer would need 2^31 entries or more.
         */
        RMASampleIngressProtocol(Region region, DataLayout::Continuous const& dataLayout, std::uint32_t maxSyncBatchSize);

        /** \copydoc IngressProtocol::registerMemory()
         * \note This registers the bounce buffer entries, the ring and the descriptor slots, in that order, and returns the bounce buffer
         * entries alone. The ring and the descriptor slots are published through continuousInfo().
         */
        [[nodiscard]]
        virtual std::vector<RemoteRegion> registerMemory(std::shared_ptr<Domain> domain) override;

        /** \copydoc IngressProtocol::bounceBufferInfo()
         */
        [[nodiscard]]
        virtual std::optional<TargetInfoBounceBufferInfo> bounceBufferInfo() const override;

        /** \copydoc IngressProtocol::continuousInfo()
         * \throws Exception::invalidState if registerMemory() has not run.
         */
        [[nodiscard]]
        virtual std::optional<TargetInfoContinuous> continuousInfo() const override;

        /** \copydoc IngressProtocol::start()
         */
        virtual void start(Endpoint const& endpoint) override;

        /** \copydoc IngressProtocol::read()
         * \throws Exception::invalidState if the completion has no immediate data, or the descriptor slot it names was overwritten before it
         * was read.
         * \throws Exception::invalidArgument if the slot does not exist or the descriptor in it describes no batch the ring can hold, or the
         * bounce buffer entry holds more samples than the ring or the entry.
         * \throws std::out_of_range if the bounce buffer entry does not exist.
         */
        [[nodiscard]]
        virtual std::optional<Target::ReadResult> read(Endpoint const& endpoint, Completion const& completion) override;

        /** \brief This protocol cannot read grains, since it is designed for remote writes of audio samples.
         */
        [[nodiscard]]
        virtual bool canReadGrains() const noexcept override;

        /** \brief This protocol can read samples, since it is designed for remote writes of audio samples.
         */
        [[nodiscard]]
        virtual bool canReadSamples() const noexcept override;

        /** \copydoc IngressProtocol::reset()
         */
        virtual void reset() override;

    private:
        void postReceives(Endpoint const& endpoint, std::size_t count);
        bool tryPostOne(Endpoint const& endpoint);

        /** \brief Read the batch a descriptor write announced.
         */
        [[nodiscard]]
        Target::SampleReadResult readDescriptor(std::uint32_t immData) const;

        /** \brief Helper function to create an AudioBounceBuffer based on the given audio data layout and maximum synchronous batch size.
         * Entries are as big as the maximum number of samples that can be transferred in a single batch, which is determined by maxSyncBatchSize.
         * The number of entries is determined by how many batches are needed to cover the entire history buffer.
         */
        static AudioBounceBuffer makeAudioBounceBuffer(DataLayout::Continuous const& layout, std::uint32_t maxSyncBatchSize);

    private:
        AudioBounceBuffer _bounceBuffer;                   /**< Entries written by initiators that predate TargetInfoContinuous. */
        Region _region;                                    /**< The flow's ring. */
        DataLayout::Continuous _layout;                    /**< Layout of the ring. */
        std::vector<AudioEntryHeader> _descriptors;        /**< Descriptor slots, written by initiators. */
        std::optional<TargetInfoContinuous> _continuous{}; /**< The published layout, set once registerMemory() ran. */
        std::optional<ImmediateDataPool> _immData{};       /**< Receives for immediate data, where the provider needs them. */
    };
}
