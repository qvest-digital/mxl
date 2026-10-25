// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include "mxl/fabrics.h"
#include "Address.hpp"
#include "Endpoint.hpp"
#include "RemoteRegion.hpp"

namespace mxl::lib::fabrics::ofi
{

    /** \brief Information about the bounce buffer on the target, if the target uses one.
     */
    struct TargetInfoBounceBufferInfo
    {
        std::size_t entryCount; /**< Number of entries in the bounce buffer. */
        std::size_t entrySize;  /**< Size of each bounce buffer entry in bytes. */
    };

    /** \brief Layout of a target's audio ring, published so an initiator can write samples straight into it.
     *
     * The ring is channel-major with bufferLength samples per channel, and the descriptor region holds descriptorCount AudioEntryHeader
     * descriptor slots. An initiator writes a batch of samples into the ring, then writes {headIndex, count} into a slot with immediate data
     * built by ImmDataSample.
     *
     * Both regions are carried here and not in TargetInfo::remoteRegions. The same target publishes its bounce buffer for initiators that
     * predate this layout, and those require remoteRegions to hold exactly one region per bounce buffer entry.
     */
    struct TargetInfoContinuous
    {
        /** \brief The version of the audio write protocol described here. An initiator refuses a target with another version. */
        constexpr static std::uint32_t ProtocolVersion = 2;

        std::uint32_t version;       /**< Version of the audio write protocol the target speaks. */
        std::size_t sampleSize;      /**< Size of one sample in bytes. */
        std::size_t channelCount;    /**< Number of channels. */
        std::size_t bufferLength;    /**< Samples per channel in the ring. */
        std::size_t descriptorCount; /**< Number of descriptor slots. */
        RemoteRegion ring;           /**< The target's ring. */
        RemoteRegion descriptors;    /**< The target's descriptor slots. */

        bool operator==(TargetInfoContinuous const&) const noexcept = default;
    };

    /** \brief TargetInfo contains all the information required by an initiator to operate transfers to the given target.
     *
     * In the context of libfabric this means, the fi_addr, all the buffer addresses and sizes, and the remote protection key.
     */
    struct TargetInfo
    {
    public:
        /** \brief Cast an `mxlFabricsTargetInfo` opaque pointer to a TargetInfo pointer.
         */
        [[nodiscard]]
        static TargetInfo* fromAPI(mxlFabricsTargetInfo api) noexcept;

        /** \brief Cast a pointer to a `TargetInfo` instance to an `mxlFabricsTargetInfo` opaque pointer
         */
        [[nodiscard]]
        ::mxlFabricsTargetInfo toAPI() noexcept;

        /** \brief Serialize a TargetInfo instance to a JSON representation
         */
        [[nodiscard]]
        std::string toJSON() const;

        /** \brief Construct a TargetInfo instance from a JSON representation
         * \throws Exception::invalidArgument if the JSON is malformed or describes an audio layout of a protocol version other than
         * TargetInfoContinuous::ProtocolVersion.
         */
        [[nodiscard]]
        static TargetInfo fromJSON(std::string const& s);

        bool operator==(TargetInfo const& other) const noexcept;

    public:
        Endpoint::Id id;                /**< A unique identifier of the target's endpoint */
        RawFabricAddress fabricAddress; /**< Target's endpoint libfabric address */
        Provider provider;              /**< Remote targets provider */
        std::vector<RemoteRegion>
            remoteRegions; /**< Target's memory regions (and keys) which an initiator can operate on. This is used only for RMA operations */
        std::optional<TargetInfoBounceBufferInfo> bounceBufferInfo; /**< Information about the bounce buffer on the target, if the target uses one.
                                                                       A target that receives audio publishes it for initiators that predate
                                                                       TargetInfoContinuous. */
        std::optional<TargetInfoContinuous> continuous;             /**< Layout of the target's audio ring, if the target receives audio. */
    };
}
