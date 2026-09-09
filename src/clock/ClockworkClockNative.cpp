// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
 * ClockworkClockNative.cpp — the universal ClockworkClock composition root.
 *
 * Owns the ClockworkClockState atomics as a private Impl member and composes the
 * three axes: TimeSource (audio-thread NTP — native WallClock+IIR, or the
 * self-driven worklet clock on WASM/ESP32), MidiTimelines (MIDI follower
 * registry), and LinkSession (the Ableton Link clock-sync session, or a
 * thread-free session-of-one when Link is not compiled). This file keeps the
 * SAB binding, the public delegations, and the one orchestration that spans
 * the session and whoever rides on it (setLinkVisibility → the
 * LinkVisibilityListener, which is LinkAudioHost in the engine).
 *
 * Every target builds this same composition root: native (all four cells of
 * SYNTH×LINK), the freestanding guard, WASM, and ESP32. On the lean targets the
 * LinkSession header supplies the inline session-of-one, and the TimeSource
 * is the self-driven worklet clock. clockwork_clock_wasm_init (below, on
 * worklet builds) binds the SAB region + the worklet clock's offset pointers.
 *
 * State coherence: single-atomic-per-field, with two ordered write pairs —
 * beat_origin_ntp+bpm and is_playing_at_ntp+is_playing (shared_memory.h
 * documents both). The audio thread reads tempo + isPlaying from Link's
 * captureAudioSessionState (already coherent) and beat_origin from an
 * individual atomic. is_playing + is_playing_at_ntp are written as
 * timestamp-then-flag(release) and read as flag(acquire)-then-timestamp, so
 * a reader that sees a new flag sees the matching timestamp.
 */
#include "clock/ClockworkClock.h"
#include "clock/clock_math.h"
#include "clock/LinkSession.h"
#include "clock/MidiTimelines.h"
#include "clock/TimeSource.h"
#include "shared_memory.h"

#include <cmath>

#if CLOCKWORK_WORKLET_CLOCK
// Resolves to the real emscripten header on WASM and to the NativeShim stub on
// ESP32 (EMSCRIPTEN_KEEPALIVE → nothing there), so clockwork_clock_wasm_init keeps
// the WASM export attribute without breaking the embedded build.
#include <emscripten/emscripten.h>
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─── Impl ────────────────────────────────────────────────────────────────────

struct ClockworkClock::Impl {
    ClockworkClockState ownedState;
    // Where the clock state actually lives: the private ownedState until the
    // engine binds it into the shared arena (bindStateToShm), after which it's
    // the SHM CLOCK_STATE region — same shape as web.
    ClockworkClockState* boundState{&ownedState};

    // ─── Audio-thread time-source (native WallClock + IIR) ───────────────────
    // The audio-thread NTP clock: one IIR step per callback, freewheel bypass
    // for deterministic offline rendering. The ClockworkClock now()/nowAt()/
    // updateAudioThreadNTP()/wallNow()/resetAudioThreadTime()/setFreewheelClock()
    // methods are thin delegations to it.
    TimeSource timeSource;

    // ─── MIDI follower timelines (in-process registry) ───────────────────────
    // Fixed K-slot registry of midi:<port> timelines, separate from the Link
    // (ClockworkClockState) timeline. Constructed with the owning ClockworkClock& so its
    // beat math reads the same clock core; the ClockworkClock::timeline*/
    // *MidiTimeline* methods are thin delegations to it.
    MidiTimelines midiTimelines;

    // ─── Link clock-sync session ─────────────────────────────────────────────
    // The Ableton Link session (cross-machine tempo/transport/peers) when Link is
    // compiled; a thread-free session-of-one otherwise. The session-mutator and
    // event-callback methods are thin delegations to it. Constructed with the
    // owning ClockworkClock& (for the SAB mirror) and the MIDI-staleness tick the
    // session's worker drives off the RT thread.
    LinkSession linkSession;

    // Whoever rides on the session's substrate (LinkAudioHost): told around
    // every visibility transition. Null until set; plain pointer, app-thread
    // set before any transition, cleared by the listener's own destructor.
    LinkVisibilityListener* visibilityListener{nullptr};

    explicit Impl(ClockworkClock& clock)
        : midiTimelines(clock),
          linkSession(clock, [this] { midiTimelines.tickMidiStaleness(); }) {
        ClockworkClockState::initDefaults(ownedState);
    }
};

// ─── ctor/dtor + state accessor ───────────────────────────────────────────

ClockworkClock::ClockworkClock() : mImpl(std::make_unique<Impl>(*this)) {
    // The session's deferred worker resolves an async enable/disable into a
    // visibility transition; route it back through setLinkVisibility so Link
    // Audio sinks / subs / thread priority all tear down and bring up together.
    mImpl->linkSession.setApplyVisibility(
        [this](LinkVisibility v) { setLinkVisibility(v); });
    // Start the session worker only now that mImpl is assigned. The worker's
    // periodic tick calls back through this ClockworkClock (tickMidiStaleness → mImpl),
    // so spawning it inside the LinkSession member ctor — while make_unique is
    // still writing mImpl — is a data race on mImpl with no happens-before edge.
    mImpl->linkSession.startWorker();
}

ClockworkClock::~ClockworkClock() {
    // Join the session worker before any Impl member it reaches is destroyed.
    // Member reverse-destruction would expose midiTimelines while linkSession —
    // and its still-live worker — has not yet been reached, so the worker could
    // run applyVisibility/the staleness tick against a half-destroyed Impl.
    // Stopping it first closes that window.
    mImpl->linkSession.stopWorker();
}

void ClockworkClock::stopBackgroundWork() {
    mImpl->linkSession.stopWorker();
}

ClockworkClockState*       ClockworkClock::state()       { return mImpl->boundState; }
const ClockworkClockState* ClockworkClock::state() const { return mImpl->boundState; }

// Move the clock state into the shared arena region so the native SHM has the
// same shape as web. Copies the current (pre-bind) state across, then repoints.
// Called once at engine init, before the audio thread starts — no concurrency.
void ClockworkClock::bindStateToShm(ClockworkClockState* region) {
    if (!region || region == mImpl->boundState) return;
    region->copyFrom(*mImpl->boundState);
    mImpl->boundState = region;

    // Binding into the arena is the moment this clock starts answering real
    // clients, so it is the moment it needs a real epoch. No-op on a Link
    // build (Link owns the origin) and no-op if one is already set — see
    // LinkSession::anchorToWallClockIfUnset.
    mImpl->linkSession.anchorToWallClockIfUnset();
}

// Point the clock back at its own state before the arena goes away.
//
// shutdown() frees the SHM arena (mShmemCreator.reset), and shutdown() is
// reachable twice — the caller's, then the destructor's. Without this the
// second pass reads the region after it was freed, which the Link-disabled
// build does on every teardown: its LinkSession stub answers getVisibility()
// out of the clock state, where the real one asks Link. Copying the values
// back first means the clock still reports what it last knew.
void ClockworkClock::unbindStateFromShm() {
    ClockworkClockState* owned = &mImpl->ownedState;
    if (mImpl->boundState == owned) return;
    owned->copyFrom(*mImpl->boundState);
    mImpl->boundState = owned;
}

#if CLOCKWORK_WORKLET_CLOCK

// ─── Worklet (WASM/ESP32) boot binding ──────────────────────────────────────
// The worklet clock evaluates the SAB time formula from these offset pointers.
void ClockworkClock::bindWorkletClock(const double* ntpStartTime,
                                  const std::atomic<int32_t>* driftOffset,
                                  const std::atomic<int32_t>* globalOffset) {
    mImpl->timeSource.bindOffsets(ntpStartTime, driftOffset, globalOffset);
}

extern "C" {

// The WASM host's boot hook. init_memory publishes the composition-root instance
// to g_active_clockwork_clock, then calls this with the SAB CLOCK_STATE region
// and the three SAB offset pointers. We bind both onto that instance: the state
// (so ClockworkClock::state() reads/writes the SAB) and the worklet clock offsets
// (so nowAt() evaluates the SAB time formula). Signature is fixed — kept stable
// across the ClockworkClock decomposition so the WASM/JS side needs no change.
EMSCRIPTEN_KEEPALIVE
void clockwork_clock_wasm_init(ClockworkClockState* clockwork_clock_state,
                          const double* ntp_start_time_ptr,
                          const std::atomic<int32_t>* drift_offset_ptr,
                          const std::atomic<int32_t>* global_offset_ptr) {
    ClockworkClock* clock = g_active_clockwork_clock.load(std::memory_order_acquire);
    if (!clock) return;
    clock->bindStateToShm(clockwork_clock_state);
    clock->bindWorkletClock(ntp_start_time_ptr, drift_offset_ptr, global_offset_ptr);
}

}  // extern "C"

#endif  // CLOCKWORK_WORKLET_CLOCK

// ─── MIDI follower timelines (native) ──────────────────────────────────────
// The registry lives in MidiTimelines (constructed with this ClockworkClock as its
// clock core). These are thin delegations; id 0 = Link is handled inside.

int ClockworkClock::claimMidiTimeline(const char* normalized, const char* raw) {
    return mImpl->midiTimelines.claimMidiTimeline(normalized, raw);
}

void ClockworkClock::freeMidiTimeline(int id) {
    mImpl->midiTimelines.freeMidiTimeline(id);
}

int ClockworkClock::resolveTimeline(const char* name) const {
    return mImpl->midiTimelines.resolveTimeline(name);
}

int ClockworkClock::resolveOrClaimTimeline(const char* name) {
    return mImpl->midiTimelines.resolveOrClaimTimeline(name);
}

void ClockworkClock::setMidiTimelineTempo(int id, double bpm) {
    mImpl->midiTimelines.setMidiTimelineTempo(id, bpm);
}

void ClockworkClock::midiTimelinePulse(int id, uint64_t tsUs) {
    mImpl->midiTimelines.midiTimelinePulse(id, tsUs);
}

void ClockworkClock::setMidiTimelineTransport(int id, int kind, double beat) {
    mImpl->midiTimelines.setMidiTimelineTransport(id, kind, beat);
}

bool ClockworkClock::setMidiTimelineMeter(int id, int num, int den) {
    return mImpl->midiTimelines.setMidiTimelineMeter(id, num, den);
}

void ClockworkClock::tickMidiStaleness() {
    mImpl->midiTimelines.tickMidiStaleness();
}

clockwork::Timeline ClockworkClock::timeline(int id) const {
    if (id != 0) return mImpl->midiTimelines.timeline(id);
    return clockwork::Timeline::fromClockState(state());
}

bool ClockworkClock::timelineRt(int id, clockwork::Timeline& out) const {
    return mImpl->midiTimelines.timelineRt(id, out);
}

std::vector<ClockworkClock::TimelineInfo> ClockworkClock::listTimelines() const {
    return mImpl->midiTimelines.listTimelines();
}

void ClockworkClock::setTimelinesChangedCallback(std::function<void()> cb) {
    mImpl->midiTimelines.setTimelinesChangedCallback(std::move(cb));
}

// ─── Session mutators (app-thread) ─────────────────────────────────────────
// Thin delegations to LinkSession, which keeps the ClockworkClockState SAB mirror in
// sync (writes through Link + its callbacks on the Ableton path; direct SAB
// writes on the session-of-one path).

void ClockworkClock::setBpm(double bpm) {
    // Guard div-by-zero in beat math (timeAtBeat / requestBeatAtTime).
    if (!(bpm >= 1.0)) bpm = 1.0;
    mImpl->linkSession.setBpm(bpm);
}

void ClockworkClock::setIsPlaying(bool playing, double atNtpSeconds) {
    mImpl->linkSession.setIsPlaying(playing, atNtpSeconds);
}

bool ClockworkClock::setMeter(int num, int den) {
    if (!clockwork_timeline_meter_valid(num, den)) return false;
    if (ClockworkClockState* s = state()) s->setMeter(num, den);
    return true;
}

void ClockworkClock::setLinkEnabled(bool enabled) {
    // Route through setLinkVisibility so Link Audio sinks / subs / thread
    // priority all tear down and bring up together.
    if (enabled) {
        if (getLinkVisibility() == LinkVisibility::Off) {
            setLinkVisibility(mImpl->linkSession.lastNonOffVisibility());
        }
    } else {
        setLinkVisibility(LinkVisibility::Off);
    }
}

void ClockworkClock::setStartStopSyncEnabled(bool enabled) {
    mImpl->linkSession.setStartStopSyncEnabled(enabled);
}

void ClockworkClock::requestBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    mImpl->linkSession.requestBeatAtTime(beat, atNtpSeconds, quantum);
}

void ClockworkClock::forceBeatAtTime(double beat, double atNtpSeconds, double quantum) {
    mImpl->linkSession.forceBeatAtTime(beat, atNtpSeconds, quantum);
}

void ClockworkClock::requestSetLinkEnabledAsync(bool enabled) {
    mImpl->linkSession.requestSetLinkEnabledAsync(enabled);
}

// ─── Getters (app-thread) ──────────────────────────────────────────────────

bool ClockworkClock::isLinkEnabled() const {
    return mImpl->linkSession.isEnabled();
}

bool ClockworkClock::isStartStopSyncEnabled() const {
    return mImpl->linkSession.isStartStopSyncEnabled();
}

size_t ClockworkClock::numPeers() const {
    return mImpl->linkSession.numPeers();
}

// ─── Link event callbacks ────────────────────────────────────────────────────

void ClockworkClock::setTempoChangedCallback(std::function<void(double)> cb) {
    mImpl->linkSession.setTempoChangedCallback(std::move(cb));
}

void ClockworkClock::setNumPeersChangedCallback(std::function<void(std::size_t)> cb) {
    mImpl->linkSession.setNumPeersChangedCallback(std::move(cb));
}

void ClockworkClock::setStartStopChangedCallback(
    std::function<void(bool, double)> cb) {
    mImpl->linkSession.setStartStopChangedCallback(std::move(cb));
}

// ─── Link: visibility / peer name ───────────────────────────────────────────
// setLinkVisibility drives the LinkSession enable/visibility primitives and
// tells the visibility listener (the engine's LinkAudioHost) around them, in
// the order a clean substrate transition needs.

void ClockworkClock::setLinkVisibility(LinkVisibility v) {
#if !CLOCKWORK_LINK
    // NO LINK COMPILED IN MEANS NO VISIBILITY TO SET, and saying otherwise is
    // a lie that travels.
    //
    // This function is common to both build shapes, and below it sets
    // SC_FLAG_LINK_ENABLED for any non-Off visibility. On a no-Link build
    // setLinkEnabled(true) still reached here — lastNonOffVisibility() answers
    // LoopbackOnly from the stub — so the flag went up with no session behind
    // it. That flag is not private: it lives in the shared clock state, part of
    // the published ClockworkClock protocol any client can read (the bit is named in
    // js/lib/clockwork_clock_protocol.js), so raising it advertised a capability the
    // build does not have.
    //
    // Same family as the beat_origin_ntp epoch bug: a path upstream never
    // compiled, because it defaults Link ON, promoted to the shipping path by
    // clockwork defaulting it OFF (Link is opt-in). Clamp it here, at the
    // single writer, rather than teaching each reader to distrust the flag.
    v = LinkVisibility::Off;
#endif
    if (v == getLinkVisibility()) return;

    if (v != LinkVisibility::Off) mImpl->linkSession.setLastNonOffVisibility(v);

    // Tear down then bring up. Disabling Link forces immediate gateway teardown
    // so the loopback flag transition is instant — no ~5 s rescan window.
    // Whatever binds to the current Link substrate (Link Audio's sinks and
    // subscriptions, keyed on session-scoped channel ids) drops BEFORE the
    // session disables and is rebuilt after the next non-Off transition.
    if (auto* l = mImpl->visibilityListener) l->linkWillChangeVisibility();
    mImpl->linkSession.prepareDisable();

    // Interface-filter flag picked up by the next enable.
    mImpl->linkSession.setLoopbackOnly(v == LinkVisibility::LoopbackOnly);

    ClockworkClockState* s = state();
    if (v == LinkVisibility::Off) {
        if (s) s->setFlag(SC_FLAG_LINK_ENABLED, false);
        return;
    }

    // Flag-flip before bring-up so observers never see "Link on, flag off"
    // mid-transition.
    if (s) s->setFlag(SC_FLAG_LINK_ENABLED, true);

    // Bring up in order: enable (ScanIpIfAddrs respects loopback flag), raise
    // thread priority, then hand the session back to the listener.
    mImpl->linkSession.enableWithPriority();
    if (auto* l = mImpl->visibilityListener) l->linkDidEnable();
}

void ClockworkClock::setLinkVisibilityListener(LinkVisibilityListener* listener) {
    mImpl->visibilityListener = listener;
}

ClockworkClock::LinkVisibility ClockworkClock::getLinkVisibility() const {
    return mImpl->linkSession.getVisibility();
}

void ClockworkClock::setPeerName(const char* name) {
    mImpl->linkSession.setPeerName(name);
}

const char* ClockworkClock::peerName() const {
    return mImpl->linkSession.peerName();
}

// ─── Link peers ─────────────────────────────────────────────────────────────

std::vector<ClockworkClock::PeerInfo> ClockworkClock::listPeers() const {
    return mImpl->linkSession.listPeers();
}

LinkSession& ClockworkClock::linkSession() {
    return mImpl->linkSession;
}

void ClockworkClock::publishLinkMetrics(PerformanceMetrics* m, double quantum) {
    if (!m) return;
    // No-op on a session-of-one (no Link).
    mImpl->linkSession.publishLinkClockMetrics(m, quantum);
}

// ─── Audio-thread NTP (delegated to TimeSource) ─────────────────────────────
// Thin delegations to the native time-source (WallClock + IIR). RT-safe;
// link-time-concrete, no vtable on the audio-thread path.

double ClockworkClock::now() const {
    return mImpl->timeSource.now();
}

double ClockworkClock::nowAt(double audioCurrentTime) const {
    return mImpl->timeSource.nowAt(audioCurrentTime);
}

double ClockworkClock::wallNow() const {
    return mImpl->timeSource.wallNow();
}

double ClockworkClock::updateAudioThreadNTP(double samplePosition,
                                         double sampleRate,
                                         double audioCurrentTime) {
    return mImpl->timeSource.updateAudioThreadNTP(samplePosition, sampleRate,
                                                  audioCurrentTime);
}

void ClockworkClock::resetAudioThreadTime(double samplePosition, double sampleRate) {
    mImpl->timeSource.resetAudioThreadTime(samplePosition, sampleRate);
}

void ClockworkClock::setFreewheelClock(bool enabled) {
    mImpl->timeSource.setFreewheelClock(enabled);
}
