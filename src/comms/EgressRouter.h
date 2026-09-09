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

// True when the frame went somewhere: a transport door, or the debug log.
inline bool clockwork_route_egress(IOscTransport& transport, uint32_t origin, uint32_t route,
                                   const uint8_t* osc, uint32_t len,
                                   const std::function<void(const std::string&)>& onDebug) {
    if (len >= clockwork::kDebugArgOffset
        && std::memcmp(osc, CLOCKWORK_SYS("debug"), CLOCKWORK_SYS_LEN("debug")) == 0) {
        if (!onDebug) return false;
        const char* s = reinterpret_cast<const char*>(osc) + clockwork::kDebugArgOffset;
        onDebug(std::string(s, strnlen(s, len - clockwork::kDebugArgOffset)));
        return true;
    }
    switch (route) {
        case EGRESS_REPLY:            return transport.send(origin, osc, len, /*networkOnly*/ false);
        case EGRESS_SEND_TO_CALLER:   return transport.send(origin, osc, len, /*networkOnly*/ true);
        case EGRESS_BROADCAST_LINK:    transport.broadcastLink(osc, len);    return true;
        case EGRESS_BROADCAST_MIDI:    transport.broadcastMidi(osc, len);    return true;
        case EGRESS_BROADCAST_GAMEPAD: transport.broadcastGamepad(osc, len); return true;
        case EGRESS_BROADCAST_OSC:     transport.broadcastOsc(osc, len);     return true;
        case EGRESS_BROADCAST_NOTIFY:
        default:                       transport.broadcastNotify(osc, len);  return true;
    }
}
