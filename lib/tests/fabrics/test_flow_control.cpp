// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include <uuid.h>
#include <catch2/catch_test_macros.hpp>
#include <picojson/picojson.h>
#include <mxl/fabrics.h>
#include "mxl/flow.h"
#include "mxl/mxl.h"
#include "../Utils.hpp"

namespace
{
    constexpr auto CONNECT_TIMEOUT = std::chrono::seconds(5);

    void requireBalanced(mxlFabricsTargetStats const& stats)
    {
        REQUIRE(stats.transfersPosted == (stats.transfersCompleted + stats.transfersFailed + stats.transfersInFlight));
    }

    mxlFabricsInterfaceConfig interfaceFor(mxlFabricsProvider provider, char const* node)
    {
        return mxlFabricsInterfaceConfig{
            .version = MXL_FABRICS_API_VERSION,
            .provider = provider,
            .caps =
                {
                       .version = MXL_FABRICS_API_VERSION,
                       .flags = MXL_FABRICS_IFACE_CAP_BLOCKING_OPERATIONS | MXL_FABRICS_IFACE_CAP_REMOTE_WRITE,
                       .maxMessageSize = 0,
                       },
            .address = {.node = node,                                  .service = nullptr                                                                                                              },
            .attr = nullptr,
        };
    }

    /** One initiator reading a v210 flow and sending it to targets that each write a flow of their own, all connection oriented.
     */
    class Rig
    {
    public:
        Rig(std::string const& domain, std::size_t nbTargets, mxlFabricsProvider provider, char const* node, char const* initiatorOptions)
            : _instance{mxlCreateInstance(domain.c_str(), "")}
        {
            REQUIRE(mxlFabricsCreateInstance(_instance, nullptr, &_fabrics) == MXL_STATUS_OK);

            auto flowDef = mxl::tests::readFile("../data/v210_flow.json");
            auto json = picojson::value{};
            REQUIRE(picojson::parse(json, flowDef).empty());
            auto root = json.get<picojson::object>();
            auto engine = std::mt19937{std::random_device{}()};
            auto uuidGen = uuids::uuid_random_generator{engine};

            auto const makeWriter = [&]()
            {
                auto id = uuids::to_string(uuidGen());
                root.at("id") = picojson::value{id};
                auto def = picojson::value{root}.serialize();
                mxlFlowWriter writer;
                REQUIRE(mxlCreateFlowWriter(_instance, def.c_str(), nullptr, &writer, nullptr, nullptr) == MXL_STATUS_OK);
                _writers.push_back(writer);
                return id;
            };

            auto const sourceId = makeWriter();
            REQUIRE(mxlCreateFlowReader(_instance, sourceId.c_str(), "", &_reader) == MXL_STATUS_OK);

            for (std::size_t i = 0; i < nbTargets; ++i)
            {
                static_cast<void>(makeWriter());
                mxlFabricsTarget target;
                REQUIRE(mxlFabricsCreateTarget(_fabrics, &target) == MXL_STATUS_OK);
                auto config = mxlFabricsTargetConfig{
                    .version = MXL_FABRICS_API_VERSION,
                    .interface = interfaceFor(provider, node),
                    .writer = _writers.back(),
                };
                mxlFabricsTargetInfo info;
                REQUIRE(mxlFabricsTargetSetup(target, &config, nullptr, &info) == MXL_STATUS_OK);
                targets.push_back(target);
                infos.push_back(info);
            }

            REQUIRE(mxlFabricsCreateInitiator(_fabrics, &initiator) == MXL_STATUS_OK);
            auto config = mxlFabricsInitiatorConfig{
                .version = MXL_FABRICS_API_VERSION,
                .interface = interfaceFor(provider, node),
                .reader = _reader,
            };
            REQUIRE(mxlFabricsInitiatorSetup(initiator, &config, initiatorOptions) == MXL_STATUS_OK);
        }

        ~Rig()
        {
            for (auto info : infos)
            {
                mxlFabricsFreeTargetInfo(info);
            }
            mxlFabricsDestroyInitiator(_fabrics, initiator);
            for (auto target : targets)
            {
                if (target != nullptr)
                {
                    mxlFabricsDestroyTarget(_fabrics, target);
                }
            }
            mxlReleaseFlowReader(_instance, _reader);
            for (auto writer : _writers)
            {
                mxlReleaseFlowWriter(_instance, writer);
            }
            mxlFabricsDestroyInstance(_fabrics);
            mxlDestroyInstance(_instance);
        }

        Rig(Rig const&) = delete;
        Rig& operator=(Rig const&) = delete;

        /** Read every target once. Reading is what makes a target accept a connection and re-post its receives. Returns how many reported
         * a grain.
         */
        std::size_t pollTargets()
        {
            auto arrived = std::size_t{0};
            for (auto target : targets)
            {
                auto index = std::uint64_t{};
                if ((target != nullptr) && (mxlFabricsTargetReadGrainNonBlocking(target, &index) == MXL_STATUS_OK))
                {
                    ++arrived;
                }
            }
            return arrived;
        }

        void addAllAndConnect()
        {
            for (auto info : infos)
            {
                REQUIRE(mxlFabricsInitiatorAddTarget(initiator, info) == MXL_STATUS_OK);
            }
            auto const deadline = std::chrono::steady_clock::now() + CONNECT_TIMEOUT;
            while (true)
            {
                static_cast<void>(pollTargets());
                auto const status = mxlFabricsInitiatorMakeProgressNonBlocking(initiator);
                REQUIRE(((status == MXL_STATUS_OK) || (status == MXL_ERR_NOT_READY)));
                if (status == MXL_STATUS_OK)
                {
                    return;
                }
                REQUIRE(std::chrono::steady_clock::now() < deadline);
            }
        }

        /** Make progress, reading the targets meanwhile, until the initiator has nothing outstanding. False on timeout.
         */
        bool drain(std::chrono::steady_clock::duration timeout)
        {
            auto const deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                static_cast<void>(pollTargets());
                auto const status = mxlFabricsInitiatorMakeProgressNonBlocking(initiator);
                if (status == MXL_STATUS_OK)
                {
                    return true;
                }
                REQUIRE(status == MXL_ERR_NOT_READY);
            }
            return false;
        }

        mxlFabricsTargetStats statsOf(std::size_t i)
        {
            auto stats = mxlFabricsTargetStats{};
            REQUIRE(mxlFabricsInitiatorGetTargetStats(initiator, infos[i], &stats) == MXL_STATUS_OK);
            requireBalanced(stats);
            return stats;
        }

        /** Commit grain index on the source flow, which moves its head there.
         */
        void commitSource(std::uint64_t index)
        {
            auto info = mxlGrainInfo{};
            std::uint8_t* payload = nullptr;
            REQUIRE(mxlFlowWriterOpenGrain(_writers.front(), index, &info, &payload) == MXL_STATUS_OK);
            info.validSlices = info.totalSlices;
            REQUIRE(mxlFlowWriterCommitGrain(_writers.front(), &info) == MXL_STATUS_OK);
        }

        std::vector<mxlFabricsTarget> targets;
        std::vector<mxlFabricsTargetInfo> infos;
        mxlFabricsInitiator initiator{nullptr};

    private:
        mxlInstance _instance;
        mxlFabricsInstance _fabrics{nullptr};
        mxlFlowReader _reader{nullptr};
        std::vector<mxlFlowWriter> _writers;
    };

    constexpr std::uint16_t V210_SLICES = 1080;
}

TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Fabrics: RC initiator holds at most grainsInFlight grains per target",
    "[fabrics][flow-control]")
{
    // No progress is made while the grains are offered, so no completion is read and the windows stay full. The grains that do not fit
    // are skipped and counted per target, and the call reports not-ready.
    auto rig = Rig{domain, 2, MXL_FABRICS_PROVIDER_TCP, "127.0.0.1", R"({"grainsInFlight": 3})"};
    rig.addAllAndConnect();

    for (std::uint64_t grain = 0; grain < 7; ++grain)
    {
        REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, grain, 0, V210_SLICES) == ((grain < 3) ? MXL_STATUS_OK : MXL_ERR_NOT_READY));
    }
    for (std::size_t i = 0; i < rig.targets.size(); ++i)
    {
        auto const stats = rig.statsOf(i);
        REQUIRE(stats.transfersPosted == 3);
        REQUIRE(stats.transfersInFlight == 3);
        REQUIRE(stats.transfersSkipped == 4);
        REQUIRE(stats.writesInFlight == 3);
    }

    REQUIRE(rig.drain(CONNECT_TIMEOUT));
    for (std::size_t i = 0; i < rig.targets.size(); ++i)
    {
        auto const stats = rig.statsOf(i);
        REQUIRE(stats.transfersCompleted == 3);
        REQUIRE(stats.transfersInFlight == 0);
        REQUIRE(stats.lastCompletedIndex == 2);
        REQUIRE(stats.accountingErrors == 0);
    }

    // The window opens again once completions were read.
    REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 7, 0, V210_SLICES) == MXL_STATUS_OK);
    REQUIRE(rig.statsOf(0).transfersPosted == 4);
}

TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Fabrics: a grain sent in chunks reaches the target once, with its last chunk",
    "[fabrics][flow-control]")
{
    auto rig = Rig{domain, 1, MXL_FABRICS_PROVIDER_TCP, "127.0.0.1", R"({"chunksPerGrain": 4})"};
    rig.addAllAndConnect();

    constexpr std::uint16_t chunk = V210_SLICES / 4;
    for (std::uint16_t start = 0; start < (V210_SLICES - chunk); start += chunk)
    {
        REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 0, start, start + chunk) == MXL_STATUS_OK);
    }
    // The first three chunks carry no immediate data: the target has nothing to report however long it is read.
    REQUIRE(rig.drain(CONNECT_TIMEOUT));
    auto const quiet = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < quiet)
    {
        REQUIRE(rig.pollTargets() == 0);
    }
    REQUIRE(rig.statsOf(0).transfersCompleted == 0);

    REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 0, V210_SLICES - chunk, V210_SLICES) == MXL_STATUS_OK);
    auto const deadline = std::chrono::steady_clock::now() + CONNECT_TIMEOUT;
    auto arrived = std::size_t{0};
    while ((arrived == 0) && (std::chrono::steady_clock::now() < deadline))
    {
        static_cast<void>(mxlFabricsInitiatorMakeProgressNonBlocking(rig.initiator));
        arrived = rig.pollTargets();
    }
    REQUIRE(arrived == 1);
    REQUIRE(rig.drain(CONNECT_TIMEOUT));
    auto const stats = rig.statsOf(0);
    REQUIRE(stats.transfersPosted == 1);
    REQUIRE(stats.transfersCompleted == 1);
    REQUIRE(stats.lastCompletedIndex == 0);
}

TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Fabrics: a target that misses the first chunk of a grain skips the grain",
    "[fabrics][flow-control]")
{
    // The target connects while grain 0 is half sent. Opening the grain on a later chunk would have the last chunk's immediate data mark
    // the whole grain valid at the target with only its second half written.
    auto rig = Rig{domain, 1, MXL_FABRICS_PROVIDER_TCP, "127.0.0.1", R"({"chunksPerGrain": 4})"};
    rig.addAllAndConnect();

    constexpr std::uint16_t chunk = V210_SLICES / 4;
    for (std::uint16_t start = 2 * chunk; start < V210_SLICES; start += chunk)
    {
        REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 0, start, start + chunk) == MXL_STATUS_OK);
    }
    REQUIRE(rig.drain(CONNECT_TIMEOUT));
    auto const quiet = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < quiet)
    {
        REQUIRE(rig.pollTargets() == 0);
    }
    auto stats = rig.statsOf(0);
    REQUIRE(stats.transfersPosted == 0);
    REQUIRE(stats.transfersSkipped == 1);
    REQUIRE(stats.writesInFlight == 0);

    // The next grain, sent from its first chunk, goes over whole.
    for (std::uint16_t start = 0; start < V210_SLICES; start += chunk)
    {
        REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 1, start, start + chunk) == MXL_STATUS_OK);
    }
    REQUIRE(rig.drain(CONNECT_TIMEOUT));
    stats = rig.statsOf(0);
    REQUIRE(stats.transfersPosted == 1);
    REQUIRE(stats.transfersCompleted == 1);
    REQUIRE(stats.lastCompletedIndex == 1);
}

TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Fabrics: a chunked grain that cannot fit all its chunks is skipped whole",
    "[fabrics][flow-control]")
{
    // Two grains of four chunks: the window holds 8 writes. No progress is made, so nothing completes. Grain 0 is given up after three
    // chunks, which stay in flight; grain 1 takes four more. Grain 2 then has room for one write only. Starting it would post its first
    // chunk and run out of room on the second, leaving a partial grain at the target that the next grain's slot reuse never completes.
    auto rig = Rig{domain, 1, MXL_FABRICS_PROVIDER_TCP, "127.0.0.1", R"({"grainsInFlight": 2, "chunksPerGrain": 4})"};
    rig.addAllAndConnect();

    constexpr std::uint16_t chunk = V210_SLICES / 4;
    for (std::uint16_t start = 0; start < (V210_SLICES - chunk); start += chunk)
    {
        REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 0, start, start + chunk) == MXL_STATUS_OK);
    }
    for (std::uint16_t start = 0; start < V210_SLICES; start += chunk)
    {
        REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 1, start, start + chunk) == MXL_STATUS_OK);
    }
    REQUIRE(rig.statsOf(0).writesInFlight == 7);

    REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 2, 0, chunk) == MXL_ERR_NOT_READY);
    for (std::uint16_t start = chunk; start < V210_SLICES; start += chunk)
    {
        REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, 2, start, start + chunk) == MXL_STATUS_OK);
    }
    auto stats = rig.statsOf(0);
    REQUIRE(stats.writesInFlight == 7);
    REQUIRE(stats.transfersPosted == 1);
    REQUIRE(stats.transfersSkipped == 2);

    REQUIRE(rig.drain(CONNECT_TIMEOUT));
    stats = rig.statsOf(0);
    REQUIRE(stats.transfersCompleted == 1);
    REQUIRE(stats.lastCompletedIndex == 1);
    REQUIRE(stats.accountingErrors == 0);
}

TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Fabrics: a grain completing after its source slot was rewritten counts as torn",
    "[fabrics][flow-control]")
{
    auto rig = Rig{domain, 1, MXL_FABRICS_PROVIDER_TCP, "127.0.0.1", nullptr};
    rig.addAllAndConnect();

    // Far above any ring depth, so the second grain's slot is certain to have been rewritten when it completes.
    constexpr std::uint64_t head = 1'000'000;
    constexpr std::uint64_t lap = 100'000;

    rig.commitSource(head);
    REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, head, 0, V210_SLICES) == MXL_STATUS_OK);
    REQUIRE(rig.drain(CONNECT_TIMEOUT));
    REQUIRE(rig.statsOf(0).transfersTorn == 0);

    rig.commitSource(head + 1);
    REQUIRE(mxlFabricsInitiatorTransferGrain(rig.initiator, head + 1, 0, V210_SLICES) == MXL_STATUS_OK);
    rig.commitSource(head + 1 + lap);
    REQUIRE(rig.drain(CONNECT_TIMEOUT));
    auto const stats = rig.statsOf(0);
    REQUIRE(stats.transfersCompleted == 2);
    REQUIRE(stats.transfersTorn == 1);
}

TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Fabrics: RC initiator refuses targets beyond maxTargets", "[fabrics][flow-control]")
{
    auto rig = Rig{domain, 2, MXL_FABRICS_PROVIDER_TCP, "127.0.0.1", R"({"maxTargets": 1})"};
    REQUIRE(mxlFabricsInitiatorAddTarget(rig.initiator, rig.infos[0]) == MXL_STATUS_OK);
    REQUIRE(mxlFabricsInitiatorAddTarget(rig.initiator, rig.infos[1]) == MXL_ERR_INVALID_STATE);
}

TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Fabrics: initiator flow control options are validated", "[fabrics][flow-control]")
{
    auto instance = mxlCreateInstance(domain.c_str(), "");
    mxlFabricsInstance fabrics;
    REQUIRE(mxlFabricsCreateInstance(instance, nullptr, &fabrics) == MXL_STATUS_OK);
    auto flowDef = mxl::tests::readFile("../data/v210_flow.json");
    mxlFlowWriter writer;
    REQUIRE(mxlCreateFlowWriter(instance, flowDef.c_str(), nullptr, &writer, nullptr, nullptr) == MXL_STATUS_OK);
    mxlFlowReader reader;
    REQUIRE(mxlCreateFlowReader(instance, "5fbec3b1-1b0f-417d-9059-8b94a47197ed", "", &reader) == MXL_STATUS_OK);

    auto setupWith = [&](char const* options)
    {
        mxlFabricsInitiator initiator;
        REQUIRE(mxlFabricsCreateInitiator(fabrics, &initiator) == MXL_STATUS_OK);
        auto config = mxlFabricsInitiatorConfig{
            .version = MXL_FABRICS_API_VERSION,
            .interface = interfaceFor(MXL_FABRICS_PROVIDER_TCP, "127.0.0.1"),
            .reader = reader,
        };
        auto const status = mxlFabricsInitiatorSetup(initiator, &config, options);
        REQUIRE(mxlFabricsDestroyInitiator(fabrics, initiator) == MXL_STATUS_OK);
        return status;
    };

    REQUIRE(setupWith(R"({"grainsInFlight": 16, "chunksPerGrain": 8, "maxTargets": 4})") == MXL_STATUS_OK);
    REQUIRE(setupWith(R"({"grainsInFlight": 0})") == MXL_ERR_INVALID_ARG);
    REQUIRE(setupWith(R"({"grainsInFlight": 17})") == MXL_ERR_INVALID_ARG);
    REQUIRE(setupWith(R"({"chunksPerGrain": 0})") == MXL_ERR_INVALID_ARG);
    REQUIRE(setupWith(R"({"maxTargets": "many"})") == MXL_ERR_INVALID_ARG);

    REQUIRE(mxlFabricsDestroyInstance(fabrics) == MXL_STATUS_OK);
    REQUIRE(mxlReleaseFlowReader(instance, reader) == MXL_STATUS_OK);
    REQUIRE(mxlReleaseFlowWriter(instance, writer) == MXL_STATUS_OK);
    REQUIRE(mxlDestroyInstance(instance) == MXL_STATUS_OK);
}

// Needs a verbs device, so it is hidden from the default run. With soft-RoCE: create an rxe link on a netdev, set
// MXL_TEST_VERBS_NODE to that netdev's address, and run the test binary with "[.rxe]" where librdmacm can open the device.
TEST_CASE_PERSISTENT_FIXTURE(mxl::tests::mxlDomainFixture, "Fabrics: verbs completion queue holds every write of two full targets",
    "[.rxe][fabrics][flow-control]")
{
    auto const* node = std::getenv("MXL_TEST_VERBS_NODE");
    if (node == nullptr)
    {
        SKIP("MXL_TEST_VERBS_NODE is not set");
    }

    // Two targets keep reading, so their writes complete, while the initiator reads no completion. Every write of both send queues then
    // lands in the one completion queue of the initiator. Sized for a single send queue it overruns, the provider drops the completions
    // that did not fit, and the initiator waits for them forever.
    auto rig = Rig{domain, 2, MXL_FABRICS_PROVIDER_VERBS, node, nullptr};
    rig.addAllAndConnect();

    for (std::uint64_t grain = 0; grain < 1000; ++grain)
    {
        auto const status = mxlFabricsInitiatorTransferGrain(rig.initiator, grain, 0, V210_SLICES);
        REQUIRE(((status == MXL_STATUS_OK) || (status == MXL_ERR_NOT_READY)));
        static_cast<void>(rig.pollTargets());
    }
    auto const settle = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < settle)
    {
        static_cast<void>(rig.pollTargets());
    }

    REQUIRE(rig.drain(std::chrono::seconds(10)));
}
