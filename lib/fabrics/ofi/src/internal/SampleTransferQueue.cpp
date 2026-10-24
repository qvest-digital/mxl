// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include "SampleTransferQueue.hpp"
#include <algorithm>
#include <mxl-internal/Logging.hpp>
#include "Exception.hpp"

namespace mxl::lib::fabrics::ofi
{
    SampleTransferQueue::SampleTransferQueue(Ordering ordering, std::size_t maxCount, std::size_t descriptorCount)
        : _ordering{ordering}
        , _maxCount{maxCount}
        , _descriptorCount{descriptorCount}
        , _acceptedEnd{}
        , _pending{}
        , _current{}
        , _dropping{false}
        , _inFlight{0}
        , _postedTotal{0}
        , _completedTotal{0}
        , _slotsBusy{}
        , _nextSlot{0}
    {
        if ((maxCount == 0) || (descriptorCount == 0))
        {
            throw Exception::invalidArgument(
                "A sample transfer queue needs a batch size and descriptor slots, got {} and {}.", maxCount, descriptorCount);
        }
    }

    void SampleTransferQueue::push(std::uint64_t headIndex, std::size_t count)
    {
        if ((count == 0) || (count > headIndex) || (count > _maxCount))
        {
            throw Exception::invalidArgument(
                "Cannot transfer {} samples ending at index {}: a batch holds 1 to {} samples.", count, headIndex, _maxCount);
        }

        auto start = headIndex - count;
        if (_acceptedEnd)
        {
            if (headIndex <= *_acceptedEnd)
            {
                return;
            }
            start = std::max(start, *_acceptedEnd);
        }
        _acceptedEnd = headIndex;

        if (_pending)
        {
            start = _pending->start;
        }

        // A target writer commits at most half its ring at once, so a longer range could never be committed. Its oldest samples go; the target
        // zero-fills them when it commits this range.
        if ((headIndex - start) > _maxCount)
        {
            if (!_dropping)
            {
                MXL_WARN("Audio for a target is arriving faster than it completes, dropping {} samples per channel.", headIndex - start - _maxCount);
                _dropping = true;
            }
            start = headIndex - _maxCount;
        }
        else
        {
            _dropping = false;
        }

        _pending = Range{.start = start, .end = headIndex};
    }

    void SampleTransferQueue::pump(Sink& sink)
    {
        for (;;)
        {
            if (!_current)
            {
                if (!_pending)
                {
                    return;
                }

                auto const range = *_pending;
                _current = Batch{.range = range, .writes = sink.plan(range.end, range.end - range.start), .posted = 0};
                _pending.reset();
            }

            while (_current->posted < _current->writes.size())
            {
                sink.post(_current->writes[_current->posted], std::nullopt);
                ++_current->posted;
                posted();
            }

            releaseSlots();
            if (((_ordering == Ordering::Gated) && (_inFlight > 0)) || (_slotsBusy.size() >= _descriptorCount))
            {
                return;
            }

            auto const [write, immData] = sink.describe(_current->range.end, _current->range.end - _current->range.start, _nextSlot);
            sink.post(write, immData);
            posted();
            _slotsBusy.push_back(_postedTotal);
            _nextSlot = (_nextSlot + 1) % _descriptorCount;
            _current.reset();
        }
    }

    void SampleTransferQueue::complete() noexcept
    {
        if (_inFlight > 0)
        {
            --_inFlight;
            ++_completedTotal;
        }
    }

    void SampleTransferQueue::fail() noexcept
    {
        complete();
        if (_current)
        {
            MXL_WARN("A write to a target failed, dropping the audio batch ending at index {}.", _current->range.end);
            _current.reset();
        }
    }

    bool SampleTransferQueue::hasPendingWork() const noexcept
    {
        return _inFlight > 0;
    }

    std::size_t SampleTransferQueue::reset() noexcept
    {
        auto const inFlight = _inFlight;
        _acceptedEnd.reset();
        _pending.reset();
        _current.reset();
        _dropping = false;
        _inFlight = 0;
        _postedTotal = 0;
        _completedTotal = 0;
        _slotsBusy.clear();
        _nextSlot = 0;
        return inFlight;
    }

    void SampleTransferQueue::posted() noexcept
    {
        ++_inFlight;
        ++_postedTotal;
    }

    void SampleTransferQueue::releaseSlots() noexcept
    {
        while (!_slotsBusy.empty() && (_slotsBusy.front() <= _completedTotal))
        {
            _slotsBusy.pop_front();
        }
    }
}
