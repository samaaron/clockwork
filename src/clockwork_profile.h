// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_profile.h — the sizes a core's arena is laid out from
 * (docs/ENGINES.md): the three rings, the scope, the taps, the
 * guest's window and persistent bytes. Every other region is a fixed shape.
 *
 * A core is laid out from the target's own sizes unless its host names a
 * profile before the core's first build: clockwork_set_profile (lanes.h),
 * or ClockworkEmbedConfig::profile (clockwork_embed.h). Clients need none of
 * this: they read every region's place and size from the arena's table.
 *
 * Pure C.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkProfile {
    uint32_t struct_bytes;         /* sizeof(ClockworkProfile) */
    uint32_t in_ring_bytes;        /* messages to the engine; bulk payloads ride it */
    uint32_t rt_out_ring_bytes;    /* the audio thread's replies */
    uint32_t nrt_out_ring_bytes;   /* every other thread's replies, notifications, log */
    uint32_t scope_frames;         /* per scope slot, out and in */
    uint32_t scope_channels;       /* the widest a scope slot carries */
    uint32_t tap_slots;            /* the guest's tap streams */
    uint32_t track_tap_slots;      /* the engine's, numbered after the guest's */
    uint32_t tap_ring_frames;      /* per tap stream */
    uint32_t window_bytes;         /* the guest's window */
    uint32_t persist_bytes;        /* the guest's persistent bytes */
} ClockworkProfile;

/* This target's own sizes, to start a profile from. The caller sets
 * `out->struct_bytes`; nothing is written when it is short. */
void clockwork_profile_default(ClockworkProfile* out);

/* A plugin's: a host that opens an engine per track, many to a process.
 * Small rings, a stereo scope and a few short taps, no track taps; the
 * guest's window and persistent bytes as the target's. As
 * clockwork_profile_default for `out`. */
void clockwork_profile_plugin(ClockworkProfile* out);

/* NULL when an arena can be laid out from `profile`; else why not, as text
 * for a person (a ring under 4 KB, a scope or tap ring shorter than the
 * largest block, more tap slots than an arena holds, ...). Any thread. */
const char* clockwork_profile_check(const ClockworkProfile* profile);

#ifdef __cplusplus
}
#endif
