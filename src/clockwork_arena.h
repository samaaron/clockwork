// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_arena.h — the arena's table of contents, as every reader sees it.
 *
 * The arena is the one span of memory an engine and its observers share: the
 * static buffer in a wasm heap, a mapped segment between processes, a block
 * of process memory under an embedded engine. Its FIRST BYTES are this
 * header: a magic word, a version, and a table naming every region the arena
 * holds — where it is, how big it is, who writes it, and the geometry a reader
 * needs to walk it (a ring's frame magic, a slot array's stride). A reader
 * with nothing but the base pointer finds everything from here, and a reader
 * built from another memory profile, another language, or no engine tree at
 * all finds it where THIS engine put it.
 *
 * There used to be three of these: compile-time constants for in-process
 * code, a positional struct exported to JavaScript, and a segment header for
 * out-of-process peers. Each drifted from the others in its own way. Now the
 * constants lay the arena out (shared_memory.h) and write this table once at
 * boot, and every reader — the C++ client, the Rust bindings, the worklet —
 * reads the table. The constants are the engine's; the table is the contract.
 *
 * ── The two halves ────────────────────────────────────────────────────────
 * The arena is two regions with two owners, and the header says where the
 * line is:
 *
 *   [0, block_bytes)                     THE CLOCKWORK BLOCK. Everything
 *                                        clockwork keeps for the hosts and
 *                                        clients of an engine: the control
 *                                        block, metrics, the clocks, the
 *                                        channel map, the three rings, the
 *                                        audio taps, the client slots. The
 *                                        guest is never handed a pointer into
 *                                        it that it may write through.
 *   [guest_offset, guest_offset+bytes)   THE GUEST REGION. What the guest
 *                                        is given base and length to: its
 *                                        config, its publish window, its
 *                                        scope slots, its persistent bytes.
 *                                        Clockwork lays it out and never
 *                                        reads a byte of what the guest puts
 *                                        there.
 *
 * ── Owners ────────────────────────────────────────────────────────────────
 * Every entry names who writes it. That is the fence: the guest is in-process
 * and on the audio thread, a SharedArrayBuffer has no page protection, so
 * ownership is a matter of which pointers are handed out and what the table
 * says. A tool that colours a memory map by owner reads it from here.
 *
 * ── Lifetime ──────────────────────────────────────────────────────────────
 * Written by whoever creates the arena, before anything else is placed in it,
 * with `state` stored last (release). A reader checks magic and version,
 * acquires, and only then trusts an offset. Offsets never move for the life of
 * an arena: a device restart rebuilds the guest, not the map, so a reader that
 * resolved a region at open holds it until close.
 *
 * ── Versioning ────────────────────────────────────────────────────────────
 * VERSION changes when the header or entry shape changes, or when a region's
 * meaning does. Adding a region does not: a reader looks entries up by id and
 * ignores ids it does not know. A reader refuses a version it was not written
 * for rather than reading through offsets that may mean something else.
 *
 * Pure C: no C++, no dependencies beyond <stdint.h>. Included by
 * shared_memory.h, mirrored by rust/clockwork-abi and js/lib/arena.js, and
 * held to both by a compiled probe.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCKWORK_ARENA_MAGIC        0x43574152u   /* 'CWAR' */
#define CLOCKWORK_ARENA_VERSION      1u
/* The header's reservation at the front of the arena. Larger than the struct
 * so entries can be added without moving the first region. */
#define CLOCKWORK_ARENA_HEADER_BYTES 4096u
#define CLOCKWORK_ARENA_MAX_ENTRIES  48u
#define CLOCKWORK_ARENA_GEOM_WORDS   12u

/* `state` */
#define CLOCKWORK_ARENA_LAYING_OUT   0u
#define CLOCKWORK_ARENA_PUBLISHED    1u

/* Who writes a region. */
enum ClockworkArenaOwner {
    CLOCKWORK_OWNER_CLOCKWORK = 1,   /* the engine, on the audio or a control thread */
    CLOCKWORK_OWNER_GUEST     = 2,   /* the DSP, through the pointers it was handed */
    CLOCKWORK_OWNER_CLIENT    = 3,   /* a client, through the client ABI */
    CLOCKWORK_OWNER_HOST      = 4,   /* the host, once, at boot or device start */
};

/* The regions. Stable numbers: a reader looks these up, never a position.
 * The scope-stream slots a client numbers are ONE index space across two of
 * them: the guest's SCOPE region carries indices [0, slots), and the
 * engine's TRACK_TAPS region carries [first_index, first_index + slots) —
 * the same slot shape, a different owner, a different half of the arena. */
enum ClockworkArenaRegion {
    /* ── the clockwork block ── */
    CLOCKWORK_ARENA_CONTROL         = 1,   /* ControlPointers: ring cursors and flags */
    CLOCKWORK_ARENA_METRICS         = 2,   /* PerformanceMetrics: u32 counters */
    CLOCKWORK_ARENA_NATIVE_STATS    = 3,   /* NATIVE_STAT_*: DSP load, overruns, NRT timings */
    CLOCKWORK_ARENA_CLOCK_STATE     = 4,   /* ClockworkClockState */
    CLOCKWORK_ARENA_CLOCK_ANCHORS   = 5,   /* the host's anchors: NTP start (f64), drift, global offset */
    CLOCKWORK_ARENA_SAMPLE_CLOCK    = 6,   /* sample position <-> DAC time, seqlocked */
    CLOCKWORK_ARENA_CHANNEL_MAP     = 7,   /* ClockworkChannelMapState */
    CLOCKWORK_ARENA_NODE_ID_COUNTER = 8,   /* one atomic u32 */
    CLOCKWORK_ARENA_IN_RING         = 9,   /* OSC into the engine */
    CLOCKWORK_ARENA_OUT_RING        = 10,  /* RT egress, written inside the tick */
    CLOCKWORK_ARENA_NRT_OUT_RING    = 11,  /* control-thread egress */
    CLOCKWORK_ARENA_AUDIO_TAPS      = 12,  /* shm_audio_buffer slots: OUT then IN, written in the tick */
    CLOCKWORK_ARENA_CLIENT_SLOTS    = 13,  /* a co-resident client's handle and stack */
    CLOCKWORK_ARENA_TRACK_TAPS      = 18,  /* scope-stream slots the engine writes from plugin-track
                                              returns; numbered after the guest's scope slots */
    /* ── the guest region ── */
    CLOCKWORK_ARENA_GUEST_CONFIG    = 14,  /* host -> guest, copied at boot */
    CLOCKWORK_ARENA_GUEST_WINDOW    = 15,  /* DspConfig::shm_window */
    CLOCKWORK_ARENA_SCOPE           = 16,  /* scope streams: global header, then slots */
    CLOCKWORK_ARENA_GUEST_PERSIST   = 17,  /* DspConfig::persistent */
};

/* The two audio taps, as slot indices into AUDIO_TAPS: what left for the
 * device this block, and what arrived from it. Clockwork writes both inside
 * every tick, on every host, at the device's live channel count up to the
 * slot's ceiling (CLOCKWORK_GEOM_TAPS_CHANNELS). */
#define CLOCKWORK_TAP_OUT 0u
#define CLOCKWORK_TAP_IN  1u

/* What the geometry words of an entry mean, per region. Regions not listed
 * carry none. */
enum ClockworkArenaGeom {
    /* rings */
    CLOCKWORK_GEOM_RING_MAX_MESSAGE    = 0,
    CLOCKWORK_GEOM_RING_MESSAGE_MAGIC  = 1,
    CLOCKWORK_GEOM_RING_PADDING_MAGIC  = 2,
    CLOCKWORK_GEOM_RING_PADDING_MARKER = 3,
    CLOCKWORK_GEOM_RING_HEADER_BYTES   = 4,
    /* metrics */
    CLOCKWORK_GEOM_METRICS_FIELDS      = 0,
    /* clock anchors: byte offsets within the region */
    CLOCKWORK_GEOM_ANCHOR_NTP_START    = 0,   /* f64 */
    CLOCKWORK_GEOM_ANCHOR_DRIFT        = 1,   /* i32, microseconds */
    CLOCKWORK_GEOM_ANCHOR_GLOBAL       = 2,   /* i32, milliseconds */
    /* audio taps */
    CLOCKWORK_GEOM_TAPS_SLOTS          = 0,
    CLOCKWORK_GEOM_TAPS_SLOT_BYTES     = 1,
    CLOCKWORK_GEOM_TAPS_HEADER_BYTES   = 2,
    CLOCKWORK_GEOM_TAPS_FRAMES         = 3,
    CLOCKWORK_GEOM_TAPS_CHANNELS       = 4,
    CLOCKWORK_GEOM_TAPS_SAMPLE_RATE    = 5,
    /* scope streams */
    CLOCKWORK_GEOM_SCOPE_SLOTS         = 0,
    CLOCKWORK_GEOM_SCOPE_HEADER_BYTES  = 1,
    CLOCKWORK_GEOM_SCOPE_SLOT_BYTES    = 2,
    CLOCKWORK_GEOM_SCOPE_SLOT_HEADER   = 3,
    CLOCKWORK_GEOM_SCOPE_RING_FRAMES   = 4,
    CLOCKWORK_GEOM_SCOPE_CHANNELS      = 5,
    /* track taps: scope-stream slots, same shape as the guest's */
    CLOCKWORK_GEOM_TRACK_SLOTS         = 0,
    CLOCKWORK_GEOM_TRACK_SLOT_BYTES    = 1,
    CLOCKWORK_GEOM_TRACK_SLOT_HEADER   = 2,
    CLOCKWORK_GEOM_TRACK_RING_FRAMES   = 3,
    CLOCKWORK_GEOM_TRACK_CHANNELS      = 4,
    CLOCKWORK_GEOM_TRACK_FIRST_INDEX   = 5,   /* the slot index the region begins at */
    /* client slots */
    CLOCKWORK_GEOM_SLOTS_COUNT         = 0,
    CLOCKWORK_GEOM_SLOTS_SLOT_BYTES    = 1,
    CLOCKWORK_GEOM_SLOTS_HEADER_BYTES  = 2,
    CLOCKWORK_GEOM_SLOTS_STRUCTS_OFF   = 3,
    CLOCKWORK_GEOM_SLOTS_STRUCTS_BYTES = 4,
    CLOCKWORK_GEOM_SLOTS_STACK_OFF     = 5,
    CLOCKWORK_GEOM_SLOTS_STACK_BYTES   = 6,
};

/* One region. 64 bytes. */
typedef struct ClockworkArenaEntry {
    uint32_t id;       /* ClockworkArenaRegion; 0 marks an unused entry */
    uint32_t offset;   /* from the arena base */
    uint32_t bytes;
    uint32_t owner;    /* ClockworkArenaOwner */
    uint32_t geom[CLOCKWORK_ARENA_GEOM_WORDS];
} ClockworkArenaEntry;

/* The header, at arena offset 0. */
typedef struct ClockworkArenaHeader {
    uint32_t magic;          /* CLOCKWORK_ARENA_MAGIC */
    uint32_t version;        /* CLOCKWORK_ARENA_VERSION */
    uint32_t header_bytes;   /* CLOCKWORK_ARENA_HEADER_BYTES: the first region starts here */
    uint32_t instance_id;    /* which engine, when a process holds more than one; 0 today */
    uint32_t arena_bytes;    /* the whole arena */
    uint32_t block_bytes;    /* the clockwork block: [0, block_bytes) */
    uint32_t guest_offset;   /* the guest region */
    uint32_t guest_bytes;
    uint32_t entry_count;
    uint32_t entry_bytes;    /* sizeof(ClockworkArenaEntry) */
    uint32_t state;          /* CLOCKWORK_ARENA_PUBLISHED once the table may be trusted */
    uint32_t reserved[5];
    ClockworkArenaEntry entries[CLOCKWORK_ARENA_MAX_ENTRIES];
} ClockworkArenaHeader;

/* The entry for a region, or NULL if the arena has none. */
static inline const ClockworkArenaEntry*
clockwork_arena_find(const ClockworkArenaHeader* h, uint32_t id) {
    uint32_t i;
    if (!h) return (const ClockworkArenaEntry*)0;
    for (i = 0; i < h->entry_count && i < CLOCKWORK_ARENA_MAX_ENTRIES; ++i)
        if (h->entries[i].id == id) return &h->entries[i];
    return (const ClockworkArenaEntry*)0;
}

/* True when the first `bytes` of `base` carry a header this reader can use:
 * the magic, a version it knows, a table that fits, and every entry inside
 * the arena it describes. `why` names the first failure. Does not acquire. */
static inline int clockwork_arena_check(const ClockworkArenaHeader* h, uint32_t bytes,
                                        const char** why) {
    uint32_t i;
#define CLOCKWORK_ARENA_FAIL(w) do { if (why) *why = (w); return 0; } while (0)
    if (!h || bytes < sizeof(ClockworkArenaHeader))  CLOCKWORK_ARENA_FAIL("no room for an arena header");
    if (h->magic != CLOCKWORK_ARENA_MAGIC)           CLOCKWORK_ARENA_FAIL("not a clockwork arena");
    if (h->version != CLOCKWORK_ARENA_VERSION)       CLOCKWORK_ARENA_FAIL("arena version this reader does not know");
    if (h->state != CLOCKWORK_ARENA_PUBLISHED)       CLOCKWORK_ARENA_FAIL("arena not yet published");
    if (h->header_bytes < sizeof(ClockworkArenaHeader)) CLOCKWORK_ARENA_FAIL("header reservation smaller than the header");
    if (h->entry_bytes != sizeof(ClockworkArenaEntry)) CLOCKWORK_ARENA_FAIL("entry shape this reader does not know");
    if (h->entry_count > CLOCKWORK_ARENA_MAX_ENTRIES) CLOCKWORK_ARENA_FAIL("more entries than the table holds");
    if (h->arena_bytes > bytes)                      CLOCKWORK_ARENA_FAIL("arena larger than the memory it is in");
    if (h->block_bytes > h->arena_bytes)             CLOCKWORK_ARENA_FAIL("clockwork block larger than the arena");
    if (h->guest_offset > h->arena_bytes || h->guest_bytes > h->arena_bytes - h->guest_offset)
        CLOCKWORK_ARENA_FAIL("guest region outside the arena");
    for (i = 0; i < h->entry_count; ++i) {
        const ClockworkArenaEntry* e = &h->entries[i];
        if (e->id == 0) CLOCKWORK_ARENA_FAIL("an empty entry inside the count");
        if (e->offset < h->header_bytes) CLOCKWORK_ARENA_FAIL("a region overlaps the header");
        if (e->offset > h->arena_bytes || e->bytes > h->arena_bytes - e->offset)
            CLOCKWORK_ARENA_FAIL("a region runs past the arena");
    }
#undef CLOCKWORK_ARENA_FAIL
    return 1;
}

#ifdef __cplusplus
}
#endif
