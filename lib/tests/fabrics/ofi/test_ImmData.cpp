// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <catch2/catch_test_macros.hpp>
#include "ImmData.hpp"

using namespace mxl::lib::fabrics::ofi;

TEST_CASE("ofi: audio descriptor immediate data names the slot and tags the head index", "[ofi][ImmData]")
{
    auto const headIndex = std::uint64_t{0x0123'4567'89AB'CDEFULL};
    auto const packed = ImmDataSample{255, headIndex}.data();

    auto const [slot, headTag] = ImmDataSample{packed}.unpack();
    REQUIRE(slot == 255);
    REQUIRE(headTag == 0xCDEF);
    REQUIRE(headTag == ImmDataSample::headTag(headIndex));

    // A slot refilled with a later batch carries another head index, so the tag of the write that filled it first no longer matches.
    REQUIRE(ImmDataSample::headTag(headIndex + 480) != headTag);
}
