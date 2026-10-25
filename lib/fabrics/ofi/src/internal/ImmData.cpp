// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include "ImmData.hpp"
#include <cstdint>
#include <bit>

namespace mxl::lib::fabrics::ofi
{
    ImmDataGrain::ImmDataGrain(std::uint32_t data) noexcept
    {
        _inner = data;
    }

    ImmDataGrain::ImmDataGrain(std::uint64_t index, std::uint16_t sliceIndex) noexcept
    {
        auto ringBufferIndex = static_cast<std::uint16_t>(index);
        _inner = std::bit_cast<std::uint32_t>(Unpacked{.ringBufferSlot = ringBufferIndex, .sliceIndex = sliceIndex});
    }

    ImmDataGrain::Unpacked ImmDataGrain::unpack() const noexcept
    {
        return std::bit_cast<Unpacked>(_inner);
    }

    std::uint32_t ImmDataGrain::data() const noexcept
    {
        return _inner;
    }

    ImmDataSample::ImmDataSample(std::uint32_t data) noexcept
        : _inner{data}
    {}

    namespace
    {
        constexpr auto DescriptorFlag = std::uint32_t{1U} << 31U;
        constexpr auto HeadTagMask = std::uint32_t{0x7FFFU};
    }

    ImmDataSample::ImmDataSample(std::size_t slot, std::uint64_t headIndex) noexcept
        : _inner{DescriptorFlag | static_cast<std::uint32_t>(slot & 0xFFFFU) | (static_cast<std::uint32_t>(headTag(headIndex)) << 16U)}
    {}

    ImmDataSample::Unpacked ImmDataSample::unpack() const noexcept
    {
        return Unpacked{.slot = static_cast<std::uint16_t>(_inner & 0xFFFFU), .headTag = static_cast<std::uint16_t>((_inner >> 16U) & HeadTagMask)};
    }

    std::uint32_t ImmDataSample::data() const noexcept
    {
        return _inner;
    }

    std::uint16_t ImmDataSample::headTag(std::uint64_t headIndex) noexcept
    {
        return static_cast<std::uint16_t>(headIndex & HeadTagMask);
    }

    bool ImmDataSample::isDescriptor(std::uint32_t data) noexcept
    {
        return (data & DescriptorFlag) != 0;
    }
}
