// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_arena.h — the arena's table of contents, as every reader sees it.
 *
 * The arena is the one span of memory an engine and its observers share: the
 * static buffer in a wasm heap, a mapped segment between processes, a block
 * of process memory under an embedded engine. Its FIRST BYTES are this
 * header: a magic word, a version, a table of AREAS and a table of REGIONS.
 * A reader with nothing but the base pointer finds everything from here,
 * and a reader built from another memory profile, another language, or no
 * engine tree at all finds it where THIS engine put it.
 *
 * The constants in shared_memory.h lay the arena out and write this table
 * once at boot; every reader — the C++ client, the Rust bindings, the
 * worklet, a memory map — reads the table. The constants are the engine's;
 * the table is the contract. docs/MEMORY.md is the model; docs/ARENA.md is
 * this table's contract in prose.
 *
 * ── Areas ─────────────────────────────────────────────────────────────────
 * All of an engine's memory is twelve areas (ClockworkArenaArea). Five lie
 * inside the arena, in this order:
 *
 *   clockwork-state      metrics, stats, the clocks, the channel map
 *   clockwork-audio      the scope and the taps
 *   clockwork-transport  the ring cursors, the node id counter, the rings
 *   guest-public         the guest's window
 *   guest-private        the guest's config and persistent store
 *
 * The first three are CLOCKWORK'S HALF: formats clockwork defines, the same
 * under every guest. The last two are THE GUEST'S HALF: space clockwork
 * reserves and the guest fills. Three more areas lie outside the arena and
 * are named here so a reader finds all of memory from one place:
 *
 *   assets        bulk the client hands the guest, read in place (samples)
 *   returns       bulk the guest hands the client
 *   heap          what clockwork and the guest allocate at run time
 *   guest-memory  the guest's working memory (DspConfig::memory)
 *
 * and three the program's own, placed where the host can say (the web can;
 * a native host says UNKNOWN until it is taught): its static data (the arena
 * lies inside it), its stack, and what its code has taken with malloc and
 * new (the placement arena, the heap and the guest's memory lie inside that).
 *
 * An area in the arena is an offset from the arena base. One outside it is
 * placed in a SPACE (ClockworkArenaSpace): the segment, or the engine's own
 * process; a reader that is not in that space cannot reach it and says so.
 * (A fourth outside area, the guest's working memory, is listed so a map can
 * draw it; no client reads it.)
 *
 * ── Regions, and the five facts ──────────────────────────────────────────
 * Every region in the arena has an entry, and every entry says:
 *
 *   owner     who writes it                 (ClockworkArenaOwner)
 *   audience  who reads it                  (ClockworkArenaAudience, a bit set)
 *   cadence   how often the engine touches  (ClockworkArenaCadence)
 *   tier      the RAM speed it needs         (ClockworkArenaTier)
 *   lifetime  when its bytes are reset       (ClockworkArenaLifetime)
 *
 * plus the geometry a reader walks it by (a ring's frame magic, a slot
 * array's stride). A region that is PUBLIC is a data contract, read by hand
 * by any reader in a client process; one that is TRANSPORT is touched only
 * through the client code; the rest is the guest's, or the host's. Tier
 * follows cadence: what the audio thread touches every block wants fast RAM,
 * and on a tiered device (memory_profile.h) that is where it is placed.
 *
 * The guest is in-process and on the audio thread, and a SharedArrayBuffer
 * has no page protection, so none of this is enforced by hardware: it is
 * enforced by which pointers are handed out, and stated here so a tool and
 * a reviewer can check it.
 *
 * ── Lifetime ──────────────────────────────────────────────────────────────
 * Written by whoever creates the arena, before anything else is placed in
 * it, with `state` stored last (release). A reader checks magic and version,
 * acquires, and only then trusts an offset. Offsets never move for the life
 * of an arena: a device restart rebuilds the guest, not the map, so a reader
 * that resolved a region at open holds it until close. Two things are
 * written after publication, each with its own release: the guest window's
 * identity, when the guest binds, and the outside areas, when the
 * engine learns where they are.
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
#define CLOCKWORK_ARENA_VERSION      2u
#define CLOCKWORK_ARENA_HEADER_BYTES 4096u         /* reserved at the front; the first region starts here */
#define CLOCKWORK_ARENA_MAX_AREAS    16u
#define CLOCKWORK_ARENA_MAX_ENTRIES  48u
#define CLOCKWORK_ARENA_GEOM_WORDS   8u

/* `state` */
#define CLOCKWORK_ARENA_LAYING_OUT   0u
#define CLOCKWORK_ARENA_PUBLISHED    1u

/* The areas. */
enum ClockworkArenaArea {
    CLOCKWORK_AREA_CLOCKWORK_STATE     = 1,   /* public state any client reads */
    CLOCKWORK_AREA_CLOCKWORK_AUDIO     = 2,   /* audio streams any client reads */
    CLOCKWORK_AREA_CLOCKWORK_TRANSPORT = 3,   /* messages, through the client code only */
    CLOCKWORK_AREA_GUEST_PUBLIC        = 4,   /* the guest's window, in the guest's format */
    CLOCKWORK_AREA_GUEST_PRIVATE       = 5,   /* the guest's config and persistent store */
    CLOCKWORK_AREA_ASSETS              = 6,   /* outside the arena: client -> guest bulk, read in place */
    CLOCKWORK_AREA_RETURNS             = 7,   /* outside the arena: guest -> client bulk */
    CLOCKWORK_AREA_GUEST_MEMORY_BULK   = 8,   /* outside the arena: the guest's bulk memory (DspConfig::memory_bulk),
                                                 the span clockwork's allocator runs over for it */
    CLOCKWORK_AREA_GUEST_MEMORY_FAST   = 9,   /* outside the arena: the guest's fast memory (DspConfig::memory),
                                                 laid out by the guest by hand */
    CLOCKWORK_AREA_STATIC_DATA         = 10,  /* the program's static data: its globals. Natively the arena lies
                                                 inside it; on the web the arena is in the engine's memory (15) */
    CLOCKWORK_AREA_STACK               = 11,  /* the engine thread's call stack: on the web, the audio worklet's */
    CLOCKWORK_AREA_MALLOC              = 12,  /* what the program's code has taken with malloc and new, to the
                                                 frontier at boot; on the web the engine's memory lies inside it */
    CLOCKWORK_AREA_RUST_HEAP           = 13,  /* outside the arena: what clockwork's Rust code allocates inside an
                                                 engine entry point (a sink's or a port's queues); one block, for
                                                 the life of the process, that the RT router serves */
    CLOCKWORK_AREA_ENGINE_MEMORY       = 14,  /* the engine's memory: one block that the guest's two spans are
                                                 carved from. On the web it holds the arena at its start and the
                                                 rust heap too, and lies inside what malloc took; a start-over
                                                 for a new engine carves it again in the same order from the same
                                                 block, so nothing has to be remembered. Natively the arena and
                                                 the rust heap are outside it */
};

/* Where an area's base is measured from. */
enum ClockworkArenaSpace {
    CLOCKWORK_SPACE_UNKNOWN = 0,   /* not stated: the engine has not placed it, or this build has none */
    CLOCKWORK_SPACE_ARENA   = 1,   /* an offset from the arena base */
    CLOCKWORK_SPACE_SEGMENT = 2,   /* an offset from the shared segment's base */
    CLOCKWORK_SPACE_PROCESS = 3,   /* an address in the engine's process (on the web, in wasm memory) */
};

/* Who writes a region. */
enum ClockworkArenaOwner {
    CLOCKWORK_OWNER_CLOCKWORK = 1,   /* the engine, on the audio or a control thread */
    CLOCKWORK_OWNER_GUEST     = 2,   /* the DSP, through the pointers it was handed */
    CLOCKWORK_OWNER_CLIENT    = 3,   /* a client, through the client code */
    CLOCKWORK_OWNER_HOST      = 4,   /* the host, once, at boot or device start */
};

/* Who READS a region: a bit set. 0 is nobody but the owner. */
enum ClockworkArenaAudience {
    CLOCKWORK_AUDIENCE_PUBLIC    = 1,   /* a data contract for any reader in a client process */
    CLOCKWORK_AUDIENCE_GUEST     = 2,   /* the guest, through a pointer clockwork hands it */
    CLOCKWORK_AUDIENCE_HOST      = 4,   /* the host */
    CLOCKWORK_AUDIENCE_TRANSPORT = 8,   /* the client code only, never by hand */
};

/* How often the engine thread touches a region. */
enum ClockworkArenaCadence {
    CLOCKWORK_CADENCE_BLOCK  = 1,   /* every block, inside the tick */
    CLOCKWORK_CADENCE_CHANGE = 2,   /* when something changes: a message, a device, a node */
    CLOCKWORK_CADENCE_BOOT   = 3,   /* at boot, then not at all */
};

/* The RAM speed a region needs. One tier on desktop, web and in a DAW; two
 * on a device with internal SRAM and PSRAM (memory_profile.h). */
enum ClockworkArenaTier {
    CLOCKWORK_TIER_FAST = 1,
    CLOCKWORK_TIER_BULK = 2,
};

/* When a region's bytes are reset. */
enum ClockworkArenaLifetime {
    CLOCKWORK_LIFETIME_ARENA     = 1,   /* never: they live as long as the arena */
    CLOCKWORK_LIFETIME_COLD_BOOT = 2,   /* at a cold boot only; a rebuild keeps them */
    CLOCKWORK_LIFETIME_REBUILD   = 3,   /* at every build of the engine: a device change too */
};

/* The regions. Stable numbers — a region keeps its number for good, so one
 * added later takes the next number whatever its place in memory — each
 * named for its half. A reader looks these up, never a position. */
enum ClockworkArenaRegion {
    /* ── clockwork-state ── */
    CLOCKWORK_ARENA_CLOCKWORK_METRICS         = 1,   /* PerformanceMetrics: u32 counters */
    CLOCKWORK_ARENA_CLOCKWORK_NATIVE_STATS    = 2,   /* NATIVE_STAT_*: DSP load, overruns, NRT timings */
    CLOCKWORK_ARENA_CLOCKWORK_CLOCK_ANCHORS   = 3,   /* the host's anchor: the global clock offset */
    CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE     = 4,   /* ClockworkClockState */
    CLOCKWORK_ARENA_CLOCKWORK_SAMPLE_CLOCK    = 5,   /* sample position <-> DAC time, seqlocked */
    CLOCKWORK_ARENA_CLOCKWORK_CHANNEL_MAP     = 6,   /* ClockworkChannelMapState */
    /* ── clockwork-audio ── */
    CLOCKWORK_ARENA_CLOCKWORK_SCOPE           = 7,   /* scope slots: device OUT then IN, written in the tick */
    CLOCKWORK_ARENA_CLOCKWORK_TAPS            = 8,   /* tap streams: a header, the guest's slots, then the
                                                        track slots, one index space (geometry below) */
    /* ── clockwork-transport ── */
    CLOCKWORK_ARENA_CLOCKWORK_RING_CURSORS    = 9,   /* RingCursors: each ring's cursors, the ingress lock, flags */
    CLOCKWORK_ARENA_CLOCKWORK_NODE_ID_COUNTER = 10,  /* one atomic u32 the clients share */
    CLOCKWORK_ARENA_CLOCKWORK_IN_RING         = 11,  /* OSC into the engine */
    CLOCKWORK_ARENA_CLOCKWORK_RT_OUT_RING     = 12,  /* RT egress, written inside the tick */
    CLOCKWORK_ARENA_CLOCKWORK_NRT_OUT_RING    = 13,  /* control-thread egress */
    /* ── guest-public ── */
    CLOCKWORK_ARENA_GUEST_WINDOW              = 14,  /* DspConfig::shm_window */
    /* ── guest-private ── */
    CLOCKWORK_ARENA_GUEST_CONFIG              = 15,  /* host -> guest, copied at boot */
    CLOCKWORK_ARENA_GUEST_PERSIST             = 16,  /* DspConfig::persistent */
    /* ── clockwork-transport, continued: a later addition keeps its number ── */
    CLOCKWORK_ARENA_CLOCKWORK_CONTROL_RING    = 17,  /* clockwork's own control verbs: ControlRingCursors
                                                        then the ring; lifetime arena (docs/RESTARTS.md) */
};

/* The two scope slots, as slot indices into the scope: what left for the
 * device this block, and what arrived from it. Clockwork writes both inside
 * every tick, on every host, at the device's live channel count up to the
 * slot's ceiling (CLOCKWORK_GEOM_SCOPE_CHANNELS). */
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
    /* the guest window: what the guest says its window is (DspInfo::window_magic,
     * window_version), written when the guest binds; 0 until then */
    CLOCKWORK_GEOM_WINDOW_MAGIC        = 0,
    CLOCKWORK_GEOM_WINDOW_VERSION      = 1,
    /* clock anchors: byte offsets within the region */
    CLOCKWORK_GEOM_ANCHOR_GLOBAL       = 0,   /* i32, milliseconds */
    /* scope */
    CLOCKWORK_GEOM_SCOPE_SLOTS         = 0,
    CLOCKWORK_GEOM_SCOPE_SLOT_BYTES    = 1,
    CLOCKWORK_GEOM_SCOPE_HEADER_BYTES  = 2,
    CLOCKWORK_GEOM_SCOPE_FRAMES        = 3,
    CLOCKWORK_GEOM_SCOPE_CHANNELS      = 4,
    CLOCKWORK_GEOM_SCOPE_SAMPLE_RATE   = 5,
    /* taps: a header, then TAP_SLOTS slots the guest claims, then
     * TAP_TRACK_SLOTS the engine writes from plugin-track returns, every slot
     * the same shape. A client numbers them from 0, the guest's first. */
    CLOCKWORK_GEOM_TAP_SLOTS           = 0,
    CLOCKWORK_GEOM_TAP_HEADER_BYTES    = 1,
    CLOCKWORK_GEOM_TAP_SLOT_BYTES      = 2,
    CLOCKWORK_GEOM_TAP_SLOT_HEADER     = 3,
    CLOCKWORK_GEOM_TAP_RING_FRAMES     = 4,
    CLOCKWORK_GEOM_TAP_CHANNELS        = 5,
    CLOCKWORK_GEOM_TAP_TRACK_SLOTS     = 6,
    CLOCKWORK_GEOM_TAP_TRACK_FIRST     = 7,   /* the index of the first track slot (== TAP_SLOTS) */
};

/* One area. 24 bytes. `base` and `bytes` are 64-bit so an address in a
 * 64-bit process fits; inside the arena they are small. */
typedef struct ClockworkArenaAreaEntry {
    uint32_t area;      /* ClockworkArenaArea; 0 marks an unused slot */
    uint32_t space;     /* ClockworkArenaSpace */
    uint32_t base_lo;   /* the offset or address, low word */
    uint32_t base_hi;
    uint32_t bytes_lo;
    uint32_t bytes_hi;
} ClockworkArenaAreaEntry;

/* One region. 64 bytes. */
typedef struct ClockworkArenaEntry {
    uint32_t id;         /* ClockworkArenaRegion; 0 marks an unused entry */
    uint32_t offset;     /* from the arena base */
    uint32_t bytes;
    uint32_t owner;      /* ClockworkArenaOwner */
    uint32_t audience;   /* ClockworkArenaAudience bits */
    uint32_t cadence;    /* ClockworkArenaCadence */
    uint32_t tier;       /* ClockworkArenaTier */
    uint32_t lifetime;   /* ClockworkArenaLifetime */
    uint32_t geom[CLOCKWORK_ARENA_GEOM_WORDS];
} ClockworkArenaEntry;

/* The header, at arena offset 0. */
typedef struct ClockworkArenaHeader {
    uint32_t magic;             /* CLOCKWORK_ARENA_MAGIC */
    uint32_t version;           /* CLOCKWORK_ARENA_VERSION */
    uint32_t header_bytes;      /* CLOCKWORK_ARENA_HEADER_BYTES: the first region starts here */
    uint32_t instance_id;       /* which engine, when a process holds more than one; 0 today */
    uint32_t arena_bytes;       /* the whole arena */
    uint32_t clockwork_bytes;   /* clockwork's half: [0, clockwork_bytes) */
    uint32_t guest_offset;      /* the guest's half: [guest_offset, guest_offset + guest_bytes) */
    uint32_t guest_bytes;
    uint32_t area_count;
    uint32_t area_bytes;        /* sizeof(ClockworkArenaAreaEntry) */
    uint32_t entry_count;
    uint32_t entry_bytes;       /* sizeof(ClockworkArenaEntry) */
    uint32_t state;             /* CLOCKWORK_ARENA_PUBLISHED once the table may be trusted */
    uint32_t generation;        /* the guest instance's generation, as one word (docs/RESTARTS.md):
                                 * 2g+1 while there is no instance, 2g while there is one, g the
                                 * generation the last build opened — the number /clockwork/setup
                                 * names. 1 in a new arena. Moved by the engine with atomic stores;
                                 * read with one load, by every side */
    uint32_t reserved[2];
    ClockworkArenaAreaEntry areas[CLOCKWORK_ARENA_MAX_AREAS];
    ClockworkArenaEntry entries[CLOCKWORK_ARENA_MAX_ENTRIES];
} ClockworkArenaHeader;

/* The generation word, read: which generation the last build opened, and
 * whether its instance is up. A reader may hold the header across a rebuild
 * (its offsets never move), so these are questions to ask again, not facts
 * to cache. */
static inline uint32_t clockwork_arena_generation(const ClockworkArenaHeader* h) {
    return h ? (h->generation >> 1u) : 0u;
}
static inline int clockwork_arena_has_instance(const ClockworkArenaHeader* h) {
    return h ? ((h->generation & 1u) == 0u) : 0;
}

/* NOLINTBEGIN(modernize-use-nullptr): a C header, compiled as C by the ABI probe (rust/clockwork-abi/tests/probe.c) */

/* The entry for a region, or NULL if the arena has none. */
static inline const ClockworkArenaEntry*
clockwork_arena_find(const ClockworkArenaHeader* h, uint32_t id) {
    uint32_t i;
    if (!h) return (const ClockworkArenaEntry*)0;
    for (i = 0; i < h->entry_count && i < CLOCKWORK_ARENA_MAX_ENTRIES; ++i)
        if (h->entries[i].id == id) return &h->entries[i];
    return (const ClockworkArenaEntry*)0;
}

/* The area entry for an area, or NULL if the table has none. */
static inline const ClockworkArenaAreaEntry*
clockwork_arena_area(const ClockworkArenaHeader* h, uint32_t area) {
    uint32_t i;
    if (!h) return (const ClockworkArenaAreaEntry*)0;
    for (i = 0; i < h->area_count && i < CLOCKWORK_ARENA_MAX_AREAS; ++i)
        if (h->areas[i].area == area) return &h->areas[i];
    return (const ClockworkArenaAreaEntry*)0;
}

/* The area a region lies in, by its offset: one of the five in the arena, or
 * 0 when it lies in none (which clockwork_arena_check refuses). */
static inline uint32_t clockwork_arena_area_of(const ClockworkArenaHeader* h, const ClockworkArenaEntry* e) {
    uint32_t i;
    if (!h || !e) return 0u;
    for (i = 0; i < h->area_count && i < CLOCKWORK_ARENA_MAX_AREAS; ++i) {
        const ClockworkArenaAreaEntry* a = &h->areas[i];
        if (a->space != CLOCKWORK_SPACE_ARENA) continue;
        if (e->offset >= a->base_lo && e->offset + e->bytes <= a->base_lo + a->bytes_lo) return a->area;
    }
    return 0u;
}

/* True when the arena has region `id` and it is a data contract a client may
 * read by hand. False for a region the arena lacks, one that is transport or
 * private, and one from a writer that never said. */
static inline int clockwork_arena_public(const ClockworkArenaHeader* h, uint32_t id) {
    const ClockworkArenaEntry* e = clockwork_arena_find(h, id);
    return e != 0 && (e->audience & CLOCKWORK_AUDIENCE_PUBLIC) != 0u;
}

/* True when the first `bytes` of `base` carry a header this reader can use:
 * the magic, a version it knows, tables that fit, every area in the arena
 * inside it and inside its half, every region inside exactly one area, and
 * the five facts of every region stated. `why` names the first failure.
 * Does not acquire. */
static inline int clockwork_arena_check(const ClockworkArenaHeader* h, uint32_t bytes,
                                        const char** why) {
    uint32_t i, j;
#define CLOCKWORK_ARENA_FAIL(w) do { if (why) *why = (w); return 0; } while (0)
    if (!h || bytes < sizeof(ClockworkArenaHeader))  CLOCKWORK_ARENA_FAIL("no room for an arena header");
    if (h->magic != CLOCKWORK_ARENA_MAGIC)           CLOCKWORK_ARENA_FAIL("not a clockwork arena");
    if (h->version != CLOCKWORK_ARENA_VERSION)       CLOCKWORK_ARENA_FAIL("arena version this reader does not know");
    if (h->state != CLOCKWORK_ARENA_PUBLISHED)       CLOCKWORK_ARENA_FAIL("arena not yet published");
    if (h->header_bytes < sizeof(ClockworkArenaHeader)) CLOCKWORK_ARENA_FAIL("header reservation smaller than the header");
    if (h->area_bytes != sizeof(ClockworkArenaAreaEntry)) CLOCKWORK_ARENA_FAIL("area shape this reader does not know");
    if (h->entry_bytes != sizeof(ClockworkArenaEntry)) CLOCKWORK_ARENA_FAIL("entry shape this reader does not know");
    if (h->area_count > CLOCKWORK_ARENA_MAX_AREAS)    CLOCKWORK_ARENA_FAIL("more areas than the table holds");
    if (h->entry_count > CLOCKWORK_ARENA_MAX_ENTRIES) CLOCKWORK_ARENA_FAIL("more entries than the table holds");
    if (h->arena_bytes > bytes)                      CLOCKWORK_ARENA_FAIL("arena larger than the memory it is in");
    if (h->clockwork_bytes > h->arena_bytes)         CLOCKWORK_ARENA_FAIL("clockwork's half larger than the arena");
    if (h->guest_offset != h->clockwork_bytes)       CLOCKWORK_ARENA_FAIL("the guest's half does not start where clockwork's ends");
    if (h->guest_bytes != h->arena_bytes - h->guest_offset) CLOCKWORK_ARENA_FAIL("the two halves do not cover the arena");
    for (i = 0; i < h->area_count; ++i) {
        const ClockworkArenaAreaEntry* a = &h->areas[i];
        if (a->area == 0) CLOCKWORK_ARENA_FAIL("an empty area inside the count");
        for (j = 0; j < i; ++j)
            if (h->areas[j].area == a->area) CLOCKWORK_ARENA_FAIL("an area listed twice");
        if (a->space == CLOCKWORK_SPACE_ARENA) {
            const uint32_t end = a->base_lo + a->bytes_lo;
            const int guests = a->area == CLOCKWORK_AREA_GUEST_PUBLIC || a->area == CLOCKWORK_AREA_GUEST_PRIVATE;
            if (a->base_hi != 0u || a->bytes_hi != 0u) CLOCKWORK_ARENA_FAIL("an area in the arena with a 64-bit base");
            if (a->base_lo < h->header_bytes || end < a->base_lo || end > h->arena_bytes)
                CLOCKWORK_ARENA_FAIL("an area outside the arena");
            if (guests ? a->base_lo < h->guest_offset : end > h->clockwork_bytes)
                CLOCKWORK_ARENA_FAIL("an area in the wrong half");
            for (j = 0; j < i; ++j) {
                const ClockworkArenaAreaEntry* b = &h->areas[j];
                if (b->space == CLOCKWORK_SPACE_ARENA && a->base_lo < b->base_lo + b->bytes_lo && b->base_lo < end)
                    CLOCKWORK_ARENA_FAIL("two areas overlap");
            }
        } else if (a->area <= CLOCKWORK_AREA_GUEST_PRIVATE) {
            CLOCKWORK_ARENA_FAIL("an area of the arena placed outside it");
        }
    }
    for (i = 0; i < h->entry_count; ++i) {
        const ClockworkArenaEntry* e = &h->entries[i];
        uint32_t area;
        if (e->id == 0) CLOCKWORK_ARENA_FAIL("an empty entry inside the count");
        if (e->offset < h->header_bytes) CLOCKWORK_ARENA_FAIL("a region overlaps the header");
        if (e->offset > h->arena_bytes || e->bytes > h->arena_bytes - e->offset)
            CLOCKWORK_ARENA_FAIL("a region runs past the arena");
        if (e->owner < CLOCKWORK_OWNER_CLOCKWORK || e->owner > CLOCKWORK_OWNER_HOST) CLOCKWORK_ARENA_FAIL("a region with no owner");
        if (e->audience == 0u || e->audience > 15u) CLOCKWORK_ARENA_FAIL("a region with no audience");
        if (e->cadence < CLOCKWORK_CADENCE_BLOCK || e->cadence > CLOCKWORK_CADENCE_BOOT) CLOCKWORK_ARENA_FAIL("a region with no cadence");
        if (e->tier < CLOCKWORK_TIER_FAST || e->tier > CLOCKWORK_TIER_BULK) CLOCKWORK_ARENA_FAIL("a region with no tier");
        if (e->lifetime < CLOCKWORK_LIFETIME_ARENA || e->lifetime > CLOCKWORK_LIFETIME_REBUILD) CLOCKWORK_ARENA_FAIL("a region with no lifetime");
        area = clockwork_arena_area_of(h, e);
        if (area == 0u) CLOCKWORK_ARENA_FAIL("a region in no area");
        /* The areas say what they hold: public regions in the public areas, nothing public elsewhere. */
        if (area == CLOCKWORK_AREA_CLOCKWORK_STATE || area == CLOCKWORK_AREA_CLOCKWORK_AUDIO || area == CLOCKWORK_AREA_GUEST_PUBLIC) {
            if ((e->audience & CLOCKWORK_AUDIENCE_PUBLIC) == 0u) CLOCKWORK_ARENA_FAIL("a non-public region in a public area");
        } else if ((e->audience & CLOCKWORK_AUDIENCE_PUBLIC) != 0u) {
            CLOCKWORK_ARENA_FAIL("a public region outside the public areas");
        }
        if (area == CLOCKWORK_AREA_CLOCKWORK_TRANSPORT && (e->audience & CLOCKWORK_AUDIENCE_TRANSPORT) == 0u)
            CLOCKWORK_ARENA_FAIL("a region in the transport area that is not transport");
    }
#undef CLOCKWORK_ARENA_FAIL
    return 1;
}

/* NOLINTEND(modernize-use-nullptr) */

#ifdef __cplusplus
}
#endif
