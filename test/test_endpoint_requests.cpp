// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_endpoint_requests.cpp — src/native/EndpointRequests.h: a worker that
 * feeds the audio thread asks to be woken for what only the audio thread
 * sees, and the block that answers wakes it, once. It also covers
 * WakeRequest (workers/wait_word.h), the same ask made of any other source,
 * such as a peer's buffers.
 *
 * The danger is a lost wake: the worker looks, finds the ring full, and
 * sleeps, while the room that came between its look and its sleep wakes
 * nobody. Two threads cannot be made to interleave on demand, so the cases
 * here run the two sides as steps — the real EndpointRequests and the real
 * wait word, one step at a time — through every interleaving there is, and
 * check that no schedule leaves the worker asleep with work it could do.
 */
#include <catch2/catch_test_macros.hpp>

#include "native/EndpointRequests.h"
#include "workers/wait_word.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using clockwork::EndpointRequests;

namespace {

// One run of the two sides. The worker is run_worker around a pass that
// looks for its condition and, finding none, asks; the audio thread makes
// the condition true and then runs one block, which answers.
struct Schedule {
    // What the worker waits for, and how it asks for it.
    uint32_t wants;
    uint32_t pairs;
    // Whether the worker passes again after a new request (the rule the
    // header states) or sleeps at once.
    bool relook;
    // Asked through a WakeRequest (wait_word.h) rather than EndpointRequests:
    // the source rings rather than answers.
    bool viaWakeRequest = false;

    EndpointRequests requests;
    clockwork::WakeRequest wake;
    ClockworkWaitWord word;
    bool condition = false;   // room in pair 0, or a block that dated its reads

    enum Pc { Read, Look, Ask, Sleep, Asleep, Done };
    Pc pc = Read;
    uint32_t seen = 0;
    int passes = 0;

    // The audio thread's steps, in order: the condition comes true, then the
    // block answers.
    int audioStep = 0;
    static constexpr int kAudioSteps = 2;

    bool workerCanStep() const {
        if (pc == Done) return false;
        if (pc == Asleep) return word.value.load() != seen;   // only a bump wakes it
        return true;
    }
    void workerStep() {
        switch (pc) {
            case Read:   seen = word.value.load(); pc = Look; break;
            case Look:   ++passes; pc = condition ? Done : Ask; break;
            case Ask: {
                const bool fresh = viaWakeRequest ? wake.ask() : requests.ask(wants, pairs);
                pc = (fresh && relook) ? Read : Sleep;
                break;
            }
            case Sleep:  pc = word.value.load() != seen ? Read : Asleep; break;
            case Asleep: pc = Read; break;
            case Done:   break;
        }
    }
    void audioStepNext() {
        if (audioStep == 0) {
            condition = true;
        } else if (viaWakeRequest) {
            wake.ring(word);
        } else if (requests.answer([this](uint32_t pair) { return pair == 0 && condition; })) {
            clockwork::bump(word);
        }
        ++audioStep;
    }
};

// Every interleaving of the two sides, depth first: `path` is the steps so
// far, W for the worker and A for the audio thread, replayed onto fresh state
// (the atomics cannot be copied). Collects the schedules that end with the
// worker asleep while it has something to do.
void explore(uint32_t wants, uint32_t pairs, bool relook, bool viaWakeRequest, const std::string& path,
             std::vector<std::string>& lost) {
    Schedule s{ wants, pairs, relook, viaWakeRequest };
    for (const char c : path) {
        if (c == 'W') s.workerStep();
        else s.audioStepNext();
    }
    const bool worker = s.workerCanStep() && s.passes < 8;
    const bool audio = s.audioStep < Schedule::kAudioSteps;
    if (!worker && !audio) {
        if (s.pc == Schedule::Asleep && s.condition) lost.push_back(path);
        return;
    }
    if (worker) explore(wants, pairs, relook, viaWakeRequest, path + "W", lost);
    if (audio) explore(wants, pairs, relook, viaWakeRequest, path + "A", lost);
}

std::vector<std::string> lostWakes(uint32_t wants, uint32_t pairs, bool relook, bool viaWakeRequest = false) {
    std::vector<std::string> lost;
    explore(wants, pairs, relook, viaWakeRequest, "", lost);
    return lost;
}

}  // namespace

TEST_CASE("endpoint requests: a worker waiting for room loses no wake, in any interleaving",
          "[endpoint_requests]") {
    const auto lost = lostWakes(EndpointRequests::kRoom, 1u, true);
    INFO("first schedule that lost a wake: " << (lost.empty() ? "" : lost.front()));
    CHECK(lost.empty());
}

TEST_CASE("endpoint requests: a worker waiting for a block loses no wake, in any interleaving",
          "[endpoint_requests]") {
    const auto lost = lostWakes(EndpointRequests::kBlock, 0u, true);
    INFO("first schedule that lost a wake: " << (lost.empty() ? "" : lost.front()));
    CHECK(lost.empty());
}

TEST_CASE("endpoint requests: without the pass after a new request, a wake is lost",
          "[endpoint_requests]") {
    // The rule is not decoration: a worker that sleeps the moment it asks
    // misses a block that came between its look and its asking.
    CHECK_FALSE(lostWakes(EndpointRequests::kRoom, 1u, false).empty());
    CHECK_FALSE(lostWakes(EndpointRequests::kBlock, 0u, false).empty());
}

TEST_CASE("endpoint requests: a block answers a request once, and nothing asked is nothing answered",
          "[endpoint_requests]") {
    EndpointRequests r;
    const auto roomInZero = [](uint32_t pair) { return pair == 0; };
    CHECK_FALSE(r.answer(roomInZero));                    // nothing asked

    CHECK(r.ask(EndpointRequests::kRoom, 1u << 3));       // new: pass again
    CHECK_FALSE(r.ask(EndpointRequests::kRoom, 1u << 3)); // as it stood: sleep
    CHECK_FALSE(r.answer(roomInZero));                    // room, but not where it was asked about
    CHECK(r.answer([](uint32_t pair) { return pair == 3; }));
    CHECK_FALSE(r.answer([](uint32_t) { return true; }));   // taken

    CHECK(r.ask(EndpointRequests::kRoom, 1u));            // asked again after it was taken: new
    CHECK(r.ask(EndpointRequests::kRoom, 2u));            // other pairs: new
    CHECK(r.ask(0, 0));                                   // withdrawn: new
    CHECK_FALSE(r.answer([](uint32_t) { return true; }));
}

// The same schedules for a source that rings a WakeRequest (wait_word.h) —
// Link's thread with a peer's buffer — in place of the audio thread's answer.
TEST_CASE("wake request: a worker waiting on a source's ring loses no wake, in any interleaving",
          "[endpoint_requests][wake_request]") {
    const auto lost = lostWakes(0, 0, true, true);
    INFO("first schedule that lost a wake: " << (lost.empty() ? "" : lost.front()));
    CHECK(lost.empty());
    // And the pass after a new ask is what makes it so.
    CHECK_FALSE(lostWakes(0, 0, false, true).empty());
}

TEST_CASE("wake request: a ring with nothing asked wakes nobody, and one asked for wakes once",
          "[endpoint_requests][wake_request]") {
    ClockworkWaitWord word;
    clockwork::WakeRequest r;
    r.ring(word);
    CHECK(word.value.load() == 0);
    CHECK(r.ask());          // new: look again
    CHECK_FALSE(r.ask());    // as it stood: sleep
    r.ring(word);
    r.ring(word);            // taken by the first
    CHECK(word.value.load() == 1);
    CHECK(r.ask());
    r.withdraw();
    r.ring(word);
    CHECK(word.value.load() == 1);
}
