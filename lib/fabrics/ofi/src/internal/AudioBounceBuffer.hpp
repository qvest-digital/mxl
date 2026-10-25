// SPDX-FileCopyrightText: 2025 Contributors to the Media eXchange Layer project.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <mxl/flow.h>
#include "DataLayout.hpp"
#include "Region.hpp"

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

    /**\brief An audio bounce buffer entry is backed by a collection of samples
     * The layout is:  AudioEntryHeader, followed by samples.
     */
    class AudioBounceBufferEntry
    {
    public:
        /** \brief Construct an AudioBounceBufferEntry with the given size in bytes. The size should be large enough to hold the samples for all
         * channels of a given batch size.
         *\note The header size is not included in the size parameter, it will be added on top of it. The total size of the bounce buffer entry will
         * be header size + samples size.
         */
        explicit AudioBounceBufferEntry(std::uint32_t size);

        /** \brief Return a view on the header of the bounce buffer entry.
         */
        [[nodiscard]]
        AudioEntryHeader const* header() const noexcept;

        /** \brief Return a view to the samples data of the bounce buffer entry.
         */
        [[nodiscard]]
        std::uint8_t const* samples() const noexcept;

        /** \brief Return a view to the beginning of the bounce buffer entry data.
         */
        [[nodiscard]]
        std::uint8_t const* data() const noexcept;

        /** \brief The total size of the bounce buffer entry in bytes.
         */
        [[nodiscard]]
        std::size_t size() const noexcept;

    private:
        std::vector<std::uint8_t> _data;
    };

    /** \brief A bounce buffer for audio samples, for initiators that predate TargetInfoContinuous.
     *
     * Such an initiator writes a batch as one write into one entry: the header, then every channel's samples one channel after the other. The
     * target copies the entry into its ring when the write completes.
     */
    class AudioBounceBuffer
    {
    public:
        AudioBounceBuffer(std::size_t entryCount, std::size_t entrySize, DataLayout::Continuous layout);

        /** \brief Return the regions corresponding to the bounce buffer entries. Each region corresponds to one bounce buffer entry. The regions are
         * returned in the same order as the bounce buffer entries. */
        [[nodiscard]]
        std::vector<Region> getRegions() const noexcept;

        /** \brief Return the number of entries in the bounce buffer. */
        [[nodiscard]]
        std::size_t entryCount() const noexcept;

        /** \brief Return the size of a bounce buffer entry in bytes.
         */
        [[nodiscard]]
        std::size_t entrySize() const noexcept;

        /** \brief Copy the samples from the bounce buffer entry to the provided output region, and return the header of the entry.
         *
         * Internally the header will be used to correctly map the bounce buffer entry slices to the output region slices.
         */
        AudioEntryHeader const& unpack(std::size_t entryIndex, Region const& outRegion) const;

    private:
        std::vector<AudioBounceBufferEntry> _entries;
        DataLayout::Continuous _layout;
    };
}
