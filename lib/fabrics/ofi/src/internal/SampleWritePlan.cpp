// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#include "SampleWritePlan.hpp"
#include <algorithm>
#include "Exception.hpp"

namespace mxl::lib::fabrics::ofi
{
    namespace
    {
        /** \brief Accumulates pieces into writes that respect the limits.
         */
        class WritePacker
        {
        public:
            WritePacker(LocalRegion const& srcRing, RemoteRegion const& dstRing, WriteLimits const& limits)
                : _srcRing{srcRing}
                , _dstRing{dstRing}
                , _limits{limits}
                , _length{0}
            {}

            /** \brief Copy length bytes from srcOffset in the source ring to dstOffset in the destination ring.
             */
            void append(std::uint64_t srcOffset, std::uint64_t dstOffset, std::size_t length)
            {
                while (length > 0)
                {
                    auto const room = _limits.maxMsgSize - _length;
                    auto const localJoins = !_current.local.empty() &&
                                            ((_current.local.back().addr + _current.local.back().len) == (_srcRing.addr + srcOffset));
                    auto const remoteJoins = !_current.remote.empty() &&
                                             ((_current.remote.back().addr + _current.remote.back().len) == (_dstRing.addr + dstOffset));
                    auto const localFull = !localJoins && (_current.local.size() == _limits.iovLimit);
                    auto const remoteFull = !remoteJoins && (_current.remote.size() == _limits.rmaIovLimit);
                    if ((room == 0) || localFull || remoteFull)
                    {
                        flush();
                        continue;
                    }

                    auto const chunk = std::min(length, room);
                    if (localJoins)
                    {
                        _current.local.back().len += chunk;
                    }
                    else
                    {
                        _current.local.push_back(_srcRing.sub(srcOffset, chunk));
                    }
                    if (remoteJoins)
                    {
                        _current.remote.back().len += chunk;
                    }
                    else
                    {
                        _current.remote.push_back(_dstRing.sub(dstOffset, chunk));
                    }

                    _length += chunk;
                    srcOffset += chunk;
                    dstOffset += chunk;
                    length -= chunk;
                }
            }

            /** \brief Close the write being filled, if it holds anything.
             */
            void flush()
            {
                if (!_current.local.empty())
                {
                    _writes.push_back(std::move(_current));
                    _current = SampleWrite{};
                    _length = 0;
                }
            }

            /** \brief The writes planned so far. Call flush() first.
             */
            [[nodiscard]]
            std::vector<SampleWrite> take() noexcept
            {
                return std::move(_writes);
            }

        private:
            LocalRegion _srcRing;
            RemoteRegion _dstRing;
            WriteLimits _limits;
            std::vector<SampleWrite> _writes;
            SampleWrite _current;
            std::size_t _length;
        };
    }

    std::vector<SampleWrite> planSampleWrites(DataLayout::Continuous const& srcLayout, LocalRegion const& srcRing,
        DataLayout::Continuous const& dstLayout, RemoteRegion const& dstRing, std::uint64_t headIndex, std::size_t count, WriteLimits const& limits)
    {
        if ((srcLayout.sampleSize != dstLayout.sampleSize) || (srcLayout.channelCount != dstLayout.channelCount) || (srcLayout.sampleSize == 0))
        {
            throw Exception::invalidArgument("Source and destination rings differ: sample size {} and {}, channel count {} and {}.",
                srcLayout.sampleSize,
                dstLayout.sampleSize,
                srcLayout.channelCount,
                dstLayout.channelCount);
        }
        if ((srcRing.len < (srcLayout.sampleSize * srcLayout.channelCount * srcLayout.bufferLength)) ||
            (dstRing.len < (dstLayout.sampleSize * dstLayout.channelCount * dstLayout.bufferLength)))
        {
            throw Exception::invalidArgument("A ring region is smaller than its layout.");
        }
        if ((count == 0) || (count > headIndex) || (count > srcLayout.bufferLength) || (count > dstLayout.bufferLength))
        {
            throw Exception::invalidArgument("Cannot plan {} samples ending at index {} for rings of {} and {} samples.",
                count,
                headIndex,
                srcLayout.bufferLength,
                dstLayout.bufferLength);
        }
        if ((limits.iovLimit == 0) || (limits.rmaIovLimit == 0) || (limits.maxMsgSize == 0))
        {
            throw Exception::invalidArgument("Write limits must be greater than 0.");
        }

        auto const sampleSize = srcLayout.sampleSize;
        auto packer = WritePacker{srcRing, dstRing, limits};
        for (auto channel = std::size_t{0}; channel < srcLayout.channelCount; ++channel)
        {
            for (auto index = headIndex - count; index < headIndex;)
            {
                auto const srcPosition = index % srcLayout.bufferLength;
                auto const dstPosition = index % dstLayout.bufferLength;
                auto const run = std::min({headIndex - index, srcLayout.bufferLength - srcPosition, dstLayout.bufferLength - dstPosition});

                packer.append(((channel * srcLayout.bufferLength) + srcPosition) * sampleSize,
                    ((channel * dstLayout.bufferLength) + dstPosition) * sampleSize,
                    run * sampleSize);
                index += run;
            }
        }
        packer.flush();

        return packer.take();
    }
}
