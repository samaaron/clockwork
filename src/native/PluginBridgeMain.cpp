// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * PluginBridgeMain.cpp — the plugin bridge: the process the plugins live in.
 *
 * The engine spawns this with its own pid as the one argument. The bridge
 * maps the segment the engine laid out (src/plugin_bridge.h), opens a
 * SOURCE port over the send ring and a SINK over the return ring, opens the
 * doorbell, and from then on is two threads:
 *
 *   THE AUDIO THREAD sleeps on the doorbell. Each post means the engine has
 *   put one block of send audio in the ring: take it, apply the realtime
 *   events the engine's audio thread queued for that block (notes, controls,
 *   parameter moves, each with its frame offset), run every track's chain,
 *   put the block on the return, count a heartbeat. Nothing here blocks on
 *   anything but the doorbell.
 *
 *   THE MAIN THREAD drains the control ring — create, remove, chain edits,
 *   editors, rigs, folders, each answered through the OUT ring — pumps the
 *   platform's event loop for the editor windows, keeps the rig mirror in
 *   the segment current, and watches for the reasons to leave: the engine
 *   asked (`quit`), the engine is gone, or the segment now belongs to a
 *   later generation than the one this process joined.
 *
 * WHAT HAPPENS WHEN THE BRIDGE IS LATE. The engine keeps `slack_blocks` of
 * return audio ahead of itself, so a block that arrives late by less than
 * that is not heard. A bridge that wakes to find several blocks waiting
 * renders them all, up to a few, so a plugin's state (a reverb tail, a held
 * note) stays continuous; beyond that the oldest are dropped unrendered,
 * because a bridge that fell that far behind will not catch up by rendering
 * faster, and the engine has already played silence in their place.
 *
 * WHAT HAPPENS WHEN A PLUGIN CRASHES. This process dies with it. The
 * engine notices — no heartbeat, or the pid gone — and spawns a new one with
 * `restore` set, which reads the rig back from the mirror this one kept.
 * Before every plugin open the path goes into `loading_plugin`, so a crash
 * during a load can be blamed on the plugin that was loading.
 */
#include "plugin_bridge.h"
#include "plugin_bridge_verbs.h"
#include "RealtimeThread.h"
#include "lanes/ring_drain.h"
#include "osc_debug.h"
#include "shared_memory.h"
#include "shm_segment.hpp"
#include "clockwork_ports.h"
#include "clockwork_path.h"
#include "workers/RingBufferWriter.h"

extern "C" {
#include "plugin_discovery.h"
#include "plugin_track.h"
}
#include "plugin_editor_window.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include "cocoa_event_pump.h"
#endif

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <signal.h>
#  include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>   // va_list/va_start in log(): libc++ and MSVC pull this in
                     // transitively, libstdc++ does not, so Linux would not build
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace clockwork_bridge;
using detail_shm_segment::shm_handle;
using detail_shm_segment::shm_open_existing;
using detail_shm_segment::shm_close;

// How many waiting blocks the audio thread renders in one wake before it
// starts dropping the oldest — at least this many, and never fewer than the
// engine's slack: a device callback of B blocks posts B at once, and the
// engine sized its slack to that (slack_for), so B waiting is on time, not
// behind. Four was a fixed idea of a small callback; a 1024-frame buffer
// handed eight, and this dropped half of every one of them.
constexpr uint32_t CATCH_UP_BLOCKS = 4;
// How many control frames the main thread takes per turn of its loop, so a
// flood of parameter pages cannot starve the editor windows of events.
constexpr uint32_t CTL_FRAMES_PER_TURN = 64;

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[plugin-bridge] ");
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

// ── Where the answers go ─────────────────────────────────────────────────────
// Every reply, broadcast and debug line is one frame on the OUT ring: the
// egress route first, then the OSC, with the origin token as the frame's
// sourceId. RingBufferWriter takes one contiguous buffer, so the two parts
// are joined in a per-thread scratch first.
class RingEmit final : public TrackVerbs::Emit {
public:
    explicit RingEmit(Header* h, uint8_t* ring) : mHeader(h), mRing(ring) {}

    void reply(uint32_t token, const uint8_t* osc, uint32_t len) override {
        put(EGRESS_REPLY, token, osc, len);
    }
    void broadcast(const uint8_t* osc, uint32_t len) override {
        put(EGRESS_BROADCAST_NOTIFY, BRIDGE_ORIGIN_TOKEN, osc, len);
    }
    // Over the ring only: the engine prints its debug channel to the same
    // stderr this process inherited, so a copy here would appear twice.
    void debug(const std::string& line) override {
        char pkt[1024];
        const uint32_t n = clockwork::buildDebugOsc(pkt, line.data(), static_cast<uint32_t>(line.size()));
        put(EGRESS_BROADCAST_NOTIFY, BRIDGE_ORIGIN_TOKEN,
            reinterpret_cast<const uint8_t*>(pkt), n);
    }

private:
    void put(uint32_t route, uint32_t token, const uint8_t* osc, uint32_t len) {
        thread_local std::vector<uint8_t> scratch;
        scratch.resize(sizeof(uint32_t) + len);
        std::memcpy(scratch.data(), &route, sizeof route);
        std::memcpy(scratch.data() + sizeof(uint32_t), osc, len);
        MsgRing& r = mHeader->out;
        if (!RingBufferWriter::write(mRing, OUT_RING_SIZE, &r.head, &r.tail, &r.sequence,
                                     &r.write_lock, scratch.data(),
                                     static_cast<uint32_t>(scratch.size()), token))
            r.dropped.fetch_add(1, std::memory_order_relaxed);
    }

    Header*  mHeader;
    uint8_t* mRing;
};

// ── The process ──────────────────────────────────────────────────────────────
struct Bridge {
    shm_handle  seg {};
    Header*     h = nullptr;
    Doorbell    bell;
    ClockworkPort     send = 0;    // SOURCE over the send ring: what the engine wrote
    ClockworkPort     ret  = 0;    // SINK over the return ring: what the engine reads
    uint32_t    generation = 0;
    int32_t     enginePid = 0;

    TrackVerbs  verbs;
    RingEmit*   emit = nullptr;

    std::atomic<bool> stop{false};
    std::atomic<bool> changed{false};
    std::thread audio;

    // Staging for the block walk: LANES channels each, planar.
    std::vector<float> sendBuf, retBuf;
    std::vector<float*> sendPtrs, retPtrs;

#if defined(_WIN32)
    HANDLE parent = nullptr;
#endif
};

Bridge* g_bridge = nullptr;

bool engineAlive(const Bridge& b) {
#if defined(_WIN32)
    // run() refused to start without the handle, so a null one here is a
    // bridge that is shutting down, not an engine that is alive.
    return b.parent && WaitForSingleObject(b.parent, 0) == WAIT_TIMEOUT;
#else
    // posix_spawn made the engine our parent; when it dies we are reparented.
    return ::getppid() == b.enginePid;
#endif
}

// clockwork_track's load listener: the culprit-to-be, stamped before each open.
// Odd loading_seq = a load is in progress and the path is meaningful.
void onLoad(void* ctx, const char* path) {
    Header* h = static_cast<Header*>(ctx);
    if (path) {
        std::strncpy(h->loading_plugin, path, PATH_MAX_BYTES - 1);
        h->loading_plugin[PATH_MAX_BYTES - 1] = '\0';
    }
    h->loading_seq.fetch_add(1, std::memory_order_release);
}

// The rig, as JSON, in the segment. Length 0 while it is being rewritten.
void writeMirror(Bridge& b) {
    char* m = mirror(b.seg.ptr);
    b.h->mirror_len.store(0, std::memory_order_release);
    const uint32_t need = clockwork_track_rig_to_json(m, MIRROR_SIZE);
    if (need >= MIRROR_SIZE) {
        m[0] = '\0';
        b.emit->debug("the rig is too large to mirror; a crash would lose it");
        return;
    }
    b.h->mirror_len.store(need, std::memory_order_release);
}

void restoreFromMirror(Bridge& b) {
    const uint32_t len = b.h->mirror_len.load(std::memory_order_acquire);
    if (len == 0 || len >= MIRROR_SIZE) return;
    std::string json(mirror(b.seg.ptr), len);
    const double sr = clockwork::bitsToDouble(b.h->sample_rate_bits.load(std::memory_order_relaxed));
    const uint32_t block = b.h->block_size.load(std::memory_order_relaxed);
    uint32_t missing = 0;
    const char* err = nullptr;
    if (!clockwork_track_rig_from_json(json.c_str(), sr, block, &missing, &err)) {
        b.emit->debug(std::string("restoring the tracks failed: ") + (err ? err : "?"));
        return;
    }
    if (missing)
        b.emit->debug("restored the tracks; " + std::to_string(missing) + " plugin(s) could not be reopened");
    else
        b.emit->debug("restored the tracks");
}

// ── The audio thread ─────────────────────────────────────────────────────────
void renderOne(Bridge& b, uint32_t frames, bool applyEvents) {
    clockwork_port_read(b.send, b.sendPtrs.data(), LANES, frames);
    if (applyEvents) {
        ClockworkDrainState st;
        MsgRing& r = b.h->rt;
        clockwork_drain_ring(rt_ring(b.seg.ptr), RT_RING_SIZE, &r.head, &r.tail, st, ClockworkDrainMetrics{}, 0,
            [&](uint32_t, const uint8_t* payload, uint32_t len, uint32_t) {
                if (len > sizeof(Prefixed)) {
                    uint32_t off = 0;
                    std::memcpy(&off, payload, sizeof off);
                    b.verbs.applyRealtime(payload + sizeof(Prefixed), len - sizeof(Prefixed),
                                          std::min(off, frames - 1));
                }
                return ClockworkDrainVerdict::Consume;
            });
    }
    const int64_t when = b.h->block_time.load(std::memory_order_relaxed);
    clockwork_track_process(b.retPtrs.data(), LANES, b.sendPtrs.data(), LANES, frames, when, 0);
    clockwork_port_write(b.ret, b.retPtrs.data(), LANES, frames);
    // Which of those lanes carried a track: what the engine taps for scopes.
    // Stored before the heartbeat so the block it describes is the one the
    // engine is about to read.
    b.h->slots_live.store(clockwork_track_slot_mask(), std::memory_order_relaxed);
    b.h->bridge_heartbeat.fetch_add(1, std::memory_order_release);
}

void audioMain(Bridge& b) {
    {
        const double sr = clockwork::bitsToDouble(b.h->sample_rate_bits.load(std::memory_order_relaxed));
        const uint32_t block = b.h->block_size.load(std::memory_order_relaxed);
        const clockwork::RealtimeResult rt = clockwork::elevateCurrentThreadToRealtime(
            sr > 0 && block > 0 ? double(block) / sr : 0.0);
        if (rt.status == clockwork::RealtimeStatus::Failed)
            log("the render thread could not be made realtime (error %d)", rt.error);
    }
    while (!b.stop.load(std::memory_order_relaxed)) {
        if (!b.bell.wait()) break;
        if (b.stop.load(std::memory_order_relaxed)) break;
        // A burst of posts is one wake: whatever is in the ring is rendered
        // below, however many bells announced it.
        while (b.bell.try_wait()) {}

        const uint32_t block = std::min(b.h->block_size.load(std::memory_order_relaxed), MAX_BLOCK);
        if (block == 0) continue;
        uint32_t avail = clockwork_port_readable(b.send);
        // Too far behind: drop the oldest, unrendered.
        const uint32_t catchUp = std::max<uint32_t>(CATCH_UP_BLOCKS, b.h->slack_blocks.load(std::memory_order_relaxed));
        while (avail > catchUp * block) {
            clockwork_port_read(b.send, b.sendPtrs.data(), LANES, block);
            avail -= block;
        }
        bool first = true;
        while (avail > 0) {
            const uint32_t frames = std::min(avail, block);
            renderOne(b, frames, first);
            first = false;
            avail -= frames;
        }
    }
}

// ── The main thread ──────────────────────────────────────────────────────────
void drainControl(Bridge& b) {
    ClockworkDrainState st;
    MsgRing& r = b.h->ctl;
    clockwork_drain_ring(ctl_ring(b.seg.ptr), CTL_RING_SIZE, &r.head, &r.tail, st, ClockworkDrainMetrics{},
        CTL_FRAMES_PER_TURN,
        [&](uint32_t token, const uint8_t* payload, uint32_t len, uint32_t) {
            if (!b.verbs.handleControl(token, payload, len))
                b.emit->debug("the bridge was handed a message that is not a track verb");
            return ClockworkDrainVerdict::Consume;
        });
}

void pumpEvents() {
#if defined(__APPLE__)
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
    clockwork_pump_cocoa_events();
#elif defined(_WIN32)
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Sleep(2);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
}

#if !defined(_WIN32)
void onSignal(int) {
    if (g_bridge) g_bridge->stop.store(true, std::memory_order_relaxed);
}
#endif

int run(int32_t enginePid) {
    Bridge b;
    g_bridge = &b;
    b.enginePid = enginePid;

    try {
        b.seg = shm_open_existing(segment_name(enginePid));
    } catch (const std::exception& e) {
        log("cannot map the engine's segment: %s", e.what());
        return 2;
    }
    if (b.seg.size < SEGMENT_SIZE) {
        log("the segment is %zu bytes; this bridge expects %zu", b.seg.size, SEGMENT_SIZE);
        return 2;
    }
    b.h = header(b.seg.ptr);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (b.h->magic != MAGIC || b.h->version != VERSION) {
        log("the segment is not a plugin bridge segment (magic %08x version %u)", b.h->magic, b.h->version);
        return 2;
    }
    if (b.h->lanes != LANES || b.h->audio_ring_frames != AUDIO_RING_FRAMES) {
        log("the segment's geometry (%u lanes, %u frames) is not this bridge's", b.h->lanes, b.h->audio_ring_frames);
        return 2;
    }
    b.generation = b.h->generation.load(std::memory_order_acquire);
#if defined(_WIN32)
    // The engine's process handle is how its death is noticed here (see
    // engineAlive). Without one a bridge would outlive every engine that ever
    // spawned it, holding the segment and rendering into it forever — so no
    // handle means no bridge, and the engine, which watches for the join,
    // reports a bridge that never came up.
    b.parent = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(enginePid));
    if (!b.parent) {
        log("cannot open engine process %d to watch it: %s", enginePid,
            clockwork_path::last_error_text(GetLastError()).c_str());
        return 2;
    }
    // Per-monitor DPI awareness, declared before any window exists (it
    // cannot be changed after). Without it Windows bitmap-scales every plugin
    // editor on a high-DPI display, and a blurred Surge is the first thing a
    // person with a 4K laptop would see of this feature. The call is looked up
    // by name because it arrived in Windows 10 1703 and older SDK headers do
    // not declare it; where it is missing the process simply runs unaware, as
    // it did before.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtx = BOOL (WINAPI*)(HANDLE);
        if (auto set = reinterpret_cast<SetCtx>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
            set(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    b.h->bridge_pid.store(static_cast<int32_t>(GetCurrentProcessId()), std::memory_order_release);
#else
    b.h->bridge_pid.store(static_cast<int32_t>(::getpid()), std::memory_order_release);
    ::signal(SIGTERM, onSignal);
    ::signal(SIGINT, onSignal);
    ::signal(SIGPIPE, SIG_IGN);
#endif

    // The rings the engine reset when it made them; the bridge only joins.
    b.send = clockwork_port_open_shared("bridge/send", kClockworkPortSource, LANES, AUDIO_RING_FRAMES,
                                  send_ring(b.seg.ptr), AUDIO_RING_BYTES, 0);
    b.ret  = clockwork_port_open_shared("bridge/return", kClockworkPortSink, LANES, AUDIO_RING_FRAMES,
                                  return_ring(b.seg.ptr), AUDIO_RING_BYTES, 0);
    if (!b.send || !b.ret) {
        log("cannot open the audio rings");
        return 2;
    }
    if (!b.bell.open(doorbell_name(enginePid))) {
        log("cannot open the doorbell %s", doorbell_name(enginePid).c_str());
        return 2;
    }

    b.sendBuf.assign(size_t(LANES) * MAX_BLOCK, 0.0f);
    b.retBuf.assign(size_t(LANES) * MAX_BLOCK, 0.0f);
    for (uint32_t c = 0; c < LANES; ++c) {
        b.sendPtrs.push_back(b.sendBuf.data() + size_t(c) * MAX_BLOCK);
        b.retPtrs.push_back(b.retBuf.data() + size_t(c) * MAX_BLOCK);
    }

    RingEmit emit(b.h, out_ring(b.seg.ptr));
    b.emit = &emit;
    const double sr = clockwork::bitsToDouble(b.h->sample_rate_bits.load(std::memory_order_relaxed));
    uint32_t laneBase = b.h->lane_base.load(std::memory_order_relaxed);
    uint64_t srBits = b.h->sample_rate_bits.load(std::memory_order_relaxed);
    clockwork_track_set_lane_base(laneBase);
    clockwork_track_set_clock(&b.h->clock);
    clockwork_track_set_timelines(b.h->timelines, TIMELINE_SLOTS);
    clockwork_track_set_load_listener(&onLoad, b.h);
    plugin_set_scan_listener(&onLoad, b.h);      // a scan opens plugins too
    b.verbs.init(&emit, sr);
    b.verbs.setChangeListener([&b]() { b.changed.store(true, std::memory_order_relaxed); });

    if (b.h->restore.load(std::memory_order_acquire)) {
        restoreFromMirror(b);
        b.h->restore.store(0, std::memory_order_release);
    }
    writeMirror(b);

    // Notes and parameter moves that reached the ring while there was no
    // bridge to play them are stale by the time this one joins: a note-on
    // from before a crash must not sound now. Drop them, before the first
    // render, and only the events from here on land in a block.
    {
        ClockworkDrainState st;
        MsgRing& r = b.h->rt;
        clockwork_drain_ring(rt_ring(b.seg.ptr), RT_RING_SIZE, &r.head, &r.tail, st, ClockworkDrainMetrics{}, 0,
            [](uint32_t, const uint8_t*, uint32_t, uint32_t) { return ClockworkDrainVerdict::Consume; });
    }
    b.audio = std::thread([&b]() { audioMain(b); });
    b.h->bridge_ready.store(1, std::memory_order_release);
    // What is loaded, unasked: a client that was waiting on the last bridge
    // learns what this one has.
    b.verbs.broadcastTracks();
    b.changed.store(false, std::memory_order_relaxed);
    log("joined engine %d (generation %u, %.0f Hz, %u-frame blocks, lanes from %u)",
        enginePid, b.generation, sr, b.h->block_size.load(std::memory_order_relaxed), laneBase);

    // An application from the start, not from the first editor: a screen
    // capture set up before any plugin window exists can then already include
    // this process (plugin_editor_window.h).
    clockwork_editor_window_register_app();

    int reason = 0;
    auto lastParentCheck = std::chrono::steady_clock::now();
    while (!b.stop.load(std::memory_order_relaxed)) {
#if defined(__APPLE__)
        // One pool per turn: plugin loads, editor windows and the event pump
        // all autorelease into it, and it drains here. See cocoa_event_pump.h.
        ClockworkAutoreleasePool pool;
#endif
        drainControl(b);

        // What the engine may have changed underneath us.
        const uint64_t nowBits = b.h->sample_rate_bits.load(std::memory_order_relaxed);
        if (nowBits != srBits) {
            srBits = nowBits;
            b.verbs.setSampleRate(clockwork::bitsToDouble(srBits));
        }
        const uint32_t nowBase = b.h->lane_base.load(std::memory_order_relaxed);
        if (nowBase != laneBase) {
            laneBase = nowBase;
            clockwork_track_set_lane_base(laneBase);
            b.verbs.broadcastTracks();
        }

        if (b.changed.exchange(false, std::memory_order_relaxed)) writeMirror(b);

        if (b.h->quit.load(std::memory_order_acquire)) { reason = 0; break; }
        if (b.h->generation.load(std::memory_order_acquire) != b.generation) {
            log("the segment belongs to a later bridge; leaving");
            reason = 3;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - lastParentCheck > std::chrono::milliseconds(250)) {
            lastParentCheck = now;
            if (!engineAlive(b)) { log("the engine is gone; leaving"); reason = 4; break; }
        }
        pumpEvents();
    }

    b.stop.store(true, std::memory_order_relaxed);
    b.h->bridge_ready.store(0, std::memory_order_release);
    // The audio thread is asleep on the doorbell; wake it so it can see stop.
    b.bell.post();
    if (b.audio.joinable()) b.audio.join();
    b.verbs.shutdown();
    clockwork_track_set_load_listener(nullptr, nullptr);
    plugin_set_scan_listener(nullptr, nullptr);
    clockwork_port_close(b.send);
    clockwork_port_close(b.ret);
    b.bell.close();
    b.h->bridge_pid.store(0, std::memory_order_release);
    b.emit = nullptr;
    shm_close(b.seg);
#if defined(_WIN32)
    if (b.parent) CloseHandle(b.parent);
#endif
    g_bridge = nullptr;
    return reason;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <engine-pid>\n"
                     "The plugin bridge is started by the engine; it is not run by hand.\n", argv[0]);
        return 1;
    }
    const long pid = std::strtol(argv[1], nullptr, 10);
    if (pid <= 0) { std::fprintf(stderr, "bad engine pid: %s\n", argv[1]); return 1; }
    return run(static_cast<int32_t>(pid));
}
