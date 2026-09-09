// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * OscControl.h — the "/clockwork/osc/" engine boundary (native-only).
 *
 * A thin bridge to the Rust OSC subsystem (clockwork-osc-net, clockwork_osc.h), which
 * owns the external-facing UDP sockets: the cue server (inbound external OSC,
 * re-framed to /external-osc-cue and pushed to the /clockwork/osc/notify audience) and the
 * outbound user-OSC sender. This boundary only translates between the engine and the
 * Rust C ABI: control verbs → clockwork_osc_configure, the send verb →
 * clockwork_osc_send, and the subsystem's emit callback → the egress.
 */
#pragma once

#include "clockwork_event_sink.h"

#include <cstdint>
#include <string>

struct ClockworkOsc;       // rust/clockwork-osc-net/cpp/clockwork_osc.h
class OscEgress;
struct DrainCallCtx;

class OscControl {
public:
    void init(OscEgress* egress);
    void shutdown();

    // Handle one "/clockwork/osc/" control command off the audio thread (the control pass):
    //   /clockwork/osc/send <host:s> <port:i> <inner:b>  — send inner to host:port. Reached
    //       immediately, or as the inner blob of a /clockwork/schedule event (the scheduler
    //       re-ingests it on time → this same dispatch). A client verb: refused
    //       by name when CLOCKWORK_CLIENT_VERBS is off.
    //   /clockwork/osc/cue-server/config <port:i> <loopback:i|T/F> <cues_on:i|T/F>
    //   /clockwork/osc/cue-server/cues-on  <i|T/F>   — toggle inbound cue forwarding
    //   /clockwork/osc/cue-server/loopback <i|T/F>   — loopback-only vs all interfaces
    //   /clockwork/osc/notify/subscribe | /clockwork/osc/notify/unsubscribe
    // Returns true (always, for an "/clockwork/osc/" prefix).
    bool handleOscCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);

private:
    // clockwork_osc emit callback (ctx = this): an /external-osc-cue push for inbound
    // external OSC. May fire on the cue server's recv thread.
    static void emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len);
    // Push the current (port, loopback, cues_on) to the subsystem (idempotent;
    // the Rust side only rebinds when the port/loopback actually change).
    void applyCueConfig();
#if CLOCKWORK_CLIENT_VERBS
    // The sink onto host:port for "/clockwork/osc/send", opened on first use.
    // The client half of the surface (docs/SURFACE.md): not compiled when the
    // build has none.
    ClockworkSink sinkFor(const std::string& host, int port);
    // How many OSC endpoints a session sends to. Each sink's shape — the
    // size classes and their depths — is the OSC profile's
    // (CLOCKWORK_OSC_SINK_CLASSES in memory_profile.h); 0 at open takes it.
    static constexpr uint32_t kMaxSinks     = 64;
    static constexpr uint32_t kSinkCapacity = 0;
#endif

    OscEgress* mEgress = nullptr;
    ClockworkOsc*     mOsc      = nullptr;
    int        mPort     = 0;      // cue port (0 = unbound)
    bool       mLoopback = true;   // loopback-only by default
    bool       mCuesOn   = false;  // forward inbound external OSC as cues
};
