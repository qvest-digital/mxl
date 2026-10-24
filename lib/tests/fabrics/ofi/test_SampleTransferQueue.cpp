// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "Exception.hpp"
#include "SampleTransferQueue.hpp"

using namespace mxl::lib::fabrics::ofi;

namespace
{
    /** \brief A write the fake sink was asked to post.
     */
    struct Posted
    {
        bool descriptor;         /**< A descriptor write rather than a data write. */
        std::uint64_t headIndex; /**< End of the batch the write belongs to. */
        std::size_t count;       /**< Samples per channel of that batch. Only set for descriptors. */
        std::size_t slot;        /**< Descriptor slot. Only set for descriptors. */

        bool operator==(Posted const&) const = default;
    };

    Posted data(std::uint64_t headIndex)
    {
        return Posted{.descriptor = false, .headIndex = headIndex, .count = 0, .slot = 0};
    }

    Posted descriptor(std::uint64_t headIndex, std::size_t count, std::size_t slot)
    {
        return Posted{.descriptor = true, .headIndex = headIndex, .count = count, .slot = slot};
    }

    /** \brief Records what the queue posts instead of posting it. Every batch is planned as a fixed number of data writes.
     */
    class FakeSink final : public SampleTransferQueue::Sink
    {
    public:
        explicit FakeSink(std::size_t writesPerBatch)
            : _writesPerBatch{writesPerBatch}
            , _refuseAt{}
            , posts{}
        {}

        std::vector<SampleWrite> plan(std::uint64_t headIndex, std::size_t) override
        {
            // The batch end rides in the local address, so post() can tell which batch a data write belongs to.
            auto const write = SampleWrite{
                .local = {LocalRegion{.addr = headIndex, .len = 1, .desc = nullptr}},
                .remote = {RemoteRegion{.addr = 0, .len = 1, .rkey = 0}},
            };
            return std::vector<SampleWrite>(_writesPerBatch, write);
        }

        std::pair<SampleWrite, std::uint32_t> describe(std::uint64_t headIndex, std::size_t count, std::size_t slot) override
        {
            auto const write = SampleWrite{
                .local = {LocalRegion{.addr = headIndex, .len = count, .desc = nullptr}},
                .remote = {RemoteRegion{.addr = slot, .len = count, .rkey = 0}},
            };
            return {write, static_cast<std::uint32_t>(slot)};
        }

        void post(SampleWrite const& write, std::optional<std::uint32_t> immData) override
        {
            if (_refuseAt && (*_refuseAt == attempts++))
            {
                throw std::runtime_error{"refused"};
            }

            auto const& local = write.local.front();
            if (immData)
            {
                posts.push_back(descriptor(local.addr, local.len, *immData));
            }
            else
            {
                posts.push_back(data(local.addr));
            }
        }

        /** \brief Refuse the post attempt with this number, counted from 0.
         */
        void refuse(std::size_t attempt)
        {
            _refuseAt = attempt;
        }

        /** \brief Take what was posted since the last call.
         */
        std::vector<Posted> take()
        {
            return std::exchange(posts, {});
        }

    private:
        std::size_t _writesPerBatch;
        std::optional<std::size_t> _refuseAt;

    public:
        std::size_t attempts{0};
        std::vector<Posted> posts;
    };

    using Ordering = SampleTransferQueue::Ordering;
}

TEST_CASE("ofi: gated sample queue posts a descriptor only after every earlier write completed", "[ofi][SampleTransferQueue]")
{
    auto sink = FakeSink{3};
    auto queue = SampleTransferQueue{Ordering::Gated, 64, 8};

    queue.push(100, 10);
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(100), data(100), data(100)});
    REQUIRE(queue.hasPendingWork());

    // Completions carry nothing that tells the writes apart, and on an unordered endpoint any of them may come first. The descriptor waits
    // for the last one, whichever it is.
    queue.complete();
    queue.pump(sink);
    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take().empty());

    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{descriptor(100, 10, 0)});

    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take().empty());
    REQUIRE(!queue.hasPendingWork());
}

TEST_CASE("ofi: gated sample queue merges batches that arrive while one is in flight", "[ofi][SampleTransferQueue]")
{
    auto sink = FakeSink{2};
    auto queue = SampleTransferQueue{Ordering::Gated, 64, 8};

    queue.push(100, 10);
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(100), data(100)});

    // The source ring still holds these, so they wait as one range instead of each becoming a batch.
    queue.push(110, 10);
    queue.pump(sink);
    queue.push(120, 10);
    queue.pump(sink);
    REQUIRE(sink.take().empty());

    // The first batch's data has landed: its descriptor goes out, and the merged range's data with it.
    queue.complete();
    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{descriptor(100, 10, 0), data(120), data(120)});

    // The merged batch's descriptor waits for its data and for the previous descriptor, so the target sees them in order.
    queue.push(125, 5);
    queue.complete();
    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take().empty());

    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{descriptor(120, 20, 1), data(125), data(125)});
}

TEST_CASE("ofi: gated sample queue keeps a merged batch within what the target can commit", "[ofi][SampleTransferQueue]")
{
    // A target ring of 32 samples commits at most 16 at once.
    auto sink = FakeSink{1};
    auto queue = SampleTransferQueue{Ordering::Gated, 16, 8};

    queue.push(100, 16);
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(100)});

    queue.push(110, 10);
    queue.push(120, 10);
    queue.pump(sink);
    REQUIRE(sink.take().empty());

    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{descriptor(100, 16, 0), data(120)});

    queue.complete();
    queue.complete();
    queue.pump(sink);

    // [100, 120) waited as one range of 20 samples. The 4 oldest were dropped, which the target zero-fills when it commits.
    REQUIRE(sink.take() == std::vector{descriptor(120, 16, 1)});
}

TEST_CASE("ofi: ordered sample queue posts a descriptor right behind its data", "[ofi][SampleTransferQueue]")
{
    auto sink = FakeSink{2};
    auto queue = SampleTransferQueue{Ordering::Ordered, 64, 8};

    queue.push(100, 10);
    queue.pump(sink);
    queue.push(110, 10);
    queue.pump(sink);

    REQUIRE(sink.take() == std::vector{data(100), data(100), descriptor(100, 10, 0), data(110), data(110), descriptor(110, 10, 1)});
}

TEST_CASE("ofi: ordered sample queue reuses a descriptor slot only after its writes completed", "[ofi][SampleTransferQueue]")
{
    auto sink = FakeSink{1};
    auto queue = SampleTransferQueue{Ordering::Ordered, 64, 2};

    queue.push(100, 10);
    queue.pump(sink);
    queue.push(110, 10);
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(100), descriptor(100, 10, 0), data(110), descriptor(110, 10, 1)});

    // Both slots are in flight: the data goes out, the descriptor waits.
    queue.push(120, 10);
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(120)});

    // The first batch's two writes completed, so slot 0 is free again.
    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take().empty());
    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{descriptor(120, 10, 0)});
}

TEST_CASE("ofi: sample queue resumes a batch the provider refused part way", "[ofi][SampleTransferQueue]")
{
    auto sink = FakeSink{3};
    auto queue = SampleTransferQueue{Ordering::Ordered, 64, 8};

    sink.refuse(1);
    queue.push(100, 10);
    REQUIRE_THROWS(queue.pump(sink));
    REQUIRE(sink.take() == std::vector{data(100)});

    // The caller retries the same transfer. It is not accepted twice, and the write that was refused is posted, not the one before it.
    queue.push(100, 10);
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(100), data(100), descriptor(100, 10, 0)});
}

TEST_CASE("ofi: sample queue sends samples once", "[ofi][SampleTransferQueue]")
{
    auto sink = FakeSink{1};
    auto queue = SampleTransferQueue{Ordering::Ordered, 64, 8};

    queue.push(100, 10);
    queue.pump(sink);
    queue.push(100, 10);
    queue.pump(sink);
    queue.push(105, 10);
    queue.pump(sink);

    // The overlapping batch is cut to what was not sent yet.
    REQUIRE(sink.take() == std::vector{data(100), descriptor(100, 10, 0), data(105), descriptor(105, 5, 1)});
}

TEST_CASE("ofi: gated sample queue drops a batch whose write failed", "[ofi][SampleTransferQueue]")
{
    auto sink = FakeSink{2};
    auto queue = SampleTransferQueue{Ordering::Gated, 64, 8};

    queue.push(100, 10);
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(100), data(100)});

    // The data may not have landed, so the batch is never described.
    queue.fail();
    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take().empty());
    REQUIRE(!queue.hasPendingWork());

    queue.push(110, 10);
    queue.pump(sink);
    queue.complete();
    queue.complete();
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(110), data(110), descriptor(110, 10, 0)});
}

TEST_CASE("ofi: sample queue reset forgets everything in flight", "[ofi][SampleTransferQueue]")
{
    auto sink = FakeSink{2};
    auto queue = SampleTransferQueue{Ordering::Gated, 64, 8};

    queue.push(100, 10);
    queue.pump(sink);
    queue.push(110, 10);
    REQUIRE(sink.take() == std::vector{data(100), data(100)});

    REQUIRE(queue.reset() == 2);
    REQUIRE(!queue.hasPendingWork());
    queue.pump(sink);
    REQUIRE(sink.take().empty());

    // A new endpoint starts from scratch: nothing waits for the old writes, and the samples pending before are not sent.
    queue.push(120, 10);
    queue.pump(sink);
    REQUIRE(sink.take() == std::vector{data(120), data(120)});
}

TEST_CASE("ofi: sample queue rejects batches it cannot send", "[ofi][SampleTransferQueue]")
{
    auto queue = SampleTransferQueue{Ordering::Gated, 16, 8};

    REQUIRE_THROWS_AS(queue.push(100, 0), Exception);
    REQUIRE_THROWS_AS(queue.push(100, 17), Exception);
    REQUIRE_THROWS_AS(queue.push(5, 10), Exception);
    REQUIRE_THROWS_AS((SampleTransferQueue{Ordering::Gated, 0, 8}), Exception);
    REQUIRE_THROWS_AS((SampleTransferQueue{Ordering::Gated, 16, 0}), Exception);
}
