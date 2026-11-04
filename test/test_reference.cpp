// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_reference.cpp — reference runs: what one engine does, recorded; and
 * independence: what it does beside others, which must be the same.
 *
 * Each scenario drives one lane of the lanes fixture (no device, no wall
 * clock: frame 0 is a fixed NTP instant and every block advances by its
 * frames) and writes a trace: each block's audio as a hash, every frame on
 * both egress rings, and every metric at the end. A reference case runs one
 * scenario on the fixture's default lane, and its trace must equal the one
 * recorded for this platform in test/reference/<platform>/, so a change that
 * claims to leave behaviour alone proves it bit for bit (docs/ENGINES.md,
 * Part 4.1). The default lane is the binary's for good, so each case runs in
 * a process of its own, as ctest runs them; a second case in one process is
 * skipped and says why. A platform with no recording skips and says so.
 *
 * INDEPENDENCE (docs/ENGINES.md.2): a core's audio, replies and
 * metrics are the same beside other cores as alone. Each independence case
 * runs every scenario alone first, each on a lane of its own, held to its
 * recording where there is one; then all of them at once, a lane each, and
 * holds every lane to its trace alone. A trace that differs is a bug, never
 * a flake: the cores are independent or they are not.
 *
 * A scenario is a coroutine that pauses after each block, so lanes can take
 * turns a block at a time on one thread, or each run on a thread of its own.
 * Nothing in a run asserts as it goes — what goes wrong goes in the trace —
 * so a run may be on any thread; the cases compare on their own.
 *
 *   CLOCKWORK_REFERENCE_WRITE=<dir>   write each reference trace to <dir>
 *                                     instead of comparing (record a
 *                                     platform, or run twice and diff to
 *                                     check determinism)
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"
#include "lanes/lanes.h"
#include "metrics_schema.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <coroutine>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using lanes_test::Lane;

// The guest's bulk memory on a lane beside others: enough for every
// scenario's pool, a sixty-fourth of the desktop's figure.
constexpr size_t kBesideBulkBytes = 1u << 20;

uint64_t fnv1a(const void* data, size_t bytes, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

std::string hex(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

// The OSC timetag of absolute frame `frame` on a lane: a lane's frame 0 is
// kEpochNtp, a whole second.
uint64_t timetagAt(uint64_t frame) {
    const auto rate = static_cast<uint64_t>(lanes_test::kSampleRate);
    const uint64_t secs = static_cast<uint64_t>(lanes_test::kEpochNtp) + frame / rate;
    const uint64_t frac = ((frame % rate) << 32) / rate;
    return (secs << 32) | frac;
}

// What a host does to one lane, a block at a time: a coroutine that pauses
// after each block it renders (co_await run.render()).
class Scenario {
public:
    struct promise_type {
        std::exception_ptr failed;
        Scenario get_return_object() { return Scenario(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { failed = std::current_exception(); }
    };
    explicit Scenario(std::coroutine_handle<promise_type> h) : mH(h) {}
    Scenario(Scenario&& o) noexcept : mH(std::exchange(o.mH, {})) {}
    Scenario(const Scenario&) = delete;
    Scenario& operator=(const Scenario&) = delete;
    ~Scenario() {
        if (mH) mH.destroy();
    }
    // Run to the end of the next block, or to the end: false at the end.
    bool step() {
        if (!mH || mH.done()) return false;
        mH.resume();
        if (auto e = std::exchange(mH.promise().failed, nullptr)) std::rethrow_exception(e);
        return !mH.done();
    }
    void run() {
        while (step()) {}
    }

private:
    std::coroutine_handle<promise_type> mH;
};

// One scenario on one lane, and the trace of it.
class Run {
public:
    Run(const char* name, Lane& lane) : mName(name), mLane(lane) {
        mTrace << "scenario " << name << "\n"
               << "boot rate=" << lanes_test::kSampleRate << " block=" << lane.block()
               << " in=" << lanes_test::kInChannels << " out=" << lanes_test::kOutChannels << "\n";
    }

    const char* name() const { return mName; }
    Lane& lane() { return mLane; }
    uint32_t block() const { return mLane.block(); }
    std::string trace() const { return mTrace.str(); }

    void send(const osc_test::Packet& p) {
        if (!mLane.ingress(p.ptr(), p.size())) mTrace << "REFUSED ";
        mTrace << "send " << hex(p.data) << "\n";
    }

    // Render one block, with `in` (channel-major, kInChannels × block) as its
    // input when given, and pause there: for a scenario to co_await.
    std::suspend_always render(const std::vector<float>* in = nullptr) {
        tick(in);
        return {};
    }

    void note(const std::string& what) { mTrace << what << "\n"; }

    // The engine's own words at the end; the fields past them are a client's.
    // Beat and phase are left out: with no grid set, the clock puts beat 0 at
    // the operating system's wall clock (LinkSession.h,
    // anchorToWallClockIfUnset), not at the engine's own time, so they depend
    // on when the process ran (docs/ENGINES.md, suspect 10). The commit
    // is the build's, not the engine's behaviour.
    void finish() {
        for (const auto& f : clockwork::metrics_schema::kFields) {
            if (f.offset >= clockwork::metrics_schema::kMetricWords || std::strcmp(f.key, "clockBeatCenti") == 0 ||
                std::strcmp(f.key, "clockPhaseCenti") == 0 || std::strcmp(f.key, "clockworkCommit") == 0)
                continue;
            uint32_t word = 0;
            if (mLane.metricWord(f.offset, word)) mTrace << "metric " << f.key << " " << word << "\n";
            else                                  mTrace << "metric " << f.key << " MISSING\n";
        }
    }

private:
    void tick(const std::vector<float>* in) {
        if (in) {
            float* bus = clockwork_audio_in(mLane.core());
            if (bus && in->size() == static_cast<size_t>(lanes_test::kInChannels) * mLane.block())
                std::memcpy(bus, in->data(), in->size() * sizeof(float));
            else
                mTrace << "NO INPUT BUS\n";
        }
        const uint64_t at = mLane.tick();
        const float* out = clockwork_audio_out(mLane.core());
        if (out) {
            char line[64];
            std::snprintf(line, sizeof line, "%016llx",
                          static_cast<unsigned long long>(
                              fnv1a(out, sizeof(float) * lanes_test::kOutChannels * clockwork_block_size(mLane.core()))));
            mTrace << "block " << at << " " << line << "\n";
        } else {
            mTrace << "block " << at << " NO OUTPUT\n";
        }
        egress("rt", mLane.drainRt());
        egress("nrt", mLane.drainNrt());
    }

    void egress(const char* ring, const std::vector<lanes_test::EgressFrame>& frames) {
        for (const auto& f : frames) {
            mTrace << "  " << ring << " source=" << f.sourceId << " route=" << f.route << " seq=" << f.seq << " "
                   << osc_test::parseAddress(f.data.data(), static_cast<uint32_t>(f.data.size())) << " "
                   << hex(f.data) << (f.decodes ? "" : " UNDECODABLE") << "\n";
        }
    }

    const char* mName;
    Lane& mLane;
    std::ostringstream mTrace;
};

// ── The scenarios ───────────────────────────────────────────────────────────

Scenario pulse(Run& r) {
    for (int i = 0; i < 48; ++i) co_await r.render();
    r.finish();
}

Scenario tone(Run& r) {
    for (int i = 0; i < 4; ++i) co_await r.render();
    r.send(osc_test::message("/dummy/tone"));
    for (int i = 0; i < 60; ++i) co_await r.render();
    r.finish();
}

Scenario echo(Run& r) {
    r.send(osc_test::message("/dummy/echo"));
    std::vector<float> in(static_cast<size_t>(lanes_test::kInChannels) * r.block());
    for (int b = 0; b < 32; ++b) {
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = static_cast<float>((static_cast<int64_t>(b) * 131 + static_cast<int64_t>(i) * 7) % 2001 - 1000) / 1000.0f;
        co_await r.render(&in);
    }
    r.finish();
}

Scenario replies(Run& r) {
    for (int i = 0; i < 2; ++i) co_await r.render();
    r.send(osc_test::message("/dummy/ping"));
    r.send(osc_test::message("/dummy/events/get"));
    r.send(osc_test::message("/dummy/log", "reference log line"));
    // A verb the engine does not know answers an error.
    r.send(osc_test::message("/clockwork/no/such/verb"));
    for (int i = 0; i < 8; ++i) co_await r.render();
    r.send(osc_test::message("/dummy/ping"));
    for (int i = 0; i < 4; ++i) co_await r.render();
    r.finish();
}

Scenario timed(Run& r) {
    const uint32_t block = r.block();
    for (int i = 0; i < 10; ++i) co_await r.render();
    const uint64_t now = r.lane().position();
    // One due mid-block a few blocks ahead, one on a block's first frame,
    // one far ahead, one already past, and an untimed one.
    r.send(osc_test::bundle(timetagAt(now + 3 * block + block / 2), {osc_test::message("/dummy/when")}));
    r.send(osc_test::bundle(timetagAt(now + 6 * block), {osc_test::message("/dummy/when")}));
    r.send(osc_test::bundle(timetagAt(now + 40 * block + 7), {osc_test::message("/dummy/when"),
                                                                osc_test::message("/dummy/tone")}));
    r.send(osc_test::bundle(timetagAt(now - 5 * block), {osc_test::message("/dummy/when")}));
    r.send(osc_test::bundle(1, {osc_test::message("/dummy/when")}));
    for (int i = 0; i < 48; ++i) co_await r.render();
    r.finish();
}

Scenario bulk(Run& r) {
    uint8_t* assets = r.lane().assets();
    for (uint32_t i = 0; i < 65536; ++i) assets[i] = static_cast<uint8_t>((i * 2654435761u) >> 24);
    r.send(osc_test::message("/dummy/assets/hash", 0, 65536));
    r.send(osc_test::message("/dummy/assets/hash", 1000, 4096));
    r.send(osc_test::message("/dummy/blob/produce", 300000, 0x5a));
    for (int i = 0; i < 4; ++i) co_await r.render();
    char line[64];
    std::snprintf(line, sizeof line, "returns %016llx",
                  static_cast<unsigned long long>(fnv1a(r.lane().returns(), 300000)));
    r.note(line);
    r.finish();
}

Scenario swap(Run& r) {
    r.send(osc_test::message("/dummy/tone"));
    for (int i = 0; i < 16; ++i) co_await r.render();
    r.note("swap at " + std::to_string(r.lane().freshDsp()));
    for (int i = 0; i < 16; ++i) co_await r.render();
    r.send(osc_test::message("/dummy/ping"));
    for (int i = 0; i < 4; ++i) co_await r.render();
    r.finish();
}

Scenario comeBack(Run& r) {
    r.send(osc_test::message("/dummy/tone"));
    for (int i = 0; i < 12; ++i) co_await r.render();
    r.note("come back at the same rate");
    r.lane().comeBack();
    for (int i = 0; i < 12; ++i) co_await r.render();
    r.note("come back at 96 kHz");
    r.lane().comeBack(96000.0);
    for (int i = 0; i < 12; ++i) co_await r.render();
    r.note("come back at the fixture's own rate");
    r.lane().comeBack();
    for (int i = 0; i < 12; ++i) co_await r.render();
    r.finish();
}

struct Named {
    const char* name;
    Scenario (*make)(Run&);
};
constexpr Named kScenarios[] = {
    {"pulse", pulse}, {"tone", tone},   {"echo", echo}, {"replies", replies},
    {"timed", timed}, {"bulk", bulk},   {"swap", swap}, {"come-back", comeBack},
};
constexpr size_t kScenarioCount = std::size(kScenarios);

// ── Comparing ───────────────────────────────────────────────────────────────

// Where two traces first part, for a failure to say.
std::string firstDifference(const std::string& want, const std::string& got) {
    std::istringstream a(want), b(got);
    std::string la, lb;
    for (int n = 1;; ++n) {
        const bool ha = static_cast<bool>(std::getline(a, la));
        const bool hb = static_cast<bool>(std::getline(b, lb));
        if (!ha && !hb) return "no difference";
        if (!ha || !hb || la != lb)
            return "line " + std::to_string(n) + "\n  want: " + (ha ? la : "(the end)") + "\n  got:  " + (hb ? lb : "(the end)");
    }
}

// A trace against this platform's recording of `name`, if there is one.
// Missing, the reference case skips; an independence case goes on.
bool recorded(const char* name, std::string& want) {
    const std::string path =
        std::string(CLOCKWORK_REFERENCE_DIR) + "/" + CLOCKWORK_REFERENCE_PLATFORM + "/" + name + ".trace";
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    std::stringstream s;
    s << file.rdbuf();
    want = s.str();
    return true;
}

void compareWithRecording(const char* name, const std::string& trace) {
    if (const char* dir = std::getenv("CLOCKWORK_REFERENCE_WRITE")) {
        const std::string path = std::string(dir) + "/" + name + ".trace";
        std::ofstream(path, std::ios::binary) << trace;
        WARN("wrote " << path);
        return;
    }
    std::string want;
    if (!recorded(name, want)) SKIP("no reference recorded for " CLOCKWORK_REFERENCE_PLATFORM ": " << name);
    if (want == trace) return;
    const std::string got = std::string(name) + ".trace.actual";
    std::ofstream(got, std::ios::binary) << trace;
    FAIL("trace differs from the recording of " << name << " at " << firstDifference(want, trace)
                                              << "\n  the whole trace is in " << got);
}

// ── Reference runs ──────────────────────────────────────────────────────────

bool gRan = false;

void reference(const char* name, Scenario (*make)(Run&)) {
    if (gRan) SKIP("a reference run needs an engine of its own: run one case per process, as ctest does");
    gRan = true;
    lanes_test::boot();
    REQUIRE(lanes_test::baseNtp() == lanes_test::kEpochNtp);
    Run r(name, lanes_test::defaultLane());
    make(r).run();
    compareWithRecording(name, r.trace());
}

// ── Independence ────────────────────────────────────────────────────────────

// Every scenario alone, each on a lane of its own, closed before the next is
// made: the traces the cases hold every lane to, each held to its recording
// where this platform has one.
std::vector<std::string> alone() {
    std::vector<std::string> traces;
    for (const auto& s : kScenarios) {
        Lane lane(kBesideBulkBytes);
        Run r(s.name, lane);
        s.make(r).run();
        traces.push_back(r.trace());
        std::string want;
        if (recorded(s.name, want)) {
            INFO("scenario " << s.name << " alone on a lane of its own, against its recording");
            CHECK(firstDifference(want, traces.back()) == "no difference");
        }
    }
    return traces;
}

// Smaller than the target's own in every size a profile chooses, and room
// enough for every scenario.
ClockworkProfile smaller() {
    ClockworkProfile p {};
    p.struct_bytes = sizeof p;
    clockwork_profile_default(&p);
    p.in_ring_bytes      = 256 * 1024;
    p.rt_out_ring_bytes  = 64 * 1024;
    p.nrt_out_ring_bytes = 32 * 1024;
    p.scope_frames       = 4096;
    p.scope_channels     = 2;
    p.tap_slots          = 4;
    p.track_tap_slots    = 0;
    p.tap_ring_frames    = 4096;
    return p;
}

// Every scenario at once, a lane each; every other lane laid out from
// `profile` when there is one.
struct Beside {
    std::vector<std::unique_ptr<Lane>> lanes;
    std::vector<std::unique_ptr<Run>> runs;
    std::vector<Scenario> scenarios;
    explicit Beside(const ClockworkProfile* profile = nullptr) {
        for (const auto& s : kScenarios) {
            lanes.push_back(std::make_unique<Lane>(kBesideBulkBytes, lanes.size() % 2 ? profile : nullptr));
            runs.push_back(std::make_unique<Run>(s.name, *lanes.back()));
            scenarios.push_back(s.make(*runs.back()));
        }
    }
    // Every lane held to its trace alone.
    void check(const std::vector<std::string>& want) const {
        for (size_t i = 0; i < runs.size(); ++i) {
            INFO("scenario " << runs[i]->name() << " beside the others, against its trace alone");
            CHECK(firstDifference(want[i], runs[i]->trace()) == "no difference");
        }
    }
};

// What a thread that runs scenarios hit, if anything: the case reports it.
struct Fault {
    std::atomic<bool> set{false};
    std::string what;
    void note(const char* where) {
        if (set.exchange(true)) return;
        try { throw; }
        catch (const std::exception& e) { what = std::string(where) + ": " + e.what(); }
        catch (...) { what = where; }
    }
};

} // namespace

TEST_CASE("reference: the placeholder's pulse, nothing sent", "[reference]") { reference("pulse", pulse); }
TEST_CASE("reference: a tone asked for at once", "[reference]") { reference("tone", tone); }
TEST_CASE("reference: input echoed", "[reference]") { reference("echo", echo); }
TEST_CASE("reference: replies, notices and log lines", "[reference]") { reference("replies", replies); }
TEST_CASE("reference: messages at their times, early, due and late", "[reference]") { reference("timed", timed); }
TEST_CASE("reference: bytes through the assets and returns areas", "[reference]") { reference("bulk", bulk); }
TEST_CASE("reference: a cold swap of the guest mid-run", "[reference]") { reference("swap", swap); }
TEST_CASE("reference: audio coming back, at the same rate and at another", "[reference]") {
    reference("come-back", comeBack);
}

// One thread renders every core in turn, a block each: what a host does that
// runs its plugins one after another on its audio thread. Whatever one core
// leaves on the thread, the next must not see.
TEST_CASE("independence: every scenario beside the others, in turn on one thread, a block each",
          "[reference][independence]") {
    const auto want = alone();
    Beside b;
    for (bool going = true; going;) {
        going = false;
        for (auto& s : b.scenarios) going = s.step() || going;
    }
    b.check(want);
}

// Cores laid out from a profile of their own beside cores laid out from the
// target's (docs/ENGINES.md): a core's sizes are its own, and no
// scenario's trace depends on them.
TEST_CASE("independence: every scenario beside the others, every other core laid out from a smaller profile",
          "[reference][independence]") {
    const auto want = alone();
    const ClockworkProfile profile = smaller();
    Beside b(&profile);
    for (bool going = true; going;) {
        going = false;
        for (auto& s : b.scenarios) going = s.step() || going;
    }
    b.check(want);
}

// Each core on a thread of its own, every block started together: what a host
// does that renders its plugins in parallel.
TEST_CASE("independence: every scenario beside the others, each on a thread of its own, blocks together",
          "[reference][independence]") {
    const auto want = alone();
    Beside b;
    Fault fault;
    std::barrier together(static_cast<std::ptrdiff_t>(kScenarioCount));
    std::vector<std::thread> threads;
    for (auto& s : b.scenarios)
        threads.emplace_back([&s, &together, &fault] {
            try {
                for (;;) {
                    together.arrive_and_wait();
                    if (!s.step()) break;
                }
            } catch (...) {
                fault.note("a lane's thread");
            }
            together.arrive_and_drop();
        });
    for (auto& t : threads) t.join();
    INFO(fault.what);
    REQUIRE_FALSE(fault.set.load());
    b.check(want);
}

// Each core's every block on a thread drawn from a pool, seeded so a failure
// replays: what a host does whose audio work is stolen across its threads.
TEST_CASE("independence: every scenario beside the others, each block on a thread from a pool",
          "[reference][independence]") {
    const auto want = alone();
    Beside b;
    constexpr size_t kThreads = 3;
    const uint32_t seed = static_cast<uint32_t>(std::random_device{}());
    INFO("seed " << seed);
    std::mt19937 draw(seed);
    std::vector<uint8_t> going(kScenarioCount, 1);   // a byte each: pool threads write their own
    // Each round, which scenario steps on which thread; every thread works its
    // list, and the round ends when all have.
    std::vector<std::vector<size_t>> rounds(kThreads);
    std::barrier start(static_cast<std::ptrdiff_t>(kThreads + 1)), end(static_cast<std::ptrdiff_t>(kThreads + 1));
    std::atomic<bool> stop{false};
    Fault fault;
    std::vector<std::thread> pool;
    for (size_t t = 0; t < kThreads; ++t)
        pool.emplace_back([&, t] {
            for (;;) {
                start.arrive_and_wait();
                if (stop.load()) return;
                try {
                    for (size_t i : rounds[t]) going[i] = b.scenarios[i].step();
                } catch (...) {
                    fault.note("a pool thread");
                }
                end.arrive_and_wait();
            }
        });
    for (;;) {
        for (auto& r : rounds) r.clear();
        bool any = false;
        for (size_t i = 0; i < kScenarioCount; ++i)
            if (going[i]) {
                rounds[draw() % kThreads].push_back(i);
                any = true;
            }
        if (!any || fault.set.load()) break;
        start.arrive_and_wait();
        end.arrive_and_wait();
    }
    stop.store(true);
    start.arrive_and_wait();
    for (auto& t : pool) t.join();
    INFO(fault.what);
    REQUIRE_FALSE(fault.set.load());
    b.check(want);
}

// Neighbours made, closed and brought back at another rate between a core's
// blocks, on the thread that renders it.
TEST_CASE("independence: every scenario beside the others, with neighbours opening, closing and coming back",
          "[reference][independence]") {
    const auto want = alone();
    Beside b;
    std::vector<std::unique_ptr<Lane>> neighbours;
    int round = 0;
    for (bool going = true; going; ++round) {
        going = false;
        for (auto& s : b.scenarios) going = s.step() || going;
        switch (round % 4) {
        case 0: neighbours.push_back(std::make_unique<Lane>(kBesideBulkBytes)); break;
        case 1: if (!neighbours.empty()) neighbours.back()->comeBack(96000.0); break;
        case 2: for (auto& n : neighbours) n->tick(); break;
        case 3: if (neighbours.size() > 2) neighbours.erase(neighbours.begin()); break;
        }
    }
    b.check(want);
}

// The same churn on another thread while the cores render on theirs: one
// core's lifecycle calls overlapping another's blocks.
TEST_CASE("independence: every scenario beside the others, with neighbours churned on another thread meanwhile",
          "[reference][independence]") {
    const auto want = alone();
    Beside b;
    std::atomic<bool> done{false};
    Fault fault;
    std::thread churn([&] {
        try {
            std::vector<std::unique_ptr<Lane>> neighbours;
            for (int round = 0; !done.load(); ++round) {
                switch (round % 4) {
                case 0: neighbours.push_back(std::make_unique<Lane>(kBesideBulkBytes)); break;
                case 1: if (!neighbours.empty()) neighbours.back()->comeBack(96000.0); break;
                case 2: for (auto& n : neighbours) n->tick(); break;
                case 3: if (neighbours.size() > 2) neighbours.erase(neighbours.begin()); break;
                }
            }
        } catch (...) {
            fault.note("the churning thread");
        }
    });
    for (bool going = true; going;) {
        going = false;
        for (auto& s : b.scenarios) going = s.step() || going;
    }
    done.store(true);
    churn.join();
    INFO(fault.what);
    REQUIRE_FALSE(fault.set.load());
    b.check(want);
}
