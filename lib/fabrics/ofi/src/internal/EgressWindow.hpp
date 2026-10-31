// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mxl/fabrics.h>

namespace mxl::lib::fabrics::ofi
{
    /** \brief The writes and transfers one target may have outstanding, and what happened to the ones it was offered.
     *
     * A transfer is one grain or one batch of samples. It can take several writes: a grain sent in chunks takes one per chunk and plane.
     * Only the write that ends a transfer has to report a completion. On a reliable connection writes complete in the order they were
     * posted, so that completion also accounts for every unreported write posted before it, including the writes of a transfer that was
     * given up part way.
     *
     * Two limits apply. maxTransfers bounds how many transfers are in flight, which is what the receive queue of the target has to absorb
     * when every transfer carries immediate data. maxWrites bounds the writes in flight, which is what the send queue and the completion
     * queue of the initiator have to hold. A provider signals every write to its completion queue whether or not the caller asked for a
     * completion, and a connection that fails flushes every write it holds there, so the completion queue has to be sized for maxWrites
     * per target.
     *
     * A transfer that does not fit is not queued and not retried: it is counted as skipped and the caller moves on. Another target of the
     * same initiator is not affected.
     */
    class EgressWindow
    {
    public:
        /** \param maxTransfers Transfers allowed in flight. At least 1.
         * \param maxWrites Writes allowed in flight. At least 1.
         */
        EgressWindow(std::size_t maxTransfers, std::size_t maxWrites) noexcept;

        /** \brief Start a transfer.
         * \return true when the transfer fits and is now open, false when it was counted as skipped.
         */
        [[nodiscard]]
        bool open() noexcept;

        /** \brief Whether the open transfer can post this many more writes.
         */
        [[nodiscard]]
        bool hasRoomFor(std::size_t writes) const noexcept;

        /** \brief Record one write handed to the provider.
         * \param reportsCompletion The write was posted with a completion requested.
         * \param endsTransfer The write is the last one of the open transfer. Such a write always reports a completion.
         * \param index The grain index or sample head index of the open transfer.
         * \param bytes The payload the write carries.
         */
        void wrote(bool reportsCompletion, bool endsTransfer, std::uint64_t index, std::size_t bytes) noexcept;

        /** \brief Give up the open transfer. Writes it already posted stay in flight until a later completion accounts for them.
         */
        void abandon() noexcept;

        /** \brief Account for one reported completion.
         * \param success Whether the write completed without an error.
         */
        void complete(bool success) noexcept;

        /** \brief Forget everything in flight because the connection carrying it is gone. What was in flight is counted as failed.
         * \return The number of completions that were still expected.
         */
        std::size_t reset() noexcept;

        /** \brief Whether a reported completion is still expected.
         */
        [[nodiscard]]
        bool expectsCompletion() const noexcept;

        /** \brief Count a transfer that never reached this window, for example because the target was not connected.
         */
        void skip() noexcept;

        [[nodiscard]]
        std::size_t maxTransfers() const noexcept;

        [[nodiscard]]
        std::size_t maxWrites() const noexcept;

        [[nodiscard]]
        mxlFabricsTargetStats stats() const noexcept;

    private:
        /** \brief The writes and transfers one reported completion accounts for.
         */
        struct Segment
        {
            std::size_t transfers;
            std::size_t writes;
            std::size_t bytes;
            std::uint64_t index;
        };

        std::size_t _maxTransfers;
        std::size_t _maxWrites;

        std::deque<Segment> _segments{};    /**< One entry per completion still expected, oldest first. */
        std::size_t _openTransfers{0};      /**< Transfers posted since the last write that reports a completion. */
        std::size_t _openWrites{0};         /**< Writes posted since the last write that reports a completion. */
        std::size_t _openBytes{0};          /**< Payload of those writes. */
        std::size_t _transfersInFlight{0};
        std::size_t _writesInFlight{0};

        std::uint64_t _posted{0};
        std::uint64_t _completed{0};
        std::uint64_t _failed{0};
        std::uint64_t _skipped{0};
        std::uint64_t _bytesCompleted{0};
        std::uint64_t _lastCompletedIndex{MXL_FABRICS_NO_INDEX};
    };
}
