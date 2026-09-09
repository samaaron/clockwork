// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * EngineClock.cpp — see EngineClock.h. The clock-core /clock verbs, once.
 */
#include "clockwork_prefix.h"
#include "clock/EngineClock.h"

#include "clock/ClockworkClock.h"
#include "clock/clock_math.h"
#include "clock/timeline_osc.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"
#include <cmath>
#include <cstring>

// The wire carries NTP micros; a Timeline holds NTP seconds. 0 stays 0 both
// ways, so the "no transition yet" sentinel survives the trip.
static int64_t ntpMicros(double ntpSeconds) {
    return static_cast<int64_t>(std::llround(ntpSeconds * 1e6));
}
static double ntpSeconds(int64_t ntpMicros) { return static_cast<double>(ntpMicros) * 1e-6; }

// Compile-time backend capabilities, answered by /clockwork/clock/capabilities/get so a
// client can feature-detect which parts of the /clock surface this build
// answers (the native-only Link-session verbs live outside the shared core).
#if CLOCKWORK_LINK
static constexpr int32_t kCapLink = 1;
#else
static constexpr int32_t kCapLink = 0;
#endif
#if CLOCKWORK_LINK && CLOCKWORK_SYNTH
static constexpr int32_t kCapLinkAudio = 1;
#else
static constexpr int32_t kCapLinkAudio = 0;
#endif
#if CLOCKWORK_MIDI
static constexpr int32_t kCapMidi = 1;
#else
static constexpr int32_t kCapMidi = 0;
#endif

// THREADING CONTRACT — this handler calls clock mutators (setBpm/setIsPlaying)
// and Link-session reads (numPeers, …) that are NOT realtime-safe, and the
// follower registry behind timeline(id) takes a lock. Its RT-safety therefore
// lives in *where it is invoked*, not here:
//   • Native (Link compiled in): runs on the NRT gateway thread — `/clock` is
//     forwarded off the audio thread (nrtForwardSink → NRT command ring →
//     EngineControl). Taking a lock here is fine.
//   • WASM (no Link): runs inline on the audio thread, but CLOCKWORK_LINK is
//     undefined and the registry is uncontended, so nothing blocks.
// Never call this on the native audio thread: it would be an RT violation.
bool handleClockCoreOsc(ClockworkClock& clock, const uint8_t* data, uint32_t size,
                        const ClockReply& reply) {
    if (size < CLOCKWORK_SYS_LEN("clock/") + 1 ||
        std::memcmp(data, CLOCKWORK_SYS("clock/"), CLOCKWORK_SYS_LEN("clock/")) != 0) return false;
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        const char* addr = msg.AddressPattern();

        // Emit a one-message reply built into a stack buffer.
        auto send = [&](const osc::OutboundPacketStream& s) {
            reply(reinterpret_cast<const uint8_t*>(s.Data()),
                  static_cast<uint32_t>(s.Size()));
        };

        // ── Optional correlation token ───────────────────────────────────
        // A request may carry an int32 as its last argument (after the verb's
        // own args); the reply then echoes it as its last argument, letting
        // clients match replies to requests exactly. With address-only
        // matching a reply delayed past its caller's timeout is delivered to
        // the next caller instead, so a stale clock reading corrupts that
        // client's timeline. Handlers read their args positionally, so the
        // trailing int is invisible to them, and clients that send no token
        // get the unchanged wire format.
        bool    hasToken  = false;
        int32_t echoToken = 0;
        for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
            auto next = it; ++next;
            if (next == msg.ArgumentsEnd() && it->IsInt32()) {
                hasToken  = true;
                echoToken = it->AsInt32Unchecked();
            }
        }
        // Append the echoed token (if any), close and send the reply.
        auto finish = [&](osc::OutboundPacketStream& s) {
            if (hasToken) s << static_cast<osc::int32>(echoToken);
            s << osc::EndMessage;
            send(s);
        };

        // ── Optional <timeline> segment ──────────────────────────────────
        // /clockwork/clock/<tl>/<verb> where <tl> is a timeline name — the
        // grammar is clockwork-clock's (clockwork_timeline_is_name: link, midi,
        // midi:<handle>), the same one resolveTimeline answers for. Omitted
        // ⇒ link (back-compat: /clockwork/clock/tempo/get ≡ /clockwork/clock/link/tempo/get).
        // Parsed allocation-free (WASM runs this on the audio thread).
        int  id = 0;
        char tlname[64] = {0};   // bare segment ("link"/"midi"/"midi:<handle>"), "" = none
        const char* verb = addr + CLOCKWORK_SYS_LEN("clock/");  // past the namespace
        if (const char* slash = std::strchr(verb, '/')) {
            const size_t n = static_cast<size_t>(slash - verb);
            const bool isTl = clockwork_timeline_is_name(verb, static_cast<uint32_t>(n)) != 0;
            if (isTl && n + 1 <= sizeof(tlname)) {
                std::memcpy(tlname, verb, n);
                tlname[n] = '\0';
                id = clock.resolveTimeline(tlname);
                verb = slash + 1;
            }
        }

        // Build a "/clockwork/clock/<tlname>/<suffix>" reply address (omit the segment for
        // the bare/link case so /clockwork/clock/tempo.reply stays wire-compatible).
        auto replyAddr = [&](const char* suffix, char* out, size_t cap) -> const char* {
            if (tlname[0]) std::snprintf(out, cap, CLOCKWORK_SYS("clock/%s/%s"), tlname, suffix);
            else           std::snprintf(out, cap, CLOCKWORK_SYS("clock/%s"), suffix);
            return out;
        };

        // ── Tempo ────────────────────────────────────────────────────────
        if (std::strcmp(verb, "tempo/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const double bpm = it->AsFloatUnchecked();
            if (id == 0) {
                clock.setBpm(bpm);                      // Link timeline
            } else {
                // Manual set on a midi timeline (claim the slot if this port
                // hasn't clocked yet). A live external clock pulse overrides it
                // on its next tick — this is the manual-override / test hook.
                const int wid = clock.resolveOrClaimTimeline(tlname);
                if (wid > 0) clock.setMidiTimelineTempo(wid, bpm);
            }
            return true;
        }
        if (std::strcmp(verb, "tempo/get") == 0) {
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("tempo.reply", ra, sizeof(ra)))
              << clock.timelineBpm(id);
            finish(s);
            return true;
        }

        // ── Transport ────────────────────────────────────────────────────
        if (std::strcmp(verb, "transport/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt32()) return true;
            const bool playing = it->AsInt32Unchecked() != 0;
            if (id == 0) {
                // Stamp with wallNow — verb carries no timestamp; 0.0 would mean
                // "transitioned at NTP epoch 1900".
                clock.setIsPlaying(playing, clock.wallNow());
            } else {
                // Manual transport on a midi timeline (claim if needed): play →
                // START at beat 0, stop → STOP. A live clock overrides.
                const int wid = clock.resolveOrClaimTimeline(tlname);
                if (wid > 0)
                    clock.setMidiTimelineTransport(wid, playing ? 0 : 2, 0.0);
            }
            return true;
        }
        if (std::strcmp(verb, "transport/get") == 0) {
            // Reply: playing(i), anchored(i). Anchored = START/SPP has defined
            // the beat origin (always 1 for Link); clients gate MIDI sync on it.
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("transport.reply", ra, sizeof(ra)))
              << static_cast<int32_t>(clock.timelineIsPlaying(id) ? 1 : 0)
              << static_cast<int32_t>(clock.timelineIsAnchored(id) ? 1 : 0);
            finish(s);
            return true;
        }
        if (std::strcmp(verb, "transport/time/get") == 0) {
            // 0 = no transition yet; the sentinel goes out as itself.
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("transport/time.reply", ra, sizeof(ra)))
              << static_cast<osc::int64>(ntpMicros(clock.timeline(id).transition_ntp));
            finish(s);
            return true;
        }

        // ── Meter and bars ───────────────────────────────────────────────
        // /clockwork/clock/[<tl>/]meter <num:i> <den:i>  sets; with fewer than
        // two args it queries → meter.reply <num:i> <den:i>. A set answers
        // nothing, like tempo/set; a meter clockwork_timeline_meter_valid refuses
        // is dropped, and the meter stands. Link carries no meter, so the
        // Link timeline's is the clock state's own (ClockworkClock::setMeter) on
        // every build; a midi timeline's is its slot's.
        if (std::strcmp(verb, "meter") == 0) {
            auto it = msg.ArgumentsBegin();
            auto second = it;
            if (it != msg.ArgumentsEnd()) ++second;
            if (second != msg.ArgumentsEnd() && it->IsInt32() && second->IsInt32()) {
                const int num = it->AsInt32Unchecked();
                const int den = second->AsInt32Unchecked();
                if (id == 0) {
                    clock.setMeter(num, den);
                } else {
                    const int wid = clock.resolveOrClaimTimeline(tlname);
                    if (wid > 0) clock.setMidiTimelineMeter(wid, num, den);
                }
                return true;
            }
            const clockwork::Timeline t = clock.timeline(id);
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("meter.reply", ra, sizeof(ra)))
              << static_cast<osc::int32>(t.meter_num)
              << static_cast<osc::int32>(t.meter_den);
            finish(s);
            return true;
        }
        // /clockwork/clock/[<tl>/]bar → bar.reply <bar:d> <beat_in_bar:d> <num:i> <den:i>
        // at wall-clock now, from one snapshot: bar 0 starts at beat 0 and
        // holds meter_num * 4 / meter_den quarter-note beats. The bar is a
        // whole number carried as a double, the type every beat reply uses.
        if (std::strcmp(verb, "bar") == 0) {
            const clockwork::Timeline t = clock.timeline(id);
            const double now = wallClockNTP();
            char ra[96], buf[160];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("bar.reply", ra, sizeof(ra)))
              << t.barAt(now) << t.beatInBarAt(now)
              << static_cast<osc::int32>(t.meter_num)
              << static_cast<osc::int32>(t.meter_den);
            finish(s);
            return true;
        }

        // ── RPC: beat ↔ time, answered from one snapshot of the timeline ──
        // Times on the wire are NTP micros; the snapshot is NTP seconds.
        if (std::strcmp(verb, "rpc/beat_at_time") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt64()) return true;
            const int64_t t = it->AsInt64Unchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            (void)q;   // a beat is a beat; the quantum only shapes the phase
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("rpc/beat_at_time.reply", ra, sizeof(ra)))
              << clock.timeline(id).beatAt(ntpSeconds(t));
            finish(s);
            return true;
        }
        if (std::strcmp(verb, "rpc/phase_at_time") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt64()) return true;
            const int64_t t = it->AsInt64Unchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("rpc/phase_at_time.reply", ra, sizeof(ra)))
              << clock.timeline(id).phaseAt(ntpSeconds(t), q);
            finish(s);
            return true;
        }
        if (std::strcmp(verb, "rpc/time_at_beat") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd()) return true;
            // int64 microbeats carries a beat exactly; float32 loses ~2ms once
            // the beat count passes 32768. float32 kept for older clients.
            double b;
            if (it->IsInt64())
                b = static_cast<double>(it->AsInt64Unchecked())
                    / clockwork::kMicrobeatsPerBeat;
            else if (it->IsFloat())
                b = it->AsFloatUnchecked();
            else
                return true;
            ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            (void)it->AsFloatUnchecked();   // quantum: accepted, not needed
            char ra[96], buf[128];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("rpc/time_at_beat.reply", ra, sizeof(ra)))
              << static_cast<osc::int64>(ntpMicros(clock.timeline(id).timeAtBeat(b)));
            finish(s);
            return true;
        }
        // Combined variants — one round-trip where clients previously chained
        // two or three serial RPCs (every client sleep/sync boundary).
        if (std::strcmp(verb, "rpc/beat_phase_at_time") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsInt64()) return true;
            const int64_t t = it->AsInt64Unchecked(); ++it;
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            const double beat = clock.timeline(id).beatAt(ntpSeconds(t));
            char ra[96], buf[160];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("rpc/beat_phase_at_time.reply", ra, sizeof(ra)))
              << beat << clockwork::wrapPhase(beat, q);
            finish(s);
            return true;
        }
        if (std::strcmp(verb, "rpc/beat_phase_now") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it == msg.ArgumentsEnd() || !it->IsFloat()) return true;
            const float q = it->AsFloatUnchecked();
            const int64_t t = ntpMicros(wallClockNTP());
            const double beat = clock.timeline(id).beatAt(ntpSeconds(t));
            char ra[96], buf[160];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(replyAddr("rpc/beat_phase_now.reply", ra, sizeof(ra)))
              << static_cast<osc::int64>(t) << beat << clockwork::wrapPhase(beat, q);
            finish(s);
            return true;
        }

        // ── Enumerate timelines ──────────────────────────────────────────
        // /clockwork/clock/timelines/get → /clockwork/clock/timelines.reply with a flattened
        // [name(s) raw(s) bpm(f) clocking(i) stale(i) primary(i)] per active
        // timeline. `name` is the wire/address identity ("link" /
        // "midi:<handle>"); `raw` is the original OS device name for display.
        if (std::strcmp(verb, "timelines/get") == 0) {
            const auto tls = clock.listTimelines();
            char buf[2048];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/timelines.reply"));
            appendTimelineRows(s, tls);
            finish(s);
            return true;
        }

        // ── Start/stop sync (Link-session property; not timeline-scoped) ──
        if (std::strcmp(verb, "start_stop_sync/set") == 0) {
            auto it = msg.ArgumentsBegin();
            if (it != msg.ArgumentsEnd() && it->IsInt32())
                clock.setStartStopSyncEnabled(it->AsInt32Unchecked() != 0);
            return true;
        }
        if (std::strcmp(verb, "start_stop_sync/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/start_stop_sync.reply"))
              << static_cast<int32_t>(clock.isStartStopSyncEnabled() ? 1 : 0);
            finish(s);
            return true;
        }

        // ── Queries: enabled / time-now / peer-count (Link-global) ───────
        if (std::strcmp(verb, "enabled/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/enabled.reply"))
              << static_cast<int32_t>(clock.isLinkEnabled() ? 1 : 0);
            finish(s);
            return true;
        }
        if (std::strcmp(verb, "time/now/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/time/now.reply"))
              << static_cast<osc::int64>(ntpMicros(wallClockNTP()));
            finish(s);
            return true;
        }
        if (std::strcmp(verb, "peers/count/get") == 0) {
            char buf[64];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/peers/count.reply"))
              << static_cast<int32_t>(clock.numPeers());
            finish(s);
            return true;
        }
        // ── Capability discovery ─────────────────────────────────────────
        // Compile-time facts as name/value pairs (extensible without breaking
        // positional readers). link = the Ableton session (visibility / peers /
        // notify verbs); link_audio = the /clockwork/clock/audio/* surface; midi = the
        // MIDI subsystem feeding the follower timelines.
        if (std::strcmp(verb, "capabilities/get") == 0) {
            char buf[192];
            osc::OutboundPacketStream s(buf, sizeof(buf));
            s << osc::BeginMessage(CLOCKWORK_SYS("clock/capabilities.reply"))
              << "link"       << kCapLink
              << "link_audio" << kCapLinkAudio
              << "midi"       << kCapMidi;
            finish(s);
            return true;
        }
    } catch (...) {
        return true;  // malformed /clock — consumed, never reaches the DSP
    }
    return false;  // a /clock verb this handler doesn't own (native Link-session)
}

void replyClockUnsupported(const uint8_t* data, uint32_t size,
                           const ClockReply& reply) {
    try {
        osc::ReceivedPacket pkt(reinterpret_cast<const char*>(data),
                                static_cast<osc::osc_bundle_element_size_t>(size));
        osc::ReceivedMessage msg(pkt);
        bool    hasToken  = false;
        int32_t echoToken = 0;
        for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd(); ++it) {
            auto next = it; ++next;
            if (next == msg.ArgumentsEnd() && it->IsInt32()) {
                hasToken  = true;
                echoToken = it->AsInt32Unchecked();
            }
        }
        char buf[192];
        osc::OutboundPacketStream s(buf, sizeof(buf));
        s << osc::BeginMessage(CLOCKWORK_SYS("clock/unsupported")) << msg.AddressPattern();
        if (hasToken) s << static_cast<osc::int32>(echoToken);
        s << osc::EndMessage;
        reply(reinterpret_cast<const uint8_t*>(s.Data()),
              static_cast<uint32_t>(s.Size()));
    } catch (...) {
        // Unparseable or oversized address — drop rather than reply.
    }
}
