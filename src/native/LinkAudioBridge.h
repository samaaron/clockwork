// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * LinkAudioBridge.h — the audio side of Ableton Link.
 *
 * Owns the publish-side main sink and the user-added aux sinks, and the
 * receive-side per-channel input subscriptions. Clock sync (tempo, transport,
 * peers) is NOT here — that stays in ClockworkClock's LinkSession, whose
 * ableton::LinkAudio instance the bridge borrows by reference. LinkAudioHost
 * owns the bridge and is what the engine talks to.
 *
 * PEER AUDIO ARRIVES AS ORDINARY INPUT CHANNELS. Each subscription owns a
 * source port (clockwork_ports.h) attached to a channel pair (clockwork_port_bus.h); a
 * worker thread renders the peer's stream into it with clockwork_port_produce, and
 * pull_port_sources reads it into the DSP's input channels just before
 * dsp_process. That is the same route a WAV file takes, and clockwork_ports.h names
 * both cases in its own list of the plumbings it exists to replace.
 *
 * It used to write into the DSP's signal storage directly and stamp the bus's
 * freshness counter so the guest's input reads would accept it — scsynth's
 * protocol, in clockwork, through accessors the boundary narrowing removed. The
 * result was a bridge that could not link and a guest that had to be compiled
 * for peer audio to exist at all.
 *
 * PRODUCING AHEAD IS SAFE BECAUSE THE RENDERER WORKS IN BEATS. It converts a
 * host time to a beat range through the session state and reads the peer's
 * buffers, which carry beat ranges of their own. A tempo change moves
 * time-per-beat for everyone at once, so audio already queued stays where it
 * belongs on the timeline; there is no future timestamp to guess and nothing
 * to invalidate. See kLatencyInBeats in vendor/LinkAudioInputRenderer.hpp.
 *
 * Two compile shapes selected by CLOCKWORK_LINK_AUDIO:
 *   1 — the real bridge (this header declares; LinkAudioBridge.cpp defines).
 *   0 — an inline no-op bridge: every method returns empty/false, so a build
 *       without Link still constructs a LinkAudioHost. No separate TU.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The shapes the bridge answers in. Defined once here — both compile shapes
// below produce them — and re-exported by LinkAudioHost, which is the name
// callers use.
namespace link_audio {

struct Channel {
    std::string channelId;
    std::string channelName;
    std::string peerId;
    std::string peerName;
};

enum class ConnectionState { NotSubscribed = 0, Connecting = 1, Connected = 2, Dropout = 3 };

struct InputStatus {
    std::string     peerName;
    std::string     channelName;
    uint32_t        busIdx{0};
    uint32_t        sampleRate{0};
    // Source's actual channel count (1 or 2). 0 until the first buffer
    // arrives. A subscription always claims 2 input channels regardless.
    uint32_t        sourceNumChannels{0};
    float           bufferedSeconds{0.0f};
    ConnectionState state{ConnectionState::NotSubscribed};
    uint64_t        droppedSourceBuffers{0};   // our queue saturated
    uint64_t        networkGapBuffers{0};      // upstream loss (Info::count gaps)
    uint64_t        totalSourceBufferCalls{0}; // diagnostic: raw onSourceBuffer invocations
    uint64_t        duplicateCountCalls{0};    // diagnostic: invocations with repeated count
    double          latencySeconds{0.0};       // current playback lookahead
};

struct SinkInfo {
    std::string name;
    uint32_t    busIdx{0};
    uint32_t    numChannels{0};
    bool        hasSubscriber{false};
};

/*
 * Does a stereo peer stream starting at `busIdx` fit an engine `inChannels`
 * wide? Both busIdx and busIdx+1 have to be inside the width.
 *
 * `inChannels` is the width the DSP ALLOCATED — device inputs plus the lanes
 * reserved above them (clockwork_reserve_lanes) — not the device's own width.
 * A client subscribes into a reserved lane, which always sits above the
 * device's channels, so validating against the device refuses everything.
 *
 * Written as a subtraction rather than `busIdx + 1 >= inChannels` so a busIdx
 * of UINT32_MAX cannot wrap the addition into a pass.
 */
inline bool channelPairFits(uint32_t busIdx, uint32_t inChannels) {
    return busIdx < inChannels && (inChannels - busIdx) >= 2;
}

// Per-subscription lookahead cap: the renderer's ring capacity.
constexpr double kMaxInputLatencySeconds = 2.0;

} // namespace link_audio

// CLOCKWORK_LINK_AUDIO selects the audio half of Link: peer audio in, and a publish
// out. An explicit choice — the CLOCKWORK_LINK_AUDIO option in CMakeLists.txt, which
// defines CLOCKWORK_WITH_LINK_AUDIO — not one implied by anything else.
//
// IT DOES NOT DEPEND ON A DSP, and used to: the condition here was
// `CLOCKWORK_LINK && CLOCKWORK_SYNTH`, so peer audio existed only in a build that had
// compiled a guest. That is backwards. Link audio is clockwork moving frames
// between an endpoint and its channels, which is the same job whether the
// guest is scsynth, the dummy, or absent — and it is emphatically not something a
// guest enables.
//
// It is separate from CLOCKWORK_LINK because it pulls in the GPL renderer
// and a network endpoint, neither of which tempo sync needs. TEMPO, TRANSPORT
// AND PEERS DO NOT COME THROUGH HERE — they are the clock's LinkSession.
#if defined(CLOCKWORK_LINK) && defined(CLOCKWORK_WITH_LINK_AUDIO)
#define CLOCKWORK_LINK_AUDIO 1
#else
#define CLOCKWORK_LINK_AUDIO 0
#endif

#if CLOCKWORK_LINK_AUDIO

#include "native/vendor/LinkAudioInputRenderer.hpp"
#include "clockwork_port_bus.h"
#include "clockwork_ports.h"

#include <ableton/LinkAudio.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class LinkAudioBridge {
public:
    explicit LinkAudioBridge(ableton::LinkAudio& link) : mLink(link) {}
    /*
     * Stop the worker, then give the ports back.
     *
     * The worker is joined FIRST so nothing is rendering into a port while it
     * is being closed. Then the subscriptions go, which is what returns their
     * slots: the registry holds 64, they are reclaimed on close, and a bridge
     * that dropped its subscriptions without closing them leaked one per
     * subscription per engine. In a process that builds an engine per test
     * that is a slow exhaustion — the early tests pass, the later ones cannot
     * open a port at all, and the symptom is a subscription refused for no
     * visible reason.
     */
    ~LinkAudioBridge() {
        stopEndpoint();
        clearInputs();
    }

    LinkAudioBridge(const LinkAudioBridge&) = delete;
    LinkAudioBridge& operator=(const LinkAudioBridge&) = delete;

    // ─── Publish gate + channel discovery ────────────────────────────────
    // setPublishEnabled reflects immediately when linkEnabled (creates/destroys
    // the main sink + clears aux sinks); otherwise the next ensureMainSink (on
    // the next non-Off visibility transition) creates the sink.
    void setPublishEnabled(bool publish, bool linkEnabled);
    bool isPublishEnabled() const { return mAudioPublishEnabled; }
    std::vector<link_audio::Channel> listChannels() const;

    // ─── Input subscriptions ──────────────────────────────────────────────
    bool addInput(const char* peerName, const char* channelName, uint32_t busIdx);
    void removeInput(const char* peerName, const char* channelName);
    void clearInputs();
    bool setInputLatencySeconds(const char* peerName, const char* channelName,
                                double seconds);
    std::vector<link_audio::InputStatus> listInputs() const;

    // ─── Auxiliary sinks ──────────────────────────────────────────────────
    bool addSink(const char* name, uint32_t busIdx, uint32_t numChannels);
    void removeSink(const char* name);
    std::vector<link_audio::SinkInfo> listSinks() const;

    // ─── Endpoint: peer audio into a source port ──────────────────────────
    //
    // There is no RT-thread drain any more. Each subscription owns a source
    // port attached to its channel pair (clockwork_port_bus.h), a worker thread
    // fills it with clockwork_port_produce, and pull_port_sources reads it into the
    // DSP's input channels just before dsp_process — the same route disk
    // streaming takes. The audio thread is not involved in getting peer audio
    // here at all, and nothing reaches into the guest's storage.
    //
    // The rate is the endpoint's one dependency on the audio path: the
    // renderer resamples the peer's stream onto our timeline and needs to know
    // what "a frame" is. Set it before subscriptions arrive; the worker idles
    // until it has both a rate and something to fill.
    // `allocatedInputChannels` is the width the DSP allocated (device inputs
    // plus reserved lanes), NOT the device's width — see channelPairFits.
    void setAudioFormat(uint32_t sampleRate, uint32_t allocatedInputChannels);

    void publishAuxSinks(const float* busPool, uint32_t blockSize, uint32_t numBuses,
                         uint32_t sampleRate, uint64_t hostMicrosForBufferBegin,
                         double quantum);
    bool publishAudioBlock(const float* leftChannel, const float* rightChannel,
                           size_t numFrames, uint32_t sampleRate,
                           uint64_t hostMicrosForBufferBegin, double quantum);

    // ─── Visibility-change support (LinkAudioHost, on the clock's visibility protocol) ─
    // Clear all subs / aux sinks / the main sink — they bind to the current
    // Link substrate's session-scoped channelIds, so they must drop before the
    // Link gateway tears down and be re-added after the next non-Off transition.
    void resetForVisibilityChange();
    // Recreate the main sink if publishing is enabled (called after Link comes
    // back up). No-op when publish is off.
    void ensureMainSink();

    // ─── Metrics (read by LinkAudioHost::publishMetrics) ─────────────────
    uint32_t underruns() const {
        return mLinkAudioUnderruns.load(std::memory_order_relaxed);
    }
    // RT-safe try_lock reads of the aux-sink count and aggregated input health.
    // Each returns false (writing nothing) when the lock is contended.
    bool tryReadSinkCount(uint32_t& outCount) const;
    struct InputHealth {
        uint32_t inChannels{0};
        uint32_t streamRate{0};
        int32_t  driftPpm{0};
        uint32_t bufferedMs{0};
    };
    bool tryReadInputHealth(InputHealth& out) const;

private:
    // Up to 2048-frame stereo blocks; the DSP normally runs at 128.
    static constexpr size_t kSinkMaxSamples = 4096;

    // Audio-channel name for the main sink (Live displays one row per channel
    // under each peer). "Main" matches Live's convention for its primary output.
    std::string mChannelNameCache{"Main"};

    ableton::LinkAudio& mLink;

    std::optional<ableton::LinkAudioSink> mSink;
    // Serialises sink reset/emplace against the audio thread's publishAudioBlock
    // dereference (audio thread uses try_lock).
    mutable std::mutex mSinkMutex;

    // Audio-publish gate. False = don't create the main sink even if the Link
    // mesh is up. App must explicitly setPublishEnabled(true).
    bool mAudioPublishEnabled{false};

    // One subscription per Link channel. Multiple are active concurrently;
    // (peerName, channelName) is the replacement key. Each owns one channel
    // pair, and one source port bound to it.
    struct InputSubscription {
        std::unique_ptr<clockwork_link::LinkAudioInputRenderer<ableton::LinkAudio>> renderer;
        uint32_t    busIdx{0};
        std::string peerName;
        std::string channelName;
        // The port this subscription's audio arrives through, and the slot the
        // bus binding is keyed on. CLOCKWORK_PORT_NONE until openPortFor succeeds;
        // a subscription without one is inert rather than an error, because a
        // full port registry is a resource limit and not a bad request.
        ClockworkPort port{CLOCKWORK_PORT_NONE};
        // Distinguishes a re-arm of the same channel from a peer rejoin that
        // publishes a new id. Same-id replacements reuse the existing renderer
        // so diagnostic counters survive.
        ableton::ChannelId channelId{};
    };
    std::vector<InputSubscription> mInputSubs;
    // Lock-free fast path for the empty case.
    std::atomic<size_t> mInputSubCount{0};
    // Serialises mInputSubs mutation against the endpoint worker and the
    // app-thread listInputs. No audio thread takes this any more.
    mutable std::mutex mInputSubMutex;

    // ─── The endpoint ─────────────────────────────────────────────────────
    //
    // One worker for every subscription: it asks each port how much room there
    // is, renders that much, and offers it. Nothing here waits on the audio
    // thread and the audio thread never waits on this — if the worker is late
    // the ring drains, the audio thread reads silence, and the port's own
    // underrun counter says by how much. Same arrangement as the disk reader
    // in rust/clockwork-ports-disk.
    void startEndpoint();
    void stopEndpoint();
    void endpointLoop();
    // Detach then close. Call with mInputSubMutex held.
    void closePort(InputSubscription& sub);

    // Opened per subscription; closed when it goes. Depth is scheduling slack
    // for THIS worker, not the renderer's musical latency — that is in beats
    // and lives inside the renderer.
    static constexpr uint32_t kInputPortCapacityFrames = 8192;   // ~170ms @48k
    // Rendered per pass. Bounded so one subscription cannot hold the lock for
    // an unbounded stretch while others wait.
    static constexpr uint32_t kEndpointChunkFrames = 512;

    std::thread             mEndpointThread;
    std::atomic<bool>       mEndpointRun{false};
    std::atomic<uint32_t>   mSampleRate{0};
    // The engine's input channel count: the ceiling a subscription's
    // channel pair has to fit under. Streams come and go, so this is
    // checked per add rather than assumed once.
    std::atomic<uint32_t>   mInputChannels{0};

    // Worker-owned scratch: two planar doubles for the renderer, one
    // interleaved float for the port.
    double mScratchL[kEndpointChunkFrames]{};
    double mScratchR[kEndpointChunkFrames]{};
    float  mScratchInterleaved[kEndpointChunkFrames * 2]{};

    // Auxiliary sinks bound to user-chosen bus ranges. Mutex covers the vector
    // and the per-entry hasSubscriber flag; audio thread uses try_lock and
    // skips the block on contention.
    struct ActiveSink {
        std::string            name;
        uint32_t               busIdx;
        uint32_t               numChannels;
        ableton::LinkAudioSink sink;
        bool                   hasSubscriber{false};
    };
    mutable std::mutex      mAuxSinksMutex;
    std::vector<ActiveSink> mAuxSinks;
    // Lock-free fast path for the empty case.
    std::atomic<size_t>     mAuxSinkCount{0};

    // Audio-thread scratch for drainInputsToBuses. Held on the bridge to avoid
    // 64 KiB of stack per call (some Windows ASIO threads have <128 KiB stacks).
    // Single-thread access.
    static constexpr size_t kDrainScratchFrames = 4096;
    double mDrainScratchL[kDrainScratchFrames]{};
    double mDrainScratchR[kDrainScratchFrames]{};

    // Cumulative Link Audio receive underruns (a block the renderer couldn't
    // fully fill). Bumped in drainInputsToBuses, mirrored to metrics.
    std::atomic<uint32_t> mLinkAudioUnderruns{0};
};

#else  // !CLOCKWORK_LINK_AUDIO

// No-op bridge: a build without Link Audio constructs this and every Link
// Audio call inertly returns empty/false. Header-only — no separate TU. The
// publish-gate flag is tracked so LinkAudioHost keeps its SAB-parity
// behaviour, but no sink/renderer machinery exists.
class LinkAudioBridge {
public:
    LinkAudioBridge() = default;

    LinkAudioBridge(const LinkAudioBridge&) = delete;
    LinkAudioBridge& operator=(const LinkAudioBridge&) = delete;

    void setPublishEnabled(bool publish, bool) { mAudioPublishEnabled = publish; }
    bool isPublishEnabled() const { return mAudioPublishEnabled; }
    std::vector<link_audio::Channel> listChannels() const { return {}; }

    bool addInput(const char*, const char*, uint32_t) { return false; }
    void removeInput(const char*, const char*) {}
    void clearInputs() {}
    bool setInputLatencySeconds(const char*, const char*, double) { return false; }
    std::vector<link_audio::InputStatus> listInputs() const { return {}; }

    bool addSink(const char*, uint32_t, uint32_t) { return false; }
    void removeSink(const char*) {}
    std::vector<link_audio::SinkInfo> listSinks() const { return {}; }

    void setAudioFormat(uint32_t, uint32_t) {}
    void publishAuxSinks(const float*, uint32_t, uint32_t, uint32_t, uint64_t, double) {}
    bool publishAudioBlock(const float*, const float*, size_t, uint32_t, uint64_t,
                           double) { return false; }

    void resetForVisibilityChange() {}
    void ensureMainSink() {}

    uint32_t underruns() const { return 0; }
    bool tryReadSinkCount(uint32_t&) const { return false; }
    struct InputHealth {
        uint32_t inChannels{0};
        uint32_t streamRate{0};
        int32_t  driftPpm{0};
        uint32_t bufferedMs{0};
    };
    bool tryReadInputHealth(InputHealth&) const { return false; }

private:
    bool mAudioPublishEnabled{false};
};

#endif  // CLOCKWORK_LINK_AUDIO
