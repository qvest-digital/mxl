// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include "EgressWindow.hpp"
#include <cassert>
#include <algorithm>

namespace mxl::lib::fabrics::ofi
{
    EgressWindow::EgressWindow(std::size_t maxTransfers, std::size_t maxWrites) noexcept
        : _maxTransfers{std::max<std::size_t>(maxTransfers, 1)}
        , _maxWrites{std::max<std::size_t>(maxWrites, 1)}
    {}

    bool EgressWindow::open() noexcept
    {
        if (_transfersInFlight >= _maxTransfers)
        {
            ++_skipped;
            return false;
        }
        ++_openTransfers;
        ++_transfersInFlight;
        ++_posted;
        return true;
    }

    bool EgressWindow::hasRoomFor(std::size_t writes) const noexcept
    {
        return (_writesInFlight + writes) <= _maxWrites;
    }

    void EgressWindow::wrote(bool reportsCompletion, bool endsTransfer, std::uint64_t index, std::size_t bytes) noexcept
    {
        ++_writesInFlight;
        ++_openWrites;
        _openBytes += bytes;
        if (!reportsCompletion && !endsTransfer)
        {
            return;
        }

        auto const transfers = endsTransfer ? _openTransfers : std::size_t{0};
        _segments.push_back(Segment{.transfers = transfers, .writes = _openWrites, .bytes = _openBytes, .index = index});
        _openWrites = 0;
        _openBytes = 0;
        if (endsTransfer)
        {
            _openTransfers = 0;
        }
    }

    void EgressWindow::abandon() noexcept
    {
        if (_openTransfers == 0)
        {
            return;
        }
        // The writes stay counted: the provider still holds them until a later completion settles them.
        --_openTransfers;
        --_transfersInFlight;
        --_posted;
        ++_skipped;
    }

    std::optional<std::uint64_t> EgressWindow::complete(bool success) noexcept
    {
        // Every completion that reaches the window answers a write it recorded: an endpoint's completions after its connection was reset
        // are counted by the endpoint, not here. One with nothing expected, or one settling more than is in flight, is a bookkeeping error.
        if (_segments.empty())
        {
            assert(!"completion with nothing in flight");
            ++_accountingErrors;
            return std::nullopt;
        }

        auto const segment = _segments.front();
        _segments.pop_front();

        if ((segment.transfers > _transfersInFlight) || (segment.writes > _writesInFlight))
        {
            assert(!"completion settles more than is in flight");
            ++_accountingErrors;
        }
        _transfersInFlight -= std::min(_transfersInFlight, segment.transfers);
        _writesInFlight -= std::min(_writesInFlight, segment.writes);
        if (!success)
        {
            _failed += segment.transfers;
            return std::nullopt;
        }

        _completed += segment.transfers;
        _bytesCompleted += segment.bytes;
        if (segment.transfers == 0)
        {
            return std::nullopt;
        }
        _lastCompletedIndex = segment.index;
        return segment.index;
    }

    void EgressWindow::torn() noexcept
    {
        ++_torn;
    }

    std::size_t EgressWindow::reset() noexcept
    {
        auto const expected = _segments.size();
        _failed += _transfersInFlight;
        _segments.clear();
        _openTransfers = 0;
        _openWrites = 0;
        _openBytes = 0;
        _transfersInFlight = 0;
        _writesInFlight = 0;
        return expected;
    }

    bool EgressWindow::expectsCompletion() const noexcept
    {
        return !_segments.empty();
    }

    void EgressWindow::skip() noexcept
    {
        ++_skipped;
    }

    std::size_t EgressWindow::maxTransfers() const noexcept
    {
        return _maxTransfers;
    }

    std::size_t EgressWindow::maxWrites() const noexcept
    {
        return _maxWrites;
    }

    mxlFabricsTargetStats EgressWindow::stats() const noexcept
    {
        return mxlFabricsTargetStats{
            .transfersPosted = _posted,
            .transfersCompleted = _completed,
            .transfersFailed = _failed,
            .transfersSkipped = _skipped,
            .transfersInFlight = _transfersInFlight,
            .writesInFlight = _writesInFlight,
            .bytesCompleted = _bytesCompleted,
            .transfersTorn = _torn,
            .accountingErrors = _accountingErrors,
            .lastCompletedIndex = _lastCompletedIndex,
        };
    }
}
