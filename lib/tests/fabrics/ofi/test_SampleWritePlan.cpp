// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "Exception.hpp"
#include "SampleWritePlan.hpp"

using namespace mxl::lib::fabrics::ofi;

namespace
{
    using Sample = std::uint32_t;

    constexpr auto Unlimited = std::numeric_limits<std::size_t>::max();
    constexpr auto Untouched = Sample{0xFFFFFFFFU};

    /** \brief A source and a destination ring held in host memory, with the regions a plan is made against.
     *
     * The destination region uses virtual addresses, so a planned remote buffer points at the bytes it describes.
     */
    struct Rings
    {
        Rings(std::size_t channelCount, std::size_t srcLength, std::size_t dstLength)
            : srcLayout{.sampleSize = sizeof(Sample), .channelCount = channelCount, .bufferLength = srcLength}
            , dstLayout{.sampleSize = sizeof(Sample), .channelCount = channelCount, .bufferLength = dstLength}
            , src(channelCount * srcLength)
            , dst(channelCount * dstLength, Untouched)
        {
            // Every sample carries its channel and its index in the source ring, so a sample landing anywhere else is visible.
            for (auto channel = std::size_t{0}; channel < channelCount; ++channel)
            {
                for (auto position = std::size_t{0}; position < srcLength; ++position)
                {
                    src[(channel * srcLength) + position] = static_cast<Sample>((channel << 16U) | position);
                }
            }
        }

        [[nodiscard]]
        LocalRegion srcRing() const
        {
            return LocalRegion{.addr = reinterpret_cast<std::uint64_t>(src.data()), .len = src.size() * sizeof(Sample), .desc = nullptr};
        }

        [[nodiscard]]
        RemoteRegion dstRing() const
        {
            return RemoteRegion{.addr = reinterpret_cast<std::uint64_t>(dst.data()), .len = dst.size() * sizeof(Sample), .rkey = 7};
        }

        [[nodiscard]]
        std::vector<SampleWrite> plan(std::uint64_t headIndex, std::size_t count, WriteLimits const& limits) const
        {
            return planSampleWrites(srcLayout, srcRing(), dstLayout, dstRing(), headIndex, count, limits);
        }

        /** \brief Carry out the writes the way the provider does: gather the local buffers, scatter into the remote ones.
         */
        void apply(std::vector<SampleWrite> const& writes)
        {
            for (auto const& write : writes)
            {
                auto bytes = std::vector<std::uint8_t>{};
                for (auto const& local : write.local)
                {
                    auto const* begin = reinterpret_cast<std::uint8_t const*>(local.addr);
                    bytes.insert(bytes.end(), begin, begin + local.len);
                }

                auto offset = std::size_t{0};
                for (auto const& remote : write.remote)
                {
                    REQUIRE(remote.rkey == 7);
                    REQUIRE((offset + remote.len) <= bytes.size());
                    std::memcpy(reinterpret_cast<void*>(remote.addr), bytes.data() + offset, remote.len);
                    offset += remote.len;
                }
                REQUIRE(offset == bytes.size());
            }
        }

        /** \brief Require that the destination holds exactly the batch, in the right place, and nothing else changed.
         */
        void requireBatch(std::uint64_t headIndex, std::size_t count) const
        {
            auto expected = std::vector<Sample>(dst.size(), Untouched);
            for (auto channel = std::size_t{0}; channel < srcLayout.channelCount; ++channel)
            {
                for (auto index = headIndex - count; index < headIndex; ++index)
                {
                    expected[(channel * dstLayout.bufferLength) + (index % dstLayout.bufferLength)] =
                        src[(channel * srcLayout.bufferLength) + (index % srcLayout.bufferLength)];
                }
            }
            REQUIRE(dst == expected);
        }

        DataLayout::Continuous srcLayout;
        DataLayout::Continuous dstLayout;
        std::vector<Sample> src;
        std::vector<Sample> dst;
    };

    /** \brief Require that every write respects the limits.
     */
    void requireWithinLimits(std::vector<SampleWrite> const& writes, WriteLimits const& limits)
    {
        for (auto const& write : writes)
        {
            REQUIRE(!write.local.empty());
            REQUIRE(!write.remote.empty());
            REQUIRE(write.local.size() <= limits.iovLimit);
            REQUIRE(write.remote.size() <= limits.rmaIovLimit);

            auto localLength = std::size_t{0};
            for (auto const& local : write.local)
            {
                localLength += local.len;
            }
            auto remoteLength = std::size_t{0};
            for (auto const& remote : write.remote)
            {
                remoteLength += remote.len;
            }
            REQUIRE(localLength == remoteLength);
            REQUIRE(localLength <= limits.maxMsgSize);
        }
    }

    /** \brief Plan, check the limits, apply and check the result.
     */
    std::vector<SampleWrite> transfer(Rings& rings, std::uint64_t headIndex, std::size_t count, WriteLimits const& limits)
    {
        auto const writes = rings.plan(headIndex, count, limits);
        requireWithinLimits(writes, limits);
        rings.apply(writes);
        rings.requireBatch(headIndex, count);
        return writes;
    }
}

TEST_CASE("ofi: sample write plan without a wrap", "[ofi][SampleWritePlan]")
{
    auto rings = Rings{2, 16, 16};
    auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 4, .maxMsgSize = Unlimited};

    auto const writes = transfer(rings, 10, 4, limits);

    // One write, one buffer per channel on each side.
    REQUIRE(writes.size() == 1);
    REQUIRE(writes[0].local.size() == 2);
    REQUIRE(writes[0].remote.size() == 2);
    REQUIRE(writes[0].local[0].len == 4 * sizeof(Sample));
}

TEST_CASE("ofi: sample write plan when only the source wraps", "[ofi][SampleWritePlan]")
{
    // Indices 6..9: source positions 6, 7 then 0, 1. Destination positions 6..9 in one piece.
    auto rings = Rings{2, 8, 32};
    auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 4, .maxMsgSize = Unlimited};

    auto const writes = transfer(rings, 10, 4, limits);

    REQUIRE(writes.size() == 1);
    REQUIRE(writes[0].local.size() == 4);
    REQUIRE(writes[0].remote.size() == 2);
}

TEST_CASE("ofi: sample write plan when only the target wraps, with a different ring length", "[ofi][SampleWritePlan]")
{
    // Indices 6..9: source positions 6..9 in one piece. Destination positions 6, 7 then 0, 1.
    auto rings = Rings{2, 32, 8};
    auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 4, .maxMsgSize = Unlimited};

    auto const writes = transfer(rings, 10, 4, limits);

    REQUIRE(writes.size() == 1);
    REQUIRE(writes[0].local.size() == 2);
    REQUIRE(writes[0].remote.size() == 4);
}

TEST_CASE("ofi: sample write plan when both rings wrap at different points", "[ofi][SampleWritePlan]")
{
    // Indices 4..9. Source (8 samples): positions 4..7 then 0, 1. Destination (6 samples): positions 4, 5 then 0..3.
    // Three pieces per channel: [4, 6), [6, 8), [8, 10).
    auto rings = Rings{3, 8, 6};

    SECTION("generous limits")
    {
        auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 4, .maxMsgSize = Unlimited};
        auto const writes = transfer(rings, 10, 6, limits);

        // Per channel the first two pieces join in the source and the last two join in the destination: 2 local and 2 remote buffers.
        REQUIRE(writes.size() == 2);
        REQUIRE(writes[0].local.size() == 4);
        REQUIRE(writes[0].remote.size() == 4);
    }

    SECTION("one buffer per side")
    {
        auto const limits = WriteLimits{.iovLimit = 1, .rmaIovLimit = 1, .maxMsgSize = Unlimited};
        auto const writes = transfer(rings, 10, 6, limits);

        REQUIRE(writes.size() == 3 * 3);
    }
}

TEST_CASE("ofi: sample write plan splits on the iov limit", "[ofi][SampleWritePlan]")
{
    // The source wraps in every channel, so a channel needs two local buffers. With one per write, each fragment is a write of its own.
    auto rings = Rings{4, 8, 32};
    auto const limits = WriteLimits{.iovLimit = 1, .rmaIovLimit = 4, .maxMsgSize = Unlimited};

    auto const writes = transfer(rings, 10, 4, limits);

    REQUIRE(writes.size() == 4 * 2);
    for (auto const& write : writes)
    {
        REQUIRE(write.local.size() == 1);
        REQUIRE(write.remote.size() == 1);
    }
}

TEST_CASE("ofi: sample write plan merges and splits on the rma iov limit", "[ofi][SampleWritePlan]")
{
    // The destination wraps in every channel: each channel lands in two remote buffers.
    auto rings = Rings{4, 32, 8};

    SECTION("one remote buffer per write, as verbs RC offers")
    {
        auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 1, .maxMsgSize = Unlimited};
        auto const writes = transfer(rings, 10, 4, limits);
        REQUIRE(writes.size() == 4 * 2);
    }

    SECTION("two remote buffers per write hold one channel")
    {
        auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 2, .maxMsgSize = Unlimited};
        auto const writes = transfer(rings, 10, 4, limits);
        REQUIRE(writes.size() == 4);
    }

    SECTION("four remote buffers per write hold two channels")
    {
        auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 4, .maxMsgSize = Unlimited};
        auto const writes = transfer(rings, 10, 4, limits);
        REQUIRE(writes.size() == 2);
    }
}

TEST_CASE("ofi: sample write plan caps a write at the maximum message size", "[ofi][SampleWritePlan]")
{
    // 10 samples of 4 bytes per channel and two channels: 80 bytes, at most 16 per write.
    auto rings = Rings{2, 32, 32};
    auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 4, .maxMsgSize = 16};

    auto const writes = transfer(rings, 20, 10, limits);

    REQUIRE(writes.size() == 5);
}

TEST_CASE("ofi: sample write plan for half the target ring across its wrap", "[ofi][SampleWritePlan]")
{
    // The largest batch a target writer accepts, ending just past the wrap of the destination.
    auto rings = Rings{2, 48, 16};
    auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 1, .maxMsgSize = Unlimited};

    auto const writes = transfer(rings, 35, 8, limits);

    // Indices 27..34: destination positions 11..15 then 0..2.
    REQUIRE(writes.size() == 2 * 2);
}

TEST_CASE("ofi: sample write plan rejects what it cannot plan", "[ofi][SampleWritePlan]")
{
    auto rings = Rings{2, 16, 8};
    auto const limits = WriteLimits{.iovLimit = 4, .rmaIovLimit = 4, .maxMsgSize = Unlimited};

    REQUIRE_THROWS_AS(rings.plan(10, 0, limits), Exception);
    REQUIRE_THROWS_AS(rings.plan(10, 9, limits), Exception);
    REQUIRE_THROWS_AS(rings.plan(3, 4, limits), Exception);
    REQUIRE_THROWS_AS(rings.plan(10, 4, WriteLimits{.iovLimit = 0, .rmaIovLimit = 4, .maxMsgSize = Unlimited}), Exception);

    auto otherChannels = rings.dstLayout;
    otherChannels.channelCount = 1;
    REQUIRE_THROWS_AS(planSampleWrites(rings.srcLayout, rings.srcRing(), otherChannels, rings.dstRing(), 10, 4, limits), Exception);
}
