// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <rdma/fabric.h>
#include "Endpoint.hpp"
#include "Exception.hpp"
#include "Util.hpp"

using namespace mxl::lib::fabrics::ofi;

TEST_CASE("ofi: Domain - RegisteredRegion to Local Region", "[ofi][Domain][LocalRegion]")
{
    auto domain = getDomain();
    auto [mxlFabricsRegions, buffers] = getHostRegionGroups();
    auto regions = mxlFabricsRegions.regions();

    domain->registerRegions(regions, FI_WRITE);

    auto localRegions = domain->localRegions();
    REQUIRE(localRegions.size() == 4);

    REQUIRE(localRegions[0].addr == regions[0].base);
    REQUIRE(localRegions[0].len == 256);
    REQUIRE(localRegions[1].addr == regions[1].base);
    REQUIRE(localRegions[1].len == 512);
    REQUIRE(localRegions[2].addr == regions[2].base);
    REQUIRE(localRegions[2].len == 1024);
    REQUIRE(localRegions[3].addr == regions[3].base);
    REQUIRE(localRegions[3].len == 2048);
}

TEST_CASE("ofi: Domain - RegisteredRegion to Remote Region with Virtual Addresses", "[ofi][Domain][RemoteRegion][VirtAddr]")
{
    auto domain = getDomain(true);
    REQUIRE(domain->usingVirtualAddresses() == true);

    auto [mxlFabricsRegions, buffers] = getHostRegionGroups();
    auto regions = mxlFabricsRegions.regions();

    domain->registerRegions(regions, FI_WRITE);

    auto remoteRegions = domain->remoteRegions();
    REQUIRE(remoteRegions.size() == 4);
    REQUIRE(remoteRegions[0].addr == regions[0].base);
    REQUIRE(remoteRegions[0].len == 256);
    REQUIRE(remoteRegions[1].addr == regions[1].base);
    REQUIRE(remoteRegions[1].len == 512);
    REQUIRE(remoteRegions[2].addr == regions[2].base);
    REQUIRE(remoteRegions[2].len == 1024);
    REQUIRE(remoteRegions[3].addr == regions[3].base);
    REQUIRE(remoteRegions[3].len == 2048);
}

TEST_CASE("Domain - RegisteredRegion to Remote Region with Relative Addresses", "[ofi][Domain][RemoteRegion][RelativeAddr]")
{
    auto domain = getDomain(false);
    REQUIRE(domain->usingVirtualAddresses() == false);

    auto [mxlFabricsRegions, buffers] = getHostRegionGroups();
    auto regions = mxlFabricsRegions.regions();

    domain->registerRegions(regions, FI_WRITE);

    auto remoteRegions = domain->remoteRegions();
    REQUIRE(remoteRegions.size() == 4);
    REQUIRE(remoteRegions[0].addr == 0);
    REQUIRE(remoteRegions[0].len == 256);
    REQUIRE(remoteRegions[1].addr == 0);
    REQUIRE(remoteRegions[1].len == 512);
    REQUIRE(remoteRegions[2].addr == 0);
    REQUIRE(remoteRegions[2].len == 1024);
    REQUIRE(remoteRegions[3].addr == 0);
    REQUIRE(remoteRegions[3].len == 2048);
}

TEST_CASE("Domain - RX CQ Data Mode", "[ofi][Domain][RxCqData]")
{
    auto domain = getDomain();
    REQUIRE(domain->usingRecvBufForCqData() == false);

    auto domain2 = getDomain(false, true);
    REQUIRE(domain2->usingRecvBufForCqData() == true);
}

TEST_CASE("ofi: FabricInfo reports the RMA iov and message size limits", "[ofi][Domain][FabricInfo]")
{
    auto domain = getDomain();
    auto info = domain->fabric()->info();

    // Writes are split by these two limits, so they must come from the provider's attributes and not from the transmit iov limit.
    info->tx_attr->iov_limit = 4;
    info->tx_attr->rma_iov_limit = 3;
    info->ep_attr->max_msg_size = 4096;

    REQUIRE(info.txIovLimit() == 4);
    REQUIRE(info.rmaIovLimit() == 3);
    REQUIRE(info.maxMsgSize() == 4096);
}

TEST_CASE("ofi: Endpoint refuses a write beyond the provider limits", "[ofi][Endpoint][Limits]")
{
    auto domain = getDomain();
    auto endpoint = Endpoint::create(domain);
    auto const iovLimit = endpoint.info().txIovLimit();
    auto const rmaIovLimit = endpoint.info().rmaIovLimit();

    auto buffer = std::vector<std::uint8_t>(4096);
    auto const local = LocalRegion{.addr = reinterpret_cast<std::uint64_t>(buffer.data()), .len = 8, .desc = nullptr};
    auto const remote = RemoteRegion{.addr = 0, .len = 8, .rkey = 0};

    SECTION("more local buffers than the iov limit")
    {
        auto const locals = std::vector<LocalRegion>(iovLimit + 1, local);
        auto const remotes = std::vector<RemoteRegion>{
            RemoteRegion{.addr = 0, .len = 8 * (iovLimit + 1), .rkey = 0}
        };
        REQUIRE_THROWS_AS(endpoint.write(0, locals, remotes), Exception);
    }

    SECTION("more remote buffers than the rma iov limit")
    {
        auto const locals = std::vector<LocalRegion>{
            LocalRegion{.addr = local.addr, .len = 8 * (rmaIovLimit + 1), .desc = nullptr}
        };
        auto const remotes = std::vector<RemoteRegion>(rmaIovLimit + 1, remote);
        REQUIRE_THROWS_AS(endpoint.write(0, locals, remotes), Exception);
    }

    SECTION("local and remote lengths that differ")
    {
        auto const locals = std::vector<LocalRegion>{local};
        auto const remotes = std::vector<RemoteRegion>{
            RemoteRegion{.addr = 0, .len = 16, .rkey = 0}
        };
        REQUIRE_THROWS_AS(endpoint.write(0, locals, remotes), Exception);
    }

    SECTION("an empty write")
    {
        REQUIRE_THROWS_AS(endpoint.write(0, std::vector<LocalRegion>{}, std::vector<RemoteRegion>{}), Exception);
    }
}
