// SPDX-FileCopyrightText: 2025 Contributors to the Media eXchange Layer project.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <rdma/fabric.h>
#include <mxl/fabrics.h>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include "../../Utils.hpp"
#include "AddressVector.hpp"
#include "AudioBounceBuffer.hpp"
#include "CompletionQueue.hpp"
#include "Domain.hpp"
#include "Endpoint.hpp"
#include "EventQueue.hpp"
#include "Fabric.hpp"
#include "FabricInterfaceProbe.hpp"
#include "Provider.hpp"
#include "TargetInfo.hpp"

using namespace mxl::lib::fabrics::ofi;

namespace
{
    constexpr auto TestTimeout = std::chrono::seconds(5);
    constexpr auto FlowId = "b3bb5be7-9fe9-4324-a5bb-4c70e1084449";

    using Sample = std::uint32_t;

    Sample sampleAt(std::size_t channel, std::uint64_t index)
    {
        return static_cast<Sample>((channel << 24U) | (index & 0xFFFFFFU));
    }

    /** \brief A target as it was before targets published TargetInfoContinuous: a bounce buffer of entries, each a header followed by the
     * samples of every channel, one channel after the other.
     */
    class BounceTarget
    {
    public:
        BounceTarget(std::size_t entryCount, std::size_t entrySize, std::string service)
            : _service{std::move(service)}
            , _entries(entryCount, std::vector<std::uint8_t>(entrySize))
        {
            auto const interface = mxlFabricsInterfaceConfig{
                .version = MXL_FABRICS_API_VERSION,
                .provider = MXL_FABRICS_PROVIDER_SHM,
                .caps = {.version = MXL_FABRICS_API_VERSION, .flags = MXL_FABRICS_IFACE_CAP_REMOTE_WRITE, .maxMessageSize = 0},
                .address = {.node = "bounce-target", .service = _service.c_str()},
                .attr = nullptr,
            };
            auto [info, providerConfig] = selectSourceInterface(interface, true);

            auto fabric = Fabric::open(info.view());
            _domain = Domain::open(fabric);
            _endpoint.emplace(Endpoint::create(_domain));
            _cq = CompletionQueue::open(_domain);
            _endpoint->bind(_cq, FI_RECV | FI_TRANSMIT);
            _endpoint->bind(AddressVector::open(_domain));
            _endpoint->bind(EventQueue::open(fabric));
            _endpoint->enable();

            auto regions = std::vector<Region>{};
            for (auto const& entry : _entries)
            {
                regions.emplace_back(reinterpret_cast<std::uintptr_t>(entry.data()), entry.size(), nullptr, nullptr, Region::Location::host());
            }
            _domain->registerRegions(regions, FI_REMOTE_WRITE);

            _info.emplace(TargetInfo{
                .id = _endpoint->id(),
                .fabricAddress = _endpoint->localAddress(),
                .provider = Provider::SHM,
                .remoteRegions = _domain->remoteRegions(),
                .bounceBufferInfo = TargetInfoBounceBufferInfo{.entryCount = entryCount, .entrySize = entrySize},
                .continuous = std::nullopt,
            });
        }

        [[nodiscard]]
        TargetInfo& info() noexcept
        {
            return *_info;
        }

        /** \brief The entry a write with immediate data completed into, if one did.
         */
        [[nodiscard]]
        std::optional<std::size_t> read()
        {
            auto const completion = _cq->read();
            if (!completion)
            {
                return std::nullopt;
            }
            auto const data = completion->tryData();
            REQUIRE(data);
            REQUIRE(data->data());
            return static_cast<std::size_t>(*data->data());
        }

        /** \brief Unpack an entry the way such a target did: the header, then every channel's samples in order.
         * \return The head index and count of the entry, and every channel's samples.
         */
        [[nodiscard]]
        std::pair<AudioEntryHeader, std::vector<std::vector<Sample>>> unpack(std::size_t entryIndex, std::size_t channelCount) const
        {
            auto const& entry = _entries.at(entryIndex);
            auto header = AudioEntryHeader{};
            std::memcpy(&header, entry.data(), sizeof(header));

            auto channels = std::vector<std::vector<Sample>>(channelCount, std::vector<Sample>(header.count));
            auto const* samples = entry.data() + sizeof(header);
            for (auto& channel : channels)
            {
                std::memcpy(channel.data(), samples, header.count * sizeof(Sample));
                samples += header.count * sizeof(Sample);
            }
            return {header, channels};
        }

    private:
        std::string _service;                            /**< The shm service name of the target. */
        std::vector<std::vector<std::uint8_t>> _entries; /**< The bounce buffer entries. */
        std::shared_ptr<Domain> _domain;                 /**< The target's domain, holding the registrations. */
        std::optional<Endpoint> _endpoint;               /**< The target's endpoint. */
        std::shared_ptr<CompletionQueue> _cq;            /**< Where the writes' immediate data arrives. */
        std::optional<TargetInfo> _info;                 /**< What the target publishes. */
    };
}

TEST_CASE("ofi: audio reaches a target that only publishes a bounce buffer", "[ofi][AudioBounceTarget]")
{
    // A source ring of 19456 samples per channel, from a history of 200 ms at 48 kHz.
    auto const domain = mxl::tests::makeTempDomain();
    {
        auto options = std::ofstream{domain / "options.json"};
        options << R"({"urn:x-mxl:option:history_duration/v1.0": 200000000})";
    }

    auto* instance = mxlCreateInstance(domain.c_str(), "");
    REQUIRE(instance != nullptr);
    auto const flowDef = mxl::tests::readFile("../../data/audio_flow.json");
    auto writer = mxlFlowWriter{};
    auto config = mxlFlowConfigInfo{};
    REQUIRE(mxlCreateFlowWriter(instance, flowDef.c_str(), nullptr, &writer, &config, nullptr) == MXL_STATUS_OK);
    auto reader = mxlFlowReader{};
    REQUIRE(mxlCreateFlowReader(instance, FlowId, "", &reader) == MXL_STATUS_OK);
    auto const channelCount = config.continuous.channelCount;
    REQUIRE(config.continuous.bufferLength == 19456);

    // A batch across the source wrap.
    constexpr auto Count = std::size_t{480};
    constexpr auto HeadIndex = std::uint64_t{19456 + 200};
    auto slice = mxlMutableWrappedMultiBufferSlice{};
    REQUIRE(mxlFlowWriterOpenSamples(writer, HeadIndex, Count, &slice) == MXL_STATUS_OK);
    for (auto channel = std::size_t{0}; channel < slice.count; ++channel)
    {
        auto index = HeadIndex - Count;
        for (auto const& fragment : slice.base.fragments)
        {
            auto* samples = reinterpret_cast<Sample*>(static_cast<std::uint8_t*>(fragment.pointer) + (channel * slice.stride));
            for (auto i = std::size_t{0}; i < (fragment.size / sizeof(Sample)); ++i)
            {
                samples[i] = sampleAt(channel, index++);
            }
        }
    }
    REQUIRE(mxlFlowWriterCommitSamples(writer) == MXL_STATUS_OK);

    // shm endpoints get names of their own per run of this test case: reusing the names of closed endpoints in the same process crashed shm on
    // the first write now and then.
    static auto run = 0;
    auto const service = "legacy-" + std::to_string(++run);
    auto target = BounceTarget{4, sizeof(AudioEntryHeader) + (Count * channelCount * sizeof(Sample)), service};

    auto fabrics = mxlFabricsInstance{};
    REQUIRE(mxlFabricsCreateInstance(instance, nullptr, &fabrics) == MXL_STATUS_OK);
    auto initiator = mxlFabricsInitiator{};
    REQUIRE(mxlFabricsCreateInitiator(fabrics, &initiator) == MXL_STATUS_OK);
    auto const initiatorConfig = mxlFabricsInitiatorConfig{
        .version = MXL_FABRICS_API_VERSION,
        .interface =
            {
                        .version = MXL_FABRICS_API_VERSION,
                        .provider = MXL_FABRICS_PROVIDER_SHM,
                        .caps = {.version = MXL_FABRICS_API_VERSION, .flags = MXL_FABRICS_IFACE_CAP_REMOTE_WRITE, .maxMessageSize = 0},
                        .address = {.node = "bounce-initiator", .service = service.c_str()},
                        .attr = nullptr,
                        },
        .reader = reader,
    };
    REQUIRE(mxlFabricsInitiatorSetup(initiator, &initiatorConfig, nullptr) == MXL_STATUS_OK);

    SECTION("the old wire format arrives unchanged")
    {
        REQUIRE(mxlFabricsInitiatorAddTarget(initiator, target.info().toAPI()) == MXL_STATUS_OK);

        // A connectionless initiator writes to a target once it has its address, which happens on the first progress call.
        auto const connectDeadline = std::chrono::steady_clock::now() + TestTimeout;
        while (mxlFabricsInitiatorMakeProgressNonBlocking(initiator) != MXL_STATUS_OK)
        {
            REQUIRE(std::chrono::steady_clock::now() < connectDeadline);
        }

        auto entry = std::optional<std::size_t>{};
        auto transferred = false;
        auto const deadline = std::chrono::steady_clock::now() + TestTimeout;
        while (!entry && (std::chrono::steady_clock::now() < deadline))
        {
            if (!transferred)
            {
                auto const status = mxlFabricsInitiatorTransferSamples(initiator, HeadIndex, Count);
                REQUIRE(((status == MXL_STATUS_OK) || (status == MXL_ERR_NOT_READY)));
                transferred = (status == MXL_STATUS_OK);
            }
            static_cast<void>(mxlFabricsInitiatorMakeProgressNonBlocking(initiator));
            entry = target.read();
        }
        REQUIRE(entry);
        REQUIRE(*entry == 0);

        auto const [header, channels] = target.unpack(*entry, channelCount);
        REQUIRE(header.headIndex == HeadIndex);
        REQUIRE(header.count == Count);
        for (auto channel = std::size_t{0}; channel < channelCount; ++channel)
        {
            auto expected = std::vector<Sample>{};
            for (auto index = HeadIndex - Count; index < HeadIndex; ++index)
            {
                expected.push_back(sampleAt(channel, index));
            }
            REQUIRE(channels[channel] == expected);
        }
    }

    SECTION("a target with no audio layout is refused")
    {
        target.info().bounceBufferInfo.reset();
        REQUIRE(mxlFabricsInitiatorAddTarget(initiator, target.info().toAPI()) != MXL_STATUS_OK);
    }

    SECTION("a target with an unknown audio protocol version is refused")
    {
        target.info().bounceBufferInfo.reset();
        target.info().continuous = TargetInfoContinuous{
            .version = TargetInfoContinuous::ProtocolVersion + 1,
            .sampleSize = sizeof(Sample),
            .channelCount = channelCount,
            .bufferLength = 1024,
            .descriptorCount = 1,
        };
        REQUIRE(mxlFabricsInitiatorAddTarget(initiator, target.info().toAPI()) != MXL_STATUS_OK);
    }

    REQUIRE(mxlFabricsDestroyInitiator(fabrics, initiator) == MXL_STATUS_OK);
    REQUIRE(mxlFabricsDestroyInstance(fabrics) == MXL_STATUS_OK);
    REQUIRE(mxlReleaseFlowReader(instance, reader) == MXL_STATUS_OK);
    REQUIRE(mxlReleaseFlowWriter(instance, writer) == MXL_STATUS_OK);
    REQUIRE(mxlDestroyInstance(instance) == MXL_STATUS_OK);
    std::filesystem::remove_all(domain);
}

TEST_CASE("ofi: audio slices of an empty batch describe nothing", "[ofi][AudioBounceTarget]")
{
    auto slice = mxlMutableWrappedMultiBufferSlice{};
    getMutableMultiBufferSlices(0, 0, 9, sizeof(Sample), 2, nullptr, slice);
    REQUIRE(slice.base.fragments[0].size == 0);
    REQUIRE(slice.base.fragments[1].size == 0);
}
