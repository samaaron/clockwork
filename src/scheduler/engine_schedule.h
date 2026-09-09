// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * clockwork
 * Copyright (c) 2025 Sam Aaron
 *
 *
 * engine_schedule.h — clockwork's own timed queue.
 *
 * This replaces scheduler/dsp_schedule.h, which declared nine entry points a
 * DSP had to implement so clockwork could park, flush, count and clear a
 * schedule it did not own. None of that crossed the boundary: dsp_api.h says a DSP
 * either holds its own schedule or does not, and either way Clockwork keeps
 * a queue — timed MIDI output and timed OSC forwarding are its own features
 * and exist with no DSP attached at all.
 *
 * So everything here is clockwork-internal. Nothing in this file is implemented
 * by, or visible to, a DSP; the only thing a DSP ever sees of a timed message
 * is the timetag that arrives with it through dsp_osc().
 *
 * Tag constants come from Scheduler.h (FNV-1a over a short string), so a
 * flush names what it cancels rather than counting slots.
 *
 * ── Two switches on one axis, and how they compose ──────────────────────────
 *
 * There are two ways a timed message can end up going straight through instead
 * of waiting here, and they are NOT redundant:
 *
 *   DspInfo::holds_schedule   runtime, per DSP. "Do not hold messages for me;
 *                             hand them over as they arrive, carrying their
 *                             timetag." clockwork declares this: its VM parks
 *                             programs, not messages.
 *
 *   CLOCKWORK_SCHEDULER     compile time, per build. "This build has no
 *                             timed store at all." The store half of the Rust
 *                             crate behind Scheduler.h is not even compiled.
 *
 * They are independent because the queue is not only for the DSP. Timed MIDI
 * out and timed OSC forwarding are the Clockwork's own features, so a build with
 * the store ON may perfectly well host a DSP that holds its own schedule —
 * that is clockwork plus a host that wants timed MIDI, and it is expected to be
 * common.
 *
 * The composition rule, in one line: WITH THE OPTION OFF, holds_schedule IS
 * MOOT. There is nowhere to hold anything, so every message forwards at once
 * carrying its original timetag, whatever the DSP declared — including a DSP
 * that declared holds_schedule = 0 and therefore asked clockwork to hold. It
 * asked for something the build cannot provide, and the honest answer is to
 * hand it the message with its time intact and let it decide, rather than to
 * drop it or to flatten the time to "now". The one thing that does NOT go
 * quietly is /clockwork/schedule itself, which is refused with a reason naming
 * the option: a verb that vanishes without explanation is the worst outcome of
 * the three.
 */
#pragma once

#include <cstdint>

#if CLOCKWORK_SCHEDULER

#include "Scheduler.h"   // sched_tag_hash + SCHED_TAG_* + the generic store

// Per-event metadata. The origin token is threaded so a reply to a message
// that fired late still reaches the client that sent it.
struct EngineMeta {
    uint32_t origin = 0;
};

// The one instantiation clockwork runs. Sizes come from the build profile
// (SCHEDULER_SLOT_COUNT / SCHEDULER_DATA_POOL_SIZE) so an embedded profile can
// shrink it without touching this header.
using EngineScheduler = Scheduler<EngineMeta, SCHEDULER_SLOT_COUNT, SCHEDULER_DATA_POOL_SIZE>;

// The process-wide instance (audio_processor.cpp). Clockwork is a
// process singleton — one arena, one clock, one ingress — and the queue is
// part of that, so it is reached by accessor rather than passed around.
EngineScheduler& clockwork_engine_schedule();

#else  // !CLOCKWORK_SCHEDULER

// Deliberately nothing. No null-object store stands in: a store that silently
// swallowed an add would turn "this build cannot hold your message" into "your
// message was held and never fired", which is the failure this option exists
// to avoid. Every caller is gated on CLOCKWORK_SCHEDULER instead, so the
// absence is a compile error at any site that was not thought about.
//
// The reason a caller is missing here rather than answering differently is in
// the block above; the refusal for /clockwork/schedule and /clockwork/sched/flush
// is in audio_processor.cpp and ClockworkEngine.cpp respectively.

#endif // CLOCKWORK_SCHEDULER
