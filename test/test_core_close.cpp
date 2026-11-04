// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * test_core_close.cpp — closing a core while other threads use it
 * (docs/ENGINES.md): a close refuses new entries, wakes whoever waits
 * on the core, waits until no thread is inside it, then frees it. A core is
 * never freed under a thread that is using it. Each case here is a thread
 * using a core one way while another thread closes it; the sanitizer job
 * runs them too (ci.yml), where a freed read is loud.
 *
 * Here, in clockwork_attach_tests, because each case makes its cores and
 * closes them.
 */
#include "BootedCore.h"
#include "clockwork_client.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace {
using namespace core_test;
using namespace std::chrono_literals;

// A client over a core's arena, as a host in its process opens one.
ClockworkClient* openClient(ClockworkCore* core) {
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkClient* c = clockwork_client_open_memory(clockwork_lanes_base(core),
                                                      clockwork_arena_header(core)->arena_bytes, &st);
    REQUIRE(c != nullptr);
    return c;
}

// Whether `done` comes true within `bound`, looked at as often as a thread
// can; a case's wait on another thread, never a fixed sleep.
template <class Done>
bool within(std::chrono::milliseconds bound, Done&& done) {
    const auto until = std::chrono::steady_clock::now() + bound;
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::yield();
    }
    return true;
}
} // namespace

TEST_CASE("close: a client waiting on the core is woken and told the engine closed", "[attach][close]") {
    auto* core = new Core();
    ClockworkClient* client = openClient(core->core);
    std::atomic<bool> parked{false};
    // The waiter: parked for as long as it takes, which the close must end.
    std::promise<ClockworkStatus> answered;
    std::thread waiter([&] {
        uint32_t seen = 0;
        parked.store(true, std::memory_order_release);
        answered.set_value(clockwork_client_wait(client, &seen, CLOCKWORK_WAIT_FOREVER));
    });
    REQUIRE(within(2000ms, [&] { return parked.load(std::memory_order_acquire); }));
    // Parked in the wait itself, not merely about to be: the word counts it.
    REQUIRE(within(2000ms, [&] { return clockwork_core_said_waiters(core->core) > 0; }));

    delete core;   // the close

    auto status = answered.get_future();
    const bool returned = status.wait_for(2000ms) == std::future_status::ready;
    REQUIRE(returned);
    CHECK(status.get() == CLOCKWORK_E_CLOSED);
    waiter.join();
    clockwork_client_close(client);
}

TEST_CASE("close: a block racing the close renders nothing of a freed core, and is told the core is gone",
          "[attach][close]") {
    auto* core = new Core();
    ClockworkCore* const handle = core->core;
    std::atomic<uint32_t> rendered{0};
    std::atomic<bool> gone{false};
    // The host's render thread: block after block, as a device callback
    // would, until the core tells it there is no core.
    std::thread render([&] {
        for (uint32_t i = 0; i < 2'000'000 && !gone.load(std::memory_order_acquire); ++i) {
            const ClockworkTick r = tickHere(handle);
            if (r == CLOCKWORK_TICK_RENDERED) rendered.fetch_add(1, std::memory_order_relaxed);
            if (r == CLOCKWORK_TICK_NONE) gone.store(true, std::memory_order_release);
        }
    });
    REQUIRE(within(2000ms, [&] { return rendered.load(std::memory_order_relaxed) > 0; }));

    delete core;   // the close, under the blocks

    REQUIRE(within(2000ms, [&] { return gone.load(std::memory_order_acquire); }));
    render.join();
    CHECK(tickHere(handle) == CLOCKWORK_TICK_NONE);
}

TEST_CASE("close: two closes at once end the core once, and both return", "[attach][close]") {
    auto* core = new Core();
    ClockworkCore* const handle = core->core;
    // A fresh handle is what both threads close; the helper's own close
    // afterwards is a third, of a core already gone.
    std::atomic<uint32_t> ready{0};
    auto closer = [&] {
        ready.fetch_add(1, std::memory_order_acq_rel);
        while (ready.load(std::memory_order_acquire) < 2) std::this_thread::yield();
        clockwork_core_close(handle);
    };
    std::thread a(closer), b(closer);
    a.join();
    b.join();
    CHECK(!clockwork_core_live(handle));
    CHECK(tickHere(handle) == CLOCKWORK_TICK_NONE);
    delete core;
}
