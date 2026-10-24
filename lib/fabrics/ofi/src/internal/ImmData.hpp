// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>

namespace mxl::lib::fabrics::ofi
{

    /** \brief Immediate data representation for discrete flow transfers.
     */
    class ImmDataGrain
    {
    public:
        struct Unpacked //**< Unpacked representation of immediate data. */
        {
            std::uint16_t ringBufferSlot;
            std::uint16_t sliceIndex;
        };

    public:
        /** \brief Create immediate data from packed data.
         *
         * \param data The packed immediate data.
         */
        ImmDataGrain(std::uint32_t data) noexcept;

        /** \brief Create immediate data from ring buffer index and slice index.
         *
         * \param index The ring buffer index.
         * \param sliceIndex The slice index within the ring buffer.
         */
        ImmDataGrain(std::uint64_t index, std::uint16_t sliceIndex) noexcept;

        /** \brief Unpack the immediate data into ring buffer index and slice index.
         *
         * \return A pair containing the ring buffer index and slice index.
         */
        [[nodiscard]]
        Unpacked unpack() const noexcept;

        /** \brief Get the packed immediate data.
         *
         * \return The packed immediate data.
         */
        [[nodiscard]]
        std::uint32_t data() const noexcept;

    private:
        std::uint32_t _inner; /**< Packed representation of immediate data. */
    };

    /** \brief Immediate data of an audio descriptor write.
     *
     * The low 16 bits name the descriptor slot the write filled. The high 16 bits repeat the low 16 bits of the head index written into that
     * slot, so the target can tell a slot that was overwritten before it read it.
     */
    class ImmDataSample
    {
    public:
        struct Unpacked            /**< Unpacked representation of immediate data. */
        {
            std::uint16_t slot;    /**< The descriptor slot. */
            std::uint16_t headTag; /**< The low 16 bits of the head index in the slot. */
        };

    public:
        /** \brief Create immediate data from packed data.
         * \param data The packed immediate data.
         */
        ImmDataSample(std::uint32_t data) noexcept;

        /** \brief Create immediate data from a descriptor slot and the head index written into it.
         * \param slot The descriptor slot, below 65536.
         * \param headIndex The head index written into the slot.
         */
        ImmDataSample(std::size_t slot, std::uint64_t headIndex) noexcept;

        /** \brief Unpack the immediate data into slot and head tag.
         */
        [[nodiscard]]
        Unpacked unpack() const noexcept;

        /** \brief Get the packed immediate data.
         */
        [[nodiscard]]
        std::uint32_t data() const noexcept;

        /** \brief The head tag a head index produces.
         * \param headIndex A head index.
         * \return Its low 16 bits.
         */
        [[nodiscard]]
        static std::uint16_t headTag(std::uint64_t headIndex) noexcept;

    private:
        std::uint32_t _inner; /**< Packed representation of immediate data. */
    };
}
