// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * plugin_bridge.h — the contract between the engine and the plugin bridge.
 *
 * A PLUGIN IS A PROCESS BOUNDARY. Third-party code that allocates and locks
 * on the audio thread, opens windows, and crashes, does not run inside the
 * engine: it runs in the BRIDGE, a second process the engine spawns, and the
 * two meet in one shared-memory SEGMENT laid out by this header. The engine
 * owns the segment, the ports over it, the lanes the graph addresses and the
 * bridge's life; the bridge owns the plugins and the track model
 * (src/plugin_track.h). If a plugin takes the bridge down, the engine hears
 * silence on the return lanes for a moment, respawns it, and restores the
 * tracks from the mirror the bridge kept in the segment.
 *
 * WHAT IS IN THE SEGMENT, in order:
 *
 *   Header      — geometry, liveness, the cursors of the three message
 *                 rings, a mirror of the session clock and of the midi
 *                 follower timelines, and the path of the plugin being
 *                 loaded (so a crash can be blamed).
 *   SEND ring   — audio, engine -> bridge. LANES channels interleaved, in
 *                 the shape of a port over shared storage
 *                 (clockwork_port_open_shared): the engine's SINK port, bound at
 *                 the lane base; the bridge's SOURCE for the same memory.
 *   RETURN ring — audio, bridge -> engine. The mirror image.
 *   CTL ring    — messages, engine NRT thread -> bridge main thread: the
 *                 track verbs, as OSC, with the caller's origin token as the
 *                 frame's sourceId so the reply can find its way back.
 *   RT ring     — messages, engine audio thread -> bridge audio thread: the
 *                 events that must land on a frame (notes, controls,
 *                 parameter moves), each prefixed with its frame offset.
 *   OUT ring    — messages, bridge -> engine: replies and broadcasts, each
 *                 prefixed with the egress route; the engine drains it on
 *                 the NRT gateway and hands each to OscEgress.
 *   MIRROR      — the rig as JSON, rewritten by the bridge after every
 *                 change. What a respawn restores from.
 *
 * All three message rings are the stock ring (src/ring/ring.h): 16-byte
 * frames that never wrap, written with RingBufferWriter and read with
 * clockwork_drain_ring, the same code the peer plane uses. Their cursors live in
 * the header so the far side can write them; both readers treat every cursor
 * as untrusted, as the drain does.
 *
 * THE DOORBELL. The bridge's audio thread does not poll: it sleeps on a
 * named semaphore the engine posts once per block, from the send port's
 * transfer signal (clockwork_port_set_signal), after the block's send audio is in
 * the ring. On waking it takes one block from the send, renders every track,
 * and puts one block on the return. The engine reads the return at the top
 * of its next block.
 *
 * LATENCY AND SLACK. The engine primes the return with `slack_blocks` of
 * silence, and the round trip is exactly that many blocks: what goes out at
 * block k comes back at block k + slack. One is the latency a plugin would
 * have in-process, but then the bridge must finish every render inside the
 * gap between two callbacks or the engine reads silence where the block
 * should be. Two — the default, ~5 ms at 128 frames and 48 kHz — gives the
 * bridge a whole callback interval of grace for the cost of one more block.
 * A bridge that runs late and catches up would leave the return deeper than
 * it found it, so the engine trims it back to slack − 1 blocks (the steady
 * depth before it reads) each time it posts. A synchronous mode, where the
 * engine waits for the render with a deadline, would fit this layout with
 * one more semaphore; it is not built.
 *
 * LIVENESS runs both ways. The bridge counts blocks rendered
 * (`bridge_heartbeat`); the engine counts blocks posted (`engine_blocks`);
 * an engine that posts many blocks and sees no heartbeat has a hung bridge
 * and kills it. The bridge watches its parent and exits when the engine is
 * gone, and exits when `quit` is set. `generation` changes on every spawn,
 * so a straggler from a previous life cannot mistake the segment for its own.
 *
 * NAMES. The segment and the semaphore are named from the engine's pid, so
 * two engines on one machine never share, and so the names stay inside the
 * 31 characters macOS allows a POSIX shared object.
 */
#ifndef CLOCKWORK_PLUGIN_BRIDGE_H
#define CLOCKWORK_PLUGIN_BRIDGE_H

#include "clock/timeline_mirror.h"   // ClockworkTimelineMirrorSlot: a follower timeline, lock-free
#include "shared_memory.h"           // ClockworkClockState
#include "clockwork_path.h"                // the doorbell's name, UTF-16 for Win32

#include <cerrno>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <semaphore.h>
#  include <sys/stat.h>
#endif

namespace clockwork_bridge {

constexpr uint32_t MAGIC   = 0x54425247;   // "TBRG"
constexpr uint32_t VERSION = 3;   // 3: timelines (the meter moved the clock mirror too)

// Follower timelines mirrored per block: slot k carries timeline id k + 1.
// Sized for the native memory profile's SC_MAX_TIMELINES; the engine writes
// as many as it has and leaves the rest as they were made (all zero, which
// reads as "nothing there").
constexpr uint32_t TIMELINE_SLOTS = 8;

// The audio geometry. LANES is CLOCKWORK_TRACK_MAX * 2 (src/plugin_track.h), fixed
// here rather than included so the engine, which never sees the track model,
// does not depend on its header.
constexpr uint32_t LANES             = 32;
constexpr uint32_t AUDIO_RING_FRAMES = 2048;  // a power of two; > any block
constexpr uint32_t MAX_BLOCK         = 1024;  // the largest block either side handles
constexpr uint32_t DEFAULT_SLACK     = 2;

// How far ahead the bridge must be, in blocks, for a device callback of
// `buffer_frames`. A callback of B blocks ticks B engine blocks back to
// back, each pulling one block of return audio, in less time than the
// bridge takes to wake — so the return must already hold B, plus two: one
// because the bridge can render a block while the callback is still pulling
// (a slack of exactly B counts that as stale and drops it, a click per
// callback), one for scheduling jitter. DEFAULT_SLACK is the floor, and was
// the whole story when every callback was one or two blocks; a 1024-frame
// WASAPI buffer ticks eight, and at slack two played a quarter of each
// callback and dropped the rest — a 375 Hz buzz over the piece. Bounded by
// the ring: AUDIO_RING_FRAMES holds sixteen blocks of 128, one stays free.
// `override` (CLOCKWORK_PLUGIN_BRIDGE_SLACK in the environment) wins within
// bounds; 0 means none.
inline constexpr uint32_t slack_for(uint32_t buffer_frames, uint32_t block, uint32_t override = 0) {
    if (block == 0) block = 128;
    const uint32_t max_slack = AUDIO_RING_FRAMES / block > 1 ? AUDIO_RING_FRAMES / block - 1 : 1;
    if (override >= 1) return override < max_slack ? override : max_slack;
    const uint32_t per_callback = buffer_frames ? (buffer_frames + block - 1) / block : 1;
    const uint32_t want = per_callback <= 1 ? DEFAULT_SLACK : per_callback + 2;
    return want < max_slack ? want : max_slack;
}

constexpr uint32_t CTL_RING_SIZE = 256 * 1024;
constexpr uint32_t RT_RING_SIZE  =  64 * 1024;
constexpr uint32_t OUT_RING_SIZE = 256 * 1024;
constexpr uint32_t MIRROR_SIZE   = 4 * 1024 * 1024;
constexpr uint32_t PATH_MAX_BYTES = 1024;

// The cursors of one message ring, in the shape RingBufferWriter and
// clockwork_drain_ring take them.
struct MsgRing {
    std::atomic<int32_t> head{0};
    std::atomic<int32_t> tail{0};
    std::atomic<int32_t> sequence{0};
    std::atomic<int32_t> write_lock{0};
    std::atomic<uint32_t> dropped{0};   // frames that did not fit
    uint32_t _pad[3] {};
};
static_assert(sizeof(MsgRing) == 32, "MsgRing is laid out by hand");

struct alignas(64) Header {
    uint32_t magic;
    uint32_t version;
    uint32_t lanes;               // LANES
    uint32_t audio_ring_frames;   // AUDIO_RING_FRAMES
    uint32_t ctl_ring_size, rt_ring_size, out_ring_size, mirror_size;

    // The engine's audio geometry. sample_rate changes on a device change;
    // block_size is the maximum the engine will hand over in one block.
    std::atomic<uint64_t> sample_rate_bits;   // double, as bits
    std::atomic<uint32_t> block_size;
    // Where the engine's lanes start, for the channel numbers the bridge
    // reports (clockwork_track_set_lane_base). Changes on a cold swap.
    std::atomic<uint32_t> lane_base;
    std::atomic<uint32_t> slack_blocks;

    // Liveness.
    std::atomic<uint32_t> generation;       // bumped by the engine on every spawn
    std::atomic<int32_t>  engine_pid;
    std::atomic<int32_t>  bridge_pid;       // written by the bridge on join; 0 before
    std::atomic<uint32_t> quit;             // engine asks the bridge to leave
    std::atomic<uint32_t> restore;          // spawn with: restore the rig from the mirror
    std::atomic<uint64_t> engine_blocks;    // doorbells posted
    std::atomic<uint64_t> bridge_heartbeat; // blocks rendered
    std::atomic<uint32_t> bridge_ready;     // the bridge has joined and is rendering
    std::atomic<uint32_t> mirror_len;       // bytes of JSON in the mirror, 0 = none

    // Which lane pairs carry a track, bit k for slot k (clockwork_track_slot_mask),
    // written by the bridge every block it renders. The engine never sees
    // the track model; this is how it knows which return lanes are worth
    // tapping — one scope stream per track (TrackControl, the return port's
    // transfer signal) — and which are silence it can leave alone.
    std::atomic<uint32_t> slots_live;

    // The time of the block the engine just handed over, so the bridge can
    // render against the same clock the graph did.
    std::atomic<int64_t>  block_time;

    MsgRing ctl;   // engine -> bridge, control
    MsgRing rt;    // engine -> bridge, realtime
    MsgRing out;   // bridge -> engine

    // The session clock, mirrored by the engine each block with the same
    // key-last protocol readClockworkClock expects.
    ClockworkClockState clock;

    // The midi follower timelines, mirrored by the engine each block from
    // their lock-free published copies (ClockworkClock::timelineRt). A track bound
    // to timeline id k renders against timelines[k - 1]; an unheld slot
    // carries the id -1 placeholder. What lets a track follow a MIDI clock
    // the engine hears, from a process that never sees the registry.
    ClockworkTimelineMirrorSlot timelines[TIMELINE_SLOTS];

    // The path of the plugin the bridge is opening, empty when it is not
    // opening one. Written by the bridge before plugin_open and cleared
    // after; if the bridge dies in between, this is the culprit.
    std::atomic<uint32_t> loading_seq;
    char loading_plugin[PATH_MAX_BYTES];
};

// ── Layout ─────────────────────────────────────────────────────────────────
// Every region starts on a page, so the audio rings are 64-byte aligned as
// clockwork_port_open_shared requires and nothing shares a line across a boundary.

constexpr size_t PAGE = 4096;
constexpr size_t page_up(size_t n) { return (n + PAGE - 1) & ~(PAGE - 1); }

// clockwork_port_shared_bytes(LANES, AUDIO_RING_FRAMES), spelled out so the layout
// is a constant. CLOCKWORK_PORT_SHARED_HEADER_BYTES is 128.
constexpr size_t AUDIO_RING_BYTES  = 128 + size_t(AUDIO_RING_FRAMES) * LANES * sizeof(float);

constexpr size_t HEADER_OFFSET = 0;
constexpr size_t SEND_OFFSET   = page_up(sizeof(Header));
constexpr size_t RETURN_OFFSET = SEND_OFFSET   + page_up(AUDIO_RING_BYTES);
constexpr size_t CTL_OFFSET    = RETURN_OFFSET + page_up(AUDIO_RING_BYTES);
constexpr size_t RT_OFFSET     = CTL_OFFSET    + page_up(CTL_RING_SIZE);
constexpr size_t OUT_OFFSET    = RT_OFFSET     + page_up(RT_RING_SIZE);
constexpr size_t MIRROR_OFFSET = OUT_OFFSET    + page_up(OUT_RING_SIZE);
constexpr size_t SEGMENT_SIZE  = MIRROR_OFFSET + page_up(MIRROR_SIZE);

static_assert(sizeof(Header) <= SEND_OFFSET, "header must fit its page(s)");

inline Header*  header(void* base)      { return static_cast<Header*>(base); }
inline uint8_t* send_ring(void* base)   { return static_cast<uint8_t*>(base) + SEND_OFFSET; }
inline uint8_t* return_ring(void* base) { return static_cast<uint8_t*>(base) + RETURN_OFFSET; }
inline uint8_t* ctl_ring(void* base)    { return static_cast<uint8_t*>(base) + CTL_OFFSET; }
inline uint8_t* rt_ring(void* base)     { return static_cast<uint8_t*>(base) + RT_OFFSET; }
inline uint8_t* out_ring(void* base)    { return static_cast<uint8_t*>(base) + OUT_OFFSET; }
inline char*    mirror(void* base)      { return static_cast<char*>(base) + MIRROR_OFFSET; }

// A frame on the RT ring: the offset of the event within the block, then the
// OSC message. On the OUT ring: the egress route (EgressRoute), then the OSC.
struct Prefixed {
    uint32_t word;
    // payload follows
};

// The origin token the bridge's own frames carry on the OUT ring when they
// are broadcasts rather than replies (a reply carries the caller's token).
constexpr uint32_t BRIDGE_ORIGIN_TOKEN = 0x54425247;

// ── Names ──────────────────────────────────────────────────────────────────
// Short, and from the engine's pid. macOS caps a POSIX shm or sem name at
// 31 characters including the slash; these leave room for a 10-digit pid.

inline std::string segment_name(int32_t engine_pid) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "clockwork_bridge_%d", static_cast<int>(engine_pid));
    return buf;
}
inline std::string doorbell_name(int32_t engine_pid) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "/clockwork_bell_%d", static_cast<int>(engine_pid));
    return buf;
}

// ── The engine's side of the header ─────────────────────────────────────────

inline void init_header(Header* h, double sample_rate, uint32_t block_size,
                        uint32_t lane_base, int32_t engine_pid, uint32_t slack) {
    std::memset(static_cast<void*>(h), 0, sizeof *h);
    h->version           = VERSION;
    h->lanes             = LANES;
    h->audio_ring_frames = AUDIO_RING_FRAMES;
    h->ctl_ring_size     = CTL_RING_SIZE;
    h->rt_ring_size      = RT_RING_SIZE;
    h->out_ring_size     = OUT_RING_SIZE;
    h->mirror_size       = MIRROR_SIZE;
    h->sample_rate_bits.store(clockwork::doubleToBits(sample_rate), std::memory_order_relaxed);
    h->block_size.store(block_size, std::memory_order_relaxed);
    h->lane_base.store(lane_base, std::memory_order_relaxed);
    h->slack_blocks.store(slack, std::memory_order_relaxed);
    h->engine_pid.store(engine_pid, std::memory_order_relaxed);
    ClockworkClockState::initDefaults(h->clock);
    // Magic last: a bridge that maps the segment early sees "not ready".
    std::atomic_thread_fence(std::memory_order_release);
    h->magic = MAGIC;
}

// The clock mirror: a coherent copy under the arena's own protocol
// (ClockworkClockState::copyFrom, shared_memory.h).
inline void mirror_clock(Header* h, const ClockworkClockState* src) {
    if (src) h->clock.copyFrom(*src);
}

// ── The doorbell ────────────────────────────────────────────────────────────
//
// A named semaphore. `post` is the one thing here that runs on the engine's
// audio thread: a system call, but one that does not block — it is what a
// realtime thread is allowed to do to wake another. There is no timed wait
// because macOS has no sem_timedwait: a waiter that must be woken for a
// reason other than a block (shutdown) is woken by a post from whoever knows
// the reason.
class Doorbell {
public:
    Doorbell() = default;
    ~Doorbell() { close(); }
    Doorbell(const Doorbell&) = delete;
    Doorbell& operator=(const Doorbell&) = delete;

    // The engine creates; a leftover with the same name is removed first.
    bool create(const std::string& name) {
        close();
        mName = name;
#if defined(_WIN32)
        // CreateSemaphore adopts an existing object of the name rather than
        // failing. One can exist only while a process still holds it — a
        // bridge from a previous engine whose pid was reused, still on its
        // way out — and what it would carry over is a count of posts nobody
        // rendered. So an adopted semaphore is drained before it is used:
        // the name is kept (the bridge opens by name), the stale posts are
        // not.
        mSem = CreateSemaphoreW(nullptr, 0, 0x7fffffff, wide(name).c_str());
        if (!mSem) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS)
            while (WaitForSingleObject(mSem, 0) == WAIT_OBJECT_0) {}
        mOwner = true;
        return true;
#else
        ::sem_unlink(name.c_str());
        mSem = ::sem_open(name.c_str(), O_CREAT | O_EXCL, 0600, 0);
        if (mSem == SEM_FAILED) { mSem = nullptr; return false; }
        mOwner = true;
        return true;
#endif
    }

    // The bridge opens what the engine made.
    bool open(const std::string& name) {
        close();
        mName = name;
#if defined(_WIN32)
        mSem = OpenSemaphoreW(SYNCHRONIZE | SEMAPHORE_MODIFY_STATE, FALSE, wide(name).c_str());
        return mSem != nullptr;
#else
        mSem = ::sem_open(name.c_str(), 0);
        if (mSem == SEM_FAILED) { mSem = nullptr; return false; }
        return true;
#endif
    }

    bool valid() const { return mSem != nullptr; }

    // Audio thread. Never blocks.
    void post() {
        if (!mSem) return;
#if defined(_WIN32)
        ReleaseSemaphore(mSem, 1, nullptr);
#else
        ::sem_post(mSem);
#endif
    }

    // Blocks until posted. False if the semaphore is gone.
    bool wait() {
        if (!mSem) return false;
#if defined(_WIN32)
        return WaitForSingleObject(mSem, INFINITE) == WAIT_OBJECT_0;
#else
        int r;
        do { r = ::sem_wait(mSem); } while (r != 0 && errno == EINTR);
        return r == 0;
#endif
    }

    // Take a pending post without waiting. Used to coalesce a burst.
    bool try_wait() {
        if (!mSem) return false;
#if defined(_WIN32)
        return WaitForSingleObject(mSem, 0) == WAIT_OBJECT_0;
#else
        return ::sem_trywait(mSem) == 0;
#endif
    }

    void close() {
        if (!mSem) return;
#if defined(_WIN32)
        CloseHandle(mSem);
#else
        ::sem_close(mSem);
        if (mOwner) ::sem_unlink(mName.c_str());
#endif
        mSem = nullptr;
        mOwner = false;
    }

private:
#if defined(_WIN32)
    static std::wstring wide(const std::string& s) {
        // "Local\" scopes the name to this session, which is where the
        // engine and its bridge both are. The POSIX name's leading slash is
        // not part of a Windows object name.
        return clockwork_path::to_wide("Local\\" + s.substr(!s.empty() && s[0] == '/' ? 1 : 0));
    }
    HANDLE mSem = nullptr;
#else
    sem_t* mSem = nullptr;
#endif
    bool mOwner = false;
    std::string mName;
};

}  // namespace clockwork_bridge

#endif  // CLOCKWORK_PLUGIN_BRIDGE_H
