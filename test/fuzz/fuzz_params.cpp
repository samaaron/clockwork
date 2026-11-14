// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// The parameter table's verbs (src/params/params.cpp, docs/PARAMS.md):
// /clockwork/param/define, list, set and text, answered on the thread that
// ticks the core. A define is a blob of records and a blob of text, the same
// bytes the table holds, from the client, validated by nothing before. Each
// input is a run of frames, over one connection or another, so one frame
// meets the table the frames before it left. After every frame the table must be one every reader
// can read: its header whole, its count the slots taken, each entry's facts
// in agreement, its text inside the text, its value inside its range. Every
// reply must decode as a message of clockwork's.
//
// An input: frames back to back, each a byte choosing the verb and the
// connection, a length (u16, little-endian) and that many bytes. The verb's
// address is put in front, so the bytes are the type tags and arguments;
// verb 4 is the bytes as they come, for an address the route does not know.

#include "params/clockwork_params.h"
#include "params/params.h"

#include "IngressCallCtx.h"
#include "clockwork_prefix.h"
#include "core_state.h"
#include "shared_memory.h"

#include <osc/OscReceivedElements.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// A core with an arena that ends where the table does, so a write past the
// table is a heap overflow the sanitizer sees; the metrics history, which
// lies in front of it, is held to a pattern.
constexpr uint32_t kArenaBytes = PARAMS_START + PARAMS_SIZE;
constexpr uint8_t  kPattern = 0xA5;

struct Fixture {
    std::unique_ptr<uint8_t[]> arena{ new uint8_t[kArenaBytes]() };
    clockwork::CoreState state{ core() };
    Fixture() {
        state.arena.base.store(arena.get());
        state.arena.ready.store(true);
    }
    // A table as a cold boot leaves it: what one input does is all its own.
    void fresh() {
        std::memset(arena.get(), 0, kArenaBytes);
        std::memset(arena.get() + METRICS_HISTORY_START, kPattern, METRICS_HISTORY_SIZE);
        clockwork::params::reset(core());
    }
    ClockworkCore* core() { return reinterpret_cast<ClockworkCore*>(this); }
};

uint32_t padded(uint32_t n) { return (n + 3u) & ~3u; }

void check(bool ok) { if (!ok) std::abort(); }

// The table as any reader finds it, after a frame.
void checkTable(const uint8_t* arena) {
    for (uint32_t i = 0; i < METRICS_HISTORY_SIZE; ++i) check(arena[METRICS_HISTORY_START + i] == kPattern);
    const ClockworkParamsHeader* h = clockwork_params_header(arena + PARAMS_START);
    check(h != nullptr);
    check(h->capacity == CLOCKWORK_PARAM_SLOTS && h->text_capacity == CLOCKWORK_PARAM_TEXT_BYTES);
    check((h->generation & 1u) == 0u);
    check(h->text_used <= h->text_capacity);
    uint32_t live = 0, roles[8] = {};
    std::vector<uint8_t> run(CLOCKWORK_PARAM_TEXT_RUN_BYTES);
    for (uint32_t s = 0; s < h->capacity; ++s) {
        ClockworkParamEntry e;
        const int32_t bytes = clockwork_params_read_text(h, s, run.data(), static_cast<uint32_t>(run.size()), &e);
        check(bytes >= 0);
        const float v = clockwork_params_value(h, s);
        if (!e.live) { check(bytes == 0 && v == 0.f); continue; }
        ++live;
        check(e.id != 0u);
        check(e.kind >= CLOCKWORK_PARAM_CONTINUOUS && e.kind <= CLOCKWORK_PARAM_GROUP);
        check(std::memchr(e.name, '\0', sizeof e.name) != nullptr && e.name[0] != '\0');
        check(e.text_bytes <= CLOCKWORK_PARAM_TEXT_RUN_BYTES && e.text <= h->text_capacity - e.text_bytes);
        check(e.role <= CLOCKWORK_PARAM_ROLE_KEY_HELD);
        if (e.role != CLOCKWORK_PARAM_ROLE_NONE) check(++roles[e.role] == 1u);
        if (e.kind == CLOCKWORK_PARAM_GROUP) { check(v == 0.f); continue; }
        check(std::isfinite(e.min) && std::isfinite(e.max) && e.min < e.max);
        check(std::isfinite(v) && v >= e.min && v <= e.max);
        if (e.kind != CLOCKWORK_PARAM_CONTINUOUS) check(std::floor(v) == v);
        // a run is whole strings: a tag, words, a NUL
        for (int32_t i = 0; i < bytes;) {
            check(run[i] >= CLOCKWORK_PARAM_TEXT_FULL_NAME && run[i] <= CLOCKWORK_PARAM_TEXT_VALUE);
            const void* nul = (i + 1 < bytes) ? std::memchr(run.data() + i + 1, '\0', static_cast<size_t>(bytes - i - 1)) : nullptr;
            check(nul != nullptr);
            i = static_cast<int32_t>(static_cast<const uint8_t*>(nul) - run.data()) + 1;
        }
    }
    check(live == h->count);
}

}  // namespace

// What params.cpp reaches outside itself: the core's state, with no ports, sinks or heap, and its reply door.
// The two parts of a core's state defined in core_state.cpp, which would bring the engine with it: the
// table's verbs touch neither, so here they hold nothing.
clockwork::Assets::Assets() : slots{}, count(0) {}
void clockwork::Staging::Free::operator()(void*) const {}
extern "C" const ClockworkPorts* clockwork_core_ports(const ClockworkCore*) { return nullptr; }
extern "C" const ClockworkSinks* clockwork_core_sinks(const ClockworkCore*) { return nullptr; }
extern "C" const ClockworkHeap* clockwork_core_heap(const ClockworkCore*) { return nullptr; }
extern "C" void* clockwork_core_state(const ClockworkCore* core) {
    return core ? &reinterpret_cast<Fixture*>(const_cast<ClockworkCore*>(core))->state : nullptr;
}

extern "C" bool clockwork_emit_osc(ClockworkCore*, uint32_t, const uint8_t* osc, uint32_t len) {
    check(len != 0 && len % 4 == 0);
    check(clockwork_sys_claims(osc, len));
    // an exact copy: a reply that does not decode within its length is the sanitizer's, or oscpack's to throw
    std::vector<uint32_t> aligned(len / 4);
    std::memcpy(aligned.data(), osc, len);
    const osc::ReceivedMessage m(osc::ReceivedPacket(reinterpret_cast<const char*>(aligned.data()),
                                                     static_cast<osc::osc_bundle_element_size_t>(len)));
    for (auto a = m.ArgumentsBegin(); a != m.ArgumentsEnd(); ++a) (void)a->TypeTag();
    return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 0xFFFF) return 0;
    static Fixture fixture;
    fixture.fresh();
    static const char* const kVerbs[] = { "define", "list", "set", "text" };

    for (size_t at = 0; at + 3 <= size;) {
        const uint8_t pick = data[at];
        const uint32_t len = static_cast<uint32_t>(data[at + 1]) | (static_cast<uint32_t>(data[at + 2]) << 8u);
        at += 3;
        const uint32_t take = len < size - at ? len : static_cast<uint32_t>(size - at);
        const uint8_t* body = data + at;
        at += take;

        // An exact-size frame: a read past its length is a heap overflow the sanitizer sees.
        std::vector<uint8_t> frame;
        const uint32_t verb = pick & 7u;
        if (verb < 4u) {
            std::string address = std::string(CLOCKWORK_SYS("param/")) + kVerbs[verb];
            frame.assign(padded(static_cast<uint32_t>(address.size()) + 1u), 0);
            std::memcpy(frame.data(), address.data(), address.size());
        } else {
            // the route is reached only by a frame under its prefix
            const std::string prefix = CLOCKWORK_SYS("param/");
            frame.assign(prefix.begin(), prefix.end());
        }
        frame.insert(frame.end(), body, body + take);

        DrainCallCtx ctx;
        ctx.sourceId = (pick & 8u) ? 0x9a7e1u : 0x51de2u;   // the client's connection, or the one it has after it comes back
        clockwork::params::route(fixture.core(), &ctx, frame.data(), frame.size());
        checkTable(fixture.arena.get());
    }
    return 0;
}
