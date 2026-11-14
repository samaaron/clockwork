// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * EngineClock.h — the portable clock-core /clock verbs, shared by every build.
 *
 * tempo, transport, start/stop sync, the rpc beat/time conversions, peer count
 * and the enabled/now queries — all expressed against ClockworkClock, whose one
 * composition root (ClockworkClockNative) backs every build. The native engine calls
 * this from EngineControl (egress hub); the web worklet calls it from audio_processor's
 * IN-drain (OUT-ring egress). One implementation, so /clock behaves identically
 * everywhere. Replies leave through `reply`. Native-only Link-session verbs
 * (visibility, peers, Link Audio, notify) are NOT here — they stay in
 * EngineControl, capability-gated to the native build.
 */
#pragma once

#include <cstdint>
#include <functional>

class ClockworkClock;

using ClockReply = std::function<void(const uint8_t* data, uint32_t size)>;

// How a set of the session's timeline is applied, for a caller on the audio
// thread: the clock's own mutators may wait, and these must not. `atNtp` 0 is
// now.
struct ClockSets {
    void* ctx;
    void (*tempo)(void* ctx, double bpm, double atNtp);
    void (*playing)(void* ctx, bool playing);
    void (*meter)(void* ctx, int32_t num, int32_t den);
};

// Returns true if `data` is a clock-core /clock verb it handled (so the caller
// stops), false for a non-/clock message or a /clock verb it doesn't own.
// `sets` null: the caller may wait (a control thread), and the session's
// timeline is set through the clock's own mutators. Given: the caller is the
// audio thread, the timeline is set through `sets`, and the verbs that are
// Link's own session's are left to whoever has a thread for them.
bool handleClockCoreOsc(ClockworkClock& clock, const uint8_t* data, uint32_t size,
                        const ClockReply& reply, const ClockSets* sets = nullptr);

// The one refusal for a /clock verb no route owns: replies
// "/clockwork/clock/unsupported s:address", echoing the request's trailing int32
// correlation token like every other /clock reply. Both dispatch tails
// (native EngineControl, wasm audio_processor) call this so the refusal
// wire contract has a single authoring site.
void replyClockUnsupported(struct ClockworkCore* core, const uint8_t* data, uint32_t size,
                           const ClockReply& reply);
