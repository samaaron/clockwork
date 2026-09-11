// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * TrackControl.h — the engine's side of the plugin bridge.
 *
 * The tracks themselves, and the plugins on them, live in the BRIDGE: a
 * second process the engine spawns and talks to through one shared-memory
 * segment (src/plugin_bridge.h). What is left in the engine is the wire and
 * the audio path, and this class is both:
 *
 *   THE AUDIO PATH. The send lanes are a SINK port bound at the lane base,
 *   the return lanes a SOURCE port bound at the same place, and both are
 *   rings inside the segment. The DSP writes a send as it would write to a
 *   speaker and reads a return as it would read a microphone; what happens
 *   between is the bridge's business. The sink's transfer signal — a few
 *   instructions on the audio thread, once per block — posts the doorbell
 *   the bridge sleeps on, stamps the block time and mirrors the session
 *   clock, so the bridge renders against the timeline the graph did.
 *
 *   THE SCOPES. The source's transfer signal fires once the return block is
 *   on the input bus, and that is where each track's output is tapped into
 *   a scope stream of its own (memory_profile.h SHM_SCOPE_TRACK_SLOT_BASE +
 *   slot), so a panel can draw what a track is putting out without a scope
 *   node in the graph. Which lanes carry a track the bridge says each block
 *   (Header::slots_live); a slot that gains a track claims its stream on the
 *   audio thread and one that loses it gives the stream back — a claim and
 *   a release are a few atomic stores.
 *
 *   THE WIRE. "/clockwork/track/…" arrives here as it always did. The six
 *   verbs that PLAY a track (note, cc, bend, notes_off, param, plugin/param)
 *   are audio-thread routes: each is turned into a frame offset and put on
 *   the realtime ring for the bridge's audio thread to apply within the same
 *   block. Everything else goes on the control ring with the caller's origin
 *   token, and the bridge's answers come back on the OUT ring, each already
 *   marked with its egress route, so relaying one is a write onto the NRT
 *   egress lane — the same ring every other control-thread reply takes.
 *   One verb is read on the way through: "track/timeline" names a timeline
 *   only the engine's registry can resolve, so the id goes on the message
 *   before it is relayed (resolveTimelineVerb).
 *
 *   THE TIMELINES. The session clock reaches the bridge as a copy of the
 *   clock state, taken in the send signal. A track bound to a midi follower
 *   timeline needs that timeline's snapshot the same way, and its registry
 *   is behind a lock the audio thread may not take — so the same signal
 *   reads each slot's lock-free published copy (ClockworkClock::timelineRt) and
 *   writes it into the segment under a seqlock (Header::timelines), one
 *   slot per timeline id, for the bridge's audio thread to read.
 *
 *   THE BRIDGE'S LIFE. Spawned after the engine is up; watched from the NRT
 *   gateway once per block: a pid that has gone is a crash, a heartbeat that
 *   has stopped while blocks keep being posted is a hang. Either way the
 *   ports are reset, the bridge is respawned with `restore` set, and it
 *   reads the tracks back from the mirror the last one kept. Three deaths in
 *   thirty seconds and the next spawn starts empty, with the plugin that was
 *   loading named as the likely cause. Every state change is broadcast as
 *   "/clockwork/track/error" or "/clockwork/track/list", so a panel can say
 *   "restarting" and then show what came back.
 *
 * WHY THE ENGINE NEVER SEES A PLUGIN. Third-party code that allocates and
 * locks on the audio thread, opens windows and crashes has no place in the
 * process whose one job is to make the next block on time. The cost is a
 * round trip of `slack_blocks` — sized to the device callback (slackFor), two at the least, and
 * 48 kHz — and that is a cost every sandboxing host pays.
 */
#pragma once

#include "plugin_bridge.h"
#include "lanes/ring_drain.h"
#include "shm_scope_stream.hpp"
#include "shm_segment.hpp"
#include "clockwork_process.h"
#include "clockwork_ports.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class OscEgress;
class ClockworkClock;
class ClockworkEngine;
struct DrainCallCtx;

class TrackControl {
public:
    // Before the engine boots: the lanes the tracks route through are laid
    // out when memory is.
    static void reserveLanes();

    // After the engine is up. Lays out the segment, opens the ports over it,
    // and spawns the bridge. `clock` is the engine's: the session clock and
    // the follower timelines the bridge is shown each block.
    void init(ClockworkEngine* engine, OscEgress* egress, ClockworkClock* clock);
    void shutdown();

    // NRT half: one "/clockwork/track/" command, relayed to the bridge.
    bool handleTrackCommand(const DrainCallCtx& meta, const uint8_t* data, uint32_t size);

    // "/clockwork/track/timeline <track> <name>" names a timeline only the
    // engine's registry can resolve — and, for a "midi:<port>" it has not
    // heard, claim, so a track can be bound before the port clocks. The
    // message the bridge is sent carries the id after the name; this is that
    // rewrite, on its own so a test can drive it. `resolve` answers the id
    // for a name (ClockworkClock::resolveOrClaimTimeline: 0 link, 1.. a slot, -1
    // none). A query (no name), a bare "link", or any other verb passes
    // through byte for byte.
    static std::vector<uint8_t> resolveTimelineVerb(
        const uint8_t* data, uint32_t size, const std::function<int(const char*)>& resolve);

    // Audio-thread half, registered per verb on clockwork route table:
    // note, cc, bend, notes_off, param, plugin/param. ctx is the TrackControl.
    static bool audioSink(void* ctx, const void* callCtx, const uint8_t* data, std::size_t len);

    // NRT gateway task, once per block wake: relay what the bridge sent,
    // watch its liveness, keep its geometry current.
    void gatewayPass();

    // For tests and diagnostics.
    bool bridgeRunning() const { return mProcess.running(); }
    bool bridgeReady() const;
    int  bridgePid() const { return mProcess.pid(); }
    uint32_t generation() const;
    std::string bridgeExecutable() const { return mExe; }
    // Where the bridge is, given the directory the engine's executable is in:
    // beside it first, then CLOCKWORK_PLUGIN_BRIDGE_DIR (an installed layout),
    // else empty. What findBridge() does after CLOCKWORK_PLUGIN_BRIDGE (the
    // environment override); split out so a test can hand it a directory.
    static std::string findBridgeBeside(const std::string& exeDir);
    // Skip the spawn (a test that drives the segment itself).
    void setSpawnEnabled(bool on) { mSpawnEnabled = on; }

private:
    static void onSendTransfer(ClockworkPort port, void* ctx);
    // The bridge's slack, sized to the device: a callback of B blocks pulls
    // B returns in one go, so the return must hold B ahead of it or the
    // tail of every callback is silence and the bridge's late render is
    // dropped as stale. Read from the engine when the device (re)starts.
    static uint32_t slackFor(uint32_t bufferFrames, uint32_t block);
    void refreshSlack();
    uint32_t mDeviceBufferFrames = 0;   // what the slack was last sized for
    uint32_t mSlackPoll = 0;            // gatewayPass turns since the last look
    static void onReturnTransfer(ClockworkPort port, void* ctx);
    void tapReturns();
    void releaseScopes();

    bool createSegment();
    bool openPorts(bool reset);
    void closePorts();
    void primeReturn();
    std::string findBridge() const;
    bool spawnBridge(bool restore);
    void stopBridge(const char* why);
    void onBridgeDied(const char* how);
    void relayOut();
    void watchLiveness();
    void watchGeometry();
    void broadcastError(const char* verb, const std::string& detail, int handle = 0);
    void debug(const std::string& line);

    ClockworkEngine* mEngine = nullptr;
    OscEgress* mEgress = nullptr;
    ClockworkClock*  mClockworkClock = nullptr;

    detail_shm_segment::shm_handle mSeg {};
    clockwork_bridge::Header*  mHeader = nullptr;      // null until the segment exists
    clockwork_bridge::Doorbell mBell;
    ClockworkPort  mSend = CLOCKWORK_PORT_NONE;   // SINK: the DSP's sends, into the segment
    ClockworkPort  mReturn = CLOCKWORK_PORT_NONE; // SOURCE: the bridge's returns, out of it
    uint32_t mBoundBase = 0;          // where the ports are bound; 0 = not bound
    const ClockworkClockState* mClock = nullptr;

    // One scope stream per track slot, held while the slot has a track. The
    // handle is the slot's claim (rust/clockwork-scope: the same shape scsynth's
    // ScopeOut2 claims with, so the owner-only release rule holds for these
    // too); the writer appends the return block. Audio thread only, between
    // the signal's install and its removal.
    struct ScopeTap {
        struct Claim { void* internalData = nullptr; float* data = nullptr;
                       uint32_t channels = 0; uint32_t maxFrames = 0; } claim;
        shm_scope_stream_writer writer;
    };
    ScopeTap mScopes[clockwork_bridge::LANES / 2];

    ClockworkProcess mProcess;
    std::string mExe;
    bool mSpawnEnabled = true;
    bool mSpawnFailedReported = false;
    std::chrono::steady_clock::time_point mSpawnedAt {};
    std::deque<std::chrono::steady_clock::time_point> mDeaths;   // recent crash times
    uint64_t mLastHeartbeat = 0;
    uint64_t mBlocksAtLastHeartbeat = 0;
    std::atomic<uint32_t> mRtDropped{0};
    uint32_t mRtDroppedReported = 0;
    ClockworkDrainState mOutDrain {};
};
