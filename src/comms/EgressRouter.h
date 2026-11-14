// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * EgressRouter.h — one egress frame to the client, or to the host's log.
 *
 * Everything on the egress is for the one client, so there is nothing to
 * route: a frame is handed to the transport. /clockwork/debug is the one
 * address that is not a message at all: it is a line for the host's log.
 *
 * Both consumers of egress come through here — the engine's own gateway,
 * when it drains, and a host draining the rings itself through the client
 * API — so the rule exists once.
 */
#pragma once

#include "IOscTransport.h"
#include "clockwork_prefix.h"
#include "osc_debug.h"       // kDebugArgOffset

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

// How many the frame was meant for and did not reach: 0 or 1, there being
// one client. The caller counts them — the engine's gateway through
// clockwork_egress_count_undelivered, a host draining egress itself through
// clockwork_client_count_undelivered — so nothing that left an egress ring
// goes missing unsaid. A debug line goes to the host's log, or nowhere when
// the host keeps none, and misses no one either way.
inline uint32_t clockwork_route_egress(IOscTransport& transport, const uint8_t* osc, uint32_t len,
                                       const std::function<void(const std::string&)>& onDebug) {
    if (len >= clockwork::kDebugArgOffset
        && std::memcmp(osc, CLOCKWORK_SYS("debug"), CLOCKWORK_SYS_LEN("debug")) == 0) {
        if (!onDebug) return 0;
        const char* s = reinterpret_cast<const char*>(osc) + clockwork::kDebugArgOffset;
        onDebug(std::string(s, strnlen(s, len - clockwork::kDebugArgOffset)));
        return 0;
    }
    return transport.deliver(osc, len) ? 0 : 1;
}
