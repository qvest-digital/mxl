// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <catch2/catch_test_macros.hpp>
#include "EgressWindow.hpp"

namespace ofi = mxl::lib::fabrics::ofi;

namespace
{
    void requireBalanced(mxlFabricsTargetStats const& stats)
    {
        REQUIRE(stats.transfersPosted == (stats.transfersCompleted + stats.transfersFailed + stats.transfersInFlight));
    }
}

TEST_CASE("Fabrics: egress window holds at most maxTransfers and skips the rest", "[fabrics][flow-control]")
{
    auto window = ofi::EgressWindow{4, 4};
    for (std::uint64_t i = 0; i < 6; ++i)
    {
        if (window.open())
        {
            window.wrote(true, true, i, 100);
        }
    }
    auto stats = window.stats();
    REQUIRE(stats.transfersPosted == 4);
    REQUIRE(stats.transfersSkipped == 2);
    REQUIRE(stats.transfersInFlight == 4);
    REQUIRE(stats.lastCompletedIndex == MXL_FABRICS_NO_INDEX);
    requireBalanced(stats);

    for (auto i = 0; i < 4; ++i)
    {
        window.complete(true);
    }
    stats = window.stats();
    REQUIRE(stats.transfersCompleted == 4);
    REQUIRE(stats.transfersInFlight == 0);
    REQUIRE(stats.writesInFlight == 0);
    REQUIRE(stats.lastCompletedIndex == 3);
    REQUIRE(window.open());
    requireBalanced(stats);
}

TEST_CASE("Fabrics: egress window settles a transfer of several writes with its one completion", "[fabrics][flow-control]")
{
    auto window = ofi::EgressWindow{2, 16};
    REQUIRE(window.open());
    for (auto chunk = 0; chunk < 3; ++chunk)
    {
        window.wrote(false, false, 7, 100);
    }
    REQUIRE_FALSE(window.expectsCompletion());
    window.wrote(true, true, 7, 100);
    REQUIRE(window.expectsCompletion());
    REQUIRE(window.stats().writesInFlight == 4);

    window.complete(true);
    auto const stats = window.stats();
    REQUIRE(stats.writesInFlight == 0);
    REQUIRE(stats.transfersCompleted == 1);
    REQUIRE(stats.lastCompletedIndex == 7);
    REQUIRE(stats.bytesCompleted == 400);
    REQUIRE_FALSE(window.expectsCompletion());
}

TEST_CASE("Fabrics: egress window frees the slot of an abandoned transfer but keeps its writes", "[fabrics][flow-control]")
{
    // The writes of a grain given up half way are still in the send queue. They are settled by the completion of the next transfer,
    // which on a reliable connection cannot complete before them.
    auto window = ofi::EgressWindow{1, 8};
    REQUIRE(window.open());
    window.wrote(false, false, 1, 100);
    window.wrote(false, false, 1, 100);
    window.abandon();

    auto stats = window.stats();
    REQUIRE(stats.transfersPosted == 0);
    REQUIRE(stats.transfersSkipped == 1);
    REQUIRE(stats.transfersInFlight == 0);
    REQUIRE(stats.writesInFlight == 2);

    REQUIRE(window.open());
    window.wrote(true, true, 2, 100);
    REQUIRE(window.stats().writesInFlight == 3);
    window.complete(true);
    stats = window.stats();
    REQUIRE(stats.writesInFlight == 0);
    REQUIRE(stats.transfersCompleted == 1);
    REQUIRE(stats.lastCompletedIndex == 2);
    requireBalanced(stats);
}

TEST_CASE("Fabrics: egress window bounds writes in flight", "[fabrics][flow-control]")
{
    auto window = ofi::EgressWindow{4, 3};
    REQUIRE(window.open());
    REQUIRE(window.hasRoomFor(3));
    window.wrote(false, false, 0, 100);
    window.wrote(false, false, 0, 100);
    window.wrote(false, false, 0, 100);
    REQUIRE_FALSE(window.hasRoomFor(1));
}

TEST_CASE("Fabrics: egress window counts what a lost connection held as failed", "[fabrics][flow-control]")
{
    auto window = ofi::EgressWindow{4, 4};
    REQUIRE(window.open());
    window.wrote(true, true, 0, 100);
    REQUIRE(window.open());
    window.wrote(true, true, 1, 100);
    window.complete(false);

    REQUIRE(window.reset() == 1);
    auto const stats = window.stats();
    REQUIRE(stats.transfersFailed == 2);
    REQUIRE(stats.transfersInFlight == 0);
    REQUIRE(stats.writesInFlight == 0);
    REQUIRE_FALSE(window.expectsCompletion());
    requireBalanced(stats);

    // A completion for the old connection is not counted against the new one.
    window.complete(true);
    REQUIRE(window.stats().transfersCompleted == 0);
}

TEST_CASE("Fabrics: egress window reports the transfer a completion ended, and counts torn ones", "[fabrics][flow-control]")
{
    auto window = ofi::EgressWindow{4, 8};
    REQUIRE(window.open());
    window.wrote(false, false, 5, 100);
    REQUIRE_FALSE(window.complete(true).has_value());
    window.wrote(true, true, 5, 100);
    auto const index = window.complete(true);
    REQUIRE(index.has_value());
    REQUIRE(*index == 5);

    REQUIRE(window.open());
    window.wrote(true, true, 6, 100);
    REQUIRE_FALSE(window.complete(false).has_value());

    window.torn();
    auto const stats = window.stats();
    REQUIRE(stats.transfersTorn == 1);
    REQUIRE(stats.transfersFailed == 1);
    REQUIRE(stats.accountingErrors == 0);
}

#ifdef NDEBUG
TEST_CASE("Fabrics: egress window counts a completion it has no record of", "[fabrics][flow-control]")
{
    auto window = ofi::EgressWindow{4, 8};
    REQUIRE_FALSE(window.complete(true).has_value());
    REQUIRE(window.stats().accountingErrors == 1);
    REQUIRE(window.stats().transfersCompleted == 0);
}
#endif
