// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <picojson/wrapper.h>
#include <mxl/fabrics.h>
#include <mxl/flow.h>
#include <mxl/mxl.h>
#include "../Utils.hpp"
#include "TargetInfo.hpp"

namespace
{
    constexpr auto TestTimeout = std::chrono::seconds(5);
    constexpr auto FlowId = "b3bb5be7-9fe9-4324-a5bb-4c70e1084449";

    // A history of 200 ms gives the source 48 kHz float32 ring 19456 samples per channel, 100 ms gives the target ring 10240.
    constexpr auto SourceHistoryNs = std::uint64_t{200'000'000};
    constexpr auto TargetHistoryNs = std::uint64_t{100'000'000};

    // Six batches of 480 samples, [18880, 21760): the second crosses the source wrap at 19456 and the fourth the target wrap at 20480.
    constexpr auto BatchSize = std::size_t{480};
    constexpr auto FirstHeadIndex = std::uint64_t{18880 + BatchSize};
    constexpr auto BatchCount = std::size_t{6};

    using Sample = std::uint32_t;

    /** \brief The wire format an initiator writes audio in.
     */
    enum class Wire
    {
        Direct, /**< Straight into the target's ring, followed by a descriptor. */
        Bounce, /**< Into a bounce buffer entry, as initiators that predate TargetInfoContinuous do. */
    };

    /** \brief The sample every test writes at an index of a channel, so a sample landing anywhere else is visible.
     */
    Sample sampleAt(std::size_t channel, std::uint64_t index)
    {
        return static_cast<Sample>((channel << 24U) | (index & 0xFFFFFFU));
    }

    template<typename F, typename... Args>
    void check(F f, std::string const& msg, Args&&... args)
    {
        if (auto const status = f(std::forward<Args>(args)...); status != MXL_STATUS_OK)
        {
            throw std::runtime_error{msg + " (status " + std::to_string(status) + ")"};
        }
    }

    /** \brief A temporary domain whose options set its history duration, and therefore the length of its audio rings.
     */
    class HistoryDomain
    {
    public:
        explicit HistoryDomain(std::uint64_t historyNs)
            : _path{mxl::tests::makeTempDomain()}
        {
            auto options = std::ofstream{_path / "options.json"};
            options << R"({"urn:x-mxl:option:history_duration/v1.0": )" << historyNs << "}";
        }

        ~HistoryDomain()
        {
            std::filesystem::remove_all(_path);
        }

        HistoryDomain(HistoryDomain const&) = delete;
        HistoryDomain& operator=(HistoryDomain const&) = delete;

        [[nodiscard]]
        std::string path() const
        {
            return _path.string();
        }

    private:
        std::filesystem::path _path;
    };

    /** \brief Endpoints on the loopback interface, on ports the system picks.
     */
    struct ProviderTCP
    {
        constexpr static auto provider = MXL_FABRICS_PROVIDER_TCP;
        constexpr static char const* targetNode = "127.0.0.1";
        constexpr static char const* initiatorNode = "127.0.0.1";

        static std::string service()
        {
            return "0";
        }
    };

    /** \brief Shared memory endpoints. Every fixture gets names of its own: with the names of a closed fixture's endpoints reused in the same
     * process, shm crashed in smr_verify_peer on the first write in about one run in five.
     */
    struct ProviderSHM
    {
        constexpr static auto provider = MXL_FABRICS_PROVIDER_SHM;
        constexpr static char const* targetNode = "audio-target";
        constexpr static char const* initiatorNode = "audio-initiator";

        static std::string service()
        {
            static auto count = 0;
            return "direct-" + std::to_string(++count);
        }
    };

    /** \brief An initiator in one domain and a target in another, each with its own audio flow and ring length.
     */
    template<typename Provider>
    class AudioFixture
    {
    public:
        AudioFixture()
            : _sourceDomain{SourceHistoryNs}
            , _targetDomain{TargetHistoryNs}
            , _service{Provider::service()}
        {
            auto const flowDef = mxl::tests::readFile("../data/audio_flow.json");

            _sourceInstance = mxlCreateInstance(_sourceDomain.path().c_str(), "");
            _targetInstance = mxlCreateInstance(_targetDomain.path().c_str(), "");
            if ((_sourceInstance == nullptr) || (_targetInstance == nullptr))
            {
                throw std::runtime_error{"failed to create the mxl instances"};
            }

            // clang-format off
            check(mxlCreateFlowWriter, "source writer", _sourceInstance, flowDef.c_str(), nullptr, &_sourceWriter, &sourceConfig, nullptr);
            check(mxlCreateFlowReader, "source reader", _sourceInstance, FlowId, "", &_sourceReader);
            check(mxlCreateFlowWriter, "target writer", _targetInstance, flowDef.c_str(), nullptr, &_targetWriter, &targetConfig, nullptr);
            check(mxlCreateFlowReader, "target reader", _targetInstance, FlowId, "", &_targetReader);
            check(mxlFabricsCreateInstance, "source fabrics", _sourceInstance, nullptr, &_sourceFabrics);
            check(mxlFabricsCreateInstance, "target fabrics", _targetInstance, nullptr, &_targetFabrics);
            check(mxlFabricsCreateTarget, "target", _targetFabrics, &_target);

            auto targetConfig = mxlFabricsTargetConfig{
                .version = MXL_FABRICS_API_VERSION,
                .interface = {
                    .version = MXL_FABRICS_API_VERSION,
                    .provider = Provider::provider,
                    .caps = {.version = MXL_FABRICS_API_VERSION, .flags = MXL_FABRICS_IFACE_CAP_REMOTE_WRITE, .maxMessageSize = 0},
                    .address = {.node = Provider::targetNode, .service = _service.c_str()},
                    .attr = nullptr,
                },
                .writer = _targetWriter,
            };
            check(mxlFabricsTargetSetup, "target setup", _target, &targetConfig, nullptr, &_targetInfo);

            // clang-format on

            _initiator = addInitiator(Provider::initiatorNode);
        }

        ~AudioFixture()
        {
            for (auto const& [initiator, reader] : _initiators)
            {
                mxlFabricsDestroyInitiator(_sourceFabrics, initiator);
                mxlReleaseFlowReader(_sourceInstance, reader);
            }
            mxlFabricsDestroyTarget(_targetFabrics, _target);
            mxlFabricsFreeTargetInfo(_targetInfo);
            if (_legacyTargetInfo != nullptr)
            {
                mxlFabricsFreeTargetInfo(_legacyTargetInfo);
            }
            mxlFabricsDestroyInstance(_sourceFabrics);
            mxlFabricsDestroyInstance(_targetFabrics);
            mxlReleaseFlowReader(_targetInstance, _targetReader);
            mxlReleaseFlowWriter(_targetInstance, _targetWriter);
            mxlReleaseFlowReader(_sourceInstance, _sourceReader);
            mxlReleaseFlowWriter(_sourceInstance, _sourceWriter);
            mxlDestroyInstance(_targetInstance);
            mxlDestroyInstance(_sourceInstance);
        }

        AudioFixture(AudioFixture const&) = delete;
        AudioFixture& operator=(AudioFixture const&) = delete;

    protected:
        /** \brief The initiator every fixture has.
         */
        [[nodiscard]]
        mxlFabricsInitiator initiator() const noexcept
        {
            return _initiator;
        }

        /** \brief Set up another initiator on the source flow, with a reader of its own.
         * \param node The node of its endpoint, unique among the fixture's initiators.
         */
        mxlFabricsInitiator addInitiator(char const* node)
        {
            auto reader = mxlFlowReader{nullptr};
            auto initiator = mxlFabricsInitiator{nullptr};
            // clang-format off
            check(mxlCreateFlowReader, "source reader", _sourceInstance, FlowId, "", &reader);
            check(mxlFabricsCreateInitiator, "initiator", _sourceFabrics, &initiator);
            _initiators.emplace_back(initiator, reader);

            auto initiatorConfig = mxlFabricsInitiatorConfig{
                .version = MXL_FABRICS_API_VERSION,
                .interface = {
                    .version = MXL_FABRICS_API_VERSION,
                    .provider = Provider::provider,
                    .caps = {.version = MXL_FABRICS_API_VERSION, .flags = MXL_FABRICS_IFACE_CAP_REMOTE_WRITE, .maxMessageSize = 0},
                    .address = {.node = node, .service = _service.c_str()},
                    .attr = nullptr,
                },
                .reader = reader,
            };
            check(mxlFabricsInitiatorSetup, "initiator setup", initiator, &initiatorConfig, nullptr);
            // clang-format on
            return initiator;
        }

        /** \brief Add the target to an initiator, as it reads the target's info when writing the given wire format, and connect it.
         */
        void addTarget(mxlFabricsInitiator initiator, Wire wire)
        {
            auto const info = (wire == Wire::Direct) ? _targetInfo : legacyTargetInfo();
            check(mxlFabricsInitiatorAddTarget, "add target", initiator, info);
            connect(initiator);
        }

        /** \brief The target's TargetInfo, as the initiator receives it.
         */
        [[nodiscard]]
        mxl::lib::fabrics::ofi::TargetInfo const& targetInfo() const
        {
            return *mxl::lib::fabrics::ofi::TargetInfo::fromAPI(_targetInfo);
        }

        /** \brief Write the ramp of every channel for a batch into the source flow and commit it.
         */
        void writeSource(std::uint64_t headIndex, std::size_t count)
        {
            auto slice = mxlMutableWrappedMultiBufferSlice{};
            REQUIRE(mxlFlowWriterOpenSamples(_sourceWriter, headIndex, count, &slice) == MXL_STATUS_OK);
            for (auto channel = std::size_t{0}; channel < slice.count; ++channel)
            {
                auto index = headIndex - count;
                for (auto const& fragment : slice.base.fragments)
                {
                    auto* samples = reinterpret_cast<Sample*>(static_cast<std::uint8_t*>(fragment.pointer) + (channel * slice.stride));
                    for (auto i = std::size_t{0}; i < (fragment.size / sizeof(Sample)); ++i)
                    {
                        samples[i] = sampleAt(channel, index++);
                    }
                }
            }
            REQUIRE(mxlFlowWriterCommitSamples(_sourceWriter) == MXL_STATUS_OK);
        }

        /** \brief Offer a batch to an initiator until it accepts it. The application reads it first, as a gateway does.
         */
        void transfer(mxlFabricsInitiator initiator, std::uint64_t headIndex, std::size_t count)
        {
            auto slice = mxlWrappedMultiBufferSlice{};
            REQUIRE(mxlFlowReaderGetSamplesNonBlocking(_sourceReader, headIndex, count, &slice) == MXL_STATUS_OK);

            auto const deadline = std::chrono::steady_clock::now() + TestTimeout;
            for (;;)
            {
                auto const status = mxlFabricsInitiatorTransferSamples(initiator, headIndex, count);
                if (status == MXL_STATUS_OK)
                {
                    return;
                }
                REQUIRE(status == MXL_ERR_NOT_READY);
                REQUIRE(std::chrono::steady_clock::now() < deadline);

                // The shm provider moves data only while both sides make progress.
                progress();
                pollTarget();
            }
        }

        /** \brief Wait for the next batch on the target, committed the way the application does.
         * \return The head index and count the target reported.
         */
        std::pair<std::uint64_t, std::size_t> receive()
        {
            auto const deadline = std::chrono::steady_clock::now() + TestTimeout;
            while (_received.empty() && (std::chrono::steady_clock::now() < deadline))
            {
                progress();
                pollTarget();
            }
            REQUIRE(!_received.empty());

            auto const batch = _received.front();
            _received.erase(_received.begin());
            return batch;
        }

        /** \brief Wait until an initiator has nothing in flight.
         */
        void drain(mxlFabricsInitiator initiator)
        {
            auto const deadline = std::chrono::steady_clock::now() + TestTimeout;
            while (mxlFabricsInitiatorMakeProgressNonBlocking(initiator) != MXL_STATUS_OK)
            {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                pollTarget();
            }
        }

        /** \brief Require that the target flow holds the ramp of every channel for [headIndex - count, headIndex), byte for byte.
         */
        void requireTargetHolds(std::uint64_t headIndex, std::size_t count)
        {
            auto slice = mxlWrappedMultiBufferSlice{};
            REQUIRE(mxlFlowReaderGetSamplesNonBlocking(_targetReader, headIndex, count, &slice) == MXL_STATUS_OK);
            REQUIRE(slice.count == targetConfig.continuous.channelCount);

            for (auto channel = std::size_t{0}; channel < slice.count; ++channel)
            {
                auto expected = std::vector<Sample>{};
                auto actual = std::vector<Sample>{};
                for (auto index = headIndex - count; index < headIndex; ++index)
                {
                    expected.push_back(sampleAt(channel, index));
                }
                for (auto const& fragment : slice.base.fragments)
                {
                    auto const* bytes = static_cast<std::uint8_t const*>(fragment.pointer) + (channel * slice.stride);
                    auto const offset = actual.size();
                    actual.resize(offset + (fragment.size / sizeof(Sample)));
                    std::memcpy(actual.data() + offset, bytes, fragment.size);
                }
                REQUIRE(actual == expected);
            }
        }

    private:
        void progress()
        {
            for (auto const& [initiator, reader] : _initiators)
            {
                auto const status = mxlFabricsInitiatorMakeProgressNonBlocking(initiator);
                REQUIRE(((status == MXL_STATUS_OK) || (status == MXL_ERR_NOT_READY)));
            }
        }

        /** \brief The target's TargetInfo as an initiator that predates TargetInfoContinuous reads it.
         *
         * Such an initiator parses the same JSON and knows no "continuous" key, so it reads the regions and the bounce buffer alone. Given
         * that info, an initiator of this library writes what those initiators write: RMASampleBounceEgressProtocol is their egress
         * protocol, and it refuses a target the same way they do, unless every remote region is a bounce buffer entry.
         */
        mxlFabricsTargetInfo legacyTargetInfo()
        {
            if (_legacyTargetInfo == nullptr)
            {
                auto size = std::size_t{0};
                REQUIRE(mxlFabricsTargetInfoToString(_targetInfo, nullptr, &size) == MXL_STATUS_OK);
                auto json = std::string(size, '\0');
                REQUIRE(mxlFabricsTargetInfoToString(_targetInfo, json.data(), &size) == MXL_STATUS_OK);
                json.resize(size - 1);

                auto parsed = picojson::value{};
                REQUIRE(picojson::parse(parsed, json).empty());
                auto root = parsed.get<picojson::object>();
                root.erase("continuous");
                REQUIRE(mxlFabricsTargetInfoFromString(picojson::value{root}.serialize().c_str(), &_legacyTargetInfo) == MXL_STATUS_OK);
            }
            return _legacyTargetInfo;
        }

        /** \brief Read one batch from the target, if one arrived, and commit it the way the application does.
         */
        void pollTarget()
        {
            auto headIndex = std::uint64_t{0};
            auto count = std::size_t{0};
            auto const status = mxlFabricsTargetReadSamplesNonBlocking(_target, &headIndex, &count);
            if (status == MXL_ERR_NOT_READY)
            {
                return;
            }
            REQUIRE(status == MXL_STATUS_OK);

            // The samples are in the ring already; opening and committing them publishes them.
            auto slice = mxlMutableWrappedMultiBufferSlice{};
            REQUIRE(mxlFlowWriterOpenSamples(_targetWriter, headIndex, count, &slice) == MXL_STATUS_OK);
            REQUIRE(mxlFlowWriterCommitSamples(_targetWriter) == MXL_STATUS_OK);
            _received.emplace_back(headIndex, count);
        }

        void connect(mxlFabricsInitiator initiator)
        {
            auto const deadline = std::chrono::steady_clock::now() + TestTimeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                auto headIndex = std::uint64_t{0};
                auto count = std::size_t{0};
                static_cast<void>(mxlFabricsTargetReadSamplesNonBlocking(_target, &headIndex, &count));
                if (mxlFabricsInitiatorMakeProgressNonBlocking(initiator) == MXL_STATUS_OK)
                {
                    return;
                }
            }
            throw std::runtime_error{"failed to connect within the timeout"};
        }

    protected:
        mxlFlowConfigInfo sourceConfig{};
        mxlFlowConfigInfo targetConfig{};

    private:
        HistoryDomain _sourceDomain;
        HistoryDomain _targetDomain;
        std::string _service; /**< Service of both endpoints. */

        mxlInstance _sourceInstance{nullptr};
        mxlInstance _targetInstance{nullptr};
        mxlFlowWriter _sourceWriter{nullptr};
        mxlFlowReader _sourceReader{nullptr};
        mxlFlowWriter _targetWriter{nullptr};
        mxlFlowReader _targetReader{nullptr};
        mxlFabricsInstance _sourceFabrics{nullptr};
        mxlFabricsInstance _targetFabrics{nullptr};
        mxlFabricsTarget _target{nullptr};
        mxlFabricsTargetInfo _targetInfo{nullptr};
        mxlFabricsTargetInfo _legacyTargetInfo{nullptr}; /**< The target's info as an initiator that predates the layout reads it. */
        std::vector<std::pair<mxlFabricsInitiator, mxlFlowReader>> _initiators; /**< Every initiator and the reader it was set up with. */
        mxlFabricsInitiator _initiator{nullptr};
        std::vector<std::pair<std::uint64_t, std::size_t>> _received;           /**< Batches the target reported, oldest first. */
    };
}

TEMPLATE_TEST_CASE_METHOD(AudioFixture, "Fabrics: audio lands straight in the target ring", "[fabrics][audio]", ProviderTCP, ProviderSHM)
{
    auto const sourceLength = this->sourceConfig.continuous.bufferLength;
    auto const targetLength = this->targetConfig.continuous.bufferLength;
    auto const channelCount = this->targetConfig.continuous.channelCount;
    REQUIRE(sourceLength == 19456);
    REQUIRE(targetLength == 10240);

    // The target publishes its ring and its descriptor slots, and a bounce buffer whose entries are the remote regions.
    auto const& info = this->targetInfo();
    REQUIRE(info.continuous);
    REQUIRE(info.continuous->bufferLength == targetLength);
    REQUIRE(info.continuous->channelCount == channelCount);
    REQUIRE(info.continuous->sampleSize == sizeof(Sample));
    REQUIRE(info.continuous->ring.len == channelCount * targetLength * sizeof(Sample));
    REQUIRE(info.continuous->descriptors.len == info.continuous->descriptorCount * 16);
    REQUIRE(info.bounceBufferInfo);
    REQUIRE(info.remoteRegions.size() == info.bounceBufferInfo->entryCount);

    auto const initiator = this->initiator();
    this->addTarget(initiator, Wire::Direct);

    SECTION("one batch at a time")
    {
        for (auto batch = std::size_t{0}; batch < BatchCount; ++batch)
        {
            auto const headIndex = FirstHeadIndex + (batch * BatchSize);
            this->writeSource(headIndex, BatchSize);
            this->transfer(initiator, headIndex, BatchSize);

            auto const [receivedHead, receivedCount] = this->receive();
            REQUIRE(receivedHead == headIndex);
            REQUIRE(receivedCount == BatchSize);
            this->requireTargetHolds(headIndex, BatchSize);
            this->drain(initiator);
        }
    }

    SECTION("batches in a burst")
    {
        // Offered back to back without waiting. A connectionless endpoint merges what arrives while a batch is in flight, so the target may
        // see fewer, longer batches; together they must cover every sample once, in order, each short enough for the writer to commit.
        for (auto batch = std::size_t{0}; batch < BatchCount; ++batch)
        {
            auto const headIndex = FirstHeadIndex + (batch * BatchSize);
            this->writeSource(headIndex, BatchSize);
            this->transfer(initiator, headIndex, BatchSize);
        }

        auto const lastHeadIndex = FirstHeadIndex + ((BatchCount - 1) * BatchSize);
        auto expectedStart = FirstHeadIndex - BatchSize;
        while (expectedStart < lastHeadIndex)
        {
            auto const [receivedHead, receivedCount] = this->receive();
            REQUIRE(receivedHead - receivedCount == expectedStart);
            REQUIRE(receivedCount <= targetLength / 2);
            expectedStart = receivedHead;
        }
        REQUIRE(expectedStart == lastHeadIndex);
        this->drain(initiator);

        this->requireTargetHolds(lastHeadIndex, BatchCount * BatchSize);
    }

    SECTION("a batch longer than a bounce buffer entry")
    {
        // The bounce buffer holds the flow's sync batch size of 480 samples per entry, so only the direct path carries this batch. It
        // crosses the target wrap at 20480.
        auto const headIndex = std::uint64_t{20480 + BatchSize};
        this->writeSource(headIndex, 2 * BatchSize);
        this->transfer(initiator, headIndex, 2 * BatchSize);

        auto const [receivedHead, receivedCount] = this->receive();
        REQUIRE(receivedHead == headIndex);
        REQUIRE(receivedCount == 2 * BatchSize);
        this->requireTargetHolds(headIndex, 2 * BatchSize);
        this->drain(initiator);
    }
}

TEMPLATE_TEST_CASE_METHOD(AudioFixture, "Fabrics: audio from an initiator that predates the ring layout lands in the target ring",
    "[fabrics][audio][legacy]", ProviderTCP, ProviderSHM)
{
    auto const initiator = this->initiator();
    this->addTarget(initiator, Wire::Bounce);

    // One batch per transfer, as such an initiator sends them: the second crosses the source wrap and the fourth the target wrap, so the
    // target unpacks entries into both fragments of its ring.
    for (auto batch = std::size_t{0}; batch < BatchCount; ++batch)
    {
        auto const headIndex = FirstHeadIndex + (batch * BatchSize);
        this->writeSource(headIndex, BatchSize);
        this->transfer(initiator, headIndex, BatchSize);

        auto const [receivedHead, receivedCount] = this->receive();
        REQUIRE(receivedHead == headIndex);
        REQUIRE(receivedCount == BatchSize);
        this->requireTargetHolds(headIndex, BatchSize);
        this->drain(initiator);
    }

    auto const lastHeadIndex = FirstHeadIndex + ((BatchCount - 1) * BatchSize);
    this->requireTargetHolds(lastHeadIndex, BatchCount * BatchSize);
}

// A connectionless target takes writes from any number of initiators on its one endpoint, so both wire formats arrive there interleaved. A
// connected target serves one connection at a time and sees one format per connection.
TEST_CASE_METHOD(AudioFixture<ProviderSHM>, "Fabrics: initiators of both wire formats write to one target in turn", "[fabrics][audio][legacy]")
{
    auto const direct = this->initiator();
    auto const legacy = this->addInitiator("audio-legacy-initiator");
    this->addTarget(direct, Wire::Direct);
    this->addTarget(legacy, Wire::Bounce);

    // The legacy initiator sends the odd batches, among them the second, across the source wrap, and the fourth, across the target wrap.
    for (auto batch = std::size_t{0}; batch < BatchCount; ++batch)
    {
        auto const initiator = ((batch % 2) == 0) ? direct : legacy;
        auto const headIndex = FirstHeadIndex + (batch * BatchSize);
        this->writeSource(headIndex, BatchSize);
        this->transfer(initiator, headIndex, BatchSize);

        auto const [receivedHead, receivedCount] = this->receive();
        REQUIRE(receivedHead == headIndex);
        REQUIRE(receivedCount == BatchSize);
        this->requireTargetHolds(headIndex, BatchSize);
        this->drain(initiator);
    }

    auto const lastHeadIndex = FirstHeadIndex + ((BatchCount - 1) * BatchSize);
    this->requireTargetHolds(lastHeadIndex, BatchCount * BatchSize);
}
