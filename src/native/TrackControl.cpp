// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * TrackControl.cpp — see TrackControl.h. The engine's side of the bridge:
 * the segment, the ports, the doorbell, the relay and the watch.
 */
#include "TrackControl.h"

#include "IngressCallCtx.h"
#include "OscEgress.h"
#include "ClockworkEngine.h"
#include "clock/ClockworkClock.h"
#include "clock/Timeline.h"
#include "audio_processor.h"   // get_audio_input_bus: where a return block lands
#include "lanes/lanes.h"
#include "lanes/lanes_internal.h"   // clockwork_egress_nrt_write: the bridge's answers re-join the lane
#include "lanes/ring_drain.h"
#include "shared_memory.h"
#include "shm_segment.hpp"
#include "clockwork_port_bus.h"
#include "clockwork_config.h"
#include "clockwork_product.h"
#include "clockwork_sys.h"
#include "workers/RingBufferWriter.h"
#include "osc/OscOutboundPacketStream.h"
#include "osc/OscReceivedElements.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#ifndef CLOCKWORK_PLUGIN_BRIDGE_NAME
#define CLOCKWORK_PLUGIN_BRIDGE_NAME "clockwork-plugin-bridge"
#endif

using namespace clockwork_bridge;
using detail_shm_segment::shm_create;
using detail_shm_segment::shm_close;
using detail_shm_segment::shm_remove;

// The scope slot claim and release (rust/clockwork-scope). Declared here rather
// than through dsp/scsynth/scsynth_scope.h, whose declaration names scsynth's
// World and ScopeBufferHnd: the Rust side takes an opaque context it never
// reads and a handle of the four-field shape ScopeTap::Claim spells out.
extern "C" {
int  clockwork_scope_get(void* ctx, int index, int channels, int max_frames, void* hnd);
void clockwork_scope_release(void* ctx, void* hnd);
}

namespace {

// Blocks posted with no heartbeat before the bridge is called hung: about a
// second at 128 frames and 48 kHz. A plugin's open runs on the bridge's main
// thread and does not stop its render thread, so this is not tripped by a
// slow load; it is tripped by a plugin that took the render thread down a
// hole, which is what it is for.
constexpr uint64_t HUNG_BLOCKS = 400;
// How long a spawned bridge has to say it is ready. A restore reopens every
// plugin, and a big synth can take seconds.
constexpr auto START_TIMEOUT = std::chrono::seconds(30);
// Crash-loop guard: this many deaths inside this window, and the next spawn
// starts empty.
constexpr size_t CRASH_LOOP_COUNT = 3;
constexpr auto   CRASH_LOOP_WINDOW = std::chrono::seconds(30);
// How long a bridge asked to quit gets before it is killed.
constexpr auto QUIT_GRACE = std::chrono::seconds(2);

int32_t ownPid() {
#if defined(_WIN32)
    return static_cast<int32_t>(GetCurrentProcessId());
#else
    return static_cast<int32_t>(::getpid());
#endif
}

// Where in this block a timetag lands. 0/1 is "now"; anything else is placed
// relative to the block start, clamped into the block — a message the timed
// store fired is due in this block by construction, and one that arrived
// late is played at the block's first frame rather than dropped.
uint32_t frameOffset(const DrainCallCtx& meta) {
    if (meta.when <= 1 || meta.blockTime == 0) return 0;
    const double sr = clockwork_sample_rate();
    if (!(sr > 0.0)) return 0;
    const double delta = static_cast<double>(meta.when - meta.blockTime) / 4294967296.0;
    if (delta <= 0.0) return 0;
    const uint32_t frames = clockwork_block_size();
    double off = delta * sr;
    if (frames > 0 && off > static_cast<double>(frames - 1)) off = static_cast<double>(frames - 1);
    return static_cast<uint32_t>(off);
}

// How many follower timelines the segment is shown: every slot the registry
// has, up to the room the header made.
constexpr uint32_t kMirroredTimelines =
    SC_MAX_TIMELINES < TIMELINE_SLOTS ? SC_MAX_TIMELINES : TIMELINE_SLOTS;

// Scratch the audio thread reads a surplus return block into, to discard it.
float  g_trim[LANES][MAX_BLOCK];
float* g_trimPtrs[LANES];
struct TrimInit { TrimInit() { for (uint32_t c = 0; c < LANES; ++c) g_trimPtrs[c] = g_trim[c]; } } g_trimInit;

}  // namespace

// ── Boot ─────────────────────────────────────────────────────────────────────

void TrackControl::reserveLanes() { clockwork_reserve_lanes(LANES); }

void TrackControl::init(ClockworkEngine* engine, OscEgress* egress, ClockworkClock* clock) {
    mEngine = engine;
    mEgress = egress;
    mClockworkClock = clock;
    if (auto* arena = static_cast<uint8_t*>(clockwork_lanes_base()))
        mClock = reinterpret_cast<const ClockworkClockState*>(arena + CLOCK_STATE_START);
    if (!createSegment()) return;
    if (!mBell.create(doorbell_name(ownPid()))) {
        clockwork_log("[tracks] cannot create the doorbell %s", doorbell_name(ownPid()).c_str());
        return;
    }
    if (!openPorts(true)) return;
    mExe = findBridge();
    spawnBridge(false);
}

void TrackControl::shutdown() {
    stopBridge("shutdown");
    closePorts();
    mBell.close();
    if (mHeader) {
        // The signal is gone and the bindings with it; give the audio thread
        // a block to be out of the last signal before the pages go.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        releaseScopes();
        mHeader = nullptr;
        shm_close(mSeg);
        shm_remove(segment_name(ownPid()));
    }
    mEngine = nullptr;
    mEgress = nullptr;
    mClockworkClock = nullptr;
}

bool TrackControl::createSegment() {
    const std::string name = segment_name(ownPid());
    shm_remove(name);   // a leftover from a previous life with this pid
    try {
        mSeg = shm_create(name, SEGMENT_SIZE);
    } catch (const std::exception& e) {
        clockwork_log("[tracks] cannot create the bridge segment: %s", e.what());
        return false;
    }
    double sr = clockwork_sample_rate();
    if (!(sr > 0.0) && mEngine) sr = mEngine->currentDevice().activeSampleRate;
    if (!(sr > 0.0)) sr = 48000.0;
    uint32_t block = clockwork_block_size();
    if (block == 0 || block > MAX_BLOCK) block = std::min<uint32_t>(block ? block : 128, MAX_BLOCK);
    // The device is usually not open yet when the segment is made (the
    // bridge is spawned at boot, the device some seconds later), so this is
    // sized for whatever is known now and re-sized by refreshSlack() from the
    // gateway once the device has settled.
    mDeviceBufferFrames = mEngine ? static_cast<uint32_t>(std::max(0, mEngine->currentDevice().activeBufferSize)) : 0;
    const uint32_t slack = slackFor(mDeviceBufferFrames, block);
    mHeader = header(mSeg.ptr);
    init_header(mHeader, sr, block, clockwork_lane_base(), ownPid(), slack);
    return true;
}

uint32_t TrackControl::slackFor(uint32_t bufferFrames, uint32_t block) {
    uint32_t override = 0;
    if (const char* s = std::getenv("CLOCKWORK_PLUGIN_BRIDGE_SLACK")) {
        const long v = std::strtol(s, nullptr, 10);
        if (v >= 1) override = static_cast<uint32_t>(v);
    }
    return clockwork_bridge::slack_for(bufferFrames, block, override);   // plugin_bridge.h says why
}

// Called from the gateway. currentDevice() is not free (it copies names), so
// this looks every 64 turns — a sixth of a second at 128 frames — which is
// still well inside the time a device swap takes to settle.
void TrackControl::refreshSlack() {
    if (!mHeader || !mEngine) return;
    if (++mSlackPoll < 64) return;
    mSlackPoll = 0;
    const uint32_t frames = static_cast<uint32_t>(std::max(0, mEngine->currentDevice().activeBufferSize));
    if (frames == mDeviceBufferFrames) return;
    mDeviceBufferFrames = frames;
    const uint32_t block = std::max<uint32_t>(1, mHeader->block_size.load(std::memory_order_relaxed));
    const uint32_t slack = slackFor(frames, block);
    const uint32_t was = mHeader->slack_blocks.exchange(slack, std::memory_order_acq_rel);
    if (slack != was)
        clockwork_log("[tracks] bridge slack %u -> %u blocks (device buffer %u frames = %u blocks of %u)",
                      was, slack, frames, (frames + block - 1) / block, block);
}

// ── The ports ────────────────────────────────────────────────────────────────

bool TrackControl::openPorts(bool reset) {
    closePorts();
    mSend = clockwork_port_open_shared("tracks/send", kClockworkPortSink, LANES, AUDIO_RING_FRAMES,
                                 send_ring(mSeg.ptr), AUDIO_RING_BYTES, reset ? 1 : 0);
    mReturn = clockwork_port_open_shared("tracks/return", kClockworkPortSource, LANES, AUDIO_RING_FRAMES,
                                   return_ring(mSeg.ptr), AUDIO_RING_BYTES, reset ? 1 : 0);
    if (mSend == CLOCKWORK_PORT_NONE || mReturn == CLOCKWORK_PORT_NONE) {
        clockwork_log("[tracks] cannot open the bridge's audio ports");
        closePorts();
        return false;
    }
    const uint32_t base = clockwork_lane_base();
    if (base == 0) {
        clockwork_log("[tracks] no lanes were reserved; tracks will be silent");
    } else if (!clockwork_port_bus_attach(mSend, base) || !clockwork_port_bus_attach(mReturn, base)) {
        clockwork_log("[tracks] cannot bind the bridge's ports at lane %u", base);
        closePorts();
        return false;
    } else {
        mBoundBase = base;
    }
    mHeader->lane_base.store(base, std::memory_order_relaxed);
    primeReturn();
    // Last: from here the audio thread posts the doorbell every block, and
    // taps every return block for the scopes.
    clockwork_port_set_signal(mSend, &TrackControl::onSendTransfer, this);
    clockwork_port_set_signal(mReturn, &TrackControl::onReturnTransfer, this);
    return true;
}

void TrackControl::closePorts() {
    if (mSend != CLOCKWORK_PORT_NONE) {
        clockwork_port_set_signal(mSend, nullptr, nullptr);
        clockwork_port_bus_detach(mSend);
        clockwork_port_close(mSend);
        mSend = CLOCKWORK_PORT_NONE;
    }
    if (mReturn != CLOCKWORK_PORT_NONE) {
        clockwork_port_set_signal(mReturn, nullptr, nullptr);
        clockwork_port_bus_detach(mReturn);
        clockwork_port_close(mReturn);
        mReturn = CLOCKWORK_PORT_NONE;
    }
    mBoundBase = 0;
}

// ── The scopes ───────────────────────────────────────────────────────────────

// Audio thread, after the return block has been read onto the input bus.
void TrackControl::onReturnTransfer(ClockworkPort, void* ctx) {
    static_cast<TrackControl*>(ctx)->tapReturns();
}

void TrackControl::tapReturns() {
    Header* h = mHeader;
    if (!h || mBoundBase == 0) return;
    // Keep the claims in step with the bridge's word on which slots hold a
    // track. A lane pair with no track reads as silence, and a scope stream
    // of silence would show a track that is not there.
    const uint32_t live = h->slots_live.load(std::memory_order_relaxed);
    const float* bus = reinterpret_cast<const float*>(get_audio_input_bus());
    const uint32_t frames = clockwork_block_size();
    const uint64_t at = g_engine_frames.load(std::memory_order_relaxed);
    constexpr uint32_t slots = LANES / 2;
    for (uint32_t slot = 0; slot < slots; ++slot) {
        ScopeTap& tap = mScopes[slot];
        const bool want = (live >> slot) & 1u;
        if (want && !tap.writer.valid()) {
            // Past the region on a build that carved no track slots: the
            // claim is refused and the track simply has no scope.
            if (slot < SHM_SCOPE_TRACK_SLOTS
                && clockwork_scope_get(nullptr, static_cast<int>(SHM_SCOPE_TRACK_SLOT_BASE + slot),
                                 static_cast<int>(SHM_SCOPE_STREAM_CHANNELS), 0, &tap.claim))
                tap.writer = shm_scope_stream_writer(
                    static_cast<shm_scope_stream*>(tap.claim.internalData));
        } else if (!want && tap.writer.valid()) {
            clockwork_scope_release(nullptr, &tap.claim);
            tap.writer = shm_scope_stream_writer();
        }
        if (!tap.writer.valid() || !bus || frames == 0) continue;
        // The return lanes are channel-major on the bus, a block apart.
        const float* ch[SHM_SCOPE_STREAM_CHANNELS] = {
            bus + static_cast<size_t>(mBoundBase + 2 * slot) * frames,
            bus + static_cast<size_t>(mBoundBase + 2 * slot + 1) * frames,
        };
        tap.writer.write(ch, frames, at);
    }
}

// Control thread, once the return's signal is gone and the audio thread has
// had a block to leave it.
void TrackControl::releaseScopes() {
    for (ScopeTap& tap : mScopes) {
        if (!tap.writer.valid()) continue;
        clockwork_scope_release(nullptr, &tap.claim);
        tap.writer = shm_scope_stream_writer();
    }
}

// slack_blocks of silence ahead of the first pull, so the bridge's first
// block is due `slack` blocks after it is posted rather than one.
void TrackControl::primeReturn() {
    const uint32_t block = std::min<uint32_t>(mHeader->block_size.load(std::memory_order_relaxed), MAX_BLOCK);
    const uint32_t slack = std::max<uint32_t>(1, mHeader->slack_blocks.load(std::memory_order_relaxed));
    std::vector<float> zeros(size_t(block) * LANES, 0.0f);
    for (uint32_t i = 0; i < slack; ++i) clockwork_port_produce(mReturn, zeros.data(), block);
}

// Audio thread, after the send block is in the ring. What the bridge needs
// to render it, then the bell.
void TrackControl::onSendTransfer(ClockworkPort, void* ctx) {
    auto* self = static_cast<TrackControl*>(ctx);
    Header* h = self->mHeader;
    if (!h) return;
    h->block_time.store(clockwork_block_time(), std::memory_order_relaxed);
    mirror_clock(h, self->mClock);
    // The follower timelines, from their lock-free published copies. A slot
    // nothing holds mirrors as the placeholder; a writer caught mid-way
    // leaves the slot as it was, and the last snapshot still stands.
    if (const ClockworkClock* c = self->mClockworkClock) {
        clockwork::Timeline t;
        for (uint32_t k = 0; k < kMirroredTimelines; ++k)
            if (c->timelineRt(static_cast<int>(k + 1), t))
                clockwork::timelineMirrorWrite(h->timelines[k], t);
    }

    // Keep the return at its depth. In step, the return holds slack-1 blocks
    // here (this block's is not rendered yet); anything more is a block that
    // arrived after the pull it was due for, and is dropped so the latency it
    // would add does not stay. "More" is measured with the callback in mind:
    // a device callback of B blocks pulls B in a row while the bridge, on
    // another core, may already be answering the first of them — so up to
    // B-1 blocks beyond slack-1 are the bridge being on time, not late, and
    // trimming those skipped a real block whenever the bridge was quick.
    const uint32_t block = clockwork_block_size();
    const uint32_t slack = std::max<uint32_t>(1, h->slack_blocks.load(std::memory_order_relaxed));
    const uint32_t perCallback = std::max<uint32_t>(1, (self->mDeviceBufferFrames + block - 1) / std::max<uint32_t>(1, block));
    const uint32_t keep = (slack - 1 + perCallback - 1) * block;
    uint32_t readable = clockwork_port_readable(self->mReturn);
    while (readable > keep) {
        const uint32_t n = std::min<uint32_t>(readable - keep, MAX_BLOCK);
        clockwork_port_read(self->mReturn, g_trimPtrs, LANES, n);
        readable -= n;
    }

    h->engine_blocks.fetch_add(1, std::memory_order_release);
    if (h->bridge_ready.load(std::memory_order_acquire)) self->mBell.post();
}

// ── The bridge's life ────────────────────────────────────────────────────────

std::string TrackControl::findBridge() const {
    if (const char* env = std::getenv("CLOCKWORK_PLUGIN_BRIDGE")) {
        if (*env && juce::File(env).existsAsFile()) return env;
    }
    const juce::File self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    return findBridgeBeside(self.getParentDirectory().getFullPathName().toStdString());
}

std::string TrackControl::findBridgeBeside(const std::string& exeDir) {
    const juce::File dir(exeDir);
#if defined(_WIN32)
    const juce::File sibling = dir.getChildFile(CLOCKWORK_PLUGIN_BRIDGE_NAME ".exe");
#elif defined(__APPLE__)
    // An app bundle beside the engine, so the process has a bundle identifier
    // (what OBS and ScreenCaptureKit select an application by). A bare binary
    // of the same name is still accepted, for a build that predates the bundle.
    const juce::File bundled = dir.getChildFile(CLOCKWORK_PLUGIN_BRIDGE_NAME ".app")
                                  .getChildFile("Contents/MacOS/" CLOCKWORK_PLUGIN_BRIDGE_NAME);
    if (bundled.existsAsFile()) return bundled.getFullPathName().toStdString();
    const juce::File sibling = dir.getChildFile(CLOCKWORK_PLUGIN_BRIDGE_NAME);
#else
    const juce::File sibling = dir.getChildFile(CLOCKWORK_PLUGIN_BRIDGE_NAME);
#endif
    if (sibling.existsAsFile()) return sibling.getFullPathName().toStdString();
#ifdef CLOCKWORK_PLUGIN_BRIDGE_DIR
    // An installed layout: the bridge lives where the package put it
    // (CLOCKWORK_PLUGIN_BRIDGE_DIR at configure time — /usr/libexec/<pkg>,
    // say), not beside an engine that sits in /usr/bin.
    const juce::File installed = juce::File(CLOCKWORK_PLUGIN_BRIDGE_DIR)
                                     .getChildFile(CLOCKWORK_PLUGIN_BRIDGE_NAME
#if defined(_WIN32)
                                                   ".exe"
#endif
                                                   );
    if (installed.existsAsFile()) return installed.getFullPathName().toStdString();
#endif
    return {};
}

bool TrackControl::spawnBridge(bool restore) {
    if (!mSpawnEnabled || !mHeader) return false;
    if (mExe.empty()) {
        if (!mSpawnFailedReported) {
            mSpawnFailedReported = true;
            clockwork_log("[tracks] no plugin bridge found beside the engine"
#ifdef CLOCKWORK_PLUGIN_BRIDGE_DIR
                    " or in " CLOCKWORK_PLUGIN_BRIDGE_DIR
#endif
                    " (looked for %s); set CLOCKWORK_PLUGIN_BRIDGE to its path",
                    CLOCKWORK_PLUGIN_BRIDGE_NAME);
        }
        return false;
    }
    mHeader->bridge_ready.store(0, std::memory_order_release);
    mHeader->bridge_pid.store(0, std::memory_order_release);
    mHeader->quit.store(0, std::memory_order_release);
    mHeader->restore.store(restore ? 1 : 0, std::memory_order_release);
    mHeader->generation.fetch_add(1, std::memory_order_acq_rel);
    mLastHeartbeat = mHeader->bridge_heartbeat.load(std::memory_order_acquire);
    mBlocksAtLastHeartbeat = mHeader->engine_blocks.load(std::memory_order_acquire);
    if (!mProcess.spawn(mExe, {std::to_string(ownPid())})) {
        if (!mSpawnFailedReported) {
            mSpawnFailedReported = true;
            clockwork_log("[tracks] cannot start the plugin bridge %s", mExe.c_str());
            broadcastError("bridge", "the plugin process could not be started");
        }
        return false;
    }
    mSpawnedAt = std::chrono::steady_clock::now();
    clockwork_log("[tracks] plugin bridge started (pid %d, generation %u%s)", mProcess.pid(),
            mHeader->generation.load(std::memory_order_relaxed), restore ? ", restoring" : "");
    return true;
}

void TrackControl::stopBridge(const char* why) {
    if (!mProcess.running()) return;
    if (mHeader) {
        mHeader->quit.store(1, std::memory_order_release);
        mBell.post();
    }
    const auto deadline = std::chrono::steady_clock::now() + QUIT_GRACE;
    while (mProcess.alive() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (mProcess.alive()) {
        clockwork_log("[tracks] the plugin bridge did not leave on %s; killing it", why);
        mProcess.kill();
    }
    mProcess.wait();
    mProcess.detach();
    if (mHeader) mHeader->bridge_ready.store(0, std::memory_order_release);
}

void TrackControl::onBridgeDied(const char* how) {
    const auto now = std::chrono::steady_clock::now();
    mDeaths.push_back(now);
    while (!mDeaths.empty() && now - mDeaths.front() > CRASH_LOOP_WINDOW) mDeaths.pop_front();

    // Blame: a load in progress when it died names the plugin.
    std::string culprit;
    if (mHeader->loading_seq.load(std::memory_order_acquire) & 1u) {
        mHeader->loading_plugin[PATH_MAX_BYTES - 1] = '\0';
        culprit = mHeader->loading_plugin;
    }
    // Every ring the bridge read is reset: the counters it left mean nothing
    // to its successor.
    mHeader->bridge_ready.store(0, std::memory_order_release);
    openPorts(true);

    bool restore = mHeader->mirror_len.load(std::memory_order_acquire) > 0;
    std::string detail = std::string("the plugin process ") + how;
    if (!culprit.empty()) detail += " while loading " + culprit;
    if (mDeaths.size() >= CRASH_LOOP_COUNT) {
        restore = false;
        mDeaths.clear();
        mHeader->mirror_len.store(0, std::memory_order_release);
        detail += "; it has died " + std::to_string(CRASH_LOOP_COUNT) +
                  " times in a row, so it is restarting with no tracks";
    } else {
        detail += restore ? "; restarting with the tracks it had" : "; restarting";
    }
    clockwork_log("[tracks] %s", detail.c_str());
    broadcastError("bridge", detail);
    spawnBridge(restore);
}

// ── The gateway pass ─────────────────────────────────────────────────────────

void TrackControl::gatewayPass() {
    refreshSlack();
    if (!mHeader) return;
    relayOut();
    watchLiveness();
    watchGeometry();
    const uint32_t dropped = mRtDropped.load(std::memory_order_relaxed);
    if (dropped != mRtDroppedReported) {
        mRtDroppedReported = dropped;
        debug("track events dropped: the realtime ring to the plugin process is full (" +
              std::to_string(dropped) + " so far)");
    }
}

// The bridge's answers are already framed [route][osc] with the caller's
// token, which is exactly the NRT egress lane's shape — so each one is
// re-queued onto that lane rather than handed to the transport directly.
// That keeps the bridge invisible in the only way that matters: a host that
// drains the lanes (clockwork_egress_nrt_drain) sees a track reply the same way it
// sees any other, the lane metrics count it, and the gateway's own NRT drain
// (which runs after this pass) delivers it in the same wake. A refused write
// means the lane is full; the frame stays on the OUT ring for the next pass
// rather than being lost, and the bridge feels the back-pressure.
void TrackControl::relayOut() {
    MsgRing& r = mHeader->out;
    clockwork_drain_ring(out_ring(mSeg.ptr), OUT_RING_SIZE, &r.head, &r.tail, mOutDrain, ClockworkDrainMetrics{},
        256,
        [](uint32_t token, const uint8_t* payload, uint32_t len, uint32_t) {
            if (len <= sizeof(Prefixed)) return ClockworkDrainVerdict::Consume;
            uint32_t route = 0;
            std::memcpy(&route, payload, sizeof route);
            const uint8_t* osc = payload + sizeof(Prefixed);
            const uint32_t oscLen = len - static_cast<uint32_t>(sizeof(Prefixed));
            return clockwork_egress_nrt_write(route, token, osc, oscLen)
                ? ClockworkDrainVerdict::Consume : ClockworkDrainVerdict::Retain;
        });
}

void TrackControl::watchLiveness() {
    if (!mProcess.running()) return;
    if (!mProcess.alive()) {
        char how[96];
        const int code = mProcess.exitCode();
        if (code < 0) std::snprintf(how, sizeof how, "crashed (signal %d)", -code);
        else          std::snprintf(how, sizeof how, "exited (code %d)", code);
        mProcess.detach();
        onBridgeDied(how);
        return;
    }
    const uint64_t hb = mHeader->bridge_heartbeat.load(std::memory_order_acquire);
    const uint64_t blocks = mHeader->engine_blocks.load(std::memory_order_acquire);
    if (mHeader->bridge_ready.load(std::memory_order_acquire)) {
        if (hb != mLastHeartbeat) {
            mLastHeartbeat = hb;
            mBlocksAtLastHeartbeat = blocks;
        } else if (blocks - mBlocksAtLastHeartbeat > HUNG_BLOCKS) {
            mProcess.kill();
            mProcess.wait();
            mProcess.detach();
            onBridgeDied("stopped rendering");
        }
    } else if (std::chrono::steady_clock::now() - mSpawnedAt > START_TIMEOUT) {
        mProcess.kill();
        mProcess.wait();
        mProcess.detach();
        onBridgeDied("did not start");
    }
}

// What the engine may have changed underneath the bridge: the rate on a
// device change, the lane base on a cold swap that moved it.
void TrackControl::watchGeometry() {
    const double sr = clockwork_sample_rate();
    if (sr > 0.0) {
        const uint64_t bits = clockwork::doubleToBits(sr);
        if (mHeader->sample_rate_bits.load(std::memory_order_relaxed) != bits)
            mHeader->sample_rate_bits.store(bits, std::memory_order_relaxed);
    }
    const uint32_t block = clockwork_block_size();
    if (block > 0 && block <= MAX_BLOCK && mHeader->block_size.load(std::memory_order_relaxed) != block)
        mHeader->block_size.store(block, std::memory_order_relaxed);
    const uint32_t base = clockwork_lane_base();
    if (base != 0 && base != mBoundBase && mSend != CLOCKWORK_PORT_NONE) {
        clockwork_port_bus_detach(mSend);
        clockwork_port_bus_detach(mReturn);
        if (clockwork_port_bus_attach(mSend, base) && clockwork_port_bus_attach(mReturn, base)) {
            mBoundBase = base;
            mHeader->lane_base.store(base, std::memory_order_relaxed);
            clockwork_log("[tracks] lanes moved to %u", base);
        } else {
            clockwork_log("[tracks] cannot rebind the bridge's ports at lane %u", base);
        }
    }
}

// ── The wire ─────────────────────────────────────────────────────────────────

std::vector<uint8_t> TrackControl::resolveTimelineVerb(
    const uint8_t* data, uint32_t size, const std::function<int(const char*)>& resolve) {
    std::vector<uint8_t> asIs(data, data + size);
    try {
        osc::ReceivedMessage msg(osc::ReceivedPacket(
            reinterpret_cast<const char*>(data), static_cast<osc::osc_bundle_element_size_t>(size)));
        if (std::strcmp(msg.AddressPattern(), CLOCKWORK_SYS("track/timeline")) != 0) return asIs;
        auto it = msg.ArgumentsBegin();
        const auto end = msg.ArgumentsEnd();
        if (it == end) return asIs;
        const auto track = it++;                       // int id or string name: copied as it came
        if (it == end || !it->IsString()) return asIs; // a query
        const char* name = it->AsStringUnchecked();
        if (std::strcmp(name, "link") == 0) return asIs;   // the bridge knows this one

        char buf[512];
        osc::OutboundPacketStream s(buf, sizeof buf);
        s << osc::BeginMessage(CLOCKWORK_SYS("track/timeline"));
        if (track->IsInt32()) s << static_cast<osc::int32>(track->AsInt32Unchecked());
        else if (track->IsString()) s << track->AsStringUnchecked();
        else return asIs;
        s << name << static_cast<osc::int32>(resolve(name)) << osc::EndMessage;
        return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(s.Data()),
                                    reinterpret_cast<const uint8_t*>(s.Data()) + s.Size());
    } catch (...) {
        return asIs;   // malformed: the bridge refuses it as it would have
    }
}

bool TrackControl::handleTrackCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size) {
    if (!mHeader) {
        broadcastError("bridge", "plugin hosting is not available in this engine");
        return true;
    }
    // A timeline name is the engine's to resolve; the bridge gets the id.
    std::vector<uint8_t> resolved;
    if (size > CLOCKWORK_SYS_LEN("track/timeline")
        && std::memcmp(data, CLOCKWORK_SYS("track/timeline"), CLOCKWORK_SYS_LEN("track/timeline")) == 0) {
        resolved = resolveTimelineVerb(data, size, [this](const char* name) {
            return mClockworkClock ? mClockworkClock->resolveOrClaimTimeline(name) : -1;
        });
        data = resolved.data();
        size = static_cast<uint32_t>(resolved.size());
    }
    MsgRing& r = mHeader->ctl;
    if (!RingBufferWriter::write(ctl_ring(mSeg.ptr), CTL_RING_SIZE, &r.head, &r.tail, &r.sequence,
                                 &r.write_lock, data, size, meta.sourceId)) {
        r.dropped.fetch_add(1, std::memory_order_relaxed);
        broadcastError("bridge", mProcess.running()
                       ? "the plugin process is not keeping up; try again"
                       : "the plugin process is not running");
    }
    return true;
}

bool TrackControl::audioSink(void* ctx, const void* callCtx, const uint8_t* data, std::size_t len) {
    auto* self = static_cast<TrackControl*>(ctx);
    Header* h = self ? self->mHeader : nullptr;
    if (!h || len == 0 || len > RT_RING_SIZE / 8) return true;
    static const DrainCallCtx kEmpty{};
    const DrainCallCtx& meta = callCtx ? *static_cast<const DrainCallCtx*>(callCtx) : kEmpty;
    // [frame offset][osc], joined on the stack: a track event is small.
    uint8_t frame[2048];
    if (sizeof(Prefixed) + len > sizeof frame) return true;
    const uint32_t off = frameOffset(meta);
    std::memcpy(frame, &off, sizeof off);
    std::memcpy(frame + sizeof(Prefixed), data, len);
    MsgRing& r = h->rt;
    if (!RingBufferWriter::write(rt_ring(self->mSeg.ptr), RT_RING_SIZE, &r.head, &r.tail, &r.sequence,
                                 &r.write_lock, frame, static_cast<uint32_t>(sizeof(Prefixed) + len),
                                 meta.sourceId)) {
        r.dropped.fetch_add(1, std::memory_order_relaxed);
        self->mRtDropped.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

void TrackControl::broadcastError(const char* verb, const std::string& detail, int handle) {
    if (!mEgress) return;
    // /clockwork/track/error <verb:string> <detail:string> <handle:int> — the
    // same message the bridge sends, so a panel has one thing to listen for.
    std::vector<char> buf(256 + detail.size());
    osc::OutboundPacketStream s(buf.data(), buf.size());
    s << osc::BeginMessage(CLOCKWORK_SYS("track/error")) << verb << detail.c_str()
      << static_cast<osc::int32>(handle) << osc::EndMessage;
    mEgress->broadcastToTargets(reinterpret_cast<const uint8_t*>(s.Data()),
                                static_cast<uint32_t>(s.Size()));
}

void TrackControl::debug(const std::string& line) {
    if (mEgress) mEgress->debug(line.data(), static_cast<uint32_t>(line.size()));
}

bool TrackControl::bridgeReady() const {
    return mHeader && mHeader->bridge_ready.load(std::memory_order_acquire) != 0;
}

uint32_t TrackControl::generation() const {
    return mHeader ? mHeader->generation.load(std::memory_order_acquire) : 0;
}
