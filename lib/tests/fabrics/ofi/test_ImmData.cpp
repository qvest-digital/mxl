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
    REQUIRE(headTag == 0x4DEF);
    REQUIRE(headTag == ImmDataSample::headTag(headIndex));

    // A slot refilled with a later batch carries another head index, so the tag of the write that filled it first no longer matches.
    REQUIRE(ImmDataSample::headTag(headIndex + 480) != headTag);
}

TEST_CASE("ofi: audio descriptor immediate data is told apart from a bounce buffer entry index", "[ofi][ImmData]")
{
    // A target serves initiators that write descriptors and initiators that write bounce buffer entries on the same endpoint, and the
    // immediate data is all it has to tell them apart. A descriptor sets bit 31 whatever its slot and head index, an entry index never does.
    auto const descriptor = ImmDataSample{65535, 0xFFFF'FFFF'FFFF'FFFFULL};
    REQUIRE(ImmDataSample::isDescriptor(descriptor.data()));
    REQUIRE(ImmDataSample::isDescriptor(ImmDataSample{0, 0}.data()));

    REQUIRE(!ImmDataSample::isDescriptor(0));
    REQUIRE(!ImmDataSample::isDescriptor(40));
    REQUIRE(!ImmDataSample::isDescriptor(0x7FFF'FFFFU));

    // The flag does not leak into the slot or the tag.
    auto const [slot, headTag] = ImmDataSample{descriptor.data()}.unpack();
    REQUIRE(slot == 65535);
    REQUIRE(headTag == 0x7FFF);
}
