// SPDX-FileCopyrightText: 2025 Contributors to the Media eXchange Layer project.
// SPDX-License-Identifier: Apache-2.0

#include "AudioBounceBuffer.hpp"
#include <cstddef>
#include <cstdint>

namespace mxl::lib::fabrics::ofi
{
    void getMutableMultiBufferSlices(std::uint64_t index, std::size_t count, std::size_t bufferLength, std::size_t sampleWordSize,
        std::size_t channelCount, std::uint8_t* baseBufferPtr, mxlMutableWrappedMultiBufferSlice& slice) noexcept
    {
        auto const startOffset = (index + bufferLength - count) % bufferLength;
        auto const endOffset = (index % bufferLength);

        // Empty unless count > 0: start and end offsets coincide both for no samples and for a full ring, and read as the latter the second
        // length underflowed to nearly SIZE_MAX.
        auto const firstLength = (count == 0) ? 0 : (startOffset < endOffset) ? count : bufferLength - startOffset;
        auto const secondLength = count - firstLength;

        slice.base.fragments[0].pointer = baseBufferPtr + (sampleWordSize * startOffset);
        slice.base.fragments[0].size = sampleWordSize * firstLength;

        slice.base.fragments[1].pointer = baseBufferPtr;
        slice.base.fragments[1].size = sampleWordSize * secondLength;

        slice.stride = sampleWordSize * bufferLength;
        slice.count = channelCount;
    }
}
