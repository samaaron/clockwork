// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * MidiControl.cpp — see MidiControl.h. The Rust subsystem runs midir on its own
 * threads; its callbacks may fire off the audio thread, so everything they touch
 * (the egress ring, ClockworkClock setters) is already thread-safe.
 */
#include "clockwork_prefix.h"
#include "MidiControl.h"

#include "ClientVerbs.h"        // refuseClientVerb, when the build has none
#include "IngressCallCtx.h"
#include "OscEgress.h"
#include "SubscribeAck.h"
#include "clock/ClockworkClock.h"
#include "clock/timeline_osc.h"
#include "clockwork_config.h"   // clockwork_log
#include "clockwork_midi.h"
#include "lanes/lanes.h"        // clockwork_ingress_write: an event enters like any other

extern "C" const char* clockwork_app_name();
#include "clock/MidiClockOut.h"
#include "osc/OscReceivedElements.h"
#include "osc/OscOutboundPacketStream.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

void MidiControl::init(OscEgress* egress, ClockworkClock* clock, MidiClockOut* clockOut) {
    mEgress   = egress;
    mClock    = clock;
    mClockOut = clockOut;
    // Push /clockwork/clock/timelines whenever the timeline set changes (add/remove/
    // stale/primary). Fires off the RT thread (MIDI feed / staleness worker).
    if (mClock)
        mClock->setTimelinesChangedCallback([this]() {
            broadcastTimelines();
        });
    if (!mMidi) {
        mMidi = clockwork_midi_create(this, &MidiControl::emitCb, &MidiControl::clockCb,
                               &MidiControl::transportCb, clockwork_app_name());
    }
}

void MidiControl::shutdown() {
    if (mMidi) {
        clockwork_midi_destroy(mMidi);
        mMidi = nullptr;
    }
}

bool MidiControl::handleMidiCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    const uint32_t token = meta.sourceId;
    if (size < CLOCKWORK_SYS_LEN("midi/") + 1 ||
        std::memcmp(data, CLOCKWORK_SYS("midi/"), CLOCKWORK_SYS_LEN("midi/")) != 0) return false;
    // Hold the origin for synchronous emitCb REPLYs during this call (see header).
    mReplyToken = token;

    // Subscription drives the egress audience (owned by the transport). The
    // address is the leading, NUL-terminated OSC string.
    const char* addr = reinterpret_cast<const char*>(data);
    if (std::strcmp(addr, CLOCKWORK_SYS("midi/notify/subscribe")) == 0) {
        if (mEgress && mEgress->subscribeCallerToMidiNotify(token) && mMidi)
            clockwork_midi_emit_ports(mMidi);   // ports snapshot to the new subscriber
        ackSubscribeIfTokened(mEgress, token, data, size,
                              CLOCKWORK_SYS("midi/notify/subscribe.reply"));
        return true;
    }
    if (std::strcmp(addr, CLOCKWORK_SYS("midi/notify/unsubscribe")) == 0) {
        if (mEgress) mEgress->unsubscribeCallerFromMidiNotify(token);
        return true;
    }

#if !CLOCKWORK_CLIENT_VERBS
    // The client half is not in this build (docs/SURFACE.md): every
    // "/clockwork/midi/out/*" send and the beat verb are refused BY NAME,
    // here, before the Rust subsystem — which would otherwise send them at
    // once, exactly as if the option were on.
    if (clockwork_sys_is_client_verb(addr)) {
        refuseClientVerb(mEgress, token, data, size);
        return true;
    }
#endif

    if (handleClockOutVerb(data, size)) return true;

#if CLOCKWORK_CLIENT_VERBS
    if (handleOutVerb(meta, data, size)) return true;
#endif
    if (mMidi) clockwork_midi_handle_osc(mMidi, data, size);
    return true;
}

#if CLOCKWORK_CLIENT_VERBS
// Every "/clockwork/midi/out/*" send leaves through the sink for its port,
// carrying its time. The time is the message's own: the trailing timetag if
// the verb named one, else the call's — which for a note inside
// "/clockwork/schedule" is the moment it was scheduled for, threaded here from
// the audio thread by nrtForwardSink. The sink holds it to that moment (or
// hands it to a platform that timestamps, CoreMIDI / ALSA), and counts it.
// Sending it from here at once, which is what clockwork_midi_handle_osc does, would
// release it at the block it fired in — up to a block early, plus whatever
// this thread's wake cost. The Rust side still owns the encoding.
bool MidiControl::handleOutVerb(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    struct Ctx { int64_t when; uint32_t dropped; } ctx{ meta.when, 0 };
    const auto onMessage = [](void* c, const uint8_t* port, uint32_t portLen,
                              const uint8_t* bytes, uint32_t len, uint64_t verbWhen) {
        auto* self = static_cast<Ctx*>(c);
        const std::string name(reinterpret_cast<const char*>(port), portLen);
        const ClockworkSink sink = midi_sink_for_port(name.empty() ? "*" : name);
        // 0 and 1 both mean now to the sink; a verb's own time wins over the
        // call's, as it does when the verb is handled by the Rust side.
        const int64_t when = verbWhen ? static_cast<int64_t>(verbWhen)
                                      : (self->when ? self->when : 1);
        if (sink == CLOCKWORK_SINK_NONE || !clockwork_sink_send(sink, bytes, len, when))
            ++self->dropped;
    };
    if (!clockwork_midi_encode_out(data, size, &ctx, onMessage)) return false;
    if (ctx.dropped)
        clockwork_log("WARNING: %s: %u message(s) had no port to go to",
                reinterpret_cast<const char*>(data), ctx.dropped);
    return true;
}
#endif // CLOCKWORK_CLIENT_VERBS

// The clock-out verbs, all delivered by MidiClockOut with each byte carrying
// its own time to its MIDI sink — so they work whatever the DSP declares
// about holding a schedule and whether or not clockwork's timed queue is compiled
// in. /clockwork/midi/clock/tick (one immediate pulse) and /clockwork/midi/clock/sync
// (the per-port clock-in follow toggle) are not handled here and fall through
// to the Rust subsystem.
//
//   /clockwork/midi/clock/beat <port:s> <durMs:f>
//       One beat's worth (24 ticks) spread over durMs, from now (a client's
//       midi_clock_beat). No reply.
//   /clockwork/midi/clock/follow <port:s> [<timeline:s>="link"] [token:i]
//       A continuous clock on <port> following <timeline> — "link", "midi"
//       (the primary follower) or "midi:<handle>", the names /clockwork/clock/
//       uses; a "midi:<handle>" not yet seen is claimed, as the clock verbs'
//       write path does. A port already following is re-targeted.
//       Replies follow.reply <port> <timeline> [token]. A name that resolves
//       to nothing (bare "midi" with no clock arriving, or a malformed name)
//       is refused with a log line and no reply, like a tempo/set on one.
//   /clockwork/midi/clock/unfollow <port:s> [token:i]
//       Stop it. Replies unfollow.reply <port> [token] whether or not the
//       port was following.
//   /clockwork/midi/clock/followers [token:i]
//       Replies followers.reply [<port:s> <timeline:s>]* [token], flat rows
//       like /clockwork/clock/timelines.reply.
//
// <port> is a MIDI port name, or "*" for every open MIDI sink. The token is
// the trailing int32 the /clockwork/clock verbs take: echoed last in the reply.
bool MidiControl::handleClockOutVerb(const uint8_t* data, uint32_t size) {
    if (!mClock || !mClockOut || size < 16) return false;
    const char* addr = reinterpret_cast<const char*>(data);
    enum { Beat, Follow, Unfollow, Followers } verb;
    if      (std::strcmp(addr, CLOCKWORK_SYS("midi/clock/beat"))      == 0) verb = Beat;
    else if (std::strcmp(addr, CLOCKWORK_SYS("midi/clock/follow"))    == 0) verb = Follow;
    else if (std::strcmp(addr, CLOCKWORK_SYS("midi/clock/unfollow"))  == 0) verb = Unfollow;
    else if (std::strcmp(addr, CLOCKWORK_SYS("midi/clock/followers")) == 0) verb = Followers;
    else return false;

    // Positional strings and a number, plus the optional trailing token.
    std::string port = "*", timeline = "link";
    double  num      = 0.0;
    bool    hasToken = false;
    int32_t token    = 0;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(size)));
        int strings = 0;
        for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
            auto next = it; ++next;
            const bool last = next == msg.ArgumentsEnd();
            if (last && it->IsInt32() && verb != Beat) {
                hasToken = true;
                token    = it->AsInt32Unchecked();
            } else if (it->IsString()) {
                if (strings == 0)      port     = it->AsStringUnchecked();
                else if (strings == 1) timeline = it->AsStringUnchecked();
                ++strings;
            } else if (it->IsFloat())  num = it->AsFloatUnchecked();
            else if (it->IsDouble())   num = it->AsDoubleUnchecked();
            else if (it->IsInt32())    num = it->AsInt32Unchecked();
        }
    } catch (...) { return true; }  // malformed — swallow, as the beat verb always has

    char buf[2048];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    auto finish = [&](osc::OutboundPacketStream& out) {
        if (hasToken) out << static_cast<osc::int32>(token);
        out << osc::EndMessage;
        if (mEgress)
            mEgress->reply(mReplyToken, reinterpret_cast<const uint8_t*>(out.Data()),
                           static_cast<uint32_t>(out.Size()));
    };

    switch (verb) {
    case Beat:
        mClockOut->onBeat(*mClock, port, num / 1000.0);
        return true;

    case Follow: {
        // The write-path resolver: a midi:<handle> nobody has clocked yet is
        // still a valid thing to follow, and is claimed here as tempo/set
        // would claim it.
        const int id = mClock->resolveOrClaimTimeline(timeline.c_str());
        if (id < 0) {
            clockwork_log("WARNING: %s: no timeline named '%s' to follow on '%s'",
                    addr, timeline.c_str(), port.c_str());
            return true;
        }
        // The sink is resolved HERE, on the command thread (see MidiClockOut.h).
        if (!mClockOut->follow(port, timeline, id, midi_clock_sink_for_port(port))) {
            clockwork_log("WARNING: %s: %u ports already follow a clock; '%s' refused",
                    addr, MidiClockOut::kMaxFollowers, port.c_str());
            return true;
        }
        s << osc::BeginMessage(CLOCKWORK_SYS("midi/clock/follow.reply"))
          << port.c_str() << timeline.c_str();
        finish(s);
        return true;
    }

    case Unfollow:
        mClockOut->unfollow(port);
        s << osc::BeginMessage(CLOCKWORK_SYS("midi/clock/unfollow.reply")) << port.c_str();
        finish(s);
        return true;

    case Followers:
        s << osc::BeginMessage(CLOCKWORK_SYS("midi/clock/followers.reply"));
        for (const auto& f : mClockOut->followers())
            s << f.port.c_str() << f.timeline.c_str();
        finish(s);
        return true;
    }
    return true;
}

void MidiControl::refreshDevices() {
    if (mMidi) clockwork_midi_refresh(mMidi);
}

// Hotplug logging. /clockwork/midi/ports broadcasts fire on a device change or an
// enable toggle, never per event, so this is flood-safe. (The snapshot a new
// subscriber gets is a ports.reply to that caller, and never reaches here.)
// Per-event traffic (/clockwork/midi/in/*, /clockwork/midi/out/*, clock pulses) is
// deliberately not logged.
// Payload: <nIn:i> [name:s enabled:i]* <nOut:i> [name:s enabled:i]*
static void logMidiPortsChange(const uint8_t* data, uint32_t len) {
    if (std::strcmp(reinterpret_cast<const char*>(data), CLOCKWORK_SYS("midi/ports")) != 0) return;
    std::string ins, outs;
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data),
            static_cast<osc::osc_bundle_element_size_t>(len)));
        auto it = msg.ArgumentsBegin();
        auto readList = [&](std::string& names) {
            if (it == msg.ArgumentsEnd() || !it->IsInt32()) return;
            const int n = it->AsInt32Unchecked(); ++it;
            for (int i = 0; i < n && it != msg.ArgumentsEnd(); ++i) {
                if (!it->IsString()) return;
                if (!names.empty()) names += ", ";
                names += it->AsStringUnchecked(); ++it;   // name
                if (it != msg.ArgumentsEnd()) ++it;       // enabled flag
            }
        };
        readList(ins);
        readList(outs);
    } catch (...) { return; }
    clockwork_log("[midi] ports: in=[%s] out=[%s]", ins.c_str(), outs.c_str());
}

// A reply goes to its caller. An EVENT — what a port sent, a ports change —
// goes into the IN ring, on the subsystem's own thread, to be answered on the
// audio thread by clockwork_event_route: out to the MIDI audience, and to the
// guest if it asked. The same door a host's front on the web pushes events
// through, so an event reaches a client or a guest the same way whichever
// side produced it. A full ring drops it and says so.
void MidiControl::emitCb(void* ctx, int32_t kind, const uint8_t* osc, uint32_t len) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (!self->mEgress) return;
    if (kind == CLOCKWORK_MIDI_EMIT_REPLY) {
        self->mEgress->reply(self->mReplyToken, osc, len);
    } else {
        logMidiPortsChange(osc, len);
        if (!clockwork_ingress_write(osc, len, 0)) {
            static std::atomic<uint32_t> dropped{0};
            if (dropped.fetch_add(1, std::memory_order_relaxed) < 8)
                clockwork_log("WARNING: MIDI event %s dropped — IN ring full",
                              reinterpret_cast<const char*>(osc));
        }
    }
}

// One 0xF8 pulse feeds the port's own midi timeline (claimed by normalised
// handle, labelled with the raw OS name) — NOT the Link timeline. claim is
// idempotent, so per-pulse calls just resolve the existing slot; the engine
// anchors the beat on the pulse count.
void MidiControl::clockCb(void* ctx, const uint8_t* norm, uint32_t normLen,
                          const uint8_t* raw, uint32_t rawLen, uint64_t tsUs) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (!self->mClock) return;
    const std::string n(reinterpret_cast<const char*>(norm), normLen);
    const std::string r(reinterpret_cast<const char*>(raw),  rawLen);
    const int id = self->mClock->claimMidiTimeline(n.c_str(), r.c_str());
    if (id > 0) self->mClock->midiTimelinePulse(id, tsUs);
}

void MidiControl::transportCb(void* ctx, const uint8_t* norm, uint32_t normLen,
                              const uint8_t* raw, uint32_t rawLen, int32_t kind, double beat) {
    auto* self = static_cast<MidiControl*>(ctx);
    if (!self->mClock) return;
    const std::string n(reinterpret_cast<const char*>(norm), normLen);
    const std::string r(reinterpret_cast<const char*>(raw),  rawLen);
    const int id = self->mClock->claimMidiTimeline(n.c_str(), r.c_str());
    if (id > 0) self->mClock->setMidiTimelineTransport(id, kind, beat);
}

void MidiControl::broadcastTimelines() {
    if (!mEgress || !mClock) return;
    const auto tls = mClock->listTimelines();
    char buf[2048];
    osc::OutboundPacketStream s(buf, sizeof(buf));
    s << osc::BeginMessage(CLOCKWORK_SYS("clock/timelines"));
    appendTimelineRows(s, tls);
    s << osc::EndMessage;
    mEgress->broadcastLinkNotify(reinterpret_cast<const uint8_t*>(s.Data()),
                                 static_cast<uint32_t>(s.Size()));
}

