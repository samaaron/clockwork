// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_main.cpp — the test binary's entry point.
 *
 * JUCE wants its GUI/message-loop scaffolding initialised before any
 * AudioDeviceManager or Thread work (on Windows that is COM init), so the
 * session runs inside a ScopedJuceInitialiser_GUI.
 *
 * It also installs a global RT-allocation listener. The audio path sets a
 * thread-local guard (src/rt_alloc.h) and this binary's operator new/delete
 * overrides count allocations made while it is set. The listener resets the
 * counter before each test and reports at the end if any fired. It WARNS
 * rather than fails: the point is to catalogue offenders as they appear, not
 * to make an unrelated test red.
 */
#include "rt_alloc.h"
#include "DebugTail.h"

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#if CLOCKWORK_DEVICE
#include <juce_events/juce_events.h>
#endif

#include <cstdio>
#include <string>

namespace {

struct RTAllocListener : Catch::EventListenerBase {
    using Catch::EventListenerBase::EventListenerBase;

    void testCaseStarting(Catch::TestCaseInfo const&) override {
        rt_alloc::reset();
    }

    void testCaseEnded(Catch::TestCaseStats const& stats) override {
        // The [rt_alloc] tests manage their own counts and assert on them
        // explicitly; passive listening would double-count.
        if (stats.testInfo->tagsAsString().find("[rt_alloc]") != std::string::npos)
            return;
        const int64_t a = rt_alloc::g_allocs.load(std::memory_order_relaxed);
        const int64_t f = rt_alloc::g_frees.load(std::memory_order_relaxed);
        if (a > 0 || f > 0) {
            std::fprintf(stderr,
                "\n[rt-alloc] %s: %lld allocs / %lld frees on the audio thread\n",
                stats.testInfo->name.c_str(),
                static_cast<long long>(a), static_cast<long long>(f));
        }
    }
};

} // namespace

CATCH_REGISTER_LISTENER(RTAllocListener)

int main(int argc, char* argv[]) {
    // A fatal signal prints the last engine debug lines and a backtrace
    // (DebugTail.h); Catch2 reports first, then hands the signal back here.
    debug_tail::installFatalHandlers();
#if CLOCKWORK_DEVICE
    // Only the device layer needs a JUCE message loop. A deviceless build has
    // no JUCE at all, so there is nothing to initialise.
    juce::ScopedJuceInitialiser_GUI juceInit;
#endif
    return Catch::Session().run(argc, argv);
}
