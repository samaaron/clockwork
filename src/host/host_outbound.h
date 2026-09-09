// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    clockwork
    Copyright (c) 2025 Sam Aaron


    Host outbound backends, registered on the host's ClockworkSysRoutes so a fired
    event routes exactly as it does in the engine — by address, through the same
    dispatcher — with the host's two leaves (OSC send, MIDI send) in place of the
    DSP default. "/clockwork/osc/send <host:s> <port:i> <inner:b>" sends inner to
    host:port; a "/clockwork/midi/..." message goes to the MIDI sender. Anything else
    reaches the fallback below, which reports and drops it.
*/

#pragma once

#include "clockwork_prefix.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "osc_reader.h"

namespace clockwork_host {

// host bytes are NUL-terminated; inner is the OSC payload to send.
using SendOsc  = std::function<void(const char* host, int port,
                                    const uint8_t* inner, uint32_t len)>;
using SendMidi = std::function<void(const uint8_t* inner, uint32_t len)>;

// The host's two delivery leaves, the routeCtx its OscIngress backends carry.
struct HostSenders { SendOsc osc; SendMidi midi; };

// Outbound backend: "/clockwork/osc/send <host> <port> <inner>" → osc leaf.
inline bool hostOscSendRoute(void* routeCtx, const void* /*callCtx*/,
                             const uint8_t* data, std::size_t len) {
    auto* s = static_cast<HostSenders*>(routeCtx);
    OscReader r(data, len);
    if (!r.ok()) return true;
    const char* host; int32_t port; const uint8_t* inner; uint32_t innerLen;
    if (!r.readString(host) || !r.readInt32(port) || !r.readBlob(inner, innerLen))
        return true;
    if (port > 0 && port <= 65535 && innerLen > 0 && s->osc)
        s->osc(host, port, inner, innerLen);
    return true;
}

// Outbound backend: a "/clockwork/midi/..." message → MIDI leaf.
inline bool hostMidiRoute(void* routeCtx, const void* /*callCtx*/,
                          const uint8_t* data, std::size_t len) {
    auto* s = static_cast<HostSenders*>(routeCtx);
    if (s->midi) s->midi(data, static_cast<uint32_t>(len));
    return true;
}

// Route-table default: no backend claims this address (e.g. a DSP verb on a
// host with no DSP) — report and drop, mirroring the engine's no-backend log.
inline bool hostUnroutedRoute(void* /*routeCtx*/, const void* /*callCtx*/,
                              const uint8_t* data, std::size_t len) {
    uint32_t a = 0; while (a < len && data[a] != '\0') ++a;
    std::fprintf(stderr, "clockwork-scheduler: no backend for OSC %.*s — dropped\n",
                 static_cast<int>(a), reinterpret_cast<const char*>(data));
    return true;
}

}  // namespace clockwork_host
