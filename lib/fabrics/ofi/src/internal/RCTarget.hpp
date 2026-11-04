// SPDX-FileCopyrightText: 2026 Contributors to the Media eXchange Layer project.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <variant>
#include "mxl/fabrics.h"
#include "Endpoint.hpp"
#include "PassiveEndpoint.hpp"
#include "Protocol.hpp"
#include "QueueHelpers.hpp"
#include "Target.hpp"

namespace mxl::lib::fabrics::ofi
{
    /** \brief Reliable Connected (RC) Target implementation.
     *
     * An RC Target uses connection-oriented endpoint type. It listens for incoming connection requests from an Initiator to establish connection.
     * Once connected, it polls its completion queues to see if new data can be consumed. be read from the connected endpoint.
     *
     * The passive endpoint listens from setup until the target is destroyed, so the address in the TargetInfo stays valid for the target's
     * whole lifetime. A connection request is accepted in every state. One that arrives while a connection is up or being set up closes that
     * connection, because a peer that went away without a shutdown event leaves a connection that would otherwise never end.
     */
    class RCTarget : public Target
    {
    public:
        /** \brief Set up a fresh RCTarget and its associated TargetInfo based on the given configuration.
         *
         * \param config The configuration to use for setting up the target.
         * \param options Optional tuning parameters (e.g. completion queue depth).
         * \return A pair consisting of the newly setup RCTarget and its associated TargetInfo.
         */
        [[nodiscard]]
        static std::pair<std::unique_ptr<RCTarget>, std::unique_ptr<TargetInfo>> setup(mxlFabricsTargetConfig const& config, FabricInfoView info,
            TargetSetupOptions const& options = {});

        virtual std::optional<Target::ReadResult> read();
        virtual std::optional<Target::ReadResult> readBlocking(std::chrono::steady_clock::duration timeout) final;

        /** \brief Shut down the target.
         */
        virtual void shutdown() final;

    private:
        /** \brief The wait for connection request state.
         *
         * The RCTarget has no connection, and is waiting for an incoming connection request.
         */
        struct WaitForConnectionRequest
        {};

        /** \brief The wait for connection state.
         *
         * The RCTarget has accepted a connection request, and is waiting for the connected event.
         */
        struct WaitForConnection
        {
            Endpoint ep;
        };

        /** \brief The connected state.
         *
         * The RCTarget is now connected to an Initiator, and can receive data.
         */
        struct Connected
        {
            Endpoint ep;
        };

        /** \brief The internal state of the RCTarget.
         */
        using State = std::variant<WaitForConnectionRequest, WaitForConnection, Connected>;

    private:
        /** \brief Construct a new RCTarget associated with the given domain and passive endpoint.
         *
         * \param domain The domain to create the RCTarget on.
         * \param pep The passive endpoint to use for listening for incoming connection requests.
         * \param options The setup options applied when the completion queue is created for the accepted connection.
         */
        RCTarget(PassiveEndpoint pep, std::unique_ptr<IngressProtocol> proto, std::shared_ptr<Domain> domain, TargetSetupOptions options);

        /** \brief Internal method to drive progress based on the current state.
         *
         * \param timeout The maximum duration to block waiting for progress.
         * \return The result of the read operation.
         */
        template<QueueReadMode>
        std::optional<Target::ReadResult> readNext(std::chrono::steady_clock::duration timeout);

        /** \brief Accept a connection request on a new endpoint.
         *
         * \param request The connection request event read from the passive endpoint's event queue.
         * \return WaitForConnection holding the new endpoint, or WaitForConnectionRequest if the provider failed to accept it.
         */
        [[nodiscard]]
        State accept(Event const& request);

        [[nodiscard]]
        static PassiveEndpoint makeListener(std::shared_ptr<Fabric> const& fabric);

    private:
        std::unique_ptr<IngressProtocol> _proto;
        std::shared_ptr<Domain> _domain;
        TargetSetupOptions _setupOptions; /**< Setup options applied when accepting connections. */
        PassiveEndpoint _pep;             /**< Listens on the published address for the lifetime of the target. */
        State _state;                     /**< The current state of the RCTarget. */
    };
}
