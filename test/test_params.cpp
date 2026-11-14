// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_params.cpp — the parameter table (docs/PARAMS.md), driven through the
 * lanes ABI a block at a time: the three verbs under /clockwork/param/, the
 * guest's three calls, and the region a reader maps: the table is read, and
 * never announced. test_params_engine.cpp runs the verbs on a full native engine.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "clockwork_arena.h"
#include "clockwork_prefix.h"
#include "clockwork_sys.h"
#include "lanes/lanes.h"
#include "metrics_schema.h"
#include "params/clockwork_params.h"
#include "params/params.h"
#include "shared_memory.h"

using Catch::Approx;

namespace {

// The client's connection, and the connection it has after it comes back: the table knows a client, not which.
constexpr uint32_t kPanel = 0x9a7e1;
constexpr uint32_t kHost  = 0x4057;

using Frames = std::vector<lanes_test::EgressFrame>;

std::vector<osc_test::ParsedReply> at(const Frames& frames, const char* address) {
    std::vector<osc_test::ParsedReply> out;
    for (const auto& f : frames) {
        osc_test::ParsedReply r = osc_test::parseReply(f.data.data(), static_cast<uint32_t>(f.data.size()));
        if (r.address == address) out.push_back(r);
    }
    return out;
}

const lanes_test::EgressFrame* frameAt(const Frames& frames, const char* address) {
    for (const auto& f : frames)
        if (osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size())) == address)
            return &f;
    return nullptr;
}

ClockworkParamEntry entry(uint32_t id, const char* name, uint32_t kind,
                          float min, float max, float def,
                          uint32_t unit = CLOCKWORK_PARAM_UNIT_NONE,
                          uint32_t flags = CLOCKWORK_PARAM_AUTOMATABLE, uint32_t group = 0) {
    ClockworkParamEntry e{};
    e.id = id; e.kind = kind; e.unit = unit; e.flags = flags; e.group = group;
    e.min = min; e.max = max; e.def = def;
    std::snprintf(e.name, sizeof e.name, "%s", name);
    return e;
}

ClockworkParamEntry retire(uint32_t id) { ClockworkParamEntry e{}; e.id = id; return e; }

osc_test::Packet defineOf(const std::vector<ClockworkParamEntry>& entries) {
    return osc_test::messageWithBlob(CLOCKWORK_SYS("param/define"), entries.data(),
                                     entries.size() * sizeof(ClockworkParamEntry));
}

osc_test::Packet setOf(uint32_t id, float value) {
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("param/set")) << static_cast<osc::int32>(id) << value;
    return b.end();
}

// Send from `who`, tick once, return what came out.
Frames ask(const osc_test::Packet& p, uint32_t who) {
    REQUIRE(lanes_test::ingress(p.ptr(), p.size(), who));
    lanes_test::defaultLane().tick();
    return lanes_test::drainRt();
}

const ClockworkParamsHeader* table(ClockworkCore* core) {
    const ClockworkParamsHeader* t = clockwork_params_table(core);
    REQUIRE(t != nullptr);
    return t;
}
const ClockworkParamsHeader* table() { return table(lanes_test::core()); }

ClockworkParamEntry entryIn(const ClockworkParamsHeader* t, uint32_t slot) {
    ClockworkParamEntry e{};
    REQUIRE(clockwork_params_read_entry(t, slot, &e) >= 0);
    return e;
}

int slotOf(const ClockworkParamsHeader* t, uint32_t id) {
    for (uint32_t s = 0; s < t->capacity; ++s) {
        const ClockworkParamEntry e = entryIn(t, s);
        if (e.live && e.id == id) return static_cast<int>(s);
    }
    return -1;
}
int slotOf(uint32_t id) { return slotOf(table(), id); }

float valueOf(uint32_t id) {
    const int s = slotOf(id);
    REQUIRE(s >= 0);
    return clockwork_params_value(table(), static_cast<uint32_t>(s));
}

// Every entry retired by whoever holds it, so a case starts from an empty table.
void clearTable() {
    lanes_test::boot();
    const ClockworkParamsHeader* t = table();
    // A group goes after its members, and the deepest first: one pass per depth.
    for (uint32_t pass = 0; pass <= CLOCKWORK_PARAM_GROUP_DEPTH; ++pass) {
        for (uint32_t s = 0; s < t->capacity; ++s) {
            const ClockworkParamEntry e = entryIn(t, s);
            if (!e.live) continue;
            const ClockworkParamEntry gone = retire(e.id);
            clockwork_param_define(lanes_test::core(), &gone, 1, nullptr, 0, e.definer, nullptr, nullptr);
        }
    }
    REQUIRE(t->count == 0);
    lanes_test::drainRt();
}

ClockworkParamEntry groupEntry(uint32_t id, const char* name, uint32_t parent = 0) {
    return entry(id, name, CLOCKWORK_PARAM_GROUP, 0.f, 0.f, 0.f, CLOCKWORK_PARAM_UNIT_NONE, 0u, parent);
}

// One entry's text: each string's tag, its bytes and a NUL.
std::string run(std::initializer_list<std::pair<uint8_t, std::string>> strings) {
    std::string out;
    for (const auto& [tag, words] : strings) {
        out.push_back(static_cast<char>(tag));
        out += words;
        out.push_back('\0');
    }
    return out;
}

// A define with text: each entry's run appended to the blob it points into.
struct Define {
    std::vector<ClockworkParamEntry> entries;
    std::string blob;
    Define& add(ClockworkParamEntry e, const std::string& text = {}) {
        e.text = static_cast<uint32_t>(blob.size());
        e.text_bytes = static_cast<uint32_t>(text.size());
        blob += text;
        entries.push_back(e);
        return *this;
    }
    osc_test::Packet packet() const {
        osc_test::Builder b;
        b.begin(CLOCKWORK_SYS("param/define"))
            << osc::Blob(entries.data(), static_cast<osc::osc_bundle_element_size_t>(entries.size() * sizeof(ClockworkParamEntry)))
            << osc::Blob(blob.data(), static_cast<osc::osc_bundle_element_size_t>(blob.size()));
        return b.end();
    }
};

// The reason `who`'s define or set was refused, or nothing when it landed.
std::string refusal(const osc_test::Packet& p, uint32_t who = kPanel) {
    const auto err = at(ask(p, who), CLOCKWORK_SYS("error"));
    return err.empty() ? std::string{} : err[0].argString(1);
}

std::string textOf(uint32_t id) {
    const int s = slotOf(id);
    REQUIRE(s >= 0);
    std::vector<uint8_t> out(CLOCKWORK_PARAM_TEXT_RUN_BYTES);
    const int32_t n = clockwork_params_read_text(table(), static_cast<uint32_t>(s), out.data(),
                                                 static_cast<uint32_t>(out.size()), nullptr);
    REQUIRE(n >= 0);
    return std::string(reinterpret_cast<const char*>(out.data()), static_cast<size_t>(n));
}

ClockworkParamEntry tapered(ClockworkParamEntry e, float taper) { e.taper = taper; return e; }
ClockworkParamEntry withRole(ClockworkParamEntry e, uint32_t role) { e.role = role; return e; }

} // namespace

// ── The region ──────────────────────────────────────────────────────────────

TEST_CASE("params: the region is in the table, public, and laid out as the header says", "[params][arena]") {
    lanes_test::boot();
    const ClockworkArenaHeader* h = clockwork_arena_header(lanes_test::core());
    const ClockworkArenaEntry* e = clockwork_arena_find(h, CLOCKWORK_ARENA_CLOCKWORK_PARAMS);
    REQUIRE(e != nullptr);
    CHECK(clockwork_arena_public(h, CLOCKWORK_ARENA_CLOCKWORK_PARAMS));
    CHECK(clockwork_arena_area_of(h, e) == CLOCKWORK_AREA_CLOCKWORK_STATE);
    CHECK(e->owner == CLOCKWORK_OWNER_CLOCKWORK);
    CHECK(e->lifetime == CLOCKWORK_LIFETIME_COLD_BOOT);
    CHECK(e->geom[CLOCKWORK_GEOM_PARAMS_CAPACITY] == CLOCKWORK_PARAM_SLOTS);
    CHECK(e->geom[CLOCKWORK_GEOM_PARAMS_ENTRY_BYTES] == sizeof(ClockworkParamEntry));
    CHECK(e->geom[CLOCKWORK_GEOM_PARAMS_HEADER_BYTES] == sizeof(ClockworkParamsHeader));

    const ClockworkParamsHeader* t = clockwork_params_header(reinterpret_cast<const uint8_t*>(h) + e->offset);
    REQUIRE(t != nullptr);
    CHECK(t == table());
    CHECK(t->version == CLOCKWORK_PARAMS_VERSION);
    CHECK(t->capacity == CLOCKWORK_PARAM_SLOTS);
    CHECK(t->values_offset == sizeof(ClockworkParamsHeader));
    CHECK(t->entries_offset % 16 == 0);
    CHECK(t->entries_offset >= t->values_offset + t->capacity * 4u);
    CHECK(t->entries_offset + t->capacity * sizeof(ClockworkParamEntry) <= e->bytes);
    CHECK(t->entry_bytes == sizeof(ClockworkParamEntry));
    CHECK(clockwork_params_header(nullptr) == nullptr);
    const uint8_t notATable[64] = {};
    CHECK(clockwork_params_header(notATable) == nullptr);
}

// ── define ──────────────────────────────────────────────────────────────────

TEST_CASE("params: a define takes slots, a redefine keeps a slot and its value, a retire frees it", "[params]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    const uint32_t gen0 = clockwork_params_generation(t), count0 = t->count;

    auto out = ask(defineOf({ entry(10, "cutoff", CLOCKWORK_PARAM_CONTINUOUS, 20.f, 20000.f, 1000.f, CLOCKWORK_PARAM_UNIT_HERTZ),
                              entry(11, "drive",  CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.25f) }), kPanel);
    auto reply = at(out, CLOCKWORK_SYS("param/define.reply"));
    REQUIRE(reply.size() == 1);
    CHECK(reply[0].argInt(0) == static_cast<int32_t>(gen0 + 1));
    CHECK(reply[0].argInt(1) == static_cast<int32_t>(count0 + 2));
    CHECK(clockwork_params_generation(t) == gen0 + 1);
    CHECK(t->count == count0 + 2);
    // The asker is answered and nobody else is told: a reader sees the
    // generation move, and has every default from there.
    CHECK(out.size() == 1);

    const int slot10 = slotOf(10), slot11 = slotOf(11);
    REQUIRE(slot10 >= 0); REQUIRE(slot11 >= 0); CHECK(slot10 != slot11);
    CHECK(valueOf(10) == Approx(1000.f));
    const ClockworkParamEntry e10 = entryIn(t, static_cast<uint32_t>(slot10));
    CHECK(std::string(e10.name) == "cutoff");
    CHECK(e10.unit == CLOCKWORK_PARAM_UNIT_HERTZ);
    CHECK(e10.definer == CLOCKWORK_PARAM_DEFINER_CLIENT);

    // Move the value, then redefine: the slot and the value stay, the facts change.
    ask(setOf(10, 440.f), kPanel);
    CHECK(valueOf(10) == Approx(440.f));
    out = ask(defineOf({ entry(10, "cutoff hz", CLOCKWORK_PARAM_CONTINUOUS, 20.f, 20000.f, 2000.f, CLOCKWORK_PARAM_UNIT_HERTZ) }), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("param/define.reply")).size() == 1);
    CHECK(slotOf(10) == slot10);
    CHECK(valueOf(10) == Approx(440.f));
    CHECK(std::string(entryIn(t, static_cast<uint32_t>(slot10)).name) == "cutoff hz");
    CHECK(clockwork_params_generation(t) == gen0 + 2);
    CHECK(t->count == count0 + 2);

    // A redefine holds the value it keeps to the new range.
    ask(defineOf({ entry(10, "cutoff hz", CLOCKWORK_PARAM_CONTINUOUS, 20.f, 100.f, 50.f, CLOCKWORK_PARAM_UNIT_HERTZ) }), kPanel);
    CHECK(valueOf(10) == Approx(100.f));

    // Retire one: the other stays where it was, and a new id takes the free slot.
    out = ask(defineOf({ retire(10) }), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("param/define.reply")).size() == 1);
    CHECK(slotOf(10) == -1);
    CHECK(slotOf(11) == slot11);
    CHECK(t->count == count0 + 1);
    ask(defineOf({ entry(12, "mix", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.5f) }), kPanel);
    CHECK(slotOf(12) == slot10);
    CHECK(slotOf(11) == slot11);
    CHECK(valueOf(12) == Approx(0.5f));
}

TEST_CASE("params: what the client defined is the client's on whichever connection, and the guest's is not", "[params]") {
    clearTable();
    ask(defineOf({ entry(20, "level", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.5f) }), kPanel);
    ask(setOf(20, 0.25f), kPanel);
    const ClockworkParamsHeader* t = table();
    const int slot = slotOf(20);

    // The client comes back on another connection: its entries are still its own to redefine, in the slot they had.
    auto out = ask(defineOf({ entry(21, "pan",   CLOCKWORK_PARAM_CONTINUOUS, -1.f, 1.f, 0.f),
                              entry(20, "level", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 2.f, 0.5f) }), kHost);
    CHECK(at(out, CLOCKWORK_SYS("error")).empty());
    REQUIRE(at(out, CLOCKWORK_SYS("param/define.reply")).size() == 1);
    CHECK(slotOf(20) == slot);
    CHECK(valueOf(20) == Approx(0.25f));
    CHECK(entryIn(t, static_cast<uint32_t>(slot)).max == Approx(2.f));
    CHECK(slotOf(21) >= 0);
    // And to retire.
    out = ask(defineOf({ retire(20) }), kHost);
    CHECK(at(out, CLOCKWORK_SYS("error")).empty());
    CHECK(slotOf(20) == -1);

    // The guest's are the guest's: the client may not redefine or retire one, and nothing of that define lands.
    const ClockworkParamEntry guests = entry(22, "drive", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f);
    REQUIRE(clockwork_param_define(lanes_test::core(), &guests, 1, nullptr, 0, CLOCKWORK_PARAM_DEFINER_GUEST, nullptr, nullptr) == 1);
    const uint32_t gen = clockwork_params_generation(t), count = t->count;
    out = ask(defineOf({ entry(23, "new", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f),
                         entry(22, "drive", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 9.f, 0.f) }), kPanel);
    auto err = at(out, CLOCKWORK_SYS("error"));
    REQUIRE(err.size() == 1);
    CHECK(err[0].argString(0) == CLOCKWORK_SYS("param/define"));
    CHECK(err[0].argString(1) == std::string(CLOCKWORK_PARAM_REFUSED_HELD));
    CHECK(at(out, CLOCKWORK_SYS("param/define.reply")).empty());
    CHECK(slotOf(23) == -1);
    CHECK(clockwork_params_generation(t) == gen);
    CHECK(t->count == count);
    REQUIRE(at(ask(defineOf({ retire(22) }), kPanel), CLOCKWORK_SYS("error")).size() == 1);
    CHECK(slotOf(22) >= 0);

    // A definer is the guest, the host or the client, and nothing else.
    const ClockworkParamEntry stray = entry(24, "stray", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f);
    CHECK(clockwork_param_define(lanes_test::core(), &stray, 1, nullptr, 0, 0x1234u, nullptr, nullptr) == -1);
    CHECK(slotOf(24) == -1);
    CHECK(clockwork_param_set(lanes_test::core(), static_cast<uint32_t>(slotOf(21)), 0.5f, 0x1234u) == 0);
    CHECK(valueOf(21) == Approx(0.f));
}

TEST_CASE("params: a define that cannot land is refused with the reason and changes nothing", "[params]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    const uint32_t gen = clockwork_params_generation(t), count = t->count;
    const auto refusedFor = [](const osc_test::Packet& p) {
        const auto err = at(ask(p, kPanel), CLOCKWORK_SYS("error"));
        REQUIRE(err.size() == 1);
        return err[0].argString(1);
    };
    const uint32_t kind = CLOCKWORK_PARAM_CONTINUOUS;

    std::vector<ClockworkParamEntry> tooMany;
    for (uint32_t i = 0; i <= t->capacity - t->count; ++i) tooMany.push_back(entry(1000 + i, "p", kind, 0.f, 1.f, 0.f));
    CHECK(refusedFor(defineOf(tooMany)) == CLOCKWORK_PARAM_REFUSED_FULL);

    CHECK(refusedFor(defineOf({ entry(30, "upside down", kind, 1.f, 0.f, 0.5f) })) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(refusedFor(defineOf({ entry(30, "default outside", kind, 0.f, 1.f, 2.f) })) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(refusedFor(defineOf({ entry(30, "nan", kind, 0.f, std::nanf(""), 0.f) })) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(refusedFor(defineOf({ entry(30, "wide switch", CLOCKWORK_PARAM_SWITCH, 0.f, 2.f, 0.f) })) == CLOCKWORK_PARAM_REFUSED_RANGE);

    ClockworkParamEntry longName = entry(31, "", kind, 0.f, 1.f, 0.f);
    std::memset(longName.name, 'x', sizeof longName.name);   // no NUL within the name
    CHECK(refusedFor(defineOf({ longName })) == CLOCKWORK_PARAM_REFUSED_NAME);

    CHECK(refusedFor(defineOf({ entry(32, "no kind", 99u, 0.f, 1.f, 0.f) })) == CLOCKWORK_PARAM_REFUSED_KIND);
    CHECK(refusedFor(defineOf({ entry(32, "no unit", kind, 0.f, 1.f, 0.f, 99u) })) == CLOCKWORK_PARAM_REFUSED_KIND);
    CHECK(refusedFor(defineOf({ entry(32, "no flag", kind, 0.f, 1.f, 0.f, CLOCKWORK_PARAM_UNIT_NONE, 1u << 9) })) == CLOCKWORK_PARAM_REFUSED_KIND);

    // Not a whole number of entries, not a blob at all, no id, an id twice.
    const uint8_t half[sizeof(ClockworkParamEntry) / 2] = {};
    CHECK(refusedFor(osc_test::messageWithBlob(CLOCKWORK_SYS("param/define"), half, sizeof half)) == CLOCKWORK_REFUSED_MALFORMED);
    CHECK(refusedFor(osc_test::message(CLOCKWORK_SYS("param/define"), int32_t{ 1 })) == CLOCKWORK_REFUSED_MALFORMED);
    CHECK(refusedFor(defineOf({ entry(0, "zero", kind, 0.f, 1.f, 0.f) })) == CLOCKWORK_REFUSED_MALFORMED);
    CHECK(refusedFor(defineOf({ entry(33, "a", kind, 0.f, 1.f, 0.f), entry(33, "b", kind, 0.f, 1.f, 0.f) })) == CLOCKWORK_REFUSED_MALFORMED);

    // A retire of an id not there.
    CHECK(refusedFor(defineOf({ retire(34) })) == CLOCKWORK_PARAM_REFUSED_UNKNOWN);

    CHECK(t->count == count);
    CHECK(clockwork_params_generation(t) == gen);
}

TEST_CASE("params: a retire makes room for a new id in the same define, at capacity", "[params]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    const uint32_t cap = t->capacity;
    // A blob is bounded by what a ring takes, so the table fills a few at a time.
    for (uint32_t i = 0; i < cap; i += 16) {
        std::vector<ClockworkParamEntry> some;
        for (uint32_t j = i; j < i + 16 && j < cap; ++j) some.push_back(entry(2000 + j, "p", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f));
        REQUIRE(at(ask(defineOf(some), kPanel), CLOCKWORK_SYS("param/define.reply")).size() == 1);
    }
    REQUIRE(t->count == cap);
    const int freed = slotOf(2000 + 3);
    REQUIRE(at(ask(defineOf({ entry(9000, "new", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) }), kPanel), CLOCKWORK_SYS("error")).size() == 1);

    // The new entry first in the blob: the retire still frees the slot it takes.
    const auto out = ask(defineOf({ entry(9000, "new", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.25f), retire(2000 + 3) }), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("param/define.reply")).size() == 1);
    CHECK(t->count == cap);
    CHECK(slotOf(2000 + 3) == -1);
    CHECK(slotOf(9000) == freed);
    CHECK(valueOf(9000) == Approx(0.25f));
}

// ── set ─────────────────────────────────────────────────────────────────────

TEST_CASE("params: a set is held to the range and shaped by the kind", "[params]") {
    clearTable();
    ask(defineOf({ entry(40, "gain",   CLOCKWORK_PARAM_CONTINUOUS, -60.f, 6.f, 0.f, CLOCKWORK_PARAM_UNIT_DECIBELS),
                   entry(41, "steps",  CLOCKWORK_PARAM_STEPPED, 0.f, 7.f, 3.f),
                   entry(42, "on",     CLOCKWORK_PARAM_SWITCH, 0.f, 1.f, 0.f),
                   entry(43, "root",   CLOCKWORK_PARAM_NOTE, 0.f, 127.f, 60.f) }), kPanel);

    auto out = ask(setOf(40, 100.f), kPanel);
    CHECK(valueOf(40) == Approx(6.f));                  // the value as held, not as asked
    CHECK(out.empty());                                 // a set is answered by the table, and announced to nobody

    ask(setOf(40, -1000.f), kPanel);  CHECK(valueOf(40) == Approx(-60.f));
    ask(setOf(41, 2.6f), kPanel);     CHECK(valueOf(41) == Approx(3.f));
    ask(setOf(41, 2.5f), kPanel);     CHECK(valueOf(41) == Approx(3.f));
    ask(setOf(41, 9.f), kPanel);      CHECK(valueOf(41) == Approx(7.f));
    ask(setOf(42, 0.7f), kPanel);     CHECK(valueOf(42) == Approx(1.f));
    ask(setOf(42, 0.2f), kPanel);     CHECK(valueOf(42) == Approx(0.f));
    ask(setOf(43, 60.4f), kPanel);    CHECK(valueOf(43) == Approx(60.f));
    ask(setOf(43, 200.f), kPanel);    CHECK(valueOf(43) == Approx(127.f));

    // A value that is not finite is no value: refused, the value untouched.
    for (const float bad : { std::nanf(""), HUGE_VALF }) {
        out = ask(setOf(40, bad), kPanel);
        CHECK(valueOf(40) == Approx(-60.f));
        CHECK(at(out, CLOCKWORK_SYS("error")).size() == 1);
    }

    // Many in one message, and anyone may set another's.
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("param/set")) << osc::int32(40) << -6.f << osc::int32(41) << 5.f;
    out = ask(b.end(), kHost);
    CHECK(valueOf(40) == Approx(-6.f));
    CHECK(valueOf(41) == Approx(5.f));
    CHECK(out.empty());
}

TEST_CASE("params: a set is all or none", "[params]") {
    clearTable();
    ask(defineOf({ entry(44, "a", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) }), kPanel);
    osc_test::Builder b;
    b.begin(CLOCKWORK_SYS("param/set")) << osc::int32(44) << 1.f << osc::int32(45) << 1.f;
    const auto out = ask(b.end(), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("error")).size() == 1);
    CHECK(at(out, CLOCKWORK_SYS("param/changed")).empty());
    CHECK(valueOf(44) == Approx(0.f));
}

TEST_CASE("params: an unknown id and another's read-only entry refuse a client's set", "[params]") {
    clearTable();
    // The guest's meter: the guest writes it, and the client may not.
    const ClockworkParamEntry meter = entry(50, "meter", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f,
                                            CLOCKWORK_PARAM_UNIT_NONE, CLOCKWORK_PARAM_READ_ONLY);
    REQUIRE(clockwork_param_define(lanes_test::core(), &meter, 1, nullptr, 0, CLOCKWORK_PARAM_DEFINER_GUEST, nullptr, nullptr) == 1);
    auto out = ask(setOf(50, 0.5f), kPanel);
    auto err = at(out, CLOCKWORK_SYS("error"));
    REQUIRE(err.size() == 1);
    CHECK(err[0].argString(0) == CLOCKWORK_SYS("param/set"));
    CHECK(err[0].argString(1) == std::string(CLOCKWORK_PARAM_REFUSED_READ_ONLY));
    CHECK(valueOf(50) == Approx(0.f));
    CHECK(clockwork_param_set(lanes_test::core(), static_cast<uint32_t>(slotOf(50)), 0.5f, CLOCKWORK_PARAM_DEFINER_GUEST) == 1);
    CHECK(valueOf(50) == Approx(0.5f));
    // The client's own read-only entry is the client's to write, on whichever connection.
    ask(defineOf({ entry(52, "reading", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f,
                         CLOCKWORK_PARAM_UNIT_NONE, CLOCKWORK_PARAM_READ_ONLY) }), kPanel);
    out = ask(setOf(52, 0.75f), kHost);
    CHECK(at(out, CLOCKWORK_SYS("error")).empty());
    CHECK(valueOf(52) == Approx(0.75f));
    // And not the guest's.
    CHECK(clockwork_param_set(lanes_test::core(), static_cast<uint32_t>(slotOf(52)), 0.1f, CLOCKWORK_PARAM_DEFINER_GUEST) == 0);

    out = ask(setOf(51, 0.5f), kPanel);
    err = at(out, CLOCKWORK_SYS("error"));
    REQUIRE(err.size() == 1);
    CHECK(err[0].argString(1) == std::string(CLOCKWORK_PARAM_REFUSED_UNKNOWN));
    // A malformed set: no pairs, or half of one.
    out = ask(osc_test::message(CLOCKWORK_SYS("param/set")), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("error")).size() == 1);
    CHECK(at(out, CLOCKWORK_SYS("error"))[0].argString(1) == std::string(CLOCKWORK_REFUSED_MALFORMED));
    out = ask(osc_test::message(CLOCKWORK_SYS("param/set"), int32_t{ 50 }), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("error")).size() == 1);
    CHECK(at(out, CLOCKWORK_SYS("error"))[0].argString(1) == std::string(CLOCKWORK_REFUSED_MALFORMED));
}

#if CLOCKWORK_SCHEDULER
TEST_CASE("params: a timed set, through /clockwork/schedule, lands in the block of its sample", "[params][scheduler]") {
    clearTable();
    ask(defineOf({ entry(60, "cutoff", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) }), kPanel);
    lanes_test::Lane& lane = lanes_test::defaultLane();
    // A sample inside the third block from now, clear of the block's edges.
    const uint64_t target = lane.position() + 2ull * lane.block() + lane.block() / 2u;
    const auto inner = setOf(60, 0.75f);
    osc_test::Builder sched;
    sched.begin(CLOCKWORK_SYS("schedule")) << lane.ntpAt(target)
          << osc::Blob(inner.ptr(), static_cast<osc::osc_bundle_element_size_t>(inner.size()));
    const auto b = sched.end();
    REQUIRE(lanes_test::ingress(b.ptr(), b.size(), kPanel));
    lane.tick(); CHECK(valueOf(60) == Approx(0.f));
    lane.tick(); CHECK(valueOf(60) == Approx(0.f));
    lane.tick(); CHECK(valueOf(60) == Approx(0.75f));
    CHECK(lanes_test::drainRt().empty());
}
#endif

// ── list ────────────────────────────────────────────────────────────────────

TEST_CASE("params: list answers the generation and one entry per live slot, to the asker", "[params]") {
    clearTable();
    ask(defineOf({ groupEntry(7, "g"),
                   entry(70, "a", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 100.f, 50.f, CLOCKWORK_PARAM_UNIT_PERCENT, CLOCKWORK_PARAM_AUTOMATABLE, 7),
                   entry(71, "b", CLOCKWORK_PARAM_SWITCH, 0.f, 1.f, 1.f, CLOCKWORK_PARAM_UNIT_NONE, CLOCKWORK_PARAM_HIDDEN) }), kPanel);
    ask(setOf(70, 25.f), kHost);
    const ClockworkParamsHeader* t = table();
    auto out = ask(osc_test::message(CLOCKWORK_SYS("param/list")), kHost);
    auto head = at(out, CLOCKWORK_SYS("param/list.reply"));
    REQUIRE(head.size() == 1);
    CHECK(head[0].argInt(0) == static_cast<int32_t>(clockwork_params_generation(t)));
    CHECK(head[0].argInt(1) == static_cast<int32_t>(t->count));
    CHECK(head[0].argInt(2) == 0);
    auto rows = at(out, CLOCKWORK_SYS("param/entry"));
    REQUIRE(rows.size() == t->count);
    const osc_test::ParsedReply* a = nullptr;
    for (const auto& r : rows) if (r.argInt(0) == 70) a = &r;
    REQUIRE(a != nullptr);
    // id name kind min max default unit flags group slot value
    CHECK(a->argString(1) == "a");
    CHECK(a->argInt(2) == CLOCKWORK_PARAM_CONTINUOUS);
    CHECK(a->argFloat(3) == Approx(0.f));
    CHECK(a->argFloat(4) == Approx(100.f));
    CHECK(a->argFloat(5) == Approx(50.f));
    CHECK(a->argInt(6) == CLOCKWORK_PARAM_UNIT_PERCENT);
    CHECK(a->argInt(7) == CLOCKWORK_PARAM_AUTOMATABLE);
    CHECK(a->argInt(8) == 7);
    CHECK(a->argInt(9) == slotOf(70));
    CHECK(a->argFloat(10) == Approx(25.f));
    // Every frame of the answer went to the asker, none to the audience.
    for (const auto& f : out) {
        const auto addr = osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size()));
        if (addr == CLOCKWORK_SYS("param/entry") || addr == CLOCKWORK_SYS("param/list.reply")) {
            CHECK(f.sourceId == kHost);
        }
    }
}

TEST_CASE("params: list answers a page at a time, so it fits the smallest ring", "[params]") {
    clearTable();
    std::vector<ClockworkParamEntry> many;
    constexpr uint32_t kMany = 40;
    for (uint32_t i = 0; i < kMany; ++i) many.push_back(entry(3000 + i, "p", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f));
    REQUIRE(at(ask(defineOf(many), kPanel), CLOCKWORK_SYS("param/define.reply")).size() == 1);

    std::vector<int32_t> ids;
    int32_t from = 0, pages = 0;
    do {
        const auto out = ask(osc_test::message(CLOCKWORK_SYS("param/list"), from), kPanel);
        const auto head = at(out, CLOCKWORK_SYS("param/list.reply"));
        REQUIRE(head.size() == 1);
        CHECK(head[0].argInt(1) == static_cast<int32_t>(kMany));
        const auto rows = at(out, CLOCKWORK_SYS("param/entry"));
        CHECK(rows.size() <= 16);
        for (const auto& r : rows) ids.push_back(r.argInt(0));
        from = head[0].argInt(2);
        ++pages;
    } while (from != 0 && pages < 8);
    CHECK(pages == 3);
    REQUIRE(ids.size() == kMany);
    for (uint32_t i = 0; i < kMany; ++i) CHECK(ids[i] == static_cast<int32_t>(3000 + i));

    // Past the table is an empty last page; before it is malformed.
    auto out = ask(osc_test::message(CLOCKWORK_SYS("param/list"), int32_t{ 1 << 20 }), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("param/list.reply")).size() == 1);
    CHECK(at(out, CLOCKWORK_SYS("param/entry")).empty());
    out = ask(osc_test::message(CLOCKWORK_SYS("param/list"), int32_t{ -1 }), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("error")).size() == 1);
    CHECK(at(out, CLOCKWORK_SYS("param/list.reply")).empty());
}

// ── who is told ─────────────────────────────────────────────────────────────

TEST_CASE("params: replies and refusals go to the asker, and a change is announced to nobody", "[params][audience]") {
    clearTable();
    auto out = ask(defineOf({ entry(80, "a", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) }), kPanel);
    const auto* reply = frameAt(out, CLOCKWORK_SYS("param/define.reply"));
    REQUIRE(reply != nullptr);
    CHECK(reply->sourceId == kPanel);
    // The table is how a change is seen: its generation for the set of entries, its values for a value.
    CHECK(out.size() == 1);
    out = ask(setOf(80, 1.f), kHost);
    CHECK(out.empty());
    CHECK(valueOf(80) == Approx(1.f));
    out = ask(setOf(81, 1.f), kHost);
    const auto* err = frameAt(out, CLOCKWORK_SYS("error"));
    REQUIRE(err != nullptr);
    CHECK(err->sourceId == kHost);
}

// ── a reader ────────────────────────────────────────────────────────────────

TEST_CASE("params: a value moving does not move the generation", "[params][reader]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    ask(defineOf({ entry(90, "a", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) }), kPanel);
    const uint32_t gen = clockwork_params_generation(t);
    ask(setOf(90, 0.5f), kPanel);
    CHECK(clockwork_params_generation(t) == gen);
    CHECK(clockwork_params_value(t, static_cast<uint32_t>(slotOf(90))) == Approx(0.5f));
    CHECK(clockwork_params_value(t, t->capacity) == 0.f);
    ClockworkParamEntry outside{};
    CHECK(clockwork_params_read_entry(t, t->capacity, &outside) == 0);
}

// A reader on another thread, as a mapped panel is, while defines rewrite the
// same entries. Every fact of an entry comes from one define, so an entry that
// mixes two was torn. The count of reads says the reader did look, and the
// count of generations it saw that it looked while the table moved.
TEST_CASE("params: a reader on another thread never sees a torn entry", "[params][reader][threads]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    constexpr uint32_t kSlots = 8;
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> reads{ 0 }, torn{ 0 }, generations{ 0 };
    std::thread reader([&] {
        uint32_t last = clockwork_params_generation(t);
        while (!stop.load(std::memory_order_relaxed)) {
            for (uint32_t s = 0; s < kSlots; ++s) {
                ClockworkParamEntry e{};
                if (clockwork_params_read_entry(t, s, &e) != 1) continue;
                reads.fetch_add(1, std::memory_order_relaxed);
                char want[CLOCKWORK_PARAM_NAME_BYTES];
                std::snprintf(want, sizeof want, "round %u", static_cast<uint32_t>(e.def));
                const float round = e.def;
                if (e.min != round || e.max != round + 1.f || std::strcmp(e.name, want) != 0)
                    torn.fetch_add(1, std::memory_order_relaxed);
            }
            const uint32_t g = clockwork_params_generation(t);
            if (g != last) { last = g; generations.fetch_add(1, std::memory_order_relaxed); }
        }
    });
    bool landed = true;
    uint32_t round = 1;
    // Until the reader has seen plenty, with a bound so a stuck reader fails
    // on its counts and never hangs.
    for (; round <= 200000 && (reads.load(std::memory_order_relaxed) < 20000 || generations.load(std::memory_order_relaxed) < 100); ++round) {
        std::vector<ClockworkParamEntry> set;
        char name[CLOCKWORK_PARAM_NAME_BYTES];
        std::snprintf(name, sizeof name, "round %u", round);
        const auto r = static_cast<float>(round);
        for (uint32_t i = 0; i < kSlots; ++i)
            set.push_back(entry(4000 + i, name, CLOCKWORK_PARAM_CONTINUOUS, r, r + 1.f, r));
        landed = landed && clockwork_param_define(lanes_test::core(), set.data(), kSlots, nullptr, 0, CLOCKWORK_PARAM_DEFINER_CLIENT, nullptr, nullptr) == kSlots;
        lanes_test::drainRt();
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    INFO("rounds " << round << ", reads " << reads.load() << ", generations seen " << generations.load());
    CHECK(landed);
    CHECK(reads.load() >= 20000);
    CHECK(generations.load() >= 100);
    CHECK(torn.load() == 0);
}

// The bytes of one table, for the web reader's test: js/lib/params.js reads
// test/fixtures/params_region.bin and must find what this case put there.
// Written with CLOCKWORK_WRITE_FIXTURES=1; otherwise the committed file must
// equal what the code produces, so the two readers always read one fixture.
TEST_CASE("params: the committed fixture is what this build writes", "[params][reader][fixture]") {
    clearTable();
    // As a cold boot leaves it: a table other cases have used keeps its text
    // where they left it until a define wants the room, and the file is the
    // bytes of a table nothing else has written.
    clockwork::params::reset(lanes_test::core());
    Define d;
    d.add(groupEntry(9, "tone"), run({ { CLOCKWORK_PARAM_TEXT_DOC, "What shapes the tone" } }))
     .add(tapered(entry(1, "cutoff", CLOCKWORK_PARAM_CONTINUOUS, 20.f, 20000.f, 1000.f, CLOCKWORK_PARAM_UNIT_HERTZ, CLOCKWORK_PARAM_AUTOMATABLE, 9), CLOCKWORK_PARAM_TAPER_LOG),
          run({ { CLOCKWORK_PARAM_TEXT_FULL_NAME, "Filter cutoff" }, { CLOCKWORK_PARAM_TEXT_DOC, "Where the filter closes" } }))
     .add(entry(2, "steps",  CLOCKWORK_PARAM_STEPPED, 0.f, 7.f, 3.f, CLOCKWORK_PARAM_UNIT_CUSTOM, CLOCKWORK_PARAM_HIDDEN, 9),
          run({ { CLOCKWORK_PARAM_TEXT_UNIT, "steps" } }))
     .add(withRole(entry(3, "on", CLOCKWORK_PARAM_SWITCH, 0.f, 1.f, 1.f, CLOCKWORK_PARAM_UNIT_NONE, 0u), CLOCKWORK_PARAM_ROLE_BYPASS))
     .add(entry(4, "wave", CLOCKWORK_PARAM_CHOICE, 0.f, 2.f, 1.f, CLOCKWORK_PARAM_UNIT_NONE, CLOCKWORK_PARAM_AUTOMATABLE, 9),
          run({ { CLOCKWORK_PARAM_TEXT_VALUE, "saw" }, { CLOCKWORK_PARAM_TEXT_VALUE, "square" }, { CLOCKWORK_PARAM_TEXT_VALUE, "é" } }));
    REQUIRE(refusal(d.packet()).empty());
    ask(setOf(1, 440.f), kHost);
    const ClockworkArenaHeader* h = clockwork_arena_header(lanes_test::core());
    const ClockworkArenaEntry* e = clockwork_arena_find(h, CLOCKWORK_ARENA_CLOCKWORK_PARAMS);
    REQUIRE(e != nullptr);
    const uint8_t* region = reinterpret_cast<const uint8_t*>(h) + e->offset;
    const ClockworkParamsHeader* t = table();
    // The header, the values, every entry and the text this holds, the empty
    // slots' zeros included, so the file is a region a reader maps. Past the
    // text in use is nothing but zeros, so it is cut there.
    const uint32_t used = t->text_offset + t->text_used;
    std::vector<uint8_t> bytes(region, region + used);
    // The generation and the definer are this run's: pinned so the file is stable.
    auto* hdr = reinterpret_cast<ClockworkParamsHeader*>(bytes.data());
    hdr->generation = 2;   // the seqlock word: generation 1, nothing writing
    for (uint32_t s = 0; s < 5; ++s)
        reinterpret_cast<ClockworkParamEntry*>(bytes.data() + hdr->entries_offset + s * hdr->entry_bytes)->definer = 7;
    const std::string path = std::string(CLOCKWORK_TEST_SOURCE_DIR) + "/fixtures/params_region.bin";
    if (std::getenv("CLOCKWORK_WRITE_FIXTURES")) {
        std::ofstream f(path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        REQUIRE(f.good());
    }
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.good());
    std::vector<uint8_t> committed((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    INFO("regenerate with CLOCKWORK_WRITE_FIXTURES=1");
    CHECK(committed == bytes);
}

// ── Facts that agree ────────────────────────────────────────────────────────

TEST_CASE("params: facts that contradict each other are refused, and the table is untouched", "[params][facts]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    const uint32_t gen = clockwork_params_generation(t);
    const auto no = [](const ClockworkParamEntry& e) { return refusal(defineOf({ e })); };
    const uint32_t C = CLOCKWORK_PARAM_CONTINUOUS, S = CLOCKWORK_PARAM_STEPPED, W = CLOCKWORK_PARAM_SWITCH, N = CLOCKWORK_PARAM_NOTE;
    const uint32_t NONE = CLOCKWORK_PARAM_UNIT_NONE, A = CLOCKWORK_PARAM_AUTOMATABLE;

    // Flags that cannot both hold.
    CHECK(no(entry(30, "r", C, 0.f, 1.f, 0.f, NONE, CLOCKWORK_PARAM_READ_ONLY | A)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(entry(30, "h", C, 0.f, 1.f, 0.f, NONE, CLOCKWORK_PARAM_HIDDEN | A)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(entry(30, "m", C, 0.f, 1.f, 0.f, NONE, CLOCKWORK_PARAM_READ_ONLY | CLOCKWORK_PARAM_META)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(entry(30, "w", W, 0.f, 1.f, 0.f, NONE, CLOCKWORK_PARAM_WRAPS)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(entry(30, "w", N, 0.f, 127.f, 0.f, NONE, CLOCKWORK_PARAM_WRAPS)) == CLOCKWORK_PARAM_REFUSED_FACTS);

    // A unit on what has its own words.
    CHECK(no(entry(30, "u", W, 0.f, 1.f, 0.f, CLOCKWORK_PARAM_UNIT_PERCENT)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(entry(30, "u", N, 0.f, 127.f, 0.f, CLOCKWORK_PARAM_UNIT_HERTZ)) == CLOCKWORK_PARAM_REFUSED_FACTS);

    // A taper where nothing sweeps, or that is not one.
    CHECK(no(tapered(entry(30, "t", S, 0.f, 7.f, 0.f), 0.5f)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(tapered(entry(30, "t", W, 0.f, 1.f, 0.f), CLOCKWORK_PARAM_TAPER_LOG)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(tapered(entry(30, "t", C, 0.f, 1.f, 0.f), CLOCKWORK_PARAM_TAPER_LOG)) == CLOCKWORK_PARAM_REFUSED_FACTS);   // reaches zero
    CHECK(no(tapered(entry(30, "t", C, -1.f, 1.f, 0.f), CLOCKWORK_PARAM_TAPER_LOG)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(tapered(entry(30, "t", C, 0.f, 1.f, 0.f), 1.f)) == CLOCKWORK_PARAM_REFUSED_FACTS);    // even has one spelling
    CHECK(no(tapered(entry(30, "t", C, 1.f, 2.f, 1.f), -2.f)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(tapered(entry(30, "t", C, 1.f, 2.f, 1.f), std::nanf(""))) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(no(tapered(entry(30, "t", C, 1.f, 2.f, 1.f), INFINITY)) == CLOCKWORK_PARAM_REFUSED_FACTS);

    // A range the kind cannot hold.
    CHECK(no(entry(30, "s", S, 0.5f, 3.f, 1.f)) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(no(entry(30, "s", S, 0.f, 3.5f, 1.f)) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(no(entry(30, "s", S, 0.f, 3.f, 1.5f)) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(no(entry(30, "n", N, 0.f, 128.f, 0.f)) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(no(entry(30, "n", N, -1.f, 127.f, 0.f)) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(no(entry(30, "n", N, 0.f, 127.f, 60.5f)) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(no(entry(30, "w", W, 0.f, 1.f, 0.5f)) == CLOCKWORK_PARAM_REFUSED_RANGE);
    CHECK(no(entry(30, "e", C, 1.f, 1.f, 1.f)) == CLOCKWORK_PARAM_REFUSED_RANGE);    // a range of nothing

    // A name a reader cannot show.
    CHECK(no(entry(30, "", C, 0.f, 1.f, 0.f)) == CLOCKWORK_PARAM_REFUSED_NAME);
    CHECK(no(entry(30, "\xC3\x28", C, 0.f, 1.f, 0.f)) == CLOCKWORK_PARAM_REFUSED_NAME);
    CHECK(no(entry(30, "\xF4\x90\x80\x80", C, 0.f, 1.f, 0.f)) == CLOCKWORK_PARAM_REFUSED_NAME);   // past U+10FFFF

    // A role no bridge could wire.
    CHECK(no(withRole(entry(30, "r", C, 0.f, 1.f, 0.f), 99u)) == CLOCKWORK_PARAM_REFUSED_KIND);

    CHECK(t->count == 0);
    CHECK(clockwork_params_generation(t) == gen);

    // Each of these is the same kind of fact, in agreement.
    CHECK(no(entry(31, "r", C, 0.f, 1.f, 0.f, NONE, CLOCKWORK_PARAM_READ_ONLY)).empty());
    CHECK(no(entry(32, "h", C, 0.f, 1.f, 0.f, NONE, CLOCKWORK_PARAM_HIDDEN)).empty());
    CHECK(no(entry(33, "w", C, 0.f, 360.f, 0.f, NONE, A | CLOCKWORK_PARAM_WRAPS | CLOCKWORK_PARAM_META)).empty());
    CHECK(no(tapered(entry(34, "l", C, 20.f, 20000.f, 1000.f, CLOCKWORK_PARAM_UNIT_HERTZ), CLOCKWORK_PARAM_TAPER_LOG)).empty());
    CHECK(no(tapered(entry(35, "p", C, 0.f, 1.f, 0.f), 0.5f)).empty());
    CHECK(no(entry(36, "s", S, -3.f, 3.f, -1.f, CLOCKWORK_PARAM_UNIT_SEMITONES)).empty());
    CHECK(no(entry(37, "é", N, 0.f, 127.f, 60.f)).empty());
}

// ── Text ────────────────────────────────────────────────────────────────────

TEST_CASE("params: a choice carries one name per step, and nothing else has names", "[params][text]") {
    clearTable();
    const uint32_t CH = CLOCKWORK_PARAM_CHOICE;
    const uint8_t V = CLOCKWORK_PARAM_TEXT_VALUE;
    const auto names = [&](int n) {
        std::string out;
        for (int i = 0; i < n; ++i) out += run({ { V, "v" + std::to_string(i) } });
        return out;
    };
    const auto defineIt = [](const ClockworkParamEntry& e, const std::string& text) { return refusal(Define{}.add(e, text).packet()); };

    CHECK(defineIt(entry(30, "c", CH, 0.f, 3.f, 0.f), names(3)) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(entry(30, "c", CH, 0.f, 3.f, 0.f), names(5)) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(refusal(defineOf({ entry(30, "c", CH, 0.f, 3.f, 0.f) })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(entry(30, "c", CH, 1.f, 4.f, 1.f), names(4)) == CLOCKWORK_PARAM_REFUSED_RANGE);    // an index, from 0
    CHECK(defineIt(entry(30, "c", CH, 0.f, 3.f, 0.f, CLOCKWORK_PARAM_UNIT_PERCENT), names(4)) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(defineIt(entry(30, "s", CLOCKWORK_PARAM_STEPPED, 0.f, 3.f, 0.f), names(4)) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(entry(30, "c", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 3.f, 0.f), names(4)) == CLOCKWORK_PARAM_REFUSED_TEXT);

    const std::string four = run({ { V, "saw" }, { V, "square" }, { V, "tri" }, { V, "noise" } });
    REQUIRE(defineIt(entry(30, "wave", CH, 0.f, 3.f, 2.f), four).empty());
    CHECK(textOf(30) == four);
    CHECK(valueOf(30) == Approx(2.f));
    ask(setOf(30, 1.4f), kPanel);
    CHECK(valueOf(30) == Approx(1.f));

    // A redefine says new names, with a value held to the new range.
    const std::string two = run({ { V, "up" }, { V, "down" } });
    REQUIRE(defineIt(entry(30, "dir", CH, 0.f, 1.f, 0.f), two).empty());
    CHECK(textOf(30) == two);
    CHECK(valueOf(30) == Approx(1.f));
}

// A choice's steps are counted to check its names against, so a choice says
// no more steps than one entry's text could name: a name is a tag, a
// character at least and a NUL. One that says more is a bad range, whatever
// names it brings, and a maximum no count can hold is among them.
TEST_CASE("params: a choice with more steps than an entry's text could name is refused", "[params][text][hostile]") {
    clearTable();
    const uint32_t CH = CLOCKWORK_PARAM_CHOICE;
    const uint8_t V = CLOCKWORK_PARAM_TEXT_VALUE;
    const auto defineIt = [](const ClockworkParamEntry& e, const std::string& text) { return refusal(Define{}.add(e, text).packet()); };
    const float most = static_cast<float>(CLOCKWORK_PARAM_TEXT_RUN_BYTES / 3u);   // steps a run can name

    for (float max : { 4294967296.f, 8589934592.f, 1e30f, most }) {
        INFO("max " << max);
        CHECK(defineIt(entry(31, "c", CH, 0.f, max, 0.f), "") == CLOCKWORK_PARAM_REFUSED_RANGE);
        CHECK(defineIt(entry(31, "c", CH, 0.f, max, 0.f), run({ { V, "a" } })) == CLOCKWORK_PARAM_REFUSED_RANGE);
        CHECK(slotOf(31) < 0);
    }
    // One fewer is a range a choice may say: refused here only for its names.
    CHECK(defineIt(entry(31, "c", CH, 0.f, most - 1.f, 0.f), "") == CLOCKWORK_PARAM_REFUSED_TEXT);
}

TEST_CASE("params: a custom unit has words, and only a custom unit has", "[params][text]") {
    clearTable();
    const uint8_t U = CLOCKWORK_PARAM_TEXT_UNIT;
    const auto defineIt = [](const ClockworkParamEntry& e, const std::string& text) { return refusal(Define{}.add(e, text).packet()); };
    const uint32_t C = CLOCKWORK_PARAM_CONTINUOUS;
    CHECK(refusal(defineOf({ entry(30, "u", C, 0.f, 1.f, 0.f, CLOCKWORK_PARAM_UNIT_CUSTOM) })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(entry(30, "u", C, 0.f, 1.f, 0.f, CLOCKWORK_PARAM_UNIT_HERTZ), run({ { U, "kHz" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(entry(30, "u", C, 0.f, 1.f, 0.f), run({ { U, "kHz" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(entry(30, "u", C, 0.f, 1.f, 0.f, CLOCKWORK_PARAM_UNIT_CUSTOM), run({ { U, std::string(CLOCKWORK_PARAM_NAME_BYTES, 'x') } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    const std::string spb = run({ { U, "steps a beat" } });
    REQUIRE(defineIt(entry(30, "u", C, 0.f, 1.f, 0.f, CLOCKWORK_PARAM_UNIT_CUSTOM), spb).empty());
    CHECK(textOf(30) == spb);
}

TEST_CASE("params: text arrives whole, or the define is refused", "[params][text]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    const uint32_t gen = clockwork_params_generation(t), used = t->text_used;
    const uint32_t C = CLOCKWORK_PARAM_CONTINUOUS;
    const uint8_t F = CLOCKWORK_PARAM_TEXT_FULL_NAME, D = CLOCKWORK_PARAM_TEXT_DOC;
    const ClockworkParamEntry e = entry(30, "p", C, 0.f, 1.f, 0.f);
    const auto defineIt = [&](const std::string& text) { return refusal(Define{}.add(e, text).packet()); };

    CHECK(defineIt(std::string("\x02no end", 7)) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(std::string("\x02", 1)) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(run({ { 9, "no such tag" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(run({ { 0, "no tag at all" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(run({ { D, "\xC3\x28" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(run({ { D, "" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);         // no doc has one spelling
    CHECK(defineIt(run({ { F, "" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(run({ { D, "one" }, { D, "two" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(run({ { F, "one" }, { F, "two" } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(run({ { F, std::string(CLOCKWORK_PARAM_FULL_NAME_BYTES, 'x') } })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(defineIt(run({ { D, std::string(CLOCKWORK_PARAM_TEXT_RUN_BYTES, 'x') } })) == CLOCKWORK_PARAM_REFUSED_TEXT);

    // A run that points outside what came with it, however it adds up.
    const auto pointing = [&](uint32_t at, uint32_t bytes, const std::string& blob) {
        ClockworkParamEntry bad = e;
        bad.text = at; bad.text_bytes = bytes;
        Define d; d.entries.push_back(bad); d.blob = blob;
        return refusal(d.packet());
    };
    const std::string doc = run({ { D, "fine" } });
    CHECK(pointing(1, static_cast<uint32_t>(doc.size()), doc) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(pointing(0, static_cast<uint32_t>(doc.size()) + 1u, doc) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(pointing(0xFFFFFFFFu, 2u, doc) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(pointing(2u, 0xFFFFFFFFu, doc) == CLOCKWORK_PARAM_REFUSED_TEXT);
    // Text with no blob at all, and a second argument that is no blob.
    ClockworkParamEntry noBlob = e;
    noBlob.text_bytes = 4;
    CHECK(refusal(defineOf({ noBlob })) == CLOCKWORK_PARAM_REFUSED_TEXT);
    osc_test::Builder notBlob;
    notBlob.begin(CLOCKWORK_SYS("param/define")) << osc::Blob(&e, static_cast<osc::osc_bundle_element_size_t>(sizeof e)) << int32_t{ 7 };
    CHECK(refusal(notBlob.end()) == CLOCKWORK_REFUSED_MALFORMED);

    // One bad entry refuses the good one beside it.
    const std::string good = run({ { D, "good" } });
    const std::string bad = run({ { D, "\xC3\x28" } });
    CHECK(refusal(Define{}.add(entry(31, "g", C, 0.f, 1.f, 0.f), good).add(entry(32, "b", C, 0.f, 1.f, 0.f), bad).packet()) == CLOCKWORK_PARAM_REFUSED_TEXT);
    CHECK(t->count == 0);
    CHECK(t->text_used == used);
    CHECK(clockwork_params_generation(t) == gen);

    // The longest there may be lands, and reads back whole.
    const std::string longest = run({ { F, std::string(CLOCKWORK_PARAM_FULL_NAME_BYTES - 1, 'n') },
                                      { D, std::string(CLOCKWORK_PARAM_TEXT_RUN_BYTES - CLOCKWORK_PARAM_FULL_NAME_BYTES - 3, 'd') } });
    REQUIRE(longest.size() == CLOCKWORK_PARAM_TEXT_RUN_BYTES);
    REQUIRE(defineIt(longest).empty());
    CHECK(textOf(30) == longest);

    // A buffer too small is told, and is left alone past its end.
    std::vector<uint8_t> small(8, 0xAB);
    CHECK(clockwork_params_read_text(t, static_cast<uint32_t>(slotOf(30)), small.data(), 4u, nullptr) == -2);
    CHECK(small[4] == 0xAB);

    // The facts and the text of one read belong together.
    ClockworkParamEntry facts{};
    std::vector<uint8_t> out(CLOCKWORK_PARAM_TEXT_RUN_BYTES);
    CHECK(clockwork_params_read_text(t, static_cast<uint32_t>(slotOf(30)), out.data(), static_cast<uint32_t>(out.size()), &facts) == static_cast<int32_t>(longest.size()));
    CHECK(facts.id == 30);
    CHECK(facts.text_bytes == longest.size());
}

TEST_CASE("params: a free slot, a retired entry and one with no text have none", "[params][text]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    uint8_t out[16] = {};
    CHECK(clockwork_params_read_text(t, 0, out, sizeof out, nullptr) == 0);
    CHECK(clockwork_params_read_text(t, t->capacity, out, sizeof out, nullptr) == 0);
    CHECK(clockwork_params_read_text(nullptr, 0, out, sizeof out, nullptr) == 0);
    REQUIRE(refusal(defineOf({ entry(30, "bare", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) })).empty());
    CHECK(textOf(30).empty());
    REQUIRE(refusal(Define{}.add(entry(30, "bare", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f), run({ { CLOCKWORK_PARAM_TEXT_DOC, "now it has" } })).packet()).empty());
    CHECK(textOf(30) == run({ { CLOCKWORK_PARAM_TEXT_DOC, "now it has" } }));
    // A redefine says everything: with none, the text is gone.
    REQUIRE(refusal(defineOf({ entry(30, "bare", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) })).empty());
    CHECK(textOf(30).empty());
}

TEST_CASE("params: text past the room is refused, a retire gives room back, and churn never runs out", "[params][text]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    const uint32_t C = CLOCKWORK_PARAM_CONTINUOUS;
    const auto docOf = [](uint32_t id, char fill) {
        // As long as a run may be, with the id at the front so it can be told from another.
        std::string words = std::to_string(id) + ":";
        words.resize(CLOCKWORK_PARAM_TEXT_RUN_BYTES - 2, fill);
        return run({ { CLOCKWORK_PARAM_TEXT_DOC, words } });
    };
    const auto defineDoc = [&](uint32_t id, char fill) { return refusal(Define{}.add(entry(id, "p", C, 0.f, 1.f, 0.f), docOf(id, fill)).packet()); };

    // As many as fit.
    uint32_t fits = 0;
    while (defineDoc(100 + fits, 'a').empty()) ++fits;
    INFO("runs that fit: " << fits);
    REQUIRE(fits == t->text_capacity / CLOCKWORK_PARAM_TEXT_RUN_BYTES);
    const uint32_t gen = clockwork_params_generation(t);
    const uint32_t used = t->text_used;
    CHECK(defineDoc(100 + fits, 'a') == CLOCKWORK_PARAM_REFUSED_TEXT_FULL);
    CHECK(clockwork_params_generation(t) == gen);
    CHECK(t->text_used == used);
    CHECK(slotOf(100 + fits) < 0);

    // A redefine of a full table's entry fits: it gives back what it takes.
    REQUIRE(defineDoc(100, 'b').empty());
    CHECK(textOf(100) == docOf(100, 'b'));

    // Every other one retired leaves holes no new run fits in one piece of,
    // so landing these means the rest moved up, and read the same after.
    for (uint32_t i = 0; i < fits; i += 2) REQUIRE(refusal(defineOf({ retire(100 + i) })).empty());
    for (uint32_t i = 0; i < fits; i += 2) REQUIRE(defineDoc(500 + i, 'c').empty());
    for (uint32_t i = 1; i < fits; i += 2) CHECK(textOf(100 + i) == docOf(100 + i, 'a'));
    for (uint32_t i = 0; i < fits; i += 2) CHECK(textOf(500 + i) == docOf(500 + i, 'c'));

    // A small redefine over and over: far more text than the region has ever
    // held passes through it.
    clearTable();
    REQUIRE(refusal(Define{}.add(entry(30, "keep", C, 0.f, 1.f, 0.f), run({ { CLOCKWORK_PARAM_TEXT_DOC, "stays" } })).packet()).empty());
    for (uint32_t i = 0; i < 4u * t->text_capacity / 16u; ++i) {
        const std::string words = run({ { CLOCKWORK_PARAM_TEXT_DOC, "round " + std::to_string(i) } });
        REQUIRE(refusal(Define{}.add(entry(31, "churn", C, 0.f, 1.f, 0.f), words).packet()).empty());
        REQUIRE(textOf(31) == words);
    }
    CHECK(textOf(30) == run({ { CLOCKWORK_PARAM_TEXT_DOC, "stays" } }));
    CHECK(t->text_used <= t->text_capacity);
}

TEST_CASE("params: a reader on another thread never sees text from another define than its facts", "[params][text][reader][threads]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    constexpr uint32_t kSlots = 4;
    std::atomic<bool> stop{ false };
    std::atomic<uint64_t> reads{ 0 }, torn{ 0 };
    std::thread reader([&] {
        std::vector<uint8_t> out(CLOCKWORK_PARAM_TEXT_RUN_BYTES);
        while (!stop.load(std::memory_order_relaxed)) {
            for (uint32_t s = 0; s < kSlots; ++s) {
                ClockworkParamEntry e{};
                const int32_t n = clockwork_params_read_text(t, s, out.data(), static_cast<uint32_t>(out.size()), &e);
                if (n <= 0) continue;
                reads.fetch_add(1, std::memory_order_relaxed);
                const std::string want = run({ { CLOCKWORK_PARAM_TEXT_DOC, "round " + std::to_string(static_cast<uint32_t>(e.def)) + " " + std::string(static_cast<uint32_t>(e.def) % 200u, 'x') } });
                if (std::string(reinterpret_cast<const char*>(out.data()), static_cast<size_t>(n)) != want)
                    torn.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    bool landed = true;
    for (uint32_t round = 1; round <= 200000 && reads.load(std::memory_order_relaxed) < 20000; ++round) {
        Define d;
        const auto r = static_cast<float>(round);
        for (uint32_t i = 0; i < kSlots; ++i)
            d.add(entry(4000 + i, "p", CLOCKWORK_PARAM_CONTINUOUS, r, r + 1.f, r),
                  run({ { CLOCKWORK_PARAM_TEXT_DOC, "round " + std::to_string(round) + " " + std::string(round % 200u, 'x') } }));
        landed = landed && clockwork_param_define(lanes_test::core(), d.entries.data(), kSlots,
                                                  reinterpret_cast<const uint8_t*>(d.blob.data()), static_cast<uint32_t>(d.blob.size()),
                                                  CLOCKWORK_PARAM_DEFINER_CLIENT, nullptr, nullptr) == kSlots;
        lanes_test::drainRt();
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    INFO("reads " << reads.load());
    CHECK(landed);
    CHECK(reads.load() >= 20000);
    CHECK(torn.load() == 0);
}

// ── Groups ──────────────────────────────────────────────────────────────────

TEST_CASE("params: a group is an entry, and its members must find it", "[params][groups]") {
    clearTable();
    const uint32_t C = CLOCKWORK_PARAM_CONTINUOUS, A = CLOCKWORK_PARAM_AUTOMATABLE, NONE = CLOCKWORK_PARAM_UNIT_NONE;
    const auto member = [&](uint32_t id, uint32_t group) { return entry(id, "m", C, 0.f, 1.f, 0.f, NONE, A, group); };

    // A member may come before its group in one define.
    REQUIRE(refusal(defineOf({ member(30, 20), groupEntry(20, "filter") })).empty());
    CHECK(entryIn(table(), static_cast<uint32_t>(slotOf(30))).group == 20);
    CHECK(entryIn(table(), static_cast<uint32_t>(slotOf(20))).kind == CLOCKWORK_PARAM_GROUP);

    // Nothing there, a parameter that is no group, itself.
    CHECK(refusal(defineOf({ member(31, 99) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    CHECK(refusal(defineOf({ member(31, 30) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    CHECK(refusal(defineOf({ groupEntry(21, "self", 21) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    // In a circle, and a circle made by moving a group already there.
    CHECK(refusal(defineOf({ groupEntry(21, "a", 22), groupEntry(22, "b", 21) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    REQUIRE(refusal(defineOf({ groupEntry(21, "a", 20) })).empty());
    CHECK(refusal(defineOf({ groupEntry(20, "filter", 21) })) == CLOCKWORK_PARAM_REFUSED_GROUP);

    // The client's group on another of its connections is still its own; the guest's group is not.
    CHECK(refusal(defineOf({ member(32, 20) }), kHost).empty());
    const ClockworkParamEntry guestsGroup = groupEntry(25, "guest's");
    REQUIRE(clockwork_param_define(lanes_test::core(), &guestsGroup, 1, nullptr, 0, CLOCKWORK_PARAM_DEFINER_GUEST, nullptr, nullptr) == 1);
    CHECK(refusal(defineOf({ member(33, 25) })) == CLOCKWORK_PARAM_REFUSED_GROUP);

    // A group holds no value and says nothing about one.
    CHECK(refusal(defineOf({ entry(23, "g", CLOCKWORK_PARAM_GROUP, 0.f, 1.f, 0.f, NONE, 0u) })) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(refusal(defineOf({ entry(23, "g", CLOCKWORK_PARAM_GROUP, 0.f, 0.f, 0.f, CLOCKWORK_PARAM_UNIT_HERTZ, 0u) })) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(refusal(defineOf({ entry(23, "g", CLOCKWORK_PARAM_GROUP, 0.f, 0.f, 0.f, NONE, A) })) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(refusal(defineOf({ tapered(groupEntry(23, "g"), 0.5f) })) == CLOCKWORK_PARAM_REFUSED_FACTS);
    CHECK(refusal(defineOf({ withRole(groupEntry(23, "g"), CLOCKWORK_PARAM_ROLE_BYPASS) })) == CLOCKWORK_PARAM_REFUSED_ROLE);
    CHECK(refusal(Define{}.add(groupEntry(23, "g"), run({ { CLOCKWORK_PARAM_TEXT_VALUE, "v" } })).packet()) == CLOCKWORK_PARAM_REFUSED_TEXT);
    // Text a group may have.
    CHECK(refusal(Define{}.add(groupEntry(23, "g"), run({ { CLOCKWORK_PARAM_TEXT_DOC, "what is in it" } })).packet()).empty());

    // Setting one is setting nothing.
    CHECK(refusal(setOf(20, 0.5f)) == CLOCKWORK_PARAM_REFUSED_UNKNOWN);
    CHECK(clockwork_param_set(lanes_test::core(), static_cast<uint32_t>(slotOf(20)), 0.5f, CLOCKWORK_PARAM_DEFINER_GUEST) == 0);
    CHECK(clockwork_params_value(table(), static_cast<uint32_t>(slotOf(20))) == 0.f);
}

TEST_CASE("params: groups nest as deep as the limit", "[params][groups]") {
    clearTable();
    // The entry inside CLOCKWORK_PARAM_GROUP_DEPTH groups lands; one more does not.
    std::vector<ClockworkParamEntry> set;
    uint32_t parent = 0;
    for (uint32_t i = 0; i < CLOCKWORK_PARAM_GROUP_DEPTH; ++i) {
        set.push_back(groupEntry(100 + i, "g", parent));
        parent = 100 + i;
    }
    set.push_back(entry(30, "deep", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f, CLOCKWORK_PARAM_UNIT_NONE, CLOCKWORK_PARAM_AUTOMATABLE, parent));
    REQUIRE(refusal(defineOf(set)).empty());
    // A ninth group, and a parameter inside it.
    CHECK(refusal(defineOf({ groupEntry(200, "g", parent) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    // Nor by moving the whole chain inside a new one.
    CHECK(refusal(defineOf({ groupEntry(201, "top"), groupEntry(100, "g", 201) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
}

TEST_CASE("params: a group in use cannot go, until its members do", "[params][groups]") {
    clearTable();
    const uint32_t C = CLOCKWORK_PARAM_CONTINUOUS, A = CLOCKWORK_PARAM_AUTOMATABLE, NONE = CLOCKWORK_PARAM_UNIT_NONE;
    const auto member = [&](uint32_t id, uint32_t group) { return entry(id, "m", C, 0.f, 1.f, 0.f, NONE, A, group); };
    REQUIRE(refusal(defineOf({ groupEntry(20, "g"), groupEntry(21, "inner", 20), member(30, 21), member(31, 21) })).empty());

    CHECK(refusal(defineOf({ retire(21) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    CHECK(refusal(defineOf({ retire(20) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    // Turned into a parameter under them, likewise.
    CHECK(refusal(defineOf({ entry(21, "no group", C, 0.f, 1.f, 0.f) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    // One member gone is not all of them.
    CHECK(refusal(defineOf({ retire(21), retire(30) })) == CLOCKWORK_PARAM_REFUSED_GROUP);
    // A member moved out and the group retired, in one define.
    CHECK(refusal(defineOf({ retire(21), member(30, 20), member(31, 0) })).empty());
    CHECK(slotOf(21) < 0);
    CHECK(entryIn(table(), static_cast<uint32_t>(slotOf(30))).group == 20);
    // The group and its last member together.
    CHECK(refusal(defineOf({ retire(30), retire(20) })).empty());
    CHECK(slotOf(20) < 0);
}

// ── Roles ───────────────────────────────────────────────────────────────────

TEST_CASE("params: a role fits its facts, and a table holds one of each", "[params][roles]") {
    clearTable();
    const uint32_t C = CLOCKWORK_PARAM_CONTINUOUS, W = CLOCKWORK_PARAM_SWITCH, N = CLOCKWORK_PARAM_NOTE, NONE = CLOCKWORK_PARAM_UNIT_NONE, A = CLOCKWORK_PARAM_AUTOMATABLE;
    const auto sw = [&](uint32_t id, uint32_t role) { return withRole(entry(id, "s", W, 0.f, 1.f, 0.f, NONE, A), role); };
    const auto no = [](const ClockworkParamEntry& e, uint32_t who = kPanel) { return refusal(defineOf({ e }), who); };

    CHECK(no(withRole(entry(30, "x", C, 0.f, 1.f, 0.f), CLOCKWORK_PARAM_ROLE_BYPASS)) == CLOCKWORK_PARAM_REFUSED_ROLE);
    CHECK(no(withRole(entry(30, "x", C, 0.f, 1.f, 0.f), CLOCKWORK_PARAM_ROLE_KEY_HELD)) == CLOCKWORK_PARAM_REFUSED_ROLE);
    CHECK(no(withRole(entry(30, "x", W, 0.f, 1.f, 0.f), CLOCKWORK_PARAM_ROLE_KEY_NOTE)) == CLOCKWORK_PARAM_REFUSED_ROLE);
    CHECK(no(withRole(entry(30, "x", C, 40.f, 240.f, 120.f), CLOCKWORK_PARAM_ROLE_TEMPO)) == CLOCKWORK_PARAM_REFUSED_ROLE);     // not in BPM
    CHECK(no(withRole(entry(30, "x", W, 0.f, 1.f, 0.f, NONE, CLOCKWORK_PARAM_READ_ONLY), CLOCKWORK_PARAM_ROLE_BYPASS)) == CLOCKWORK_PARAM_REFUSED_ROLE);   // something has to drive it

    REQUIRE(no(sw(30, CLOCKWORK_PARAM_ROLE_BYPASS)).empty());
    REQUIRE(no(withRole(entry(31, "t", C, 40.f, 240.f, 120.f, CLOCKWORK_PARAM_UNIT_BPM), CLOCKWORK_PARAM_ROLE_TEMPO)).empty());
    REQUIRE(no(withRole(entry(32, "n", N, 0.f, 127.f, 60.f), CLOCKWORK_PARAM_ROLE_KEY_NOTE)).empty());
    REQUIRE(no(sw(33, CLOCKWORK_PARAM_ROLE_KEY_HELD)).empty());

    // A second, in another define, in one define, and by another definer.
    CHECK(no(sw(34, CLOCKWORK_PARAM_ROLE_BYPASS)) == CLOCKWORK_PARAM_REFUSED_ROLE);
    CHECK(refusal(defineOf({ sw(34, CLOCKWORK_PARAM_ROLE_KEY_HELD), sw(35, CLOCKWORK_PARAM_ROLE_KEY_HELD) })) == CLOCKWORK_PARAM_REFUSED_ROLE);
    CHECK(no(sw(34, CLOCKWORK_PARAM_ROLE_BYPASS), kHost) == CLOCKWORK_PARAM_REFUSED_ROLE);

    // The same entry again keeps it; giving it up frees it, in the same define.
    CHECK(no(sw(30, CLOCKWORK_PARAM_ROLE_BYPASS)).empty());
    CHECK(refusal(defineOf({ sw(30, CLOCKWORK_PARAM_ROLE_NONE), sw(34, CLOCKWORK_PARAM_ROLE_BYPASS) })).empty());
    CHECK(refusal(defineOf({ retire(34), sw(36, CLOCKWORK_PARAM_ROLE_BYPASS) })).empty());
    CHECK(entryIn(table(), static_cast<uint32_t>(slotOf(36))).role == CLOCKWORK_PARAM_ROLE_BYPASS);
}

// ── Reaching a reader ───────────────────────────────────────────────────────

TEST_CASE("params: list says the new facts, and text answers a run to the asker", "[params][text]") {
    clearTable();
    const std::string doc = run({ { CLOCKWORK_PARAM_TEXT_DOC, "Where the filter closes" } });
    REQUIRE(refusal(Define{}.add(tapered(entry(30, "cutoff", CLOCKWORK_PARAM_CONTINUOUS, 20.f, 20000.f, 1000.f), CLOCKWORK_PARAM_TAPER_LOG), doc)
                            .add(withRole(entry(31, "bypass", CLOCKWORK_PARAM_SWITCH, 0.f, 1.f, 0.f), CLOCKWORK_PARAM_ROLE_BYPASS)).packet()).empty());
    auto rows = at(ask(osc_test::message(CLOCKWORK_SYS("param/list")), kHost), CLOCKWORK_SYS("param/entry"));
    REQUIRE(rows.size() == 2);
    const auto& a = rows[0].argInt(0) == 30 ? rows[0] : rows[1];
    const auto& b = rows[0].argInt(0) == 30 ? rows[1] : rows[0];
    // ... slot value taper role text-bytes
    REQUIRE(a.argCount() == 14);
    CHECK(a.argFloat(11) == CLOCKWORK_PARAM_TAPER_LOG);
    CHECK(a.argInt(12) == CLOCKWORK_PARAM_ROLE_NONE);
    CHECK(a.argInt(13) == static_cast<int32_t>(doc.size()));
    CHECK(b.argInt(12) == CLOCKWORK_PARAM_ROLE_BYPASS);
    CHECK(b.argInt(13) == 0);

    auto out = ask(osc_test::message(CLOCKWORK_SYS("param/text"), int32_t{ 30 }), kHost);
    auto reply = at(out, CLOCKWORK_SYS("param/text.reply"));
    REQUIRE(reply.size() == 1);
    CHECK(reply[0].argInt(0) == 30);
    CHECK(reply[0].argInt(1) == static_cast<int32_t>(clockwork_params_generation(table())));
    const auto blob = reply[0].argBlob(2);
    CHECK(std::string(blob.begin(), blob.end()) == doc);
    const auto* frame = frameAt(out, CLOCKWORK_SYS("param/text.reply"));
    REQUIRE(frame != nullptr);
    CHECK(frame->sourceId == kHost);

    // None to tell, no such id, and no id.
    auto none = at(ask(osc_test::message(CLOCKWORK_SYS("param/text"), int32_t{ 31 }), kHost), CLOCKWORK_SYS("param/text.reply"));
    REQUIRE(none.size() == 1);
    CHECK(none[0].argBlob(2).empty());
    CHECK(refusal(osc_test::message(CLOCKWORK_SYS("param/text"), int32_t{ 99 })) == CLOCKWORK_PARAM_REFUSED_UNKNOWN);
    CHECK(refusal(osc_test::message(CLOCKWORK_SYS("param/text"))) == CLOCKWORK_REFUSED_MALFORMED);

    // The longest there may be answers in one frame.
    const std::string longest = run({ { CLOCKWORK_PARAM_TEXT_DOC, std::string(CLOCKWORK_PARAM_TEXT_RUN_BYTES - 2, 'd') } });
    REQUIRE(refusal(Define{}.add(entry(32, "long", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f), longest).packet()).empty());
    auto whole = at(ask(osc_test::message(CLOCKWORK_SYS("param/text"), int32_t{ 32 }), kHost), CLOCKWORK_SYS("param/text.reply"));
    REQUIRE(whole.size() == 1);
    const auto longBlob = whole[0].argBlob(2);
    CHECK(std::string(longBlob.begin(), longBlob.end()) == longest);
}

TEST_CASE("params: the metrics say how many there are, and what moved", "[params][metrics]") {
    clearTable();
    const ClockworkParamsHeader* t = table();
    const auto metric = [](uint32_t offset) {
        uint32_t word = 0;
        REQUIRE(lanes_test::defaultLane().metricWord(offset, word));
        return word;
    };
    // By name, from the schema every reader has, so a metric that moves slot is still found.
    const auto metricCalled = [](const char* key) {
        for (const clockwork::metrics_schema::FieldInfo& f : clockwork::metrics_schema::kFields)
            if (std::strcmp(f.key, key) == 0) return f.offset;
        FAIL("no metric called " << key);
        return 0u;
    };
    const uint32_t LIVE = metricCalled("paramsLive"), GENERATION = metricCalled("paramsGeneration"),
                   MOVES = metricCalled("paramMoves"), REFUSED = metricCalled("paramsRefused");
    CHECK(metric(LIVE) == 0);
    CHECK(metric(GENERATION) == clockwork_params_generation(t));

    const uint32_t moves = metric(MOVES), refused = metric(REFUSED), gen = metric(GENERATION);
    REQUIRE(refusal(defineOf({ entry(30, "a", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f), entry(31, "b", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) })).empty());
    CHECK(metric(LIVE) == 2);
    CHECK(metric(GENERATION) == gen + 1u);
    CHECK(metric(GENERATION) == clockwork_params_generation(t));

    ask(setOf(30, 0.5f), kPanel);
    CHECK(metric(MOVES) == moves + 1u);
    ask(setOf(30, 0.5f), kPanel);                // no move, no count
    CHECK(metric(MOVES) == moves + 1u);
    clockwork_param_set(lanes_test::core(), static_cast<uint32_t>(slotOf(31)), 0.25f, CLOCKWORK_PARAM_DEFINER_GUEST);
    CHECK(metric(MOVES) == moves + 2u);

    CHECK(metric(REFUSED) == refused);
    CHECK(!refusal(setOf(99, 0.5f)).empty());
    CHECK(metric(REFUSED) == refused + 1u);
    CHECK(!refusal(defineOf({ entry(32, "", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) })).empty());
    CHECK(metric(REFUSED) == refused + 2u);

    REQUIRE(refusal(defineOf({ retire(30) })).empty());
    CHECK(metric(LIVE) == 1);
}

TEST_CASE("params: a client maps the table as a region of its own, and reads it directly", "[params][reader]") {
    clearTable();
    const std::string doc = run({ { CLOCKWORK_PARAM_TEXT_DOC, "direct" } });
    REQUIRE(refusal(Define{}.add(entry(30, "direct", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.25f), doc).packet()).empty());
    uint8_t* const arena = static_cast<uint8_t*>(clockwork_lanes_base(lanes_test::core()));
    ClockworkStatus status = CLOCKWORK_OK;
    ClockworkClient* c = clockwork_client_open_memory(arena, reinterpret_cast<const ClockworkArenaHeader*>(arena)->arena_bytes, &status);
    REQUIRE(c != nullptr);
    ClockworkRegion r{};
    REQUIRE(clockwork_client_region(c, CLOCKWORK_REGION_PARAMS, &r) == CLOCKWORK_OK);
    CHECK(r.bytes == PARAMS_SIZE);
    CHECK(r.writable == 0);
    const ClockworkParamsHeader* h = clockwork_params_header(static_cast<const uint8_t*>(r.base));
    REQUIRE(h != nullptr);
    CHECK(h == table());
    ClockworkParamEntry e{};
    std::vector<uint8_t> out(CLOCKWORK_PARAM_TEXT_RUN_BYTES);
    const uint32_t s = static_cast<uint32_t>(slotOf(30));
    CHECK(clockwork_params_read_text(h, s, out.data(), static_cast<uint32_t>(out.size()), &e) == static_cast<int32_t>(doc.size()));
    CHECK(std::string(e.name) == "direct");
    CHECK(clockwork_params_value(h, s) == Approx(0.25f));
    clockwork_client_close(c);
}

TEST_CASE("params: a guest's define carries text", "[params][guest][text]") {
    clearTable();
    const auto declare = osc_test::message("/dummy/param/declare");
    REQUIRE(lanes_test::ingress(declare.ptr(), declare.size(), kPanel));
    lanes_test::defaultLane().tick();
    lanes_test::drainRt();
    const ClockworkParamEntry mode = entryIn(table(), static_cast<uint32_t>(slotOf(1002)));
    CHECK(mode.kind == CLOCKWORK_PARAM_CHOICE);
    CHECK(textOf(1002) == run({ { CLOCKWORK_PARAM_TEXT_VALUE, "pulse" }, { CLOCKWORK_PARAM_TEXT_VALUE, "tone" }, { CLOCKWORK_PARAM_TEXT_VALUE, "echo" } }));
}

// ── the guest ───────────────────────────────────────────────────────────────

TEST_CASE("params: two cores have two tables, each with its guest's entries", "[params][guest]") {
    lanes_test::Lane a, b;
    const ClockworkParamsHeader* ta = table(a.core());
    const ClockworkParamsHeader* tb = table(b.core());
    CHECK(ta != tb);
    const auto declare = osc_test::message("/dummy/param/declare");
    REQUIRE(a.ingress(declare.ptr(), declare.size(), kPanel));
    REQUIRE(b.ingress(declare.ptr(), declare.size(), kPanel));
    a.tick();
    b.tick();
    for (const auto* t : { ta, tb }) {
        const int level = slotOf(t, 1001);
        REQUIRE(level >= 0);
        REQUIRE(slotOf(t, 1002) >= 0);
        const ClockworkParamEntry e = entryIn(t, static_cast<uint32_t>(level));
        CHECK(e.definer == CLOCKWORK_PARAM_DEFINER_GUEST);
        CHECK(std::string(e.name) == "level");
        CHECK(clockwork_params_value(t, static_cast<uint32_t>(level)) == Approx(0.5f));
    }
    // A client's entry and a moved value in one are nothing to the other.
    const auto define = defineOf({ entry(7, "only here", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) });
    const auto set = setOf(1001, 0.9f);
    REQUIRE(a.ingress(define.ptr(), define.size(), kPanel));
    REQUIRE(a.ingress(set.ptr(), set.size(), kPanel));
    a.tick();
    b.tick();
    CHECK(slotOf(ta, 7) >= 0);
    CHECK(slotOf(tb, 7) == -1);
    CHECK(clockwork_params_value(ta, static_cast<uint32_t>(slotOf(ta, 1001))) == Approx(0.9f));
    CHECK(clockwork_params_value(tb, static_cast<uint32_t>(slotOf(tb, 1001))) == Approx(0.5f));
}

TEST_CASE("params: the guest reads and writes on the audio thread, and owns its ids", "[params][guest]") {
    clearTable();
    auto out = ask(osc_test::message("/dummy/param/declare"), kPanel);
    REQUIRE(at(out, "/dummy/param/declared").size() == 1);
    const int slotLevel = slotOf(1001);
    REQUIRE(slotLevel >= 0);
    REQUIRE(slotOf(1002) >= 0);
    CHECK(at(out, "/dummy/param/declared")[0].argInt(0) == slotLevel);
    CHECK(out.size() == 1);   // the guest's own reply, and no announcement of its define

    // A client sets it; the guest reads the held value on the audio thread.
    ask(setOf(1001, 0.9f), kPanel);
    out = ask(osc_test::message("/dummy/param/get", static_cast<int32_t>(slotLevel)), kPanel);
    auto is = at(out, "/dummy/param/is");
    REQUIRE(is.size() == 1);
    CHECK(is[0].argFloat(1) == Approx(0.9f));

    // The guest moves it: held to the range, for any reader to see, and announced to nobody.
    osc_test::Builder b;
    b.begin("/dummy/param/tell") << osc::int32(slotLevel) << 5.f;
    out = ask(b.end(), kPanel);
    CHECK(valueOf(1001) == Approx(1.f));
    CHECK(out.empty());

    // The guest's ids are its own: a client's define of one is refused.
    out = ask(defineOf({ entry(1001, "level", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.5f) }), kPanel);
    REQUIRE(at(out, CLOCKWORK_SYS("error")).size() == 1);
    CHECK(at(out, CLOCKWORK_SYS("error"))[0].argString(1) == std::string(CLOCKWORK_PARAM_REFUSED_HELD));
}

TEST_CASE("params: a rebuild keeps the table, and the guest's declare finds its slots again", "[params][guest][restart]") {
    clearTable();
    ask(osc_test::message("/dummy/param/declare"), kPanel);
    ask(defineOf({ entry(95, "mine", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.f) }), kPanel);
    const int slotLevel = slotOf(1001), slotMine = slotOf(95);
    REQUIRE(slotLevel >= 0); REQUIRE(slotMine >= 0);
    ask(setOf(1001, 0.8f), kPanel);
    ask(setOf(95, 0.3f), kPanel);
    lanes_test::defaultLane().freshDsp();   // a new guest instance over the same arena
    lanes_test::drainRt();
    CHECK(slotOf(1001) == slotLevel);
    CHECK(slotOf(95) == slotMine);
    CHECK(valueOf(1001) == Approx(0.8f));
    CHECK(valueOf(95) == Approx(0.3f));
}

// ── what a peer may write ───────────────────────────────────────────────────

// Valid frames, cut short or with bytes changed, seeded so a failure repeats:
// the verbs read a peer's bytes on the audio thread. The fixture checks every
// frame out decodes; under the sanitizers a read past a frame is fatal. The
// table comes out whole.
TEST_CASE("params: a peer's damaged frames leave the table whole", "[params][hostile]") {
    clearTable();
    std::mt19937 rng(0x9a7a);
    const auto define = defineOf({ entry(5000, "a", CLOCKWORK_PARAM_CONTINUOUS, 0.f, 1.f, 0.5f),
                                   entry(5001, "b", CLOCKWORK_PARAM_STEPPED, 0.f, 7.f, 3.f) });
    osc_test::Builder sets;
    sets.begin(CLOCKWORK_SYS("param/set")) << osc::int32(5000) << 0.25f << osc::int32(5001) << 2.f;
    const auto set = sets.end();
    const auto list = osc_test::message(CLOCKWORK_SYS("param/list"), int32_t{ 3 });
    const auto retired = defineOf({ retire(5000), retire(5001) });
    // With text, a group and a role, whose offsets and tags are damage's targets too.
    const auto texted = Define{}
        .add(groupEntry(5002, "g"), run({ { CLOCKWORK_PARAM_TEXT_DOC, "a group" } }))
        .add(entry(5003, "c", CLOCKWORK_PARAM_CHOICE, 0.f, 1.f, 0.f, CLOCKWORK_PARAM_UNIT_NONE, CLOCKWORK_PARAM_AUTOMATABLE, 5002),
             run({ { CLOCKWORK_PARAM_TEXT_VALUE, "é" }, { CLOCKWORK_PARAM_TEXT_VALUE, "b" }, { CLOCKWORK_PARAM_TEXT_FULL_NAME, "choice" } }))
        .add(withRole(entry(5004, "b", CLOCKWORK_PARAM_SWITCH, 0.f, 1.f, 0.f), CLOCKWORK_PARAM_ROLE_BYPASS),
             run({ { CLOCKWORK_PARAM_TEXT_DOC, "bypass" } }))
        .packet();
    const auto text = osc_test::message(CLOCKWORK_SYS("param/text"), int32_t{ 5003 });
    const auto gone = defineOf({ retire(5003), retire(5004), retire(5002) });
    const std::vector<osc_test::Packet> frames{ define, set, list, retired, texted, text, gone };
    uint32_t delivered = 0;
    for (int i = 0; i < 4000; ++i) {
        std::vector<uint8_t> f = frames[static_cast<size_t>(i) % frames.size()].data;
        const size_t address = (std::strlen(reinterpret_cast<const char*>(f.data())) + 4u) & ~size_t{ 3 };
        const size_t tail = f.size() - address;
        if (rng() % 3u == 0u) {
            // Cut short, on OSC's four-byte grid and off it.
            f.resize(address + rng() % tail);
        } else {
            for (uint32_t n = 1u + rng() % 6u; n > 0u; --n)
                f[address + rng() % tail] = static_cast<uint8_t>(rng());
        }
        if (lanes_test::ingress(f.data(), static_cast<uint32_t>(f.size()), kPanel)) ++delivered;
        if (i % 8 == 7) {
            lanes_test::defaultLane().tick();
            for (const auto& out : lanes_test::drainRt()) CHECK(out.decodes);
        }
    }
    lanes_test::defaultLane().tick();
    lanes_test::drainRt();
    CHECK(delivered > 3000);

    const ClockworkParamsHeader* t = table();
    uint32_t live = 0;
    for (uint32_t s = 0; s < t->capacity; ++s) {
        const ClockworkParamEntry e = entryIn(t, s);
        if (!e.live) continue;
        ++live;
        INFO("slot " << s << " id " << e.id);
        CHECK(e.id != 0);
        CHECK(e.min <= e.max);
        CHECK(std::memchr(e.name, '\0', sizeof e.name) != nullptr);
        const float v = clockwork_params_value(t, s);
        CHECK(std::isfinite(v));
        CHECK(v >= e.min);
        CHECK(v <= e.max);
    }
    CHECK(live == t->count);
}
