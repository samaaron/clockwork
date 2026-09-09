// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * osc_debug.h — the one definition of the `/clockwork/debug <text>` OSC packet.
 *
 * Debug log lines ride the egress rings as an ordinary OSC message; the host
 * dispatches the `/clockwork/debug` address to its debug channel. Both writers
 * (the audio thread's emit_debug_osc → OUT ring, and OscEgress::debug → NRT-out
 * ring) build the packet here, and both readers extract the string at the same
 * offset — so the format lives in exactly one place.
 */
#pragma once

#include "clockwork_prefix.h"
#include <cstdint>
#include <cstring>

namespace clockwork {

// Offset of the string argument: the padded address + the type tag ",s\0\0".
// DERIVED from the address, not counted by hand — the whole point of putting
// the packet in one file is that a change of prefix cannot desynchronise the
// builder below from the readers that use this offset.
inline constexpr uint32_t kDebugArgOffset = CLOCKWORK_SYS_PADDED("debug") + 4u;

// Build "/clockwork/debug <text>" into `pkt` (must hold >= 1024 bytes; `len` is
// clamped to 960 to fit). Returns the packet size in bytes.
inline uint32_t buildDebugOsc(char* pkt, const char* text, uint32_t len) {
    if (len > 960) len = 960;
    uint32_t p = 0;
    auto pad4 = [&]() { while (p & 3u) pkt[p++] = '\0'; };
    static const char kAddr[] = CLOCKWORK_SYS("debug");
    std::memcpy(pkt + p, kAddr, sizeof(kAddr)); p += sizeof(kAddr); pad4();  // incl. NUL
    pkt[p++] = ','; pkt[p++] = 's'; pkt[p++] = '\0'; pad4();                  // type tag ",s"
    std::memcpy(pkt + p, text, len); p += len; pkt[p++] = '\0'; pad4();       // string arg
    return p;
}

}  // namespace clockwork
