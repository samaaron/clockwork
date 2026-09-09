// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * LinkSession.cpp — Ableton Link clock-sync session (real impl).
 *
 * Compiled only when CLOCKWORK_LINK. Owns the ableton::LinkAudio instance, the
 * network-thread priority, the advertised peer name, the loopback-only interface
 * filter, and the deferred enable/disable worker thread. Session mutators and
 * Link's tempo/transport callbacks mirror the converged values into the
 * ClockworkClockState SAB region owned by the borrowed ClockworkClock&. The Link Audio
 * bridge borrows the instance via linkAudio(); visibility orchestration
 * (sink/sub teardown ordering) is composed by ClockworkClock::setLinkVisibility.
 */
#include "clockwork_product.h"
#include "clock/LinkSession.h"
#include "clock/clock_math.h"

#if CLOCKWORK_LINK

#include "shared_memory.h"

extern "C" const char* clockwork_app_name();

// LinkAudio.hpp must be used INSTEAD of Link.hpp ("LinkAudio and Link should not
// be used simultaneously" per Ableton's docs).
#include <ableton/LinkAudio.hpp>

// Loopback-only interface filtering. The flag is not in upstream Link: it comes
// from a patched ScanIpIfAddrs.hpp, and no such patch is carried here (CMake
// fetches Link-4.0 unmodified), so this TU compiles only against a Link patched
// to export it. setLoopbackOnly toggles it and Link's InterfaceScanner constrains
// discovery/multicast/unicast to loopback on its next scan. Same flag on both
// platforms — keep the include and the accessor switched in lockstep.
#if defined(_WIN32)
#include <ableton/platforms/windows/ScanIpIfAddrs.hpp>
#else
#include <ableton/platforms/posix/ScanIpIfAddrs.hpp>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

using clockwork::doubleToBits;

namespace {

inline std::atomic<bool>& linkLoopbackOnlyFlag() {
#if defined(_WIN32)
    return ableton::platforms::windows::loopbackOnly();
#else
    return ableton::platforms::posix::loopbackOnly();
#endif
}

// Render an 8-byte Link NodeId/PeerId/ChannelId as 16 lowercase hex.
template <typename Bytes>
inline std::string bytesToHex16(const Bytes& bytes) {
    char buf[17];
    for (size_t k = 0; k < bytes.size(); ++k) {
        std::snprintf(buf + 2 * k, 3, "%02x", bytes[k]);
    }
    return std::string(buf, 16);
}

}  // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct LinkSession::Impl {
    ClockworkClock& clock;

    // Off RT thread, ~every 250 ms — drives MIDI follower staleness. Wired by
    // ClockworkClock so the session never references MIDI.
    std::function<void()> periodicTick;

    // The deferred worker resolves an async enable/disable into a visibility
    // transition through ClockworkClock::setLinkVisibility (which orders Link Audio
    // sink/sub teardown). Set after construction.
    std::function<void(LinkVisibility)> applyVisibility;

    // Link starts disabled; setLinkVisibility(non-Off) flips peer discovery on.
    // While disabled the instance still hosts session state — we read
    // tempo/transport from it even with no peers.
    ableton::LinkAudio link{clockwork::kDefaultBpm, CLOCKWORK_PRODUCT_NAME};

    // RT priority for Link's network thread. Without it the receiver hears
    // periodic packet-burst pulsing when the OS preempts the thread (mirrors
    // Ableton's linkaudiohut example).
    ableton::link::platform::ThreadPriority threadPriority{};

    // Backing storage for the peer name we advertise. Link::setName takes by
    // value; we keep the C-string stable for peerName(). Defaults to the
    // published app name (--app-name); /clockwork/clock/peer_name/set overrides.
    std::string peerNameCache{clockwork_app_name()};

    // Last non-Off visibility, restored by setLinkEnabled(false→true). Defaults
    // to LoopbackOnly: bare enable on a fresh ClockworkClock stays loopback-only
    // until the caller explicitly opts into NetworkWide via setLinkVisibility.
    LinkVisibility lastNonOffVisibility{LinkVisibility::LoopbackOnly};

    // Deferred Link enable/disable for audio-thread callers.
    // -1 = no request, 0 = disable, 1 = enable.
    std::thread             deferredWorker;
    std::mutex              deferredMtx;
    std::condition_variable deferredCv;
    std::atomic<int>        deferredLinkEnableReq{-1};
    std::atomic<bool>       deferredQuit{false};

    // App tempo-changed callback (→ /clockwork/clock/notify/tempo broadcast). Set once at
    // boot; invoked for BOTH remote peer changes (via Link's setTempoCallback)
    // and local sets (via setBpm) so a client's cached tempo never goes stale.
    std::function<void(double)> appTempoCb;

    // Mirror a new session tempo into the SAB, re-anchor beat_origin so mirror-
    // domain beatAtTime stays coherent with Link-domain beatAtTime under the new
    // tempo, then fire the app callback. Runs on Link's network thread (remote)
    // or the caller thread (local setBpm) — both non-RT, both use the app-safe
    // captureAppSessionState.
    void applyTempoChange(double bpmIn) {
        double bpm = bpmIn;
        if (!(bpm >= 1.0)) bpm = 1.0;
        auto st = link.captureAppSessionState();
        const auto now = link.clock().micros();
        constexpr double kBeatOriginQuantum = 4.0;
        const double currentBeat = st.beatAtTime(now, kBeatOriginQuantum);
        // Mirrored from Link's own beat, not re-derived from the mirror: the
        // grid Link converged on is the truth here.
        if (ClockworkClockState* s = clock.state())
            s->setTempo(bpm, clockwork::originFor(currentBeat, wallClockNTP(), bpm));
        if (appTempoCb) appTempoCb(bpm);
    }

    explicit Impl(ClockworkClock& c, std::function<void()> tick)
        : clock(c), periodicTick(std::move(tick)) {}
};

// ─── ctor/dtor ─────────────────────────────────────────────────────────────

LinkSession::LinkSession(ClockworkClock& clock, std::function<void()> periodicTick)
    : mImpl(std::make_unique<Impl>(clock, std::move(periodicTick))) {}

void LinkSession::startWorker() {
    auto* impl = mImpl.get();
    // Guard against a double-start: the worker owns a std::thread and assigning
    // over a joinable one calls std::terminate. ClockworkClock starts it exactly once,
    // after its mImpl is live (see the header note on the construction-order race).
    if (impl->deferredWorker.joinable()) return;
    impl->deferredWorker = std::thread([impl] {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(impl->deferredMtx);
                // Wake on a request/quit, or every ~250 ms to sweep midi
                // timeline staleness (freeze stale tempo, free after grace).
                impl->deferredCv.wait_for(lk, std::chrono::milliseconds(250), [impl] {
                    return impl->deferredQuit.load(std::memory_order_acquire)
                        || impl->deferredLinkEnableReq.load(
                               std::memory_order_acquire) != -1;
                });
                if (impl->deferredQuit.load(std::memory_order_acquire)) return;
                const int req = impl->deferredLinkEnableReq.exchange(
                    -1, std::memory_order_acq_rel);
                lk.unlock();
                // Resolve enable/disable into a visibility transition, matching
                // ClockworkClock::setLinkEnabled: enable from Off restores the last
                // non-Off visibility; disable goes to Off; enable while already
                // enabled is a no-op (applyVisibility short-circuits same-state).
                if (req == 1) {
                    if (impl->applyVisibility &&
                        !impl->link.isEnabled()) {
                        impl->applyVisibility(impl->lastNonOffVisibility);
                    }
                } else if (req == 0) {
                    if (impl->applyVisibility) impl->applyVisibility(LinkVisibility::Off);
                }
            }
            if (impl->periodicTick) impl->periodicTick();
        }
    });
}

LinkSession::~LinkSession() {
    stopWorker();
}

void LinkSession::stopWorker() {
    // Set the quit flag under the same mutex the worker waits on: a notify_all()
    // issued in the window between the worker's predicate check and its park in
    // wait() would otherwise be lost, leaving the worker asleep forever and this
    // join() hung. Idempotent: once joined, joinable() is false and re-entry is
    // a cheap flag-set + notify with nothing to join.
    {
        std::lock_guard<std::mutex> lk(mImpl->deferredMtx);
        mImpl->deferredQuit.store(true, std::memory_order_release);
    }
    mImpl->deferredCv.notify_all();
    if (mImpl->deferredWorker.joinable()) mImpl->deferredWorker.join();
}

// ─── Session mutators (mirror into the SAB) ──────────────────────────────────

void LinkSession::setBpm(double bpm) {
    auto st = mImpl->link.captureAppSessionState();
    st.setTempo(bpm, mImpl->link.clock().micros());
    mImpl->link.commitAppSessionState(st);
    // Mirror the input synchronously so getBpm() reflects it immediately. Link's
    // tempo callback (applyTempoChange) then re-syncs the mirror to Link's
    // converged value and re-anchors beat_origin. commitAppSessionState posts
    // async session-timing work on EVERY tempo change — local commits included,
    // not just remote peers — so that callback also delivers the
    // /clockwork/clock/notify/tempo broadcast for this local set. Firing appTempoCb here
    // as well would double-notify (and double re-anchor) on every local set.
    // RE-ANCHOR WITH IT (ClockworkClockState::retempo): this write is synchronous
    // while the callback's re-anchor is not, so without it every local tempo
    // change opened a window where the mirror reported a jumped beat.
    if (ClockworkClockState* s = mImpl->clock.state()) s->retempo(bpm, wallClockNTP());
}

void LinkSession::setIsPlaying(bool playing, double atNtpSeconds) {
    auto st = mImpl->link.captureAppSessionState();
    st.setIsPlaying(playing, mImpl->link.clock().micros());
    mImpl->link.commitAppSessionState(st);
    if (ClockworkClockState* s = mImpl->clock.state()) s->setTransport(playing, atNtpSeconds);
}

void LinkSession::setStartStopSyncEnabled(bool enabled) {
    mImpl->link.enableStartStopSync(enabled);
    if (ClockworkClockState* s = mImpl->clock.state()) s->setFlag(SC_FLAG_START_STOP_SYNC, enabled);
}

void LinkSession::requestBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    auto st = mImpl->link.captureAppSessionState();
    st.requestBeatAtTime(beat, mImpl->link.clock().micros(), quantum);
    double tempo = st.tempo();
    if (!(tempo >= 1.0)) tempo = 1.0;
    mImpl->link.commitAppSessionState(st);
    // Mirror the implied NTP beat origin so snapshot's beatAtTime math works
    // against NTP (Link stores in mClock domain, not NTP).
    if (ClockworkClockState* s = mImpl->clock.state())
        s->setOrigin(clockwork::originFor(beat, atNtpSeconds, tempo));
}

void LinkSession::forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    auto st = mImpl->link.captureAppSessionState();
    st.forceBeatAtTime(beat, mImpl->link.clock().micros(), quantum);
    double tempo = st.tempo();
    if (!(tempo >= 1.0)) tempo = 1.0;
    mImpl->link.commitAppSessionState(st);
    if (ClockworkClockState* s = mImpl->clock.state())
        s->setOrigin(clockwork::originFor(beat, atNtpSeconds, tempo));
}

// ─── Enable / async enable ───────────────────────────────────────────────────

void LinkSession::setApplyVisibility(std::function<void(LinkVisibility)> apply) {
    mImpl->applyVisibility = std::move(apply);
}

void LinkSession::requestSetLinkEnabledAsync(bool enabled) {
    // Under the worker's mutex (see dtor) so the request can't be lost to a
    // notify that races the worker's park in wait().
    {
        std::lock_guard<std::mutex> lk(mImpl->deferredMtx);
        mImpl->deferredLinkEnableReq.store(enabled ? 1 : 0,
                                           std::memory_order_release);
    }
    mImpl->deferredCv.notify_all();
}

LinkSession::LinkVisibility LinkSession::lastNonOffVisibility() const {
    return mImpl->lastNonOffVisibility;
}

void LinkSession::setLastNonOffVisibility(LinkVisibility v) {
    mImpl->lastNonOffVisibility = v;
}

// ─── Visibility primitives ───────────────────────────────────────────────────

void LinkSession::prepareDisable() {
    mImpl->link.enableLinkAudio(false);
    mImpl->link.enable(false);
    auto* impl = mImpl.get();
    mImpl->link.callOnLinkThread([impl]() { impl->threadPriority.reset(); });
}

void LinkSession::enableWithPriority() {
    auto* impl = mImpl.get();
    mImpl->link.enable(true);
    mImpl->link.callOnLinkThread([impl]() { impl->threadPriority.setHigh(); });
    mImpl->link.enableLinkAudio(true);
}

void LinkSession::setLoopbackOnly(bool loopbackOnly) {
    linkLoopbackOnlyFlag().store(loopbackOnly, std::memory_order_relaxed);
}

bool LinkSession::isEnabled() const {
    return mImpl->link.isEnabled();
}

LinkSession::LinkVisibility LinkSession::getVisibility() const {
    if (!mImpl->link.isEnabled()) return LinkVisibility::Off;
    return linkLoopbackOnlyFlag().load(std::memory_order_relaxed)
        ? LinkVisibility::LoopbackOnly
        : LinkVisibility::NetworkWide;
}

// ─── Status ──────────────────────────────────────────────────────────────────

bool LinkSession::isStartStopSyncEnabled() const {
    return mImpl->link.isStartStopSyncEnabled();
}

size_t LinkSession::numPeers() const {
    return mImpl->link.numPeers();
}

std::vector<LinkSession::PeerInfo> LinkSession::listPeers() const {
    std::vector<PeerInfo> out;
    if (!mImpl->link.isEnabled()) return out;
    auto peers = mImpl->link.sessionPeers();
    out.reserve(peers.size());
    for (auto& p : peers) {
        const auto& st = p.first;
        const auto& gw = p.second;
        PeerInfo info;
        info.nodeId          = bytesToHex16(st.ident());
        info.gatewayIp       = gw.to_string();
        info.isLoopback      = gw.is_loopback();
        info.measurementIp   = st.measurementEndpoint.address().to_string();
        info.measurementPort = st.measurementEndpoint.port();
        if (st.audioEndpoint) {
            info.audioIp   = st.audioEndpoint->address().to_string();
            info.audioPort = st.audioEndpoint->port();
        }
        out.push_back(std::move(info));
    }
    return out;
}

void LinkSession::publishLinkClockMetrics(PerformanceMetrics* m, double quantum) const {
    if (!m) return;
    // One realtime-safe, lock-free capture, read coherently for every Link field
    // below. This MUST use captureAudioSessionState() (the audio-thread variant):
    // captureAppSessionState() takes a lock and must never run on the audio
    // thread. Beat/phase are queried in Link's own clock domain at "now",
    // mirroring drainLinkAudioInputsToBuses / the tempo-changed callback.
    const auto st = mImpl->link.captureAudioSessionState();
    const auto nowMicros = mImpl->link.clock().micros();

    m->link_peers.store(static_cast<uint32_t>(mImpl->link.numPeers()),
                        std::memory_order_relaxed);
    const double bpm = st.tempo();
    m->link_tempo_mbpm.store(bpm > 0.0 ? static_cast<uint32_t>(bpm * 1000.0 + 0.5) : 0u,
                             std::memory_order_relaxed);
    const double beat = st.beatAtTime(nowMicros, quantum);
    m->link_beat_centi.store(beat > 0.0 ? static_cast<uint32_t>(beat * 100.0) : 0u,
                             std::memory_order_relaxed);
    const double phase = st.phaseAtTime(nowMicros, quantum);
    m->link_phase_centi.store(phase > 0.0 ? static_cast<uint32_t>(phase * 100.0) : 0u,
                              std::memory_order_relaxed);
    m->link_playing.store(st.isPlaying() ? 1u : 0u, std::memory_order_relaxed);
}

// ─── Peer name ─────────────────────────────────────────────────────────────

void LinkSession::setPeerName(const char* name) {
    std::string requested = name ? name : "";
    if (mImpl->peerNameCache == requested) return;
    mImpl->peerNameCache = std::move(requested);
    mImpl->link.setPeerName(mImpl->peerNameCache);
}

const char* LinkSession::peerName() const {
    return mImpl->peerNameCache.c_str();
}

// ─── Event callbacks (fired on Link's network thread; mirror into SAB) ───────

void LinkSession::anchorToWallClockIfUnset() {
    ClockworkClockState* s = mImpl->clock.state();
    if (!s) return;
    if (clockwork::bitsToDouble(s->beat_origin_ntp.load(std::memory_order_relaxed)) != 0.0)
        return;
    const double now = wallClockNTP();
    s->setOrigin(now);
    s->setTransport(false, now);
}

void LinkSession::setTempoChangedCallback(std::function<void(double)> cb) {
    // One shared path for tempo changes (applyTempoChange): mirror into the SAB,
    // re-anchor beat_origin, then fire cb. Link invokes it for remote peers and
    // for the local commit setBpm makes (see setBpm). cb may be empty (shutdown
    // clears it) — applyTempoChange null-checks it, and clamps bpm so a corrupt
    // peer's NaN can't leak out.
    auto* impl = mImpl.get();
    impl->appTempoCb = std::move(cb);
    impl->link.setTempoCallback(
        [impl](const double bpmIn) { impl->applyTempoChange(bpmIn); });
}

void LinkSession::setNumPeersChangedCallback(std::function<void(std::size_t)> cb) {
    mImpl->link.setNumPeersCallback(
        [cb = std::move(cb)](const std::size_t n) { if (cb) cb(n); });
}

void LinkSession::setStartStopChangedCallback(std::function<void(bool, double)> cb) {
    auto* impl = mImpl.get();
    mImpl->link.setStartStopCallback(
        [cb = std::move(cb), impl](const bool playing) {
            // Link reports the transition on its own clock; place it on the
            // wall clock by the offset between the two, sampled right now on
            // this thread. The state and the broadcast then carry one time.
            const int64_t atLink = impl->link.captureAppSessionState()
                                             .timeForIsPlaying().count();
            const int64_t nowLink = impl->link.clock().micros().count();
            const double atNtp = wallClockNTP() + static_cast<double>(atLink - nowLink) * 1e-6;
            if (ClockworkClockState* s = impl->clock.state()) s->setTransport(playing, atNtp);
            if (cb) cb(playing, atNtp);
        });
}

// ─── Audio-thread accessors ──────────────────────────────────────────────────

ableton::LinkAudio& LinkSession::linkAudio() {
    return mImpl->link;
}


int64_t LinkSession::linkClockMicrosRaw() const {
    return mImpl->link.clock().micros().count();
}

#endif  // CLOCKWORK_LINK
