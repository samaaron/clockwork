// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    clockwork
    Copyright (c) 2025 Sam Aaron


    Standalone scheduler host: a wall-clock OSC/MIDI event scheduler with no
    synthesis engine. Receives the control vocabulary (/clockwork/schedule, /clockwork/sched/flush)
    over UDP, schedules each event on the generic Scheduler, and delivers due
    events via the Rust OSC and MIDI subsystems. main() itself drives the
    scheduler tick off the wall clock.

    Usage: clockwork-scheduler [control_port=4560] [loopback=1]

    Built as `clockwork-scheduler-host` when CLOCKWORK_STANDALONE and
    CLOCKWORK_SCHEDULER are both on. It was not, for a while, and its own
    header said so: five files describing a program this tree did not produce,
    so nothing could tell whether they still compiled against a boundary that had
    moved underneath them. A reference host that is not built is not a
    reference, it is a document shaped like code.

    Still not covered by a test — it is a main(), a socket and a wall clock,
    and what it demonstrates (the lanes ABI driving the timed store) is pinned
    in test_lanes.cpp and test_scheduler.cpp against the same calls.
*/

#include "clockwork_sys.h"
#include <cstdarg>
#include <cstdio>
#include "host/clock.h"
#include "host/host_scheduler.h"
#include "host/host_outbound.h"
#include "OscIngress.h"
#include "clockwork_osc.h"
#ifdef CLOCKWORK_WITH_MIDI
#include "clockwork_midi.h"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false); }

void ingress_cb(void* ctx, int32_t /*kind*/, const uint8_t* osc, uint32_t len) {
    static_cast<clockwork_host::HostScheduler*>(ctx)->ingest(osc, len);
}
void osc_emit_noop(void*, int32_t, const uint8_t*, uint32_t) {}

#ifdef CLOCKWORK_WITH_MIDI
void midi_emit_noop(void*, int32_t, const uint8_t*, uint32_t) {}
void midi_clock_noop(void*, const uint8_t*, uint32_t, const uint8_t*, uint32_t, uint64_t) {}
void midi_transport_noop(void*, const uint8_t*, uint32_t, const uint8_t*, uint32_t, int32_t, double) {}
#endif
}  // namespace

// ── Engine-side externs the umbrella staticlib references ───────────────────
// The lean host builds the umbrella with only the osc/midi subsystems, but
// the archive's shared support code (buffers, scope) still names the engine's
// logging and shm boundaries. The host has neither an engine log ring nor a scope
// shm segment, so these are honest stand-ins: log to stderr, publish no shm.
extern "C" int clockwork_log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    return n;
}
extern "C" void* clockwork_shm_base(void) { return nullptr; }
extern "C" int clockwork_scope_geometry(size_t*, size_t*, size_t*, size_t*, unsigned*, unsigned*) { return 0; }

int main(int argc, char** argv) {
    int control_port = argc > 1 ? std::atoi(argv[1]) : 4560;
    int loopback     = argc > 2 ? std::atoi(argv[2]) : 1;

    ClockworkOsc* osc = clockwork_osc_create(nullptr, osc_emit_noop);
    if (!osc) {
        std::fprintf(stderr, "clockwork-scheduler: failed to create OSC subsystem\n");
        return 1;
    }

#ifdef CLOCKWORK_WITH_MIDI
    ClockworkMidi* midi = clockwork_midi_create(nullptr, midi_emit_noop, midi_clock_noop, midi_transport_noop, nullptr);
#endif

    clockwork_host::SendOsc sendOsc =
        [osc](const char* host, int port, const uint8_t* inner, uint32_t len) {
            clockwork_osc_send(osc, reinterpret_cast<const uint8_t*>(host),
                        static_cast<uint32_t>(std::strlen(host)), port, inner, len);
        };
#ifdef CLOCKWORK_WITH_MIDI
    clockwork_host::SendMidi sendMidi = [midi](const uint8_t* inner, uint32_t len) {
        clockwork_midi_handle_osc(midi, inner, len);
    };
#else
    clockwork_host::SendMidi sendMidi = [](const uint8_t*, uint32_t) {};
#endif

    // Fired events route by address through clockwork's own route table —
    // the same dispatcher the engine uses — with the host's two leaves; no
    // DSP route is registered, so the fallback reports any unrouted address (e.g.
    // a DSP verb) rather than swallow it. add() takes only what follows the
    // reserved prefix, so the host cannot introduce a namespace of its own
    // either. tick() ingests synchronously on the tick thread (not RT, no NRT
    // handoff).
    clockwork_host::HostSenders senders{ sendOsc, sendMidi };
    ClockworkSysRoutes outboundRoutes;
    outboundRoutes.add("osc/send", &clockwork_host::hostOscSendRoute, &senders);
    outboundRoutes.add("midi/",    &clockwork_host::hostMidiRoute,    &senders);
    outboundRoutes.setFallback(&clockwork_host::hostUnroutedRoute, nullptr);
    clockwork_host::HostScheduler sched(outboundRoutes);

    ClockworkOscIngress* ingress = clockwork_osc_ingress_start(&sched, ingress_cb, control_port, loopback);
    if (!ingress) {
        std::fprintf(stderr, "clockwork-scheduler: failed to bind control port %d\n", control_port);
#ifdef CLOCKWORK_WITH_MIDI
        clockwork_midi_destroy(midi);
#endif
        clockwork_osc_destroy(osc);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef CLOCKWORK_WITH_MIDI
    const char* caps = "scheduler + osc + midi";
#else
    const char* caps = "scheduler + osc";
#endif
    std::fprintf(stderr, "clockwork-scheduler: %s on control port %d (%s)\n",
                 caps, control_port, loopback ? "loopback" : "all interfaces");

    // Drive the scheduler at ~1 ms granularity off the wall clock; tick()
    // dispatches whatever just came due through the route table inline.
    while (g_running.load()) {
        sched.tick(clockwork_host::osc_now());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    clockwork_osc_ingress_stop(ingress);
#ifdef CLOCKWORK_WITH_MIDI
    clockwork_midi_destroy(midi);
#endif
    clockwork_osc_destroy(osc);
    return 0;
}
