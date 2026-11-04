// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
#include "core_state.h"

#include "audio_processor.h"   // teardown_memory
#include "clock/ClockworkClock.h"   // the core's own clock
#include "clockwork_event_sink.h"   // the sinks' host emitter
#include "mem_region.h"             // the staging's span
#include "memory_profile.h"          // CLOCKWORK_ASSET_SLOTS
#if CLOCKWORK_SCHEDULER
#include "scheduler/engine_schedule.h"   // ScheduleStore
#endif

#include <memory>
#include <new>

namespace clockwork {
namespace {

// What clockwork_core_close calls, while the core is still live. Closing a
// core ends its engine (docs/ENGINES.md): its instance, its memory and
// its arena go, and the next clockwork_init is a cold boot. What the engine
// says as it goes is its core's.
void free_state(void* state) {
    auto* st = static_cast<CoreState*>(state);
    if (!st) return;
    {
        const GateHold gate(st->core, CLOCKWORK_ENTRY_CONTROL, GateHold::Take::Wait);
#if !defined(__EMSCRIPTEN__) && CLOCKWORK_SYNTH
        if (st->arena.at()) teardown_memory(st->core);
#endif
#if !defined(__EMSCRIPTEN__) && !CLOCKWORK_HAS_TIERED_MEMORY
        clockwork_free_local_arena(st->core);
#endif
        // The sinks' host emitter is a record in this state: withdrawn
        // before the state goes.
        if (st->routing.host_emitter_installed) clockwork_sink_set_host_emitter(st->sinks, nullptr);
    }
#if CLOCKWORK_SCHEDULER
    delete st->schedule.store;
#endif
    delete st->clock.own;
    delete st;
}

} // namespace

void Staging::Free::operator()(void* span) const { mem::free(span); }

GateHold::GateHold(ClockworkCore* core, ClockworkEntryKind kind, Take take) : core_(core) {
    held_ = holds_gate(core);
    if (!held_) {
        const int took = clockwork_core_gate_take(core, take == Take::Wait);
        taken_  = took == CLOCKWORK_GATE_TAKEN;
        held_   = taken_;
        closed_ = took == CLOCKWORK_GATE_CLOSED;
    }
    previous_ = clockwork_core_enter({core, kind, held_});
    // A control call is a call into the core that is not a block: the host's
    // memory a block parked goes back here, so a core with no thread of its
    // own (an attached one) gives it back too.
    if (taken_ && take == Take::Wait) clockwork_release_parked_frees(core);
}

GateHold::~GateHold() {
    clockwork_core_leave(previous_);
    if (taken_) clockwork_core_gate_release(core_);
}

static_assert(CLOCKWORK_ASSET_SLOTS > 0 && (CLOCKWORK_ASSET_SLOTS & (CLOCKWORK_ASSET_SLOTS - 1)) == 0,
              "CLOCKWORK_ASSET_SLOTS is a power of two");

Assets::Assets()
    : slots(new (std::nothrow) Slot[CLOCKWORK_ASSET_SLOTS]()),
      count(slots ? CLOCKWORK_ASSET_SLOTS : 0) {}

Assets::Slot* Assets::lookup(uint32_t id) {
    for (uint32_t i = 0, h = id * 2654435761u; i < count; ++i, ++h) {
        Slot& s = slots[h & (count - 1)];
        if (s.used && s.id == id) return &s;
        if (!s.used) return nullptr;
    }
    return nullptr;
}

Assets::Slot* Assets::insert(uint32_t id, uint32_t origin) {
    for (uint32_t i = 0, h = id * 2654435761u; i < count; ++i, ++h) {
        Slot& s = slots[h & (count - 1)];
        if (!s.used || s.id == kTombstone) {
            s = Slot{};
            s.id = id; s.origin = origin; s.used = true;
            return &s;
        }
    }
    return nullptr;
}

bool make_state(ClockworkCore* core) {
    if (!clockwork_core_live(core)) return false;
    if (state(core)) return true;
    std::unique_ptr<CoreState> made(new (std::nothrow) CoreState(core));
    if (!made) return false;
    if (!clockwork_core_adopt_state(core, made.get(), &free_state)) return state(core) != nullptr;
    made.release();
    return true;
}

} // namespace clockwork
