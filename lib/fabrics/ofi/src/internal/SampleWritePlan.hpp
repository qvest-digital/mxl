// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include "DataLayout.hpp"
#include "LocalRegion.hpp"
#include "RemoteRegion.hpp"

namespace mxl::lib::fabrics::ofi
{
    /** \brief The limits of a single RMA write on an endpoint.
     */
    struct WriteLimits
    {
        std::size_t iovLimit;    /**< Most local buffers in one write, from tx_attr->iov_limit. */
        std::size_t rmaIovLimit; /**< Most remote buffers in one write, from tx_attr->rma_iov_limit. */
        std::size_t maxMsgSize;  /**< Most bytes in one write, from ep_attr->max_msg_size. */
    };

    /** \brief One RMA write.
     *
     * The local buffers are read in order as one byte stream, which fills the remote buffers in order. Both lists carry the same number of
     * bytes.
     */
    struct SampleWrite
    {
        std::vector<LocalRegion> local;   /**< Source buffers, in order. */
        std::vector<RemoteRegion> remote; /**< Destination buffers, in order. */
    };

    /** \brief Plan the writes that copy a batch of samples from a source ring into a destination ring.
     *
     * Both rings are channel-major: channel c holds bufferLength samples starting at byte c * bufferLength * sampleSize, and sample index i
     * sits at position i % bufferLength of every channel. The two rings may differ in bufferLength. The batch is the samples
     * [headIndex - count, headIndex) of every channel.
     *
     * Each channel is cut where either ring wraps. The pieces are packed, channel after channel, into as few writes as the limits allow:
     * pieces that are contiguous on both sides are joined, a write takes another piece while it has room for its local and remote buffers,
     * and a piece is split where a write reaches maxMsgSize bytes.
     *
     * \param[in] srcLayout Layout of the source ring.
     * \param[in] srcRing The whole source ring, registered for local access.
     * \param[in] dstLayout Layout of the destination ring.
     * \param[in] dstRing The whole destination ring, registered for remote write.
     * \param[in] headIndex The index one past the last sample of the batch.
     * \param[in] count The number of samples per channel in the batch.
     * \param[in] limits The limits every write must respect.
     * \return The writes, in channel order. Together they cover the batch exactly once.
     * \throws Exception::invalidArgument if the layouts differ in sample size or channel count, a ring region is smaller than its layout, count
     * is 0, larger than headIndex or longer than either ring, or a limit is 0.
     */
    [[nodiscard]]
    std::vector<SampleWrite> planSampleWrites(DataLayout::Continuous const& srcLayout, LocalRegion const& srcRing,
        DataLayout::Continuous const& dstLayout, RemoteRegion const& dstRing, std::uint64_t headIndex, std::size_t count, WriteLimits const& limits);
}
