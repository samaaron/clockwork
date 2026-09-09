/*
 * test_clockwork_clock.cpp — ClockworkClock state-machine tests.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

#include "clock/ClockworkClock.h"
#include "clock/clock_math.h"

#ifdef CLOCKWORK_LINK
#include <algorithm>
#include <vector>
// The platform interface scanner that Link's discovery binds to. Our
// loopback-mode patch adds the loopbackOnly() flag + filtering here; the
// visibility tests below assert on what this scanner returns.
#if defined(_WIN32)
#include <ableton/platforms/windows/ScanIpIfAddrs.hpp>
#else
#include <ableton/platforms/posix/ScanIpIfAddrs.hpp>
#endif
#endif  // CLOCKWORK_LINK

namespace {

// A MIDI clock feed, in the two domains ClockworkClock keeps apart.
//
// midiTimelinePulse takes an OS timestamp in microseconds, as a driver
// callback supplies it. The clock captures `wallClockNTP() - ts` at the FIRST
// pulse of a run and applies that offset to every pulse after it, so a pulse
// at device time t0 + k*iv lands at NTP n0 + k*iv. Anchoring n0 here is what
// lets a case name the exact NTP of any pulse it wants the beat at, rather
// than reading "now" and hoping the two agree.
struct PulseFeed {
    ClockworkClock& sc;
    int             id;
    double          ivSec;
    int64_t         tsUs;
    double          n0;
    int             sent = 0;

    PulseFeed(ClockworkClock& c, int timeline, double bpm)
        : sc(c), id(timeline), ivSec(60.0 / bpm / 24.0) {
        tsUs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        n0 = wallClockNTP();
    }

    // Restart the device series at engine-now, as a real stream does after
    // START: the clock re-captures its offset on the next pulse.
    void reanchor() {
        tsUs = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        n0 = wallClockNTP();
        sent = 0;
    }

    void send(int n = 1) {
        for (int k = 0; k < n; ++k) {
            sc.midiTimelinePulse(id, static_cast<uint64_t>(
                tsUs + static_cast<int64_t>(sent) * static_cast<int64_t>(ivSec * 1e6)));
            ++sent;
        }
    }

    // The NTP instant of pulse `k` (0-based), and of the last one sent.
    double ntpOf(int k) const { return n0 + k * ivSec; }
    double ntpOfLast()  const { return ntpOf(sent - 1); }
};


// Device time -> the NTP instant ClockworkClock places it at.
//
// The clock captures `wallClockNTP() - ts` on the FIRST pulse of a run and
// applies that offset to every pulse after. Sampling the same pair here, just
// before that first pulse, lets a case name the exact NTP of any device
// timestamp it feeds instead of reading "now" and hoping the two agree.
struct DeviceClock {
    double t0us;
    double n0;
    explicit DeviceClock(double firstPulseUs)
        : t0us(firstPulseUs), n0(wallClockNTP()) {}
    double ntp(double tsUs) const { return n0 + (tsUs - t0us) * 1e-6; }
};

int64_t deviceMicros() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

namespace {
// setBpm → getBpm is eventually consistent. Link's commitAppSessionState
// updates clientState synchronously then posts session-timing work to
// its io thread; the same call's async handler can momentarily clobber
// a later commit's synchronous update before that later commit's async
// handler runs. This isn't a bug — it's Link's distributed convergence
// model. The test contract here is "eventually equals", not "instantly
// equals".
bool eventuallyBpm(ClockworkClock& sc, double expected, double eps = 1e-9) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        if (std::abs(sc.getBpm() - expected) < eps) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

#ifdef CLOCKWORK_LINK
// Invoke the platform interface scanner directly. It's synchronous and
// independent of Link's discovery thread, but reads the same process-global
// loopbackOnly() flag that ClockworkClock::setLinkVisibility toggles — so it
// reports exactly the address set Link would bind discovery to.
inline std::vector<ableton::discovery::IpAddress> scanLinkInterfaces() {
#if defined(_WIN32)
    return ableton::platforms::windows::ScanIpIfAddrs{}();
#else
    return ableton::platforms::posix::ScanIpIfAddrs{}();
#endif
}

inline bool allLoopback(const std::vector<ableton::discovery::IpAddress>& addrs) {
    return std::all_of(addrs.begin(), addrs.end(),
                       [](const ableton::discovery::IpAddress& ip) {
                           return ip.is_loopback();
                       });
}
#endif  // CLOCKWORK_LINK
}  // namespace

TEST_CASE("ClockworkClock: default state on construction", "[ClockworkClock]") {
    ClockworkClock sc;
    CHECK(sc.getBpm() == 120.0);
    CHECK(sc.isPlaying() == false);
    CHECK(sc.isLinkEnabled() == false);
    CHECK(sc.numPeers() == 0);
}

TEST_CASE("ClockworkClock: setBpm round-trips (eventually)", "[ClockworkClock]") {
    ClockworkClock sc;
    sc.setBpm(140.0); CHECK(eventuallyBpm(sc, 140.0));
    sc.setBpm(60.5); CHECK(eventuallyBpm(sc, 60.5));
    sc.setBpm(120.0); CHECK(eventuallyBpm(sc, 120.0));
}

TEST_CASE("ClockworkClock: setIsPlaying round-trips", "[ClockworkClock]") {
    ClockworkClock sc;
    CHECK(sc.isPlaying() == false);
    sc.setIsPlaying(true,  1234.5);
    CHECK(sc.isPlaying() == true);
    CHECK(sc.getIsPlayingAtNtp() == 1234.5);
    sc.setIsPlaying(false, 0.0);
    CHECK(sc.isPlaying() == false);
}

TEST_CASE("ClockworkClock: setLinkEnabled toggles Link state",
          "[ClockworkClock]") {
    ClockworkClock sc;
    // Fresh ClockworkClock starts with Link disabled.
    CHECK(sc.isLinkEnabled() == false);

#ifdef CLOCKWORK_LINK
    // On Link builds the setter delegates to ableton::Link::enable.
    sc.setLinkEnabled(true);
    CHECK(sc.isLinkEnabled() == true);
    sc.setLinkEnabled(false);
    CHECK(sc.isLinkEnabled() == false);
#else
    // No-Link builds: setter has no underlying backend; capability stays
    // false regardless of the requested state.
    sc.setLinkEnabled(true);
    CHECK(sc.isLinkEnabled() == false);
#endif

    // No peers ever discovered in a standalone ClockworkClock (no event loop
    // attached to discover anything).
    CHECK(sc.numPeers() == 0);
}

#ifdef CLOCKWORK_LINK
TEST_CASE("ClockworkClock: setLinkEnabled(true) on fresh state stays loopback-only",
          "[ClockworkClock][Link]") {
    // Privacy: bare enable on a fresh ClockworkClock must not promote to
    // NetworkWide LAN advertising.
    ClockworkClock sc;
    REQUIRE(sc.getLinkVisibility() == ClockworkClock::LinkVisibility::Off);
    sc.setLinkEnabled(true);
    CHECK(sc.getLinkVisibility() == ClockworkClock::LinkVisibility::LoopbackOnly);
}

TEST_CASE("ClockworkClock: setLinkEnabled(false→true) preserves LoopbackOnly",
          "[ClockworkClock][Link]") {
    // A disable/enable cycle must not silently upgrade a prior
    // LoopbackOnly choice to NetworkWide.
    ClockworkClock sc;
    sc.setLinkVisibility(ClockworkClock::LinkVisibility::LoopbackOnly);
    REQUIRE(sc.getLinkVisibility() == ClockworkClock::LinkVisibility::LoopbackOnly);

    sc.setLinkEnabled(false);
    REQUIRE(sc.getLinkVisibility() == ClockworkClock::LinkVisibility::Off);

    sc.setLinkEnabled(true);
    CHECK(sc.getLinkVisibility() == ClockworkClock::LinkVisibility::LoopbackOnly);
}

// The state-machine tests above only check getLinkVisibility() — the value we
// store. These check the value actually reaches the platform scanner Link
// binds to, i.e. that LoopbackOnly genuinely keeps us off non-loopback
// interfaces. Without this, a build that reports LoopbackOnly while still
// advertising on the LAN (e.g. a flag wired to a dead static) passes silently.
TEST_CASE("ClockworkClock: LoopbackOnly constrains the interface scan to loopback",
          "[ClockworkClock][Link][visibility]") {
    ClockworkClock sc;
    sc.setLinkVisibility(ClockworkClock::LinkVisibility::LoopbackOnly);

    const auto addrs = scanLinkInterfaces();
    // Loopback (lo0 / 127.0.0.1) is always up, so the filtered set is
    // non-empty and must contain nothing else.
    CHECK_FALSE(addrs.empty());
    CHECK(allLoopback(addrs));

    sc.setLinkVisibility(ClockworkClock::LinkVisibility::Off);  // reset global flag
}

TEST_CASE("ClockworkClock: NetworkWide leaves non-loopback interfaces in the scan",
          "[ClockworkClock][Link][visibility]") {
    ClockworkClock sc;
    sc.setLinkVisibility(ClockworkClock::LinkVisibility::LoopbackOnly);
    const auto loopback = scanLinkInterfaces();
    sc.setLinkVisibility(ClockworkClock::LinkVisibility::NetworkWide);
    const auto wide = scanLinkInterfaces();

    // NetworkWide must not filter: it exposes at least as many interfaces as
    // LoopbackOnly (strictly more on any host with a real NIC). Guards against
    // the loopback filter getting stuck on.
    CHECK(wide.size() >= loopback.size());

    sc.setLinkVisibility(ClockworkClock::LinkVisibility::Off);  // reset global flag
}

#endif  // CLOCKWORK_LINK

TEST_CASE("ClockworkClock: setBpm rejects non-finite and sub-1 values",
          "[ClockworkClock]") {
    // Guards beat math (timeAtBeat divides by bpm).
    ClockworkClock sc;
    sc.setBpm(120.0);

    sc.setBpm(0.0);
    CHECK(sc.getBpm() >= 1.0);
    CHECK(std::isfinite(sc.getBpm()));

    sc.setBpm(std::nan(""));
    CHECK(sc.getBpm() >= 1.0);
    CHECK(std::isfinite(sc.getBpm()));

    sc.setBpm(-5.0);
    CHECK(sc.getBpm() >= 1.0);
}

TEST_CASE("ClockworkClock: beatAtTime at non-integer-ratio BPM", "[ClockworkClock]") {
    ClockworkClock sc;
    sc.setBpm(137.0);
    // beatAtTime(t) = t * 137/60 — pick non-trivial values.
    CHECK(std::abs(sc.beatAtTime(3.0, 4.0) - (3.0 * 137.0 / 60.0)) < 1e-12);
    CHECK(std::abs(sc.beatAtTime(1.5, 4.0) - (1.5 * 137.0 / 60.0)) < 1e-12);
    CHECK(std::abs(sc.beatAtTime(0.0, 4.0)) < 1e-12);
}

TEST_CASE("ClockworkClock: timeAtBeat is inverse of beatAtTime", "[ClockworkClock]") {
    ClockworkClock sc;
    sc.setBpm(140.0);
    for (double b : {0.0, 0.5, 1.0, 4.0, 17.25}) {
        CHECK(std::abs(sc.beatAtTime(sc.timeAtBeat(b, 4.0), 4.0) - b) < 1e-12);
    }
}

TEST_CASE("ClockworkClock: phaseAtTime is non-negative and < quantum",
          "[ClockworkClock]") {
    ClockworkClock sc;
    sc.setBpm(120.0);
    for (double t : {0.0, 0.5, 1.0, 2.5, 7.0}) {
        const double phase = sc.phaseAtTime(t, 4.0);
        CHECK(phase >= 0.0);
        CHECK(phase < 4.0);
    }
    // beatAtTime(-0.5) = -1.0 → phase = 3.0
    const double phaseNeg = sc.phaseAtTime(-0.5, 4.0);
    CHECK(phaseNeg >= 0.0);
    CHECK(phaseNeg < 4.0);
    CHECK(std::abs(phaseNeg - 3.0) < 1e-12);
}

TEST_CASE("ClockworkClock: requestBeatAtTime maps beat to time", "[ClockworkClock]") {
    ClockworkClock sc;
    sc.setBpm(120.0);

    // beat 4 at time 2.0 with bpm 120 → beat_origin = 0
    sc.requestBeatAtTime(4.0, 2.0, 4.0);
    CHECK(std::abs(sc.beatAtTime(2.0, 4.0) - 4.0) < 1e-12);

    // beat 0 at time 10.0 → beat_origin moves to 10.0
    sc.requestBeatAtTime(0.0, 10.0, 4.0);
    CHECK(std::abs(sc.beatAtTime(10.0, 4.0)) < 1e-12);
    CHECK(std::abs(sc.beatAtTime(10.5, 4.0) - 1.0) < 1e-12);
}

TEST_CASE("ClockworkClock: forceBeatAtTime == requestBeatAtTime in session-of-one",
          "[ClockworkClock]") {
    ClockworkClock scA, scB;
    scA.setBpm(140.0);
    scB.setBpm(140.0);
    scA.requestBeatAtTime(8.0, 5.0, 4.0);
    scB.forceBeatAtTime(8.0, 5.0, 4.0);
    // Agreement, not bit-identity: the two routes reach the same value by
    // different arithmetic, and on i686 the x87 FPU holds intermediates at
    // 80-bit and rounds them per register spill, so they can part company in
    // the last ULP. Same tolerance as the rest of this file.
    CHECK(std::abs(scA.beatAtTime(7.0, 4.0)  - scB.beatAtTime(7.0, 4.0))  < 1e-12);
    CHECK(std::abs(scA.beatAtTime(10.0, 4.0) - scB.beatAtTime(10.0, 4.0)) < 1e-12);
}

// ── Audio-thread time source ─────────────────────────────────────────────

TEST_CASE("ClockworkClock: now() returns sensible NTP time", "[ClockworkClock]") {
    ClockworkClock sc;
    CHECK(sc.now() > 3.9e9);  // NTP epoch is 1900; today is well past 2024
}

TEST_CASE("ClockworkClock: wallNow tracks wallClockNTP within tight bound",
          "[ClockworkClock]") {
    ClockworkClock sc;
    const double direct = wallClockNTP();
    const double via_sc = sc.wallNow();
    CHECK(std::abs(via_sc - direct) < 0.01);
}

TEST_CASE("ClockworkClock: updateAudioThreadNTP publishes the returned value",
          "[ClockworkClock]") {
    ClockworkClock sc;
    sc.resetAudioThreadTime(0.0, 48000.0);
    const double returned = sc.updateAudioThreadNTP(128.0, 48000.0);
    CHECK(sc.now() == returned);
}

TEST_CASE("ClockworkClock: resetAudioThreadTime publishes a usable NTP immediately",
          "[ClockworkClock]") {
    ClockworkClock sc;
    sc.resetAudioThreadTime(0.0, 48000.0);
    CHECK(std::abs(sc.now() - wallClockNTP()) < 0.01);
}

// ─── MIDI follower timelines ─────────────────────────────────────────────

TEST_CASE("ClockworkClock: midi timeline claim is idempotent and slot-stable",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int a = sc.claimMidiTimeline("portA", "Port A");
    CHECK(a == 1);
    CHECK(sc.claimMidiTimeline("portA", "Port A") == a);          // idempotent
    CHECK(sc.resolveTimeline("midi:portA") == a);
    CHECK(sc.resolveTimeline("midi:portB") == -1);      // not claimed
    CHECK(sc.claimMidiTimeline("portB", "Port B") == 2);
    CHECK(sc.claimMidiTimeline("", "") == -1);              // empty name rejected
}

TEST_CASE("ClockworkClock: midi registry is bounded at SC_MAX_TIMELINES",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    for (int i = 1; i <= SC_MAX_TIMELINES; ++i)
        CHECK(sc.claimMidiTimeline(("p" + std::to_string(i)).c_str(), "raw") == i);
    CHECK(sc.claimMidiTimeline("overflow", "Overflow") == -1);      // registry full
}

TEST_CASE("ClockworkClock: resolveTimeline maps names to ids", "[ClockworkClock][midi]") {
    ClockworkClock sc;
    CHECK(sc.resolveTimeline("") == 0);                 // link by default
    CHECK(sc.resolveTimeline("link") == 0);
    CHECK(sc.resolveTimeline("midi:nope") == -1);       // unclaimed → placeholder
    const int a = sc.claimMidiTimeline("portA", "Port A");
    CHECK(sc.resolveTimeline("midi:portA") == a);
    CHECK(sc.resolveTimeline("midi") == a);             // bare midi → primary
}

TEST_CASE("ClockworkClock: midi tempo feed sets bpm and advances beats",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    sc.setMidiTimelineTempo(id, 140.0);
    CHECK(sc.timelineBpm(id) == Catch::Approx(140.0));

    const double t  = wallClockNTP();
    const double b0 = sc.timeline(id).beatAt(t);
    const double b1 = sc.timeline(id).beatAt(t + 1.0);                    // +1s
    CHECK(b1 - b0 == Catch::Approx(140.0 / 60.0).epsilon(1e-6));          // 140 BPM

    // time_at_beat is the inverse of beat_at_time.
    CHECK(sc.timeline(id).timeAtBeat(b1) == Catch::Approx(t + 1.0).epsilon(1e-9));
}

// On both compile shapes: Link invokes the callback for a local commit, and
// the session of one must do the same by hand, or a Link-less engine changes
// tempo in silence and every other client keeps the old one.
TEST_CASE("ClockworkClock: a local setBpm fires the tempo callback exactly once",
          "[ClockworkClock]") {
    ClockworkClock sc;
    std::atomic<int> notifications{0};
    sc.setTempoChangedCallback([&](double) { notifications.fetch_add(1); });

    sc.setBpm(90.0);   // 90 != default 60, so it is a real change

    // Wait for the notification to land (Link's handler runs on its io thread),
    // then give any duplicate a generous window to surface before asserting.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (notifications.load() < 1
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(notifications.load() >= 1);   // the notify happened at all
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let a dup show
    CHECK(notifications.load() == 1);     // exactly one — no double-fire
}

#ifdef CLOCKWORK_LINK
TEST_CASE("ClockworkClock: binding into the arena anchors the Link grid so beats stay small and a tempo change is continuous",
          "[ClockworkClock][Link]") {
    // The engine binds its clock into the SHM arena at init (bindStateToShm),
    // which is where the grid gets a real epoch. The Link path once left this a
    // no-op, trusting Link's callbacks — which do not fire at boot — so the grid
    // sat at origin 0 and read out epoch-scale beats (~4e9 = seconds since
    // 1900). A client adopted those, and the first tempo change re-anchored to
    // Link's own small beat, shifting the domain under the client: a held beat
    // resolved to a time ~4e9 seconds away, and a Link-mode sleep never woke.
    ClockworkClock sc;
    ClockworkClockState region;
    ClockworkClockState::initDefaults(region);
    sc.bindStateToShm(&region);   // engine init: anchor the grid

    const double now = wallClockNTP();
    const double beatNow = sc.beatAtTime(now, 4.0);
    // Anchored to wall time: the current beat is small (a few seconds past
    // boot), not epoch-scale. Without the anchor this was ~8e9.
    CHECK(beatNow < 1.0e6);

    // And the beat playing now stays put across a tempo change.
    const double target = sc.getBpm() * 2.0;
    sc.setBpm(target);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // let applyTempoChange land
    const double t = sc.timeAtBeat(beatNow, 4.0);
    CHECK(t == Catch::Approx(now).margin(0.5));
}
#endif  // CLOCKWORK_LINK

TEST_CASE("ClockworkClock: a local transport change fires the start/stop callback",
          "[ClockworkClock]") {
    ClockworkClock sc;
    std::atomic<int> notifications{0};
    std::atomic<bool> lastPlaying{false};
    sc.setStartStopChangedCallback([&](bool playing, double) {
        lastPlaying.store(playing);
        notifications.fetch_add(1);
    });

    sc.setIsPlaying(true, wallClockNTP());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (notifications.load() < 1
           && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(notifications.load() >= 1);
    CHECK(lastPlaying.load() == true);
}

TEST_CASE("ClockworkClock: midi tempo change preserves the current beat",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    sc.setMidiTimelineTempo(id, 120.0);
    const double before = sc.timeline(id).beatAt(wallClockNTP());
    sc.setMidiTimelineTempo(id, 174.0);                 // jump tempo
    const double after = sc.timeline(id).beatAt(wallClockNTP());
    CHECK(after == Catch::Approx(before).margin(0.05)); // no beat discontinuity
}

TEST_CASE("ClockworkClock: midi transport drives playing state", "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    CHECK_FALSE(sc.timelineIsPlaying(id));
    sc.setMidiTimelineTransport(id, /*START*/ 0, 0.0);
    CHECK(sc.timelineIsPlaying(id));
    sc.setMidiTimelineTransport(id, /*STOP*/ 2, 0.0);
    CHECK_FALSE(sc.timelineIsPlaying(id));
}

// Anchored = a transport event (START or SPP) has defined where beat 0 is.
// Without one, beats are arbitrary 24-pulse groupings counted from whichever
// pulse the engine happened to see first, so bar phase is meaningless —
// midi_sync gates on this flag.
TEST_CASE("ClockworkClock: midi timeline anchors on START, not pulses or CONTINUE",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    CHECK_FALSE(sc.timelineIsAnchored(id));

    const int64_t ts0 = deviceMicros();
    const int64_t iv = static_cast<int64_t>(60.0 / 120.0 / 24.0 * 1e6);
    for (int k = 0; k < 48; ++k)
        sc.midiTimelinePulse(id, static_cast<uint64_t>(ts0 + static_cast<int64_t>(k) * iv));
    CHECK_FALSE(sc.timelineIsAnchored(id));             // pulses alone don't anchor

    sc.setMidiTimelineTransport(id, /*CONTINUE*/ 1, 0.0);
    CHECK_FALSE(sc.timelineIsAnchored(id));             // resuming an unknown grid

    sc.setMidiTimelineTransport(id, /*START*/ 0, 0.0);
    CHECK(sc.timelineIsAnchored(id));
    sc.setMidiTimelineTransport(id, /*STOP*/ 2, 0.0);
    CHECK(sc.timelineIsAnchored(id));                   // STOP keeps the grid origin
}

TEST_CASE("ClockworkClock: midi timeline anchors on SPP (DAW playing mid-song)",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    sc.setMidiTimelineTransport(id, /*POSITION*/ 3, 8.0);
    CHECK(sc.timelineIsAnchored(id));
}

TEST_CASE("ClockworkClock: link timeline is always anchored", "[ClockworkClock][midi]") {
    ClockworkClock sc;
    CHECK(sc.timelineIsAnchored(0));                    // Link's grid always exists
}

TEST_CASE("ClockworkClock: the anchor clears when a slot is recycled",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    sc.setMidiTimelineTransport(id, /*START*/ 0, 0.0);
    CHECK(sc.timelineIsAnchored(id));
    sc.freeMidiTimeline(id);
    const int id2 = sc.claimMidiTimeline("portA", "Port A");
    CHECK_FALSE(sc.timelineIsAnchored(id2));
}

TEST_CASE("ClockworkClock: START mid-stream re-anchors beat 0 to the downbeat",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    PulseFeed feed(sc, id, 120.0);
    feed.send(48);                                      // free-run to ~beat 2
    sc.setMidiTimelineTransport(id, /*START*/ 0, 0.0);  // device downbeat = beat 0
    // START re-captures the pulse->engine clock offset on the next pulse, so
    // the synthetic series must restart at engine-now (as a real stream would).
    // The first 0xF8 after START IS the downbeat (beat 0); 24 more advance a beat.
    feed.reanchor();
    feed.send(25);
    const double beat = sc.timeline(id).beatAt(feed.ntpOfLast());
    CHECK(beat == Catch::Approx(1.0).margin(0.02));     // downbeat + 24 pulses
}

TEST_CASE("ClockworkClock: SPP re-bases the beat counter", "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    sc.setMidiTimelineTransport(id, /*POSITION*/ 3, 8.0);
    // First 0xF8 after SPP sits on the SPP position (beat 8); 24 more = 1 beat.
    PulseFeed feed(sc, id, 120.0);
    feed.send(25);
    const double beat = sc.timeline(id).beatAt(feed.ntpOfLast());
    CHECK(beat == Catch::Approx(9.0).margin(0.02));     // 8.0 + downbeat + 24/24
}

TEST_CASE("ClockworkClock: CONTINUE preserves the beat anchor", "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    sc.setMidiTimelineTransport(id, /*START*/ 0, 0.0);
    // START downbeat (beat 0) + 24 pulses = beat 1.0 before the stop.
    PulseFeed feed(sc, id, 120.0);
    feed.send(25);
    sc.setMidiTimelineTransport(id, /*STOP*/ 2, 0.0);
    sc.setMidiTimelineTransport(id, /*CONTINUE*/ 1, 0.0);
    // CONTINUE resumes the existing grid (no re-anchor), so every pulse counts
    // and the device series carries straight on.
    feed.send(24);
    const double beat = sc.timeline(id).beatAt(feed.ntpOfLast());
    CHECK(beat == Catch::Approx(2.0).margin(0.02));     // 1.0 + 24 (counts on)
    CHECK(sc.timelineIsAnchored(id));
}

TEST_CASE("ClockworkClock: midi beat tracks the pulse count", "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    PulseFeed feed(sc, id, 120.0);
    feed.send(49);                                     // downbeat + 48 -> beat 2.0
    // The beat is the exact count: the first pulse IS the downbeat (beat 0),
    // so querying at the last pulse's own instant gives 48/24 = 2.0.
    const double beat = sc.timeline(id).beatAt(feed.ntpOfLast());
    CHECK(beat == Catch::Approx(2.0).margin(0.02));
    CHECK(sc.timelineBpm(id) == Catch::Approx(120.0).margin(0.5));     // tempo recovered
}

TEST_CASE("ClockworkClock: midi tempo de-jitters a wobbly clock",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const int64_t t0 = deviceMicros();
    const double iv = 60.0 / 120.0 / 24.0 * 1e6;          // 120 BPM nominal interval
    // Feed 96 pulses whose arrivals alternate ±20% around the true interval
    // (bunched OS delivery). The tempo read-out must stay steady at ~120 BPM.
    double t = static_cast<double>(t0) - 96.0 * iv;
    for (int k = 1; k <= 96; ++k) {
        t += iv * (k % 2 ? 1.20 : 0.80);
        sc.midiTimelinePulse(id, static_cast<uint64_t>(t));
    }
    CHECK(sc.timelineBpm(id) == Catch::Approx(120.0).margin(1.0));
}

TEST_CASE("ClockworkClock: midi stall-and-burst keeps both beat and tempo",
          "[ClockworkClock][midi]") {
    // Field capture 2026-06-11 (MOTU rig): the tick stream periodically stalls
    // ~120ms then flushes the queued ticks ~300µs apart. The burst ticks are
    // real (each is 1/24 beat) so they must all land in the beat count, while
    // the garbage arrival intervals must not disturb the tempo.
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const double iv = 60.0 / 111.0 / 24.0 * 1e6;          // ~111 BPM interval
    double t = static_cast<double>(deviceMicros());
    const DeviceClock dev(t);
    int pulses = 0;
    for (int k = 0; k < 96; ++k) { sc.midiTimelinePulse(id, static_cast<uint64_t>(t)); t += iv; ++pulses; }
    const double bpmBefore = sc.timelineBpm(id);

    t += 122000.0 - iv;                                    // stall: next tick 122ms late
    for (int k = 0; k < 6; ++k) {                          // queued ticks flushed in a burst
        sc.midiTimelinePulse(id, static_cast<uint64_t>(t)); t += 300.0; ++pulses;
    }
    for (int k = 0; k < 48; ++k) { sc.midiTimelinePulse(id, static_cast<uint64_t>(t)); t += iv; ++pulses; }

    CHECK(sc.timelineBpm(id) == Catch::Approx(bpmBefore).margin(1.0));
    // Beat at the last pulse's own timestamp = ticks since the downbeat pulse
    // / 24: every burst tick counted, no phase slip.
    const double beat = sc.timeline(id).beatAt(dev.ntp(t - iv));
    CHECK(beat == Catch::Approx(static_cast<double>(pulses - 1) / 24.0).margin(0.02));
}

TEST_CASE("ClockworkClock: midi tempo only interpolates between pulses",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const int64_t t0 = deviceMicros();
    const int64_t iv = static_cast<int64_t>(60.0 / 120.0 / 24.0 * 1e6);
    const DeviceClock dev(static_cast<double>(t0 - 47 * iv));   // the first pulse sent
    for (int k = 1; k <= 48; ++k)
        sc.midiTimelinePulse(id, static_cast<uint64_t>(t0 - (48 - k) * iv));
    // Extrapolating one pulse-interval past the last pulse adds exactly 1/24 beat
    // at the ~120 BPM tempo.
    const double here  = sc.timeline(id).beatAt(dev.ntp(static_cast<double>(t0)));
    const double ahead = sc.timeline(id).beatAt(dev.ntp(static_cast<double>(t0 + iv)));
    CHECK(ahead - here == Catch::Approx(1.0 / 24.0).margin(1e-3));
}

// Accuracy test (hidden by default — real-time, busy-waits a CPU; run with
// `clockwork_tests "[accuracy]"`). Drives a real accelerando and measures
// the engine's beat against ground truth (pulse_count/24), alongside a model
// that integrates the tempo read-out (which drifts).
TEST_CASE("ClockworkClock: midi beat is drift-free through a live tempo ramp",
          "[ClockworkClock][midi][accuracy][.]") {
    using namespace std::chrono;
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("ramp", "Ramp");

    const double startBpm = 120.0, endBpm = 180.0, rampSecs = 3.0;
    const auto   wall0 = steady_clock::now();
    int64_t pulses = 0;
    double  maxDrift = 0.0;

    // For comparison: integrate the tempo read-out over elapsed time instead of
    // counting pulses.
    double  naiveBeat = 0.0;
    int64_t naivePrevUs = deviceMicros();
    double  maxNaiveDrift = 0.0;

    int64_t nextUs = deviceMicros();
    const DeviceClock dev(static_cast<double>(nextUs));
    for (;;) {
        const double el = duration<double>(steady_clock::now() - wall0).count();
        if (el >= rampSecs) break;
        const double trueBpm = startBpm + (endBpm - startBpm) * (el / rampSecs);
        nextUs += static_cast<int64_t>(60.0 / trueBpm / 24.0 * 1e6);
        while (deviceMicros() < nextUs) { /* busy-wait to the pulse instant */ }

        ++pulses;
        sc.midiTimelinePulse(id, static_cast<uint64_t>(deviceMicros()));

        const int64_t qn        = deviceMicros();
        const double  trueBeat  = static_cast<double>(pulses - 1) / 24.0;
        const double  engineBeat = sc.timeline(id).beatAt(dev.ntp(static_cast<double>(qn)));
        maxDrift = std::max(maxDrift, std::abs(engineBeat - trueBeat));

        naiveBeat += static_cast<double>(qn - naivePrevUs) * 1e-6 * sc.timelineBpm(id) / 60.0;
        naivePrevUs = qn;
        maxNaiveDrift = std::max(maxNaiveDrift, std::abs(naiveBeat - trueBeat));
    }

    WARN("ramp over " << pulses << " pulses (120->180 BPM, " << rampSecs << "s): "
         "pulse-anchored max drift = " << (maxDrift * 1000.0) << " milli-beats; "
         "naive-integrated max drift = " << (maxNaiveDrift * 1000.0) << " milli-beats");
    CHECK(maxDrift < 0.01);                       // pulse-anchored: ~0 drift
    CHECK(maxNaiveDrift > maxDrift * 5.0);        // the integrating model drifts
}

// Models a `use_bpm :midi / sample; sleep 1` live_loop across a 100->300 BPM step
// landing mid-beat-4. The spider commits each beat's play timestamp ~half a beat
// early via time_at_beat (look-ahead), so we sample the prediction there. Prints
// the gap sequence (~600 ms gaps, a short transition, then ~200 ms).
TEST_CASE("ClockworkClock: midi beat-fire spread across a 100->300 step",
          "[ClockworkClock][midi][accuracy][.]") {
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("clk", "Clk");

    std::vector<int64_t> pulseTs;
    int64_t t = deviceMicros();
    const DeviceClock dev(static_cast<double>(t));
    auto feed = [&](double bpm, int pulses) {
        const int64_t iv = static_cast<int64_t>(60.0 / bpm / 24.0 * 1e6);
        for (int p = 0; p < pulses; ++p) { pulseTs.push_back(t); t += iv; }
    };
    feed(100.0, 90);    // 3.75 beats at 100 BPM
    feed(300.0, 200);   // big step landing mid-beat-4

    const double lookBeats = 0.5;  // commit each beat ~half a beat ahead
    std::vector<double> fire;
    int nextBeat = 1;
    for (size_t i = 0; i < pulseTs.size(); ++i) {
        sc.midiTimelinePulse(id, static_cast<uint64_t>(pulseTs[i]));
        const double beatNow = sc.timeline(id).beatAt(dev.ntp(static_cast<double>(pulseTs[i])));
        while (nextBeat <= 9 && beatNow >= nextBeat - lookBeats) {
            fire.push_back(sc.timeline(id).timeAtBeat(nextBeat));
            ++nextBeat;
        }
    }
    REQUIRE(fire.size() >= 8);
    std::string gaps;
    for (size_t n = 1; n < fire.size(); ++n)
        gaps += std::to_string(std::llround((fire[n] - fire[n - 1]) * 1000.0)) + " ";
    WARN("beat-fire gaps (ms) across 100->300 step: " << gaps
         << "  [expect ~600 x3, transition, ~200 x...; any gap <200 = overshoot]");
    // Steady-state sanity: the last few gaps should be the true 300 BPM (~200 ms).
    const double tail = (fire[fire.size() - 1] - fire[fire.size() - 3]) / 2.0 * 1000.0;
    CHECK(tail == Catch::Approx(200.0).margin(8.0));
}

TEST_CASE("ClockworkClock: a never-seen midi timeline is a coherent free-run fallback",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int id = sc.resolveTimeline("midi:ghost");        // never claimed
    CHECK(id == -1);
    CHECK(sc.timelineBpm(id) == Catch::Approx(60.0));
    // beat<->time round-trips, and advances at a steady 60 BPM (1 beat / second).
    const auto tl = sc.timeline(id);
    const double t = tl.timeAtBeat(4.0);
    CHECK(tl.beatAt(t) == Catch::Approx(4.0).margin(1e-6));
    CHECK(tl.beatAt(t + 1.0) - tl.beatAt(t) == Catch::Approx(1.0).margin(1e-6));
}

TEST_CASE("ClockworkClock: a vanished midi clock keeps its tempo (not freed)",
          "[ClockworkClock][midi][.]") {
    using namespace std::chrono;
    ClockworkClock sc;
    const int id = sc.claimMidiTimeline("portA", "Port A");
    const int64_t iv = static_cast<int64_t>(60.0 / 120.0 / 24.0 * 1e6);   // 120 BPM
    int64_t ts = deviceMicros();
    for (int k = 0; k < 48; ++k) { sc.midiTimelinePulse(id, static_cast<uint64_t>(ts)); ts += iv; }
    CHECK(sc.timelineBpm(id) == Catch::Approx(120.0).margin(1.0));

    std::this_thread::sleep_for(milliseconds(1800));         // exceed the 1.5 s stale gap
    sc.tickMidiStaleness();
    // Slot is NOT reclaimed; the port still resolves and holds its last tempo.
    CHECK(sc.resolveTimeline("midi:portA") == id);
    CHECK(sc.timelineBpm(id) == Catch::Approx(120.0).margin(1.0));
}

TEST_CASE("ClockworkClock: primary follows the lowest active slot", "[ClockworkClock][midi]") {
    ClockworkClock sc;
    const int a = sc.claimMidiTimeline("portA", "Port A");
    const int b = sc.claimMidiTimeline("portB", "Port B");
    CHECK(sc.resolveTimeline("midi") == a);             // lowest slot is primary
    sc.freeMidiTimeline(a);
    CHECK(sc.resolveTimeline("midi") == b);             // promotes next slot
}

TEST_CASE("ClockworkClock: unknown timeline ids read a 60-BPM placeholder",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    CHECK(sc.timelineBpm(-1) == Catch::Approx(60.0));
    CHECK(sc.timelineBpm(99) == Catch::Approx(60.0));
    CHECK_FALSE(sc.timelineIsPlaying(-1));
    // beat<->time is a coherent free-run.
    const auto tl = sc.timeline(-1);
    CHECK(tl.beatAt(tl.timeAtBeat(2.0)) == Catch::Approx(2.0).margin(1e-6));
}

TEST_CASE("ClockworkClock: listTimelines reports link plus active midi rows",
          "[ClockworkClock][midi]") {
    ClockworkClock sc;
    auto only = sc.listTimelines();
    REQUIRE(only.size() == 1);
    CHECK(only[0].name == "link");
    sc.claimMidiTimeline("portA", "Port A");
    auto two = sc.listTimelines();
    REQUIRE(two.size() == 2);
    CHECK(two[1].name == "midi:portA");
    CHECK(two[1].primary);
}

