// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_params.h — the parameter table (docs/PARAMS.md).
 *
 * One table per engine: named values with a range, a unit and a kind, that
 * anyone may read and anyone may set. The table is read, never announced: a
 * reader sees the set of entries change by the generation, and a value by its
 * slot (the paramMoves metric counts the moves).
 *
 * THE REGION (CLOCKWORK_ARENA_CLOCKWORK_PARAMS). A header, then `capacity`
 * values, one f32 a slot, then `capacity` entries, then the text the entries
 * point into, at the offsets the header gives. Values sit apart from the
 * facts: the audio thread and a panel's reader touch only them.
 *
 * THE GENERATION is a seqlock over the entries: the header's word is odd
 * while a define writes and even between, and the generation a client sees in
 * replies and reports is that word halved. A value moving never moves it.
 *
 * THE WIRE. /clockwork/param/define carries a blob of ClockworkParamEntry
 * records, the same bytes the table holds, and a second blob of text when any
 * entry has some: an entry's `text` is then where its run starts in that blob.
 * `live` and `definer` in a define are clockwork's to write and are ignored:
 * what arrives by the verb is the client's.
 * An entry of kind CLOCKWORK_PARAM_RETIRE retires its id.
 *
 * TEXT. An entry's run is a list of strings, each a tag byte (ClockworkParamTextTag),
 * UTF-8, and a NUL, in that order. A reader skips a tag it does not know.
 *
 * ONE FIELD PER FACT. A define that contradicts itself is refused whole,
 * rather than leaving each reader to pick a side: docs/PARAMS.md lists the rules.
 */
#pragma once
#include <stdint.h>
#include "clockwork_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CLOCKWORK_PARAMS_MAGIC     0x4357504Du   /* 'CWPM' */
#define CLOCKWORK_PARAMS_VERSION   2u
#define CLOCKWORK_PARAM_NAME_BYTES 32            /* 31 bytes of UTF-8 and a NUL */

/* What a value is. A stepped parameter's steps are the integers in its
 * range; a switch is 0 or 1; a note is a MIDI note number; a choice is an index
 * from 0 into the value names its text holds, one name per step. A group holds
 * no value: it is a name, in the entries' own table, that others name as their
 * `group`. */
enum ClockworkParamKind {
    CLOCKWORK_PARAM_RETIRE     = 0,   /* in a define only: the id is retired */
    CLOCKWORK_PARAM_CONTINUOUS = 1,
    CLOCKWORK_PARAM_STEPPED    = 2,
    CLOCKWORK_PARAM_SWITCH     = 3,
    CLOCKWORK_PARAM_NOTE       = 4,
    CLOCKWORK_PARAM_CHOICE     = 5,
    CLOCKWORK_PARAM_GROUP      = 6,
};

enum ClockworkParamUnit {
    CLOCKWORK_PARAM_UNIT_NONE      = 0,
    CLOCKWORK_PARAM_UNIT_PERCENT   = 1,   /* the value is in percent: 50 is 50%, never 0.5 */
    CLOCKWORK_PARAM_UNIT_DECIBELS  = 2,
    CLOCKWORK_PARAM_UNIT_HERTZ     = 3,
    CLOCKWORK_PARAM_UNIT_SECONDS   = 4,
    CLOCKWORK_PARAM_UNIT_BPM       = 5,
    CLOCKWORK_PARAM_UNIT_SEMITONES = 6,
    CLOCKWORK_PARAM_UNIT_CUSTOM    = 7,   /* what the entry's CLOCKWORK_PARAM_TEXT_UNIT says */
};

enum ClockworkParamFlags {
    CLOCKWORK_PARAM_AUTOMATABLE = 1u << 0,   /* a host may automate it */
    CLOCKWORK_PARAM_HIDDEN      = 1u << 1,   /* no host lists or sets it; a panel may show it */
    CLOCKWORK_PARAM_READ_ONLY   = 1u << 2,   /* its definer writes it, nobody else */
    CLOCKWORK_PARAM_WRAPS       = 1u << 3,   /* its range is a circle: past max is min */
    CLOCKWORK_PARAM_META        = 1u << 4,   /* moving it may move others */
};
#define CLOCKWORK_PARAM_FLAGS_ALL 31u

/* What a parameter is for, where a host or a bridge has something to wire to
 * it. A table holds at most one of each. */
enum ClockworkParamRole {
    CLOCKWORK_PARAM_ROLE_NONE     = 0,
    CLOCKWORK_PARAM_ROLE_BYPASS   = 1,   /* a switch: on is bypassed */
    CLOCKWORK_PARAM_ROLE_TEMPO    = 2,   /* in BPM: a host that keeps the time drives it */
    CLOCKWORK_PARAM_ROLE_KEY_NOTE = 3,   /* a note: what a key played sets */
    CLOCKWORK_PARAM_ROLE_KEY_HELD = 4,   /* a switch: on while a key is down */
};

/* How a continuous parameter sweeps a knob: 0 evenly; -1 logarithmically, over
 * a range above zero; any other value above zero a power law, a position being
 * ((value - min) / (max - min)) ^ taper. One field, so it cannot disagree with
 * itself, and each sweep has one spelling: 1 is refused, because even is 0. */
#define CLOCKWORK_PARAM_TAPER_LINEAR 0.0f
#define CLOCKWORK_PARAM_TAPER_LOG    (-1.0f)

/* The strings in an entry's text. Only a VALUE may repeat. */
enum ClockworkParamTextTag {
    CLOCKWORK_PARAM_TEXT_FULL_NAME = 1,   /* when `name` had to be shortened */
    CLOCKWORK_PARAM_TEXT_DOC       = 2,   /* what it does, for a person */
    CLOCKWORK_PARAM_TEXT_UNIT      = 3,   /* a custom unit's words; no other unit has any */
    CLOCKWORK_PARAM_TEXT_VALUE     = 4,   /* a choice's names, in order; nothing else has any */
};
#define CLOCKWORK_PARAM_FULL_NAME_BYTES 256      /* a CLAP name's, its NUL included */
#define CLOCKWORK_PARAM_GROUP_DEPTH     8        /* the most groups around one entry */

/* Who defined an entry, or sets a value: the guest, the host in the engine's
 * process, or the client. An engine has one client at a time (docs/TRANSPORTS.md),
 * so which connection a define or a set came over says nothing: a client that
 * comes back finds its entries its own. */
#define CLOCKWORK_PARAM_DEFINER_GUEST  0xFFFFFFFFu
#define CLOCKWORK_PARAM_DEFINER_HOST   0xFFFFFFFEu
#define CLOCKWORK_PARAM_DEFINER_CLIENT 0xFFFFFFFDu

/* The facts of a parameter, and whether its slot is taken. 96 bytes, the
 * layout the wire's as much as the table's. */
typedef struct ClockworkParamEntry {
    uint32_t live;       /* 1 while the slot is taken; clockwork's to write */
    uint32_t id;         /* the definer's, non-zero, stable for the entry's life */
    uint32_t kind;       /* ClockworkParamKind */
    uint32_t unit;       /* ClockworkParamUnit */
    uint32_t flags;      /* ClockworkParamFlags */
    uint32_t group;      /* the id of the group it sits in, one of its definer's; 0 for none */
    float    min;
    float    max;
    float    def;
    uint32_t definer;    /* who holds the id, a CLOCKWORK_PARAM_DEFINER_ word; clockwork's to write */
    char     name[CLOCKWORK_PARAM_NAME_BYTES];   /* the short one, never empty */
    float    taper;      /* CLOCKWORK_PARAM_TAPER_LINEAR, _LOG, or a power law's */
    uint32_t role;       /* ClockworkParamRole */
    uint32_t text;       /* where its run starts: in the region's text, or a define's blob */
    uint32_t text_bytes; /* the run's length; 0 for none */
    uint32_t reserved[2];
} ClockworkParamEntry;

/* The front of the region. */
typedef struct ClockworkParamsHeader {
    uint32_t magic;           /* CLOCKWORK_PARAMS_MAGIC, written last */
    uint32_t version;         /* CLOCKWORK_PARAMS_VERSION */
    uint32_t capacity;        /* slots */
    uint32_t count;           /* slots taken */
    uint32_t generation;      /* the seqlock word: odd while a define writes */
    uint32_t values_offset;   /* from the region's start: capacity f32 */
    uint32_t entries_offset;  /* from the region's start: capacity entries */
    uint32_t entry_bytes;     /* sizeof(ClockworkParamEntry) */
    uint32_t text_offset;     /* from the region's start */
    uint32_t text_capacity;   /* bytes */
    uint32_t text_used;       /* bytes written so far, a retired run's included until a define needs them */
    uint32_t reserved[5];
} ClockworkParamsHeader;

/* The region's size for a capacity. */
#define CLOCKWORK_PARAMS_VALUES_OFFSET ((uint32_t)sizeof(ClockworkParamsHeader))
#define CLOCKWORK_PARAMS_ENTRIES_OFFSET(capacity) \
    ((CLOCKWORK_PARAMS_VALUES_OFFSET + (uint32_t)(capacity) * 4u + 15u) & ~15u)
#define CLOCKWORK_PARAMS_TEXT_OFFSET(capacity) \
    ((CLOCKWORK_PARAMS_ENTRIES_OFFSET(capacity) + (uint32_t)(capacity) * (uint32_t)sizeof(ClockworkParamEntry) + 15u) & ~15u)
#define CLOCKWORK_PARAMS_REGION_BYTES(capacity, text_bytes) \
    (CLOCKWORK_PARAMS_TEXT_OFFSET(capacity) + (uint32_t)(text_bytes))

/* Why a define or a set was refused: the reason in /clockwork/error. */
#define CLOCKWORK_PARAM_REFUSED_FULL      "no room in the parameter table"
#define CLOCKWORK_PARAM_REFUSED_RANGE     "a bad range or value"
#define CLOCKWORK_PARAM_REFUSED_NAME      "a name empty, too long or not UTF-8"
#define CLOCKWORK_PARAM_REFUSED_KIND      "no such kind, unit, flag or role"
#define CLOCKWORK_PARAM_REFUSED_FACTS     "facts that contradict each other"
#define CLOCKWORK_PARAM_REFUSED_TEXT      "text that is malformed, too long or does not fit the facts"
#define CLOCKWORK_PARAM_REFUSED_TEXT_FULL "no room for the parameter text"
#define CLOCKWORK_PARAM_REFUSED_ROLE      "a role another entry holds, or one that does not fit the facts"
#define CLOCKWORK_PARAM_REFUSED_GROUP     "a group that is not there, too deep, another definer's or still in use"
#define CLOCKWORK_PARAM_REFUSED_HELD      "an id another definer holds"
#define CLOCKWORK_PARAM_REFUSED_UNKNOWN   "no such parameter"
#define CLOCKWORK_PARAM_REFUSED_READ_ONLY "read-only"

/* ── A reader: any thread, any process that maps the region ──────────────── */

/* The table in a region's bytes, or NULL when they are not one this reader knows. */
const ClockworkParamsHeader* clockwork_params_header(const uint8_t* region);

/* The generation a client sees: one more each time the set of entries changes. */
uint32_t clockwork_params_generation(const ClockworkParamsHeader* h);

/* A slot's facts into `out`, consistent under the generation. 1 for a taken
 * slot, 0 for a free one or one outside the table, -1 when a define kept
 * writing throughout (read again). */
int clockwork_params_read_entry(const ClockworkParamsHeader* h, uint32_t slot, ClockworkParamEntry* out);

/* A slot's value now, one load. 0 for a slot outside the table. */
float clockwork_params_value(const ClockworkParamsHeader* h, uint32_t slot);

/* A slot's text into `out`, consistent with its facts in `*entry` when given,
 * which are read under the same generation. The run's length, 0 for a free slot
 * or one with no text, -1 when a define kept writing throughout (read again),
 * or -2 when `capacity` is too small: CLOCKWORK_PARAM_TEXT_RUN_BYTES always holds one. */
int32_t clockwork_params_read_text(const ClockworkParamsHeader* h, uint32_t slot, uint8_t* out, uint32_t capacity,
                                   ClockworkParamEntry* entry);

/* ── In the engine's process ─────────────────────────────────────────────── */

/* The core's table, or NULL before it has an arena. A reader's: any thread. */
const ClockworkParamsHeader* clockwork_params_table(const ClockworkCore* core);

/* A slot's value now, from any thread. */
float clockwork_param_value(const ClockworkCore* core, uint32_t slot);

/* Declare, replace or retire entries, all or none. `text` is the blob their
 * `text` fields point into, or NULL when none has any. `definer` is who the
 * entries belong to, a CLOCKWORK_PARAM_DEFINER_ word. On success returns `count`, writes each entry's slot to `slots` when
 * given (a retired entry's is left alone), and moves the generation; on
 * refusal returns -1, changes nothing, and names the reason in `*why` when
 * given. An id may appear once in a define.
 *
 * THE THREAD THAT TICKS THE CORE, or before the core first ticks, or while a
 * restart has its audio thread stopped. A client's other threads use the verb. */
int32_t clockwork_param_define(ClockworkCore* core, const ClockworkParamEntry* entries, uint32_t count,
                               const uint8_t* text, uint32_t text_bytes,
                               uint32_t definer, uint32_t* slots, const char** why);

/* Write a slot's value as `origin` (a CLOCKWORK_PARAM_DEFINER_ word), held to the
 * range and rounded for the integer kinds. Returns 1 when written, 0 when
 * refused: no such slot, an origin that is none of them, a value
 * that is not finite, or a read-only entry whose definer `origin` is not.
 *
 * THE THREAD THAT TICKS THE CORE: never allocates, never blocks. */
int clockwork_param_set(ClockworkCore* core, uint32_t slot, float value, uint32_t origin);

#ifdef __cplusplus
}
#endif
