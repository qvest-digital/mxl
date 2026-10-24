// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>
#include "SampleWritePlan.hpp"

namespace mxl::lib::fabrics::ofi
{
    /** \brief Decides when the writes of an audio stream to one target are posted.
     *
     * A batch is the samples [headIndex - count, headIndex) of every channel. Its data writes go straight into the target's ring. A descriptor
     * write follows them: it puts {headIndex, count} into a descriptor slot on the target and carries immediate data, and its arrival tells the
     * target that the batch has landed. The target commits batches in the order their descriptors arrive and zero-fills any samples skipped
     * between two commits, so descriptors must arrive in order and only after the data they describe.
     *
     * With Ordering::Ordered the endpoint places writes in the order they were posted, and a write with immediate data completes on the target
     * only after the writes before it have landed (verbs RC, tcp; FI_EP_MSG). The descriptor is posted right behind its data writes.
     *
     * With Ordering::Gated writes are placed in any order (EFA; FI_EP_RDM). A descriptor is posted only once every write posted before it has
     * completed, which covers the data it describes and the previous descriptor. Every write asks for FI_DELIVERY_COMPLETE, so a completion
     * means the data has been placed in the target's memory. The data writes of the next batch go out while a descriptor waits.
     *
     * Batches that arrive while one is in flight are merged into one pending range, which the source ring still holds. The pending range never
     * grows longer than the most a target writer can commit at once; when it would, its oldest samples are dropped and the target zero-fills
     * them when it commits the next batch.
     *
     * The queue only counts completions; it cannot tell which write completed. In ordered mode a descriptor slot is reused once as many writes
     * have completed as had been posted up to and including its descriptor, which holds when an endpoint completes writes in the order they were
     * posted.
     */
    class SampleTransferQueue
    {
    public:
        /** \brief How the endpoint orders writes. */
        enum class Ordering
        {
            Ordered, /**< Writes are placed in the order they are posted. */
            Gated,   /**< Writes may be placed in any order. */
        };

        /** \brief Where the queue gets its writes from and posts them to.
         */
        class Sink
        {
        public:
            virtual ~Sink() = default;

            /** \brief Plan the data writes of a batch.
             * \param headIndex The index one past the last sample of the batch.
             * \param count The number of samples per channel in the batch.
             * \return The data writes of the batch.
             */
            [[nodiscard]]
            virtual std::vector<SampleWrite> plan(std::uint64_t headIndex, std::size_t count) = 0;

            /** \brief Build the descriptor write of a batch.
             * \param headIndex The index one past the last sample of the batch.
             * \param count The number of samples per channel in the batch.
             * \param slot The descriptor slot to use, below the descriptor count the queue was made with.
             * \return The write and its immediate data.
             */
            [[nodiscard]]
            virtual std::pair<SampleWrite, std::uint32_t> describe(std::uint64_t headIndex, std::size_t count, std::size_t slot) = 0;

            /** \brief Post a write.
             * \param write The write to post.
             * \param immData Immediate data to send with the write, if any.
             * \throws Any exception when the write was not posted. The queue keeps the write and posts it again on the next pump().
             */
            virtual void post(SampleWrite const& write, std::optional<std::uint32_t> immData) = 0;
        };

    public:
        /** \brief Construct an empty queue.
         * \param ordering How the endpoint orders writes.
         * \param maxCount The most samples per channel in one batch: at most half the target ring, which is the most its writer commits at once,
         * and at most the source ring.
         * \param descriptorCount The number of descriptor slots on the target.
         * \throws Exception::invalidArgument if maxCount or descriptorCount is 0.
         */
        SampleTransferQueue(Ordering ordering, std::size_t maxCount, std::size_t descriptorCount);

        /** \brief Accept the batch [headIndex - count, headIndex).
         *
         * Samples at or before the end of a batch already accepted are not sent again. A batch that arrives while another is pending is merged
         * with it, gap included.
         * \param headIndex The index one past the last sample of the batch.
         * \param count The number of samples per channel in the batch.
         * \throws Exception::invalidArgument if count is 0, larger than headIndex or larger than maxCount.
         */
        void push(std::uint64_t headIndex, std::size_t count);

        /** \brief Post every write the ordering allows now.
         * \param sink Plans, builds and posts the writes.
         * \throws Whatever Sink::post() throws. The write that failed and everything after it stay queued for the next call.
         */
        void pump(Sink& sink);

        /** \brief Account for a write that completed.
         */
        void complete() noexcept;

        /** \brief Account for a write that completed with an error.
         *
         * Which write failed is unknown, so the batch whose descriptor is not posted yet is dropped: its data may not have landed.
         */
        void fail() noexcept;

        /** \brief Whether writes are in flight.
         */
        [[nodiscard]]
        bool hasPendingWork() const noexcept;

        /** \brief Forget every batch and write, for an endpoint whose writes will never complete.
         * \return The number of writes that were in flight.
         */
        std::size_t reset() noexcept;

    private:
        /** \brief A range of sample indices, end exclusive.
         */
        struct Range
        {
            std::uint64_t start; /**< First index. */
            std::uint64_t end;   /**< One past the last index. */
        };

        /** \brief The batch whose writes are being posted.
         */
        struct Batch
        {
            Range range;                     /**< The samples of the batch. */
            std::vector<SampleWrite> writes; /**< Its data writes. */
            std::size_t posted;              /**< How many of the data writes are posted. */
        };

        /** \brief Account for a posted write.
         */
        void posted() noexcept;

        /** \brief Free the descriptor slots whose writes have all completed.
         */
        void releaseSlots() noexcept;

    private:
        Ordering _ordering;
        std::size_t _maxCount;
        std::size_t _descriptorCount;

        std::optional<std::uint64_t> _acceptedEnd; /**< End of the last batch accepted. */
        std::optional<Range> _pending;             /**< Samples accepted and not yet planned. */
        std::optional<Batch> _current;             /**< The batch being posted, until its descriptor is posted. */
        bool _dropping;                            /**< Samples were dropped from the pending range, and the drop was logged. */

        std::size_t _inFlight;                     /**< Writes posted and not completed. */
        std::uint64_t _postedTotal;                /**< Writes posted since the last reset. */
        std::uint64_t _completedTotal;             /**< Writes completed since the last reset. */
        std::deque<std::uint64_t> _slotsBusy;      /**< For every descriptor in flight, _postedTotal right after it was posted. */
        std::size_t _nextSlot;                     /**< The descriptor slot the next batch uses. */
    };
}
