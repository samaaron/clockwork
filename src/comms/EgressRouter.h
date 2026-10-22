// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * EgressRouter.h — one egress frame to one transport call.
 *
 * An egress frame is [route][osc], stamped with the origin token of the
 * request it answers (clockwork_client_poll peels the route and hands all
 * three over). The route says how it leaves: to its origin, to its origin
 * but only over a wire, or to one of the notify audiences. The transport
 * knows what a token or an audience IS; this knows only which of its doors
 * to use. /clockwork/debug is the one address that is not a message at all:
 * it is a line for the host's log.
 *
 * Both consumers of egress route through here — the engine's own gateway,
 * when it drains, and a host draining the rings itself through the client
 * API — so the table exists once.
 */
#pragma once

#include "IOscTransport.h"
#include "clockwork_prefix.h"
#include "shared_memory.h"   // EgressRoute
#include "osc_debug.h"       // kDebugArgOffset

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

// How many destinations the frame was meant for and did not reach (0 when it
// reached every one): a reply whose origin no peer answers to any more, or the
// members of a notify audience the transport could not hand it to
// (IOscTransport). The caller counts them — the engine's gateway through
// clockwork_egress_count_undelivered, a host draining egress itself through
// clockwork_client_count_undelivered — so nothing that left an egress ring
// goes missing unsaid. A debug line goes to the host's log, or nowhere when
// the host keeps none, and misses no one either way.
inline uint32_t clockwork_route_egress(IOscTransport& transport, uint32_t origin, uint32_t route,
                                       const uint8_t* osc, uint32_t len,
                                       const std::function<void(const std::string&)>& onDebug) {
    if (len >= clockwork::kDebugArgOffset
        && std::memcmp(osc, CLOCKWORK_SYS("debug"), CLOCKWORK_SYS_LEN("debug")) == 0) {
        if (!onDebug) return 0;
        const char* s = reinterpret_cast<const char*>(osc) + clockwork::kDebugArgOffset;
        onDebug(std::string(s, strnlen(s, len - clockwork::kDebugArgOffset)));
        return 0;
    }
    switch (route) {
        case EGRESS_REPLY:             return transport.send(origin, osc, len, /*networkOnly*/ false) ? 0 : 1;
        case EGRESS_SEND_TO_CALLER:    return transport.send(origin, osc, len, /*networkOnly*/ true) ? 0 : 1;
        case EGRESS_BROADCAST_LINK:    return transport.broadcastLink(osc, len);
        case EGRESS_BROADCAST_MIDI:    return transport.broadcastMidi(osc, len);
        case EGRESS_BROADCAST_GAMEPAD: return transport.broadcastGamepad(osc, len);
        case EGRESS_BROADCAST_OSC:     return transport.broadcastOsc(osc, len);
        case EGRESS_BROADCAST_NOTIFY:
        default:                       return transport.broadcastNotify(osc, len);
    }
}
