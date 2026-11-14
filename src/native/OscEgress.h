// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * OscEgress.h — deferred egress framing + dispatch (transport-agnostic).
 *
 * Producer calls (reply / broadcast / debug, from EngineControl, Link, device
 * and loader threads) frame the OSC into the NRT-out ring
 * under a lock. The NRT gateway drains that ring and dispatches each message to
 * the injected IOscTransport (reply/broadcast/subscribe) — the only thing that
 * knows about sockets, pids or addresses. Debug rides the host's onDebug
 * callback. The engine here deals only in opaque origin tokens.
 */
#pragma once

#include "clockwork_core.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "ReplyChannel.h"
#include "lanes/lanes.h"     // CLOCKWORK_WANTS_*

class IOscTransport;

class OscEgress {
public:
    // transport: the egress sink (replies and pushes to the client).
    // onDebug: host log channel for /clockwork/debug.
    void init(IOscTransport*                                  transport,
              const std::function<void(const std::string&)>*  onDebug);
    // The engine's core, whose NRT-out ring every frame goes into.
    void setCore(ClockworkCore* core) { mCore = core; }
    ClockworkCore* core() const { return mCore; }


    // ── Producer side: frame into the NRT-out ring (never delivers here) ──────
    // An answer; `token` is the origin threaded from the call ctx.
    void reply(uint32_t token, const uint8_t* data, uint32_t size);
    // Answer the verb in `data` with "/clockwork/error <address> <reason>
    // <generation>", to the caller that sent it. The generation is the guest
    // instance's now, read off the arena (docs/RESTARTS.md), as every
    // statechange and setup below carries it.
    void refuse(uint32_t token, const uint8_t* data, uint32_t size, const char* reason);
    // A notice of one kind, for the client: written only while it has asked
    // for that kind (lanes.h, clockwork_client_wants).
    void broadcastToTargets(const uint8_t* data, uint32_t size);      // the engine's lifecycle
    void broadcastClockNotify(const uint8_t* data, uint32_t size);    // the timeline's moves
    void broadcastMidiNotify(const uint8_t* data, uint32_t size);     // a MIDI port's events and the ports
    void broadcastGamepadNotify(const uint8_t* data, uint32_t size);  // a controller's events and the devices
    void broadcastOscNotify(const uint8_t* data, uint32_t size);      // OSC from other programs
    void debug(const char* text, uint32_t len);        // → /clockwork/debug
    // "/clockwork/statechange ,ssi <state> <reason> <generation>": restarting
    // names the generation closing, running after a rebuild the one opened.
    void sendStateChange(const char* state, const char* reason);
    // Caller-targeted statechange for post-init registrants (stream
    // transports connect only after boot and miss the boot broadcast).
    // Note: no setup variant — /clockwork/setup is a rebuild EVENT, never
    // replayed to late joiners (it would trigger spurious client reinits).
    void sendStateChangeTo(uint32_t token, const char* state, const char* reason);
    // The guest instance's generation now, off the arena: the number every
    // refusal and statechange above carries, and a rebuilt instance's setup
    // (docs/RESTARTS.md).
    uint32_t generationNow() const;

    // ── Gateway side: deliver (only the NRT gateway calls these) ──────────────
    // Drain handler for one egress frame (OUT or NRT-out): /clockwork/debug
    // to onDebug, the interceptor, else the transport.
    void dispatchEgress(const uint8_t* osc, uint32_t oscLen);
    // Optional pre-dispatch hook; returns true to swallow the message.
    void setInterceptor(std::function<bool(const uint8_t*, uint32_t)> fn) { mInterceptor = std::move(fn); }
    // A debug line, to the host's log (onDebug).
    void deliverDebug(const char* text, uint32_t len);

    // ── What the client asked to be told: the core's switches (lanes.h) ───────
    // Turn the switches in `kinds` (CLOCKWORK_WANTS_*) on or off: true when
    // that changed any, which is when a verb sends what is there now.
    bool want(uint32_t kinds, bool on);
    bool wanted(uint32_t kind) const;

private:
    void frame(uint32_t token, const uint8_t* osc, uint32_t size);

    IOscTransport*                                  mTransport = nullptr;
    ClockworkCore*                                  mCore = nullptr;
    const std::function<void(const std::string&)>*  mOnDebug   = nullptr;
    std::function<bool(const uint8_t*, uint32_t)>    mInterceptor;  // pre-dispatch swallow hook
};
