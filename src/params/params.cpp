// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * params.cpp — the parameter table (docs/PARAMS.md, clockwork_params.h).
 *
 * The table has one writer at a time and takes no lock: the verbs arrive
 * through the drain on the thread that ticks the core, the guest calls from
 * inside its own calls, and a restart runs with that thread stopped. What it
 * owes a reader mapping the region is the generation and atomic words. A
 * change is seen there and nowhere else: a verb answers its asker, and
 * nothing is announced.
 *
 * Nothing here allocates: a define and a set each decide in one pass over the
 * frame and write in a second, so a refusal leaves the table as it was.
 */
#include "params/params.h"
#include "params/clockwork_params.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

#include "IngressCallCtx.h"
#include "audio_processor.h"     // clockwork_emit_osc
#include "clockwork_sys.h"       // CLOCKWORK_SYS, clockwork_sys_refuse
#include "core_state.h"          // arena_base, metrics_in
#include "host/osc_reader.h"
#include "shared_memory.h"       // PARAMS_START, arenaInstanceGeneration

static_assert(sizeof(ClockworkParamEntry) == 96, "an entry is the wire's layout: js/lib/params.js and rust/clockwork-abi");
static_assert(sizeof(ClockworkParamsHeader) == 64, "the header is every reader's: js/lib/params.js and rust/clockwork-abi");
static_assert(sizeof(ClockworkParamEntry) % 4 == 0, "an entry is copied word by word");

namespace {

constexpr uint32_t kEntryWords = sizeof(ClockworkParamEntry) / 4u;
constexpr uint32_t kNoSlot     = UINT32_MAX;
constexpr uint32_t kListPage   = 16;     // rows a list answers per call: a page fits the smallest ring

constexpr uint32_t padded(uint32_t bytes) { return (bytes + 3u) & ~3u; }   // text is held in whole words

std::atomic<uint32_t>& word_of(const uint32_t& w) {
    return *reinterpret_cast<std::atomic<uint32_t>*>(const_cast<uint32_t*>(&w));
}

std::atomic<uint32_t>* words_of(const void* p) {
    return reinterpret_cast<std::atomic<uint32_t>*>(const_cast<void*>(p));
}

float to_float(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }
uint32_t to_bits(float f)     { uint32_t b; std::memcpy(&b, &f, 4); return b; }

struct Table {
    ClockworkParamsHeader* header;
    std::atomic<uint32_t>* values;    // f32 bits, one a slot
    ClockworkParamEntry*   entries;
    std::atomic<uint32_t>* text;      // the runs, each starting on a word

    uint32_t capacity() const { return header->capacity; }
    float value(uint32_t slot) const { return to_float(values[slot].load(std::memory_order_relaxed)); }
    void set_value(uint32_t slot, float f) const { values[slot].store(to_bits(f), std::memory_order_relaxed); }

    // The writer's thread is the only one that changes an entry, so it reads
    // its own words plainly.
    uint32_t find(uint32_t id) const {
        for (uint32_t s = 0; s < capacity(); ++s)
            if (entries[s].live && entries[s].id == id) return s;
        return kNoSlot;
    }
    uint32_t first_free() const {
        for (uint32_t s = 0; s < capacity(); ++s)
            if (!entries[s].live) return s;
        return kNoSlot;
    }
    void store_entry(uint32_t slot, const ClockworkParamEntry& e) const {
        auto* dst = words_of(&entries[slot]);
        const auto* src = reinterpret_cast<const uint8_t*>(&e);
        for (uint32_t w = 0; w < kEntryWords; ++w) {
            uint32_t bits; std::memcpy(&bits, src + w * 4u, 4);
            dst[w].store(bits, std::memory_order_relaxed);
        }
    }
    // The text every entry holds now, in whole words.
    uint32_t text_in_use() const {
        uint32_t bytes = 0;
        for (uint32_t s = 0; s < capacity(); ++s)
            if (entries[s].live) bytes += padded(entries[s].text_bytes);
        return bytes;
    }
    // Close the gaps retired runs left, the runs in the order they lie, each
    // moved toward the front and so never over one not moved yet. Returns where
    // the text now ends. Under the generation: a reader sees before or after.
    uint32_t close_text() const {
        uint32_t end = 0, from = 0;
        for (;;) {
            uint32_t next = kNoSlot, at = UINT32_MAX;
            for (uint32_t s = 0; s < capacity(); ++s) {
                const ClockworkParamEntry& e = entries[s];
                if (e.live && e.text_bytes != 0u && e.text >= from && e.text < at) { next = s; at = e.text; }
            }
            if (next == kNoSlot) return end;
            const uint32_t bytes = padded(entries[next].text_bytes);
            if (at != end) {
                for (uint32_t w = 0; w < bytes / 4u; ++w)
                    text[end / 4u + w].store(text[at / 4u + w].load(std::memory_order_relaxed), std::memory_order_relaxed);
                word_of(entries[next].text).store(end, std::memory_order_relaxed);
            }
            from = at + bytes;
            end += bytes;
        }
    }
    // A run from a define, into the text at `at`. Returns where it ends.
    uint32_t put_text(uint32_t at, const uint8_t* run, uint32_t bytes) const {
        for (uint32_t done = 0; done < bytes; done += 4u) {
            uint32_t bits = 0;
            std::memcpy(&bits, run + done, std::min<uint32_t>(4u, bytes - done));
            text[(at + done) / 4u].store(bits, std::memory_order_relaxed);
        }
        return at + padded(bytes);
    }
};

bool has_table(const ClockworkCore* core) {
    const uint8_t* base = clockwork::arena_base(core);
    return base && clockwork_params_header(base + PARAMS_START) != nullptr;
}

Table table_of(const ClockworkCore* core) {
    uint8_t* const region = clockwork::arena_base(core) + PARAMS_START;
    auto* h = reinterpret_cast<ClockworkParamsHeader*>(region);
    return Table{ h, words_of(region + h->values_offset),
                  reinterpret_cast<ClockworkParamEntry*>(region + h->entries_offset),
                  words_of(region + h->text_offset) };
}

PerformanceMetrics& metrics_of(ClockworkCore* core) {
    return *clockwork::metrics_in(clockwork::arena_base(core));
}

// A value as an entry holds it: within the range, and whole for the integer kinds.
float held(const ClockworkParamEntry& e, float v) {
    if (e.kind != CLOCKWORK_PARAM_CONTINUOUS) v = std::round(v);
    if (v < e.min) v = e.min;
    if (v > e.max) v = e.max;
    return v;
}

bool whole(float v) { return std::floor(v) == v; }

// The guest, the host or the client: who an entry or a set may be.
bool is_definer(uint32_t who) {
    return who == CLOCKWORK_PARAM_DEFINER_GUEST || who == CLOCKWORK_PARAM_DEFINER_HOST || who == CLOCKWORK_PARAM_DEFINER_CLIENT;
}

// Strict UTF-8: no overlong form, no surrogate, nothing past U+10FFFF, so every
// reader decodes what every other does.
bool is_utf8(const uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n;) {
        const uint8_t b = p[i];
        std::size_t more = 0;
        uint32_t cp = 0, least = 0;
        if (b < 0x80u)                { ++i; continue; }
        else if ((b & 0xE0u) == 0xC0u) { more = 1; cp = b & 0x1Fu; least = 0x80u; }
        else if ((b & 0xF0u) == 0xE0u) { more = 2; cp = b & 0x0Fu; least = 0x800u; }
        else if ((b & 0xF8u) == 0xF0u) { more = 3; cp = b & 0x07u; least = 0x10000u; }
        else return false;
        if (n - i <= more) return false;
        for (std::size_t k = 1; k <= more; ++k) {
            if ((p[i + k] & 0xC0u) != 0x80u) return false;
            cp = (cp << 6u) | (p[i + k] & 0x3Fu);
        }
        if (cp < least || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
        i += more + 1;
    }
    return true;
}

bool role_fits(const ClockworkParamEntry& e) {
    if (e.flags & CLOCKWORK_PARAM_READ_ONLY) return false;   // a role is what something else drives
    switch (e.role) {
        case CLOCKWORK_PARAM_ROLE_BYPASS:
        case CLOCKWORK_PARAM_ROLE_KEY_HELD: return e.kind == CLOCKWORK_PARAM_SWITCH;
        case CLOCKWORK_PARAM_ROLE_KEY_NOTE: return e.kind == CLOCKWORK_PARAM_NOTE;
        case CLOCKWORK_PARAM_ROLE_TEMPO:    return e.unit == CLOCKWORK_PARAM_UNIT_BPM;
        default: return true;
    }
}

// One entry's facts, on their own: docs/PARAMS.md, "Facts that agree".
const char* facts_refused(const ClockworkParamEntry& e) {
    if (e.kind < CLOCKWORK_PARAM_CONTINUOUS || e.kind > CLOCKWORK_PARAM_GROUP
        || e.unit > CLOCKWORK_PARAM_UNIT_CUSTOM || (e.flags & ~CLOCKWORK_PARAM_FLAGS_ALL) != 0u
        || e.role > CLOCKWORK_PARAM_ROLE_KEY_HELD)
        return CLOCKWORK_PARAM_REFUSED_KIND;
    const void* const nul = std::memchr(e.name, '\0', sizeof e.name);
    if (nul == nullptr || nul == e.name
        || !is_utf8(reinterpret_cast<const uint8_t*>(e.name), static_cast<std::size_t>(static_cast<const char*>(nul) - e.name)))
        return CLOCKWORK_PARAM_REFUSED_NAME;

    if (e.kind == CLOCKWORK_PARAM_GROUP) {
        if (e.role != CLOCKWORK_PARAM_ROLE_NONE) return CLOCKWORK_PARAM_REFUSED_ROLE;
        if (e.min != 0.f || e.max != 0.f || e.def != 0.f || e.unit != CLOCKWORK_PARAM_UNIT_NONE
            || e.flags != 0u || e.taper != CLOCKWORK_PARAM_TAPER_LINEAR)
            return CLOCKWORK_PARAM_REFUSED_FACTS;
        return nullptr;
    }

    if (!std::isfinite(e.min) || !std::isfinite(e.max) || !std::isfinite(e.def)
        || e.min >= e.max || e.def < e.min || e.def > e.max)
        return CLOCKWORK_PARAM_REFUSED_RANGE;
    if (e.kind != CLOCKWORK_PARAM_CONTINUOUS && (!whole(e.min) || !whole(e.max) || !whole(e.def)))
        return CLOCKWORK_PARAM_REFUSED_RANGE;
    if ((e.kind == CLOCKWORK_PARAM_SWITCH && (e.min != 0.f || e.max != 1.f))
        || (e.kind == CLOCKWORK_PARAM_NOTE && (e.min < 0.f || e.max > 127.f))
        || (e.kind == CLOCKWORK_PARAM_CHOICE && e.min != 0.f))
        return CLOCKWORK_PARAM_REFUSED_RANGE;

    if ((e.flags & CLOCKWORK_PARAM_AUTOMATABLE) && (e.flags & (CLOCKWORK_PARAM_READ_ONLY | CLOCKWORK_PARAM_HIDDEN)))
        return CLOCKWORK_PARAM_REFUSED_FACTS;
    if ((e.flags & CLOCKWORK_PARAM_META) && (e.flags & CLOCKWORK_PARAM_READ_ONLY))
        return CLOCKWORK_PARAM_REFUSED_FACTS;
    if ((e.flags & CLOCKWORK_PARAM_WRAPS) && (e.kind == CLOCKWORK_PARAM_SWITCH || e.kind == CLOCKWORK_PARAM_NOTE))
        return CLOCKWORK_PARAM_REFUSED_FACTS;
    if (e.unit != CLOCKWORK_PARAM_UNIT_NONE && e.kind != CLOCKWORK_PARAM_CONTINUOUS && e.kind != CLOCKWORK_PARAM_STEPPED)
        return CLOCKWORK_PARAM_REFUSED_FACTS;

    if (e.taper != CLOCKWORK_PARAM_TAPER_LINEAR) {
        if (e.kind != CLOCKWORK_PARAM_CONTINUOUS) return CLOCKWORK_PARAM_REFUSED_FACTS;
        if (e.taper == CLOCKWORK_PARAM_TAPER_LOG) {
            if (e.min <= 0.f) return CLOCKWORK_PARAM_REFUSED_FACTS;
        } else if (!std::isfinite(e.taper) || e.taper <= 0.f || e.taper == 1.f) {
            return CLOCKWORK_PARAM_REFUSED_FACTS;
        }
    }

    if (e.role != CLOCKWORK_PARAM_ROLE_NONE && !role_fits(e)) return CLOCKWORK_PARAM_REFUSED_ROLE;
    return nullptr;
}

// One entry's text, whole and in agreement with its facts. `blob` is what the
// define brought: nothing in the entry is trusted to lie within it.
const char* text_refused(const ClockworkParamEntry& e, const uint8_t* blob, uint32_t blob_bytes) {
    uint32_t units = 0, values = 0;
    if (e.text_bytes != 0u) {
        if (blob == nullptr || e.text_bytes > blob_bytes || e.text > blob_bytes - e.text_bytes
            || e.text_bytes > CLOCKWORK_PARAM_TEXT_RUN_BYTES)
            return CLOCKWORK_PARAM_REFUSED_TEXT;
        const uint8_t* const run = blob + e.text;
        uint32_t names = 0, docs = 0;
        for (uint32_t i = 0; i < e.text_bytes;) {
            const uint8_t tag = run[i++];
            const auto* const end = (i < e.text_bytes) ? static_cast<const uint8_t*>(std::memchr(run + i, '\0', e.text_bytes - i)) : nullptr;
            if (end == nullptr) return CLOCKWORK_PARAM_REFUSED_TEXT;
            const auto words = static_cast<uint32_t>(end - (run + i));
            if (words == 0u || !is_utf8(run + i, words)) return CLOCKWORK_PARAM_REFUSED_TEXT;
            const bool fits = [&] {
                switch (tag) {
                    case CLOCKWORK_PARAM_TEXT_FULL_NAME: return ++names == 1u && words < CLOCKWORK_PARAM_FULL_NAME_BYTES;
                    case CLOCKWORK_PARAM_TEXT_DOC:       return ++docs == 1u;
                    case CLOCKWORK_PARAM_TEXT_UNIT:      return ++units == 1u && words < CLOCKWORK_PARAM_NAME_BYTES;
                    case CLOCKWORK_PARAM_TEXT_VALUE:     ++values; return words < CLOCKWORK_PARAM_NAME_BYTES;
                    default: return false;
                }
            }();
            if (!fits) return CLOCKWORK_PARAM_REFUSED_TEXT;
            i += words + 1u;
        }
    }
    if ((units != 0u) != (e.unit == CLOCKWORK_PARAM_UNIT_CUSTOM)) return CLOCKWORK_PARAM_REFUSED_TEXT;
    const uint32_t steps = e.kind == CLOCKWORK_PARAM_CHOICE ? static_cast<uint32_t>(e.max) + 1u : 0u;
    return values == steps ? nullptr : CLOCKWORK_PARAM_REFUSED_TEXT;
}

ClockworkParamEntry entry_at(const ClockworkParamEntry* in, uint32_t i) {
    ClockworkParamEntry e;
    std::memcpy(&e, reinterpret_cast<const uint8_t*>(in) + i * sizeof e, sizeof e);
    return e;
}

// What an id will be once a define has landed; false when it will be nothing.
struct After { uint32_t kind, group, definer; };
bool after_define(const Table& t, const ClockworkParamEntry* in, uint32_t count, uint32_t definer, uint32_t id, After& out) {
    for (uint32_t i = 0; i < count; ++i) {
        const ClockworkParamEntry e = entry_at(in, i);
        if (e.id != id) continue;
        if (e.kind == CLOCKWORK_PARAM_RETIRE) return false;
        out = After{ e.kind, e.group, definer };
        return true;
    }
    const uint32_t s = t.find(id);
    if (s == kNoSlot) return false;
    out = After{ t.entries[s].kind, t.entries[s].group, t.entries[s].definer };
    return true;
}
bool in_define(const ClockworkParamEntry* in, uint32_t count, uint32_t id) {
    for (uint32_t i = 0; i < count; ++i)
        if (entry_at(in, i).id == id) return true;
    return false;
}

// An entry's place once the define has landed: every group around it there,
// its definer's, and within the depth. The depth is the walk's bound, so a
// circle comes out as too deep.
bool nesting_ok(const Table& t, const ClockworkParamEntry* in, uint32_t count, uint32_t definer,
                uint32_t kind, uint32_t group, uint32_t held_by) {
    uint32_t depth = kind == CLOCKWORK_PARAM_GROUP ? 1u : 0u;
    for (uint32_t at = group; at != 0u;) {
        After g{};
        if (!after_define(t, in, count, definer, at, g) || g.kind != CLOCKWORK_PARAM_GROUP || g.definer != held_by) return false;
        if (++depth > CLOCKWORK_PARAM_GROUP_DEPTH) return false;
        at = g.group;
    }
    return true;
}

// What the define does to groups, across entries.
const char* groups_refused(const Table& t, const ClockworkParamEntry* in, uint32_t count, uint32_t definer) {
    bool moved = false;
    for (uint32_t i = 0; i < count; ++i) {
        const ClockworkParamEntry e = entry_at(in, i);
        const uint32_t s = t.find(e.id);
        const bool was_group = s != kNoSlot && t.entries[s].kind == CLOCKWORK_PARAM_GROUP;
        if (e.kind != CLOCKWORK_PARAM_RETIRE && !nesting_ok(t, in, count, definer, e.kind, e.group, definer))
            return CLOCKWORK_PARAM_REFUSED_GROUP;
        if (!was_group) continue;
        if (e.kind == CLOCKWORK_PARAM_GROUP) {
            moved = moved || t.entries[s].group != e.group;
            continue;
        }
        // A group that stops being one: whatever stays in it has lost it. An
        // entry in this define has said its own place, and was checked above.
        for (uint32_t m = 0; m < t.capacity(); ++m)
            if (t.entries[m].live && t.entries[m].group == e.id && !in_define(in, count, t.entries[m].id))
                return CLOCKWORK_PARAM_REFUSED_GROUP;
    }
    // A group that moved takes all that is in it, which may now be too deep.
    if (moved) {
        for (uint32_t m = 0; m < t.capacity(); ++m) {
            const ClockworkParamEntry& e = t.entries[m];
            if (e.live && e.group != 0u && !in_define(in, count, e.id)
                && !nesting_ok(t, in, count, definer, e.kind, e.group, e.definer))
                return CLOCKWORK_PARAM_REFUSED_GROUP;
        }
    }
    return nullptr;
}

// A role is held by one entry at a time, once the define has landed.
const char* roles_refused(const Table& t, const ClockworkParamEntry* in, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const ClockworkParamEntry e = entry_at(in, i);
        if (e.kind == CLOCKWORK_PARAM_RETIRE || e.role == CLOCKWORK_PARAM_ROLE_NONE) continue;
        for (uint32_t j = 0; j < i; ++j) {
            const ClockworkParamEntry other = entry_at(in, j);
            if (other.kind != CLOCKWORK_PARAM_RETIRE && other.role == e.role) return CLOCKWORK_PARAM_REFUSED_ROLE;
        }
        for (uint32_t s = 0; s < t.capacity(); ++s)
            if (t.entries[s].live && t.entries[s].role == e.role && !in_define(in, count, t.entries[s].id))
                return CLOCKWORK_PARAM_REFUSED_ROLE;
    }
    return nullptr;
}

int32_t define(ClockworkCore* core, const ClockworkParamEntry* in, uint32_t count, const uint8_t* text, uint32_t text_bytes,
               uint32_t definer, uint32_t* slots, const char** why) {
    const auto refuse = [why](const char* reason) { if (why) *why = reason; return -1; };
    if (!has_table(core)) return refuse(CLOCKWORK_REFUSED_NO_INSTANCE);
    const Table t = table_of(core);
    if (count == 0 || !is_definer(definer)) return refuse(CLOCKWORK_REFUSED_MALFORMED);
    if (count > t.capacity()) return refuse(CLOCKWORK_PARAM_REFUSED_FULL);

    // Decide. Nothing is written until all of it can land.
    uint32_t added = 0, retired = 0, freed = 0, needed = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const ClockworkParamEntry e = entry_at(in, i);
        if (e.id == 0) return refuse(CLOCKWORK_REFUSED_MALFORMED);
        for (uint32_t j = 0; j < i; ++j)
            if (entry_at(in, j).id == e.id) return refuse(CLOCKWORK_REFUSED_MALFORMED);
        const uint32_t s = t.find(e.id);
        if (s != kNoSlot && t.entries[s].definer != definer) return refuse(CLOCKWORK_PARAM_REFUSED_HELD);
        if (s != kNoSlot) freed += padded(t.entries[s].text_bytes);
        if (e.kind == CLOCKWORK_PARAM_RETIRE) {
            if (s == kNoSlot) return refuse(CLOCKWORK_PARAM_REFUSED_UNKNOWN);
            ++retired;
            continue;
        }
        if (const char* no = facts_refused(e)) return refuse(no);
        if (const char* no = text_refused(e, text, text_bytes)) return refuse(no);
        needed += padded(e.text_bytes);
        if (s == kNoSlot) ++added;
    }
    if (t.header->count - retired + added > t.capacity()) return refuse(CLOCKWORK_PARAM_REFUSED_FULL);
    if (const char* no = groups_refused(t, in, count, definer)) return refuse(no);
    if (const char* no = roles_refused(t, in, count)) return refuse(no);
    if (t.text_in_use() - freed + needed > t.header->text_capacity) return refuse(CLOCKWORK_PARAM_REFUSED_TEXT_FULL);

    // Write, under the generation: retires first, so a new id may take a slot
    // this same define freed. A new entry's value is in place before its facts.
    std::atomic<uint32_t>& gen = word_of(t.header->generation);
    const uint32_t odd = gen.load(std::memory_order_relaxed) + 1u;
    gen.store(odd, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    uint32_t live = t.header->count;
    for (uint32_t i = 0; i < count; ++i) {
        const ClockworkParamEntry e = entry_at(in, i);
        const uint32_t s = t.find(e.id);
        if (e.kind == CLOCKWORK_PARAM_RETIRE) {
            t.store_entry(s, ClockworkParamEntry{});
            t.set_value(s, 0.f);
            --live;
        } else if (s != kNoSlot) {
            word_of(t.entries[s].text_bytes).store(0u, std::memory_order_relaxed);   // its old run is out of use
        }
    }
    uint32_t text_end = t.header->text_used;
    if (text_end + needed > t.header->text_capacity) text_end = t.close_text();
    for (uint32_t i = 0; i < count; ++i) {
        ClockworkParamEntry e = entry_at(in, i);
        if (e.kind == CLOCKWORK_PARAM_RETIRE) continue;
        e.live = 1u;
        e.definer = definer;
        e.reserved[0] = e.reserved[1] = 0u;
        if (e.text_bytes != 0u) {
            const uint32_t at = text_end;
            text_end = t.put_text(at, text + e.text, e.text_bytes);
            e.text = at;
        } else {
            e.text = 0u;
        }
        uint32_t s = t.find(e.id);
        if (s != kNoSlot) {
            t.set_value(s, e.kind == CLOCKWORK_PARAM_GROUP ? 0.f : held(e, t.value(s)));   // a redefine keeps the value, within the new range
        } else {
            s = t.first_free();
            t.set_value(s, held(e, e.def));
            ++live;
        }
        t.store_entry(s, e);
        if (slots) slots[i] = s;
    }
    word_of(t.header->text_used).store(text_end, std::memory_order_relaxed);
    word_of(t.header->count).store(live, std::memory_order_relaxed);
    gen.store(odd + 1u, std::memory_order_release);

    PerformanceMetrics& m = metrics_of(core);
    m.params_live.store(live, std::memory_order_relaxed);
    m.params_generation.store((odd + 1u) >> 1u, std::memory_order_relaxed);
    return static_cast<int32_t>(count);
}

// Why `origin` may not write this slot's value, or null.
const char* set_refused(const Table& t, uint32_t slot, float value, uint32_t origin) {
    if (slot >= t.capacity() || !t.entries[slot].live) return CLOCKWORK_PARAM_REFUSED_UNKNOWN;
    const ClockworkParamEntry& e = t.entries[slot];
    if (e.kind == CLOCKWORK_PARAM_GROUP) return CLOCKWORK_PARAM_REFUSED_UNKNOWN;   // a group holds no value
    if ((e.flags & CLOCKWORK_PARAM_READ_ONLY) != 0u && e.definer != origin) return CLOCKWORK_PARAM_REFUSED_READ_ONLY;
    if (!std::isfinite(value)) return CLOCKWORK_PARAM_REFUSED_RANGE;
    return nullptr;
}

// The value into its slot, and the count of moves on: what a reader sees. Nothing is sent.
void write_value(ClockworkCore* core, const Table& t, uint32_t slot, float value) {
    const float now = held(t.entries[slot], value);
    if (now == t.value(slot)) return;
    t.set_value(slot, now);
    metrics_of(core).param_moves.fetch_add(1u, std::memory_order_relaxed);
}

// ── The verbs ──────────────────────────────────────────────────────────────

struct Call {
    ClockworkCore* core;
    uint32_t token;
    const uint8_t* data;
    uint32_t len;
    void refuse(const char* reason) const {
        if (clockwork::arena_base(core)) metrics_of(core).params_refused.fetch_add(1u, std::memory_order_relaxed);
        ClockworkCore* const c = core;
        const uint32_t t = token;
        clockwork_sys_refuse(data, len, reason, arenaInstanceGeneration(clockwork::arena_base(core)),
            [c, t](const uint8_t* d, uint32_t n) { clockwork_emit_osc(c, t, d, n); });
    }
    void reply(const osc::OutboundPacketStream& ps) const {
        clockwork_emit_osc(core, token, reinterpret_cast<const uint8_t*>(ps.Data()), static_cast<uint32_t>(ps.Size()));
    }
};

void verb_define(const Call& call) {
    clockwork_host::OscReader r(call.data, call.len);
    const uint8_t* blob = nullptr; uint32_t size = 0;
    if (!r.ok() || !r.readBlob(blob, size) || size == 0 || size % sizeof(ClockworkParamEntry) != 0) {
        call.refuse(CLOCKWORK_REFUSED_MALFORMED);
        return;
    }
    // The text, when any entry has some, is a second blob.
    const uint8_t* text = nullptr; uint32_t text_bytes = 0;
    if ((r.peekType() != 0 && !r.readBlob(text, text_bytes)) || r.peekType() != 0) {
        call.refuse(CLOCKWORK_REFUSED_MALFORMED);
        return;
    }
    const char* why = nullptr;
    if (define(call.core, reinterpret_cast<const ClockworkParamEntry*>(blob),
               size / static_cast<uint32_t>(sizeof(ClockworkParamEntry)), text, text_bytes,
               CLOCKWORK_PARAM_DEFINER_CLIENT, nullptr, &why) < 0) {
        call.refuse(why);
        return;
    }
    const Table t = table_of(call.core);
    std::array<char, 64> buf{};
    osc::OutboundPacketStream ps(buf.data(), buf.size());
    ps << osc::BeginMessage(CLOCKWORK_SYS("param/define.reply"))
       << static_cast<osc::int32>(clockwork_params_generation(t.header))
       << static_cast<osc::int32>(t.header->count) << osc::EndMessage;
    call.reply(ps);
}

// A page at a time from the slot asked for, 0 by default, so one list cannot
// fill a small ring: "list.reply ,iii <generation> <count> <next>", next 0 when
// this is the last page, then the page's entries.
void verb_list(const Call& call) {
    if (!has_table(call.core)) { call.refuse(CLOCKWORK_REFUSED_NO_INSTANCE); return; }
    const Table t = table_of(call.core);
    clockwork_host::OscReader r(call.data, call.len);
    int32_t first = 0;
    if (!r.ok() || (r.peekType() != 0 && !r.readInt32(first)) || first < 0) {
        call.refuse(CLOCKWORK_REFUSED_MALFORMED);
        return;
    }
    // Where this page ends, and whether a live entry lies past it.
    uint32_t rows = 0, end = static_cast<uint32_t>(first), next = 0;
    for (uint32_t s = static_cast<uint32_t>(first); s < t.capacity(); ++s) {
        if (!t.entries[s].live) continue;
        if (rows == kListPage) { next = s; break; }
        ++rows;
        end = s + 1u;
    }
    {
        std::array<char, 64> buf{};
        osc::OutboundPacketStream ps(buf.data(), buf.size());
        ps << osc::BeginMessage(CLOCKWORK_SYS("param/list.reply"))
           << static_cast<osc::int32>(clockwork_params_generation(t.header))
           << static_cast<osc::int32>(t.header->count) << static_cast<osc::int32>(next) << osc::EndMessage;
        call.reply(ps);
    }
    for (uint32_t s = static_cast<uint32_t>(first); s < end; ++s) {
        const ClockworkParamEntry& e = t.entries[s];
        if (!e.live) continue;
        std::array<char, 160> buf{};
        osc::OutboundPacketStream ps(buf.data(), buf.size());
        ps << osc::BeginMessage(CLOCKWORK_SYS("param/entry"))
           << static_cast<osc::int32>(e.id) << e.name << static_cast<osc::int32>(e.kind)
           << e.min << e.max << e.def
           << static_cast<osc::int32>(e.unit) << static_cast<osc::int32>(e.flags) << static_cast<osc::int32>(e.group)
           << static_cast<osc::int32>(s) << t.value(s)
           << e.taper << static_cast<osc::int32>(e.role) << static_cast<osc::int32>(e.text_bytes) << osc::EndMessage;
        call.reply(ps);
    }
}

// One entry's run: "text.reply ,iib <id> <generation> <run>", empty when it has
// none. Apart from list, because a page of runs would not fit the smallest
// ring, and this thread is the writer, so the bytes go out from where they lie.
void verb_text(const Call& call) {
    if (!has_table(call.core)) { call.refuse(CLOCKWORK_REFUSED_NO_INSTANCE); return; }
    const Table t = table_of(call.core);
    clockwork_host::OscReader r(call.data, call.len);
    int32_t id = 0;
    if (!r.ok() || !r.readInt32(id)) { call.refuse(CLOCKWORK_REFUSED_MALFORMED); return; }
    const uint32_t s = t.find(static_cast<uint32_t>(id));
    if (s == kNoSlot) { call.refuse(CLOCKWORK_PARAM_REFUSED_UNKNOWN); return; }
    const ClockworkParamEntry& e = t.entries[s];
    const uint32_t bytes = e.text_bytes;
    std::array<char, CLOCKWORK_PARAM_TEXT_RUN_BYTES + 96> buf{};
    osc::OutboundPacketStream ps(buf.data(), buf.size());
    ps << osc::BeginMessage(CLOCKWORK_SYS("param/text.reply"))
       << id << static_cast<osc::int32>(clockwork_params_generation(t.header))
       << osc::Blob(bytes != 0u ? static_cast<const void*>(&t.text[e.text / 4u]) : static_cast<const void*>(buf.data()),
                    static_cast<osc::osc_bundle_element_size_t>(bytes))
       << osc::EndMessage;
    call.reply(ps);
}

void verb_set(const Call& call) {
    if (!has_table(call.core)) { call.refuse(CLOCKWORK_REFUSED_NO_INSTANCE); return; }
    const Table t = table_of(call.core);
    clockwork_host::OscReader decide(call.data, call.len);
    if (!decide.ok() || decide.peekType() == 0) { call.refuse(CLOCKWORK_REFUSED_MALFORMED); return; }
    while (decide.peekType() != 0) {
        int32_t id = 0; float v = 0.f;
        if (!decide.readInt32(id) || !decide.readFloat(v)) { call.refuse(CLOCKWORK_REFUSED_MALFORMED); return; }
        if (const char* no = set_refused(t, t.find(static_cast<uint32_t>(id)), v, CLOCKWORK_PARAM_DEFINER_CLIENT)) { call.refuse(no); return; }
    }
    clockwork_host::OscReader write(call.data, call.len);
    while (write.peekType() != 0) {
        int32_t id = 0; float v = 0.f;
        write.readInt32(id);
        write.readFloat(v);
        write_value(call.core, t, t.find(static_cast<uint32_t>(id)), v);
    }
}

} // namespace

namespace clockwork::params {

void reset(ClockworkCore* core) {
    uint8_t* const region = clockwork::arena_base(core) + PARAMS_START;
    word_of(reinterpret_cast<ClockworkParamsHeader*>(region)->magic).store(0u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    auto* const words = words_of(region);
    for (uint32_t w = 0; w < PARAMS_SIZE / 4u; ++w) words[w].store(0u, std::memory_order_relaxed);
    auto* h = reinterpret_cast<ClockworkParamsHeader*>(region);
    h->version        = CLOCKWORK_PARAMS_VERSION;
    h->capacity       = CLOCKWORK_PARAM_SLOTS;
    h->values_offset  = CLOCKWORK_PARAMS_VALUES_OFFSET;
    h->entries_offset = CLOCKWORK_PARAMS_ENTRIES_OFFSET(CLOCKWORK_PARAM_SLOTS);
    h->entry_bytes    = sizeof(ClockworkParamEntry);
    h->text_offset    = CLOCKWORK_PARAMS_TEXT_OFFSET(CLOCKWORK_PARAM_SLOTS);
    h->text_capacity  = CLOCKWORK_PARAM_TEXT_BYTES;
    word_of(h->magic).store(CLOCKWORK_PARAMS_MAGIC, std::memory_order_release);
}

bool route(void* routeCtx, const void* callCtx, const uint8_t* data, std::size_t len) {
    auto* const core = static_cast<ClockworkCore*>(routeCtx);
    if (!clockwork::state(core)) return true;
    const auto* cc = static_cast<const DrainCallCtx*>(callCtx);
    const Call call{ core, cc ? cc->sourceId : 0u, data, static_cast<uint32_t>(len) };
    const char* const verb = reinterpret_cast<const char*>(data) + CLOCKWORK_SYS_LEN("param/");
    const std::size_t rest = len - CLOCKWORK_SYS_LEN("param/");
    const auto is = [&](const char* v) {
        const std::size_t n = std::strlen(v);
        return rest > n && std::memcmp(verb, v, n) == 0 && verb[n] == '\0';
    };
    if (is("define"))    verb_define(call);
    else if (is("list")) verb_list(call);
    else if (is("set"))  verb_set(call);
    else if (is("text")) verb_text(call);
    else call.refuse(CLOCKWORK_REFUSED_UNKNOWN);
    return true;
}

} // namespace clockwork::params

// ── The C shape (clockwork_params.h) ────────────────────────────────────────

extern "C" const ClockworkParamsHeader* clockwork_params_header(const uint8_t* region) {
    if (!region) return nullptr;
    const auto* h = reinterpret_cast<const ClockworkParamsHeader*>(region);
    if (word_of(h->magic).load(std::memory_order_acquire) != CLOCKWORK_PARAMS_MAGIC) return nullptr;
    return h->version == CLOCKWORK_PARAMS_VERSION ? h : nullptr;
}

extern "C" uint32_t clockwork_params_generation(const ClockworkParamsHeader* h) {
    return h ? word_of(h->generation).load(std::memory_order_acquire) >> 1u : 0u;
}

extern "C" int clockwork_params_read_entry(const ClockworkParamsHeader* h, uint32_t slot, ClockworkParamEntry* out) {
    if (!h || !out || slot >= h->capacity) return 0;
    const auto* const entry = reinterpret_cast<const uint8_t*>(h) + h->entries_offset + slot * h->entry_bytes;
    const std::atomic<uint32_t>* const src = words_of(entry);
    std::atomic<uint32_t>& gen = word_of(h->generation);
    // A define is a bounded copy, so a few tries outlast one; a reader that
    // loses every try is told, and never sleeps here.
    for (int tries = 0; tries < 1024; ++tries) {
        const uint32_t before = gen.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) continue;
        std::array<uint32_t, kEntryWords> copy{};
        for (uint32_t w = 0; w < kEntryWords; ++w) copy[w] = src[w].load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (gen.load(std::memory_order_relaxed) != before) continue;
        std::memcpy(out, copy.data(), sizeof *out);
        return out->live ? 1 : 0;
    }
    return -1;
}

extern "C" int32_t clockwork_params_read_text(const ClockworkParamsHeader* h, uint32_t slot, uint8_t* out, uint32_t capacity,
                                              ClockworkParamEntry* entry) {
    if (!h || slot >= h->capacity) return 0;
    const auto* const base = reinterpret_cast<const uint8_t*>(h);
    const std::atomic<uint32_t>* const src = words_of(base + h->entries_offset + slot * h->entry_bytes);
    const std::atomic<uint32_t>* const text = words_of(base + h->text_offset);
    std::atomic<uint32_t>& gen = word_of(h->generation);
    for (int tries = 0; tries < 1024; ++tries) {
        const uint32_t before = gen.load(std::memory_order_acquire);
        if ((before & 1u) != 0u) continue;
        std::array<uint32_t, kEntryWords> copy{};
        for (uint32_t w = 0; w < kEntryWords; ++w) copy[w] = src[w].load(std::memory_order_relaxed);
        ClockworkParamEntry e;
        std::memcpy(&e, copy.data(), sizeof e);
        // A define may have moved these under this read, so they are not yet
        // to be believed: the generation says, below.
        const uint32_t bytes = e.live ? e.text_bytes : 0u;
        const bool inside = bytes <= h->text_capacity && e.text <= h->text_capacity - bytes;
        const bool fits = out != nullptr && bytes <= capacity;
        if (inside && fits) {
            for (uint32_t done = 0; done < bytes; done += 4u) {
                const uint32_t bits = text[(e.text + done) / 4u].load(std::memory_order_relaxed);
                std::memcpy(out + done, &bits, std::min<uint32_t>(4u, bytes - done));
            }
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (gen.load(std::memory_order_relaxed) != before) continue;
        if (entry) *entry = e;
        if (bytes != 0u && !fits) return -2;
        return static_cast<int32_t>(bytes);
    }
    return -1;
}

extern "C" float clockwork_params_value(const ClockworkParamsHeader* h, uint32_t slot) {
    if (!h || slot >= h->capacity) return 0.f;
    const auto* const values = words_of(reinterpret_cast<const uint8_t*>(h) + h->values_offset);
    return to_float(values[slot].load(std::memory_order_relaxed));
}

extern "C" const ClockworkParamsHeader* clockwork_params_table(const ClockworkCore* core) {
    if (!core || !has_table(core)) return nullptr;
    return clockwork_params_header(clockwork::arena_base(core) + PARAMS_START);
}

extern "C" float clockwork_param_value(const ClockworkCore* core, uint32_t slot) {
    return clockwork_params_value(clockwork_params_table(core), slot);
}

extern "C" int32_t clockwork_param_define(ClockworkCore* core, const ClockworkParamEntry* entries, uint32_t count,
                                          const uint8_t* text, uint32_t text_bytes,
                                          uint32_t definer, uint32_t* slots, const char** why) {
    if (!core || !entries) { if (why) *why = CLOCKWORK_REFUSED_MALFORMED; return -1; }
    const int32_t n = define(core, entries, count, text, text_bytes, definer, slots, why);
    if (n < 0 && clockwork::arena_base(core)) metrics_of(core).params_refused.fetch_add(1u, std::memory_order_relaxed);
    return n;
}

extern "C" int clockwork_param_set(ClockworkCore* core, uint32_t slot, float value, uint32_t origin) {
    if (!core || !has_table(core)) return 0;
    const Table t = table_of(core);
    if (!is_definer(origin) || set_refused(t, slot, value, origin) != nullptr) {
        metrics_of(core).params_refused.fetch_add(1u, std::memory_order_relaxed);
        return 0;
    }
    write_value(core, t, slot, value);
    return 1;
}
