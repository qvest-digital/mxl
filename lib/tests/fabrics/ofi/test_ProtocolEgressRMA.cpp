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
    using Sample = std::uint32_t;

    constexpr auto ChannelCount = std::size_t{2};
    constexpr auto HeadIndex = std::uint64_t{11};
    constexpr auto SampleCount = std::size_t{5};
    constexpr auto UntouchedSample = Sample{0xFFFFFFFFU};

    // The value of the sample at an absolute index of a channel.
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
    auto const unpackedHeader = bounceBuffer.unpack(0, outRegion);
    REQUIRE(unpackedHeader.has_value());
    REQUIRE(unpackedHeader->headIndex == HeadIndex);
    REQUIRE(unpackedHeader->count == SampleCount);
    REQUIRE(target == expectedTarget);
}

TEST_CASE("ofi: audio staging size rejects an unusable target geometry", "[ofi][ProtocolEgressRMA]")
{
    constexpr auto EntryCount = std::size_t{4};
    constexpr auto EntrySize = std::size_t{64};

    auto info = TargetInfo{};
    info.remoteRegions = std::vector<RemoteRegion>(EntryCount, RemoteRegion{.addr = 0, .len = EntrySize, .rkey = 0});
    info.bounceBufferInfo = TargetInfoBounceBufferInfo{.entryCount = EntryCount, .entrySize = EntrySize};
    REQUIRE(RMASampleEgressProtocol::stagingSize(info, EntryCount) == EntryCount * EntrySize);

    // No entries, or a count that does not match the remote regions.
    REQUIRE_THROWS(RMASampleEgressProtocol::stagingSize(info, 0));
    REQUIRE_THROWS(RMASampleEgressProtocol::stagingSize(info, EntryCount - 1));

    // Empty entries.
    info.bounceBufferInfo->entrySize = 0;
    REQUIRE_THROWS(RMASampleEgressProtocol::stagingSize(info, EntryCount));

    // At most MaxStagingSize in all.
    info.bounceBufferInfo->entrySize = RMASampleEgressProtocol::MaxStagingSize / EntryCount;
    REQUIRE(RMASampleEgressProtocol::stagingSize(info, EntryCount) == RMASampleEgressProtocol::MaxStagingSize);
    info.bounceBufferInfo->entrySize += 1;
    REQUIRE_THROWS(RMASampleEgressProtocol::stagingSize(info, EntryCount));
}
