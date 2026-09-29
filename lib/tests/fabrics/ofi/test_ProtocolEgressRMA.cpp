// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "AudioBounceBuffer.hpp"
#include "ProtocolEgressRMA.hpp"

using namespace mxl::lib::fabrics::ofi;

namespace
{
    /// The sample type of the test rings.
    using Sample = std::uint32_t;

    /// The number of channels of the test rings.
    constexpr auto ChannelCount = std::size_t{2};
    /// The head index of the transfer under test.
    constexpr auto HeadIndex = std::uint64_t{11};
    /// The number of samples per channel the transfer carries.
    constexpr auto SampleCount = std::size_t{5};
    /// The value of a ring sample that no transfer wrote.
    constexpr auto UntouchedSample = Sample{0xFFFFFFFFU};

    /**
     * Get the value written for a sample.
     * @param[in] channel The channel of the sample.
     * @param[in] index The absolute index of the sample.
     * @return A value unique to channel and index.
     */
    Sample sampleValue(std::size_t channel, std::uint64_t index) noexcept
    {
        return static_cast<Sample>((channel * 1000U) + index);
    }
}

// The samples at indices 6 to 10 sit at offsets 6, 0, 1, 2, 3 of a source ring of 7 (split 1 + 4) and at offsets 6, 7, 8, 0, 1 of a
// target ring of 9 (split 3 + 2). The staging copy is channel-major, so the target gets each channel back whole despite the different split.
TEST_CASE("ofi: audio staging copy unpacks into a ring at a different position", "[ofi][ProtocolEgressRMA]")
{
    constexpr auto SourceBufferLength = std::size_t{7};
    constexpr auto TargetBufferLength = std::size_t{9};

    auto source = std::vector<Sample>(ChannelCount * SourceBufferLength, UntouchedSample);
    auto expectedTarget = std::vector<Sample>(ChannelCount * TargetBufferLength, UntouchedSample);
    for (auto channel = std::size_t{0}; channel < ChannelCount; channel++)
    {
        for (auto index = HeadIndex - SampleCount; index < HeadIndex; index++)
        {
            source[(channel * SourceBufferLength) + (index % SourceBufferLength)] = sampleValue(channel, index);
            expectedTarget[(channel * TargetBufferLength) + (index % TargetBufferLength)] = sampleValue(channel, index);
        }
    }

    auto const sourceLayout = DataLayout::Continuous{.sampleSize = sizeof(Sample), .channelCount = ChannelCount, .bufferLength = SourceBufferLength};
    auto const targetLayout = DataLayout::Continuous{.sampleSize = sizeof(Sample), .channelCount = ChannelCount, .bufferLength = TargetBufferLength};
    auto bounceBuffer = AudioBounceBuffer{1, SampleCount * ChannelCount * sizeof(Sample), targetLayout};

    auto* entryData = reinterpret_cast<std::uint8_t*>(bounceBuffer.getRegions().front().base); // NOLINT
    auto const header = AudioEntryHeader{.headIndex = HeadIndex, .count = SampleCount};
    std::memcpy(entryData, &header, sizeof(header));
    auto const sourceRegion = LocalRegion{
        .addr = reinterpret_cast<std::uintptr_t>(source.data()), .len = source.size() * sizeof(Sample), .desc = nullptr};
    RMASampleEgressProtocol::copySamples(sourceLayout, HeadIndex, SampleCount, sourceRegion, entryData + sizeof(AudioEntryHeader));

    auto target = std::vector<Sample>(ChannelCount * TargetBufferLength, UntouchedSample);
    auto const outRegion = Region{
        reinterpret_cast<std::uintptr_t>(target.data()), target.size() * sizeof(Sample), nullptr, nullptr, Region::Location::host()};
    auto const& unpackedHeader = bounceBuffer.unpack(0, outRegion);
    REQUIRE(unpackedHeader.headIndex == HeadIndex);
    REQUIRE(unpackedHeader.count == SampleCount);
    REQUIRE(target == expectedTarget);
}
