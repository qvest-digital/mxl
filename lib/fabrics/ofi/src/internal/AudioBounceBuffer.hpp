// SPDX-FileCopyrightText: 2025 Contributors to the Media eXchange Layer project.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <mxl/flow.h>

namespace mxl::lib::fabrics::ofi
{

    /** \brief Describes a batch of audio samples: the samples [headIndex - count, headIndex) of every channel.
     *
     * It is the descriptor an initiator writes into a target's descriptor slot after the samples of a batch, and the header of a bounce buffer
     * entry for targets that predate TargetInfoContinuous. Both sides read it as raw memory, so its layout is part of the wire format.
     */
    struct AudioEntryHeader
    {
        std::uint64_t headIndex; /**< The index one past the last sample of the batch. */
        std::size_t count;       /**< The number of samples per channel in the batch. */
    };

    /** \brief Describe where a batch of samples sits in a channel-major ring.
     *
     * \param[in] index The index one past the last sample of the batch.
     * \param[in] count The number of samples per channel in the batch. 0 describes no fragments.
     * \param[in] bufferLength The number of samples per channel in the ring.
     * \param[in] sampleWordSize The size of one sample in bytes.
     * \param[in] channelCount The number of channels.
     * \param[in] baseBufferPtr The start of the ring.
     * \param[out] slice The fragments of channel 0, before and after the wrap, and the stride to the next channel.
     */
    void getMutableMultiBufferSlices(std::uint64_t index, std::size_t count, std::size_t bufferLength, std::size_t sampleWordSize,
        std::size_t channelCount, std::uint8_t* baseBufferPtr, mxlMutableWrappedMultiBufferSlice& slice) noexcept;
}
