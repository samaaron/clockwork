// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_sys.h — the one address prefix clockwork reserves for itself.
 *
 * The split between clockwork and the DSP is mechanical and lives here:
 *
 *     an address beginning "/clockwork/" is the Clockwork's and is answered here;
 *     ANYTHING ELSE is forwarded to the DSP untouched.
 *
 * One reserved string, and nothing more: the DSP keeps essentially the entire
 * OSC namespace. Match on the full "/clockwork/" INCLUDING the trailing slash:
 * "/clockwork-status" and "/clockworks/..." are different address components
 * and are forwarded to the DSP like anything else.
 *
 * ONE PREFIX IS RESERVED, and it is the same in every product that embeds
 * clockwork: "/clockwork/", claimed by clockwork_sys_claims() (clockwork_prefix.h)
 * and answered here. It is not configurable by the product or the guest, and
 * that is a decision (2026-09-11), not an omission: keeping the string fixed
 * keeps the split mechanical and keeps clockwork's verbs portable across
 * products. A guest's own vocabulary lives under a prefix of its choosing —
 * SuperSonic's scsynth uses "/supersonic/" — which clockwork never claims and
 * reserves nothing about; to the product's client the two sit side by side.
 *
 * There is exactly ONE predicate, and it is clockwork_sys_claims() in clockwork_prefix.h.
 * Every clockwork verb lives under the prefix now -- the clock, MIDI, gamepad,
 * OSC and scheduler surfaces included -- so there is nothing else to decide.
 * The prefix itself is spelled ONCE, in CLOCKWORK_SYS_PREFIX_LIT; every clockwork
 * address in the C++ tree is built from it with CLOCKWORK_SYS(), so changing the
 * reserved string is a one-line edit and not a sweep of a hundred literals.
 *
 * A clockwork verb CANNOT be registered outside the prefix, because the type that
 * registers them (ClockworkSysRoutes) takes only the part AFTER it: there is no
 * parameter into which a rogue "/clock/" could be written. And the boundary itself
 * (OscSplit) has no route table at all -- two destinations, clockwork and DSP, and
 * the predicate between them.
 *
 * The verbs answered here are clockwork's own liveness surface. They name no
 * DSP concept, carry no definition format, and mean the same thing whichever
 * DSP is attached — which is exactly why a test can use them to prove the pipe
 * carries bytes without borrowing a DSP's vocabulary:
 *
 *   /clockwork/ping  [,i id]      ->  /clockwork/pong  [,i id]
 *   /clockwork/echo  ,s <text>    ->  /clockwork/echo.reply ,s <text>
 *   /clockwork/echo  ,b <bytes>   ->  /clockwork/echo.reply ,b <bytes>
 *   anything else under /clockwork/ -> /clockwork/error ,ss <address> <reason>
 *
 * An unknown /clockwork/ verb is refused rather than forwarded: the prefix is
 * claimed as a whole, so falling through to the DSP would make the rule depend
 * on which verbs clockwork happens to implement today.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "OscIngress.h"
#include "shared_memory.h"   // ClockworkClockState, readClockworkClock
#include "clockwork_prefix.h"
#include "shared_memory.h"   // ClockworkClockState, readClockworkClock
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

// Refuse one claimed address: "/clockwork/error ,ss <address> <reason>". Split
// out of the handler because on a host with an NRT thread the refusal is
// decided at the END of clockwork's own chain, not at its front.
template <class Emit>
void clockwork_sys_refuse(const uint8_t* data, uint32_t /*len*/, const char* reason, Emit&& emit) {
    char buf[512];
    try {
        osc::OutboundPacketStream ps(buf, sizeof(buf));
        ps << osc::BeginMessage(CLOCKWORK_SYS("error"))
           << reinterpret_cast<const char*>(data) << reason << osc::EndMessage;
        emit(reinterpret_cast<const uint8_t*>(ps.Data()),
             static_cast<uint32_t>(ps.Size()));
    } catch (...) {
        // A refusal that will not encode is dropped: there is nothing truthful
        // left to say, and this runs on the audio thread.
    }
}

// == The client half of the verb surface ========================================
//
// docs/SURFACE.md divides clockwork's verbs by who is in charge. The TRANSPORT
// half — ports, devices, the clock, subscriptions, inbound events — exists
// because clockwork owns the hardware, and every guest needs it. The CLIENT
// half exists because something outside clockwork is driving it event by
// event: a language runtime sending one note, one OSC message, one beat of
// MIDI clock at a time. A guest that is in charge of itself never sends any
// of them (it reaches a wire through clockwork_sink_send with its own time),
// so a build shaped for such a guest compiles them out — CLOCKWORK_CLIENT_VERBS
// — as it already compiles out the timed store (CLOCKWORK_SCHEDULER), which
// /clockwork/schedule and /clockwork/sched/flush follow.
//
// THIS IS THE LIST. The boundaries that answer these verbs consult it when
// the option is off, so that the refusal names what went and why, rather
// than the verb quietly falling through as unknown — which would tell a
// client the verb was never real rather than that this build dropped it.
inline bool clockwork_sys_is_client_verb(const char* addr) {
    if (addr == nullptr) return false;
    // Every "/clockwork/midi/out/*" send: note_on, note_off, control_change,
    // pitch_bend, program_change, channel_pressure, poly_pressure, raw, sysex,
    // clock, start, stop, continue. NOT "/clockwork/midi/out/enable", which
    // opens a port and is the transport's.
    if (std::strncmp(addr, CLOCKWORK_SYS("midi/out/"), CLOCKWORK_SYS_LEN("midi/out/")) == 0)
        return std::strcmp(addr, CLOCKWORK_SYS("midi/out/enable")) != 0;
    return std::strcmp(addr, CLOCKWORK_SYS("midi/clock/beat")) == 0
        || std::strcmp(addr, CLOCKWORK_SYS("osc/send")) == 0;
}

// The reason a client verb is refused with when the build has none.
inline constexpr const char* CLOCKWORK_CLIENT_VERBS_ABSENT =
    "client verb: this build has none (CLOCKWORK_CLIENT_VERBS=OFF)";

// What the audio-thread half of clockwork did with a packet.
enum class ClockworkSysRt {
    NotClaimed,   // not a /clockwork/ address at all -- the DSP's
    Answered,     // answered (or refused) here and now
    UnknownVerb   // ours by prefix, but not a verb this layer answers
};

// The verbs answerable ON THE AUDIO THREAD, with no engine and no NRT hop:
// ping, echo and the clock snapshot. Anything else under the prefix comes
// back UnknownVerb so the caller can pass it along its own chain (a host with
// an NRT thread forwards it there) before anyone decides it is unknown.
//
// Pure byte work: no globals, no engine, no allocation beyond the caller's
// stack buffer -- so it is directly testable without an ingress or a device.
template <class Emit>
ClockworkSysRt handle_clockwork_sys_rt(const uint8_t* data, uint32_t len, Emit&& emit,
                                       const ClockworkClockState* clock = nullptr) {
    if (!clockwork_sys_claims(data, len)) return ClockworkSysRt::NotClaimed;

    // 512 bytes: /clockwork/echo is the only verb that carries a payload back,
    // and one that does not fit is refused rather than truncated into
    // something a client would misread.
    char buf[512];
    const char* verb = reinterpret_cast<const char*>(data) + CLOCKWORK_SYS_PREFIX_LEN;

    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data), static_cast<osc::osc_bundle_element_size_t>(len)));
        auto arg = msg.ArgumentsBegin();
        const auto end = msg.ArgumentsEnd();

        if (std::strcmp(verb, "ping") == 0) {
            osc::OutboundPacketStream ps(buf, sizeof(buf));
            ps << osc::BeginMessage(CLOCKWORK_SYS("pong"));
            if (arg != end && arg->IsInt32()) ps << arg->AsInt32Unchecked();
            ps << osc::EndMessage;
            emit(reinterpret_cast<const uint8_t*>(ps.Data()),
                 static_cast<uint32_t>(ps.Size()));
            return ClockworkSysRt::Answered;
        }

        // The whole clock, coherent, from the audio thread.
        //
        // ONE SNAPSHOT, NOT SIX GETTERS: bpm and beat origin only mean
        // something together, and a client assembling them from separate
        // replies can catch a tempo change between two of them.
        //
        // It answers here rather than on the control thread because a caller
        // that wants the clock the current block is being rendered against
        // cannot wait for an NRT hop. readClockworkClock reads the published
        // mirror, which is what makes that safe: on native+Link the clock's
        // own getters route through captureAppSessionState(), documented
        // "Realtime-safe: no".
        if (std::strcmp(verb, "clock/state/get") == 0) {
            if (!clock) return ClockworkSysRt::UnknownVerb;
            const auto c = readClockworkClock(clock);
            osc::OutboundPacketStream ps(buf, sizeof(buf));
            ps << osc::BeginMessage(CLOCKWORK_SYS("clock/state.reply"))
               << c.bpm
               << (c.is_playing ? 1 : 0)
               << c.beat_origin_ntp
               << c.is_playing_at_ntp
               << static_cast<int32_t>(c.flags)
               << c.meter_num << c.meter_den;
            // Echo a caller's token back as the last argument, so a reply that
            // arrives after its caller gave up is still attributable.
            if (arg != end && arg->IsInt32()) ps << arg->AsInt32Unchecked();
            ps << osc::EndMessage;
            emit(reinterpret_cast<const uint8_t*>(ps.Data()),
                 static_cast<uint32_t>(ps.Size()));
            return ClockworkSysRt::Answered;
        }

        if (std::strcmp(verb, "echo") == 0) {
            osc::OutboundPacketStream ps(buf, sizeof(buf));
            ps << osc::BeginMessage(CLOCKWORK_SYS("echo.reply"));
            if (arg == end) {
                clockwork_sys_refuse(data, len, "echo takes one string or blob", emit);
                return ClockworkSysRt::Answered;
            }
            if (arg->IsString()) {
                ps << arg->AsStringUnchecked();
            } else if (arg->IsBlob()) {
                const void* b = nullptr;
                osc::osc_bundle_element_size_t n = 0;
                arg->AsBlobUnchecked(b, n);
                ps << osc::Blob(b, static_cast<osc::osc_bundle_element_size_t>(n));
            } else {
                clockwork_sys_refuse(data, len, "echo takes one string or blob", emit);
                return ClockworkSysRt::Answered;
            }
            ps << osc::EndMessage;
            emit(reinterpret_cast<const uint8_t*>(ps.Data()),
                 static_cast<uint32_t>(ps.Size()));
            return ClockworkSysRt::Answered;
        }
    } catch (...) {
        clockwork_sys_refuse(data, len, "malformed", emit);
        return ClockworkSysRt::Answered;
    }

    return ClockworkSysRt::UnknownVerb;
}

// Answer one /clockwork/ message with no chain behind it: the RT verbs, and a
// refusal for everything else under the prefix. This is the END of
// clockwork's chain on a host with no NRT thread (the worklet); a host that has
// one binds it to the RT verbs alone, and the refusal happens at the far end of
// that thread. Returns true iff the packet was claimed; false leaves it for the
// DSP.
template <class Emit>
bool handle_clockwork_sys_osc(const uint8_t* data, uint32_t len, Emit&& emit,
                              const ClockworkClockState* clock = nullptr) {
    switch (handle_clockwork_sys_rt(data, len, emit, clock)) {
        case ClockworkSysRt::NotClaimed:  return false;
        case ClockworkSysRt::Answered:    return true;
        case ClockworkSysRt::UnknownVerb: break;
    }
    clockwork_sys_refuse(data, len, "unknown clockwork verb", emit);
    return true;
}

// The registered handler for clockwork's audio-thread verbs, bound to the RT
// egress. Defined in audio_processor.cpp beside the other audio-thread routes.
// routeCtx is ignored -- the egress is fixed for the audio thread.
bool clockwork_clockwork_sys_route(void* routeCtx, const void* callCtx,
                      const uint8_t* data, std::size_t len);
// The same, on a host with no NRT thread that may have a FRONT of its own
// (lanes.h, clockwork_host_forward): the liveness verbs are answered here and
// anything else the prefix claimed is forwarded to the host over the egress,
// wrapped in a bundle carrying the call's time, for the host to answer or
// refuse. With forwarding off it is clockwork_clockwork_sys_route. Defined in
// audio_processor.cpp.
bool clockwork_host_forward_route(void* routeCtx, const void* callCtx,
                                  const uint8_t* data, std::size_t len);
// "/clockwork/midi/in/…", "/clockwork/gamepad/in/…", "/clockwork/midi/ports",
// "/clockwork/gamepad/devices": an INBOUND EVENT, wherever it came from — a
// native subsystem's callback, or a host's front on the main thread — reaches
// the audio thread through ingress and is answered here on every host: sent
// out over the egress to the clients subscribed to that subsystem, and handed
// to the guest if it asked (DspInfo::wants_events). One route, one shape, so
// a client and a guest cannot tell which host produced the event. Defined in
// audio_processor.cpp.
bool clockwork_event_route(void* routeCtx, const void* callCtx,
                           const uint8_t* data, std::size_t len);
// "/clockwork/asset/…": the hand-off of a committed inbox slot to the guest
// (dsp_api.h, "Assets"). Registered on the audio thread of every host, since
// the guest is only ever reached from there.
bool clockwork_asset_route(void* routeCtx, const void* callCtx,
                           const uint8_t* data, std::size_t len);

// The DSP's route: every address the predicate did not claim, handed over
// untouched. Defined in audio_processor.cpp.
bool clockwork_dsp_default_route(void* routeCtx, const void* callCtx,
                          const uint8_t* data, std::size_t len);

// == The boundary ================================================================
//
// OscSplit is clockwork/DSP boundary and the ONLY thing in this repository
// that decides which side a packet is on. It deliberately has no route table
// and no registerRoute(): two destinations, and clockwork_sys_claims() between them.
// A subsystem cannot acquire a second predicate by registering against this
// object, because there is nothing here to register against.
class OscSplit {
public:
    using Handler = OscIngress::Handler;

    // Everything under the reserved prefix.
    void setSys(Handler h, void* ctx) noexcept { mSys = { h, ctx }; }
    // Everything else, bundles included.
    void setDsp(Handler h, void* ctx) noexcept { mDsp = { h, ctx }; }

    bool wired() const noexcept { return mSys.h != nullptr || mDsp.h != nullptr; }

    // Classify one raw OSC packet and hand it to its side. Never reads past
    // len. Returns true iff a side consumed it; false when the packet is not a
    // '/'-led NUL-terminated address (or a bundle), or the side is unwired.
    bool ingest(const uint8_t* data, size_t len, const void* callCtx) const noexcept {
        if (data == nullptr || len < 4) return false;
        // A bundle is a schedule, and the schedule is the DSP's.
        if (len >= 8 && std::memcmp(data, "#bundle", 8) == 0)
            return dispatch(mDsp, callCtx, data, len);
        if (data[0] != '/') return false;
        size_t addr = 0;
        while (addr < len && data[addr] != '\0') ++addr;
        if (addr == len) return false;   // address not NUL-terminated within bounds
        return dispatch(clockwork_sys_claims(data, addr) ? mSys : mDsp, callCtx, data, len);
    }

private:
    struct Dest { Handler h = nullptr; void* ctx = nullptr; };
    static bool dispatch(const Dest& d, const void* cc, const uint8_t* data, size_t len) noexcept {
        return d.h ? d.h(d.ctx, cc, data, len) : false;
    }
    Dest mSys;
    Dest mDsp;
};

// Clockwork's OWN dispatch, behind the boundary. A plain prefix registry -- but
// one whose add() takes the part of the address AFTER the reserved prefix and
// prepends it here, so a clockwork route cannot be spelled outside the prefix
// even by accident. There is no parameter in which to write "/clock/".
//
// Because it sits behind the boundary, a route registered here can never steal an
// address from the DSP: the boundary only ever reaches it for addresses the
// predicate already claimed.
class ClockworkSysRoutes {
public:
    using Handler = OscIngress::Handler;

    // `suffix` is what follows the prefix -- "clock/", "midi/", "ping".
    // Include a trailing '/' for a namespace, omit it for a single verb.
    // Longest match wins: a single verb registered beside its namespace
    // goes to its own handler, not the namespace's.
    bool add(const char* suffix, Handler h, void* ctx) noexcept {
        if (suffix == nullptr) return false;
        char full[kMaxAddr];
        size_t n = 0;
        while (suffix[n] != '\0') { if (++n > kMaxAddr - CLOCKWORK_SYS_PREFIX_LEN - 1) return false; }
        if (n == 0) return false;   // the bare prefix would swallow every verb
        std::memcpy(full, CLOCKWORK_SYS_PREFIX, CLOCKWORK_SYS_PREFIX_LEN);
        std::memcpy(full + CLOCKWORK_SYS_PREFIX_LEN, suffix, n + 1);
        return mRoutes.registerRoute(full, h, ctx);
    }

    // Where a claimed address that matched no route goes. On a host with no
    // NRT thread that is clockwork_clockwork_sys_route (answer or refuse); on one that has
    // it, the forward to that thread, and the refusal happens at its far end.
    void setFallback(Handler h, void* ctx) noexcept { mRoutes.setDefault(h, ctx); }

    bool ingest(const uint8_t* d, size_t n, const void* cc) const noexcept {
        return mRoutes.ingest(d, n, cc);
    }

    // Plugs straight into OscSplit::setSys(&ClockworkSysRoutes::route, &routes).
    static bool route(void* self, const void* cc, const uint8_t* d, std::size_t n) {
        return static_cast<const ClockworkSysRoutes*>(self)->ingest(d, n, cc);
    }

    size_t routeCount() const noexcept { return mRoutes.routeCount(); }

private:
    static constexpr size_t kMaxAddr = 64;
    OscIngress mRoutes;
};

// The boundary the audio-thread drain classifies through, published by the engine
// at init (native: ClockworkEngine::mSplit; worklet/lanes hosts: a file-static
// in audio_processor.cpp). Mirrors g_active_clockwork_clock -- single publisher, the
// audio thread loads it `acquire` and null-checks. Defined in
// audio_processor.cpp (compiled by both targets).
extern std::atomic<OscSplit*> g_active_split;
