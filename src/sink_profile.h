// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * sink_profile.h — the sink size classes, from memory_profile.h's strings to
 * clockwork_sink_profile.
 *
 * "<cell bytes>:<cells>,..." is the whole grammar: decimal, a colon, decimal,
 * commas between. Spaces are allowed around the numbers and nothing else is.
 * A string that does not parse installs nothing, so the crate's own default
 * for the kind stands, and the caller says so in the log.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "clockwork_event_sink.h"
#include "memory_profile.h"

#define CLOCKWORK_SINK_CLASSES_MAX 8u

/* The widest payload a UDP datagram carries. A sink cell may be wider (the
 * profile rounds to 64 KB); the wire is the endpoint's limit, not the sink's. */
#define CLOCKWORK_MAX_DATAGRAM_BYTES 65507u

/* Parse `spec` into parallel arrays of at most `cap` classes. Returns how
 * many, or 0 for an empty or malformed string (nothing partial is reported). */
static inline uint32_t clockwork_parse_sink_classes(const char* spec,
                                                    uint32_t* cell_bytes,
                                                    uint32_t* cells,
                                                    uint32_t cap) {
    if (!spec || !cell_bytes || !cells || cap == 0) return 0;
    uint32_t n = 0;
    const char* p = spec;
    for (;;) {
        while (*p == ' ') ++p;
        if (*p < '0' || *p > '9') return 0;
        uint64_t w = 0;
        while (*p >= '0' && *p <= '9') { w = w * 10 + (uint64_t)(*p - '0'); if (w > 0xFFFFFFFFu) return 0; ++p; }
        while (*p == ' ') ++p;
        if (*p != ':') return 0;
        ++p;
        while (*p == ' ') ++p;
        if (*p < '0' || *p > '9') return 0;
        uint64_t d = 0;
        while (*p >= '0' && *p <= '9') { d = d * 10 + (uint64_t)(*p - '0'); if (d > 0xFFFFFFFFu) return 0; ++p; }
        while (*p == ' ') ++p;
        if (n == cap) return 0;
        cell_bytes[n] = (uint32_t)w;
        cells[n] = (uint32_t)d;
        ++n;
        if (*p == '\0') return n;
        if (*p != ',') return 0;
        ++p;
    }
}

/* Install one kind's classes from its profile string. Returns non-zero when
 * installed; zero for a string that does not parse or a shape the sink
 * substrate refuses, in which case the built-in shape stands. */
static inline int clockwork_install_sink_classes(ClockworkSinkKind kind, const char* spec) {
    uint32_t widths[CLOCKWORK_SINK_CLASSES_MAX];
    uint32_t depths[CLOCKWORK_SINK_CLASSES_MAX];
    const uint32_t n = clockwork_parse_sink_classes(spec, widths, depths,
                                                    CLOCKWORK_SINK_CLASSES_MAX);
    return n != 0 && clockwork_sink_profile(kind, n, widths, depths);
}

/* Both kinds, from memory_profile.h. What the engine does at init, and what a
 * test that opens a sink with no engine does first. */
static inline int clockwork_install_sink_profiles(void) {
    const int osc  = clockwork_install_sink_classes(kClockworkSinkOsc,  CLOCKWORK_OSC_SINK_CLASSES);
    const int midi = clockwork_install_sink_classes(kClockworkSinkMidi, CLOCKWORK_MIDI_SINK_CLASSES);
    return osc && midi;
}
