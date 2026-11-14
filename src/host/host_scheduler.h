// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    clockwork
    Copyright (c) 2025 Sam Aaron


    Host-side scheduling logic for the standalone scheduler: parse the control
    vocabulary into timed events, hold them in the generic Scheduler, and at tick
    time dispatch due events through clockwork's route table — the same
    scheduler core and fire loop (clockwork_fire_due) the engine runs, with the host's
    osc/midi backends in place of the DSP default. Decoupled from sockets so it
    is unit-testable: ingest() may be called from a recv thread (it only locks
    the assets area and wakes the scheduler thread), tick() is called from the
    single scheduler thread that owns the core, which sleeps between ticks
    until nextDue() or an arrival.
*/

#pragma once

// The standalone scheduler host exists to hold timed messages; with no store
// there is nothing for it to be. Its CMake target is only defined with the
// store, so this guard is what an accidental include from a
// CLOCKWORK_SCHEDULER=0 build hits: a clear error rather than a wall of them.
#if !CLOCKWORK_SCHEDULER
#error "host_scheduler.h requires CLOCKWORK_SCHEDULER=1 (the standalone scheduler host is the timed queue)"
#endif

#include "clockwork_sys.h"
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include "osc_reader.h"
#include "scheduler/Scheduler.h"
#include "scheduler/schedule_parse.h"
#include "scheduler/fire_due.h"
#include "OscIngress.h"
#include "workers/wait_word.h"
#include "clock.h"

#include <algorithm>
#include <chrono>
#include <stop_token>

namespace clockwork_host {

class HostScheduler {
public:
    explicit HostScheduler(ClockworkSysRoutes& routes) : mRoutes(routes) {}

    // Parse one control message and queue it. Thread-safe (locks the assets area only);
    // safe to call from the ingress recv thread. Unknown/malformed messages are
    // ignored.
    void ingest(const uint8_t* data, size_t len) {
        OscReader r(data, len);
        if (!r.ok()) return;
        const char* addr = r.address();

        if (std::strcmp(addr, CLOCKWORK_SYS("schedule")) == 0) {
            // "/clockwork/schedule <timetag> <inner blob>": store the inner message; the
            // outbound side routes it by address (/clockwork/osc/send or /clockwork/midi/*).
            SchedulePacket sp = clockwork_parse_schedule(data, static_cast<uint32_t>(len));
            if (sp.ok)
                push(Command{Command::Schedule, sp.when, SCHED_TAG_DEFAULT,
                             std::vector<uint8_t>(sp.blob, sp.blob + sp.blobLen)});
        } else if (std::strcmp(addr, CLOCKWORK_SYS("sched/flush")) == 0) {
            uint32_t tag = 0;  // empty/missing tag = flush all
            const char* t;
            if (r.peekType() == 's' && r.readString(t) && *t) tag = sched_tag_hash(t, std::strlen(t));
            push(Command{Command::Flush, {}, tag, {}});
        }
    }

    // Apply queued commands, then dispatch every event due at/through `now`
    // through clockwork's route table — the same fire loop the engine runs
    // (clockwork_fire_due),
    // with the host's osc/midi backends in place of the DSP default. Single
    // thread only (the core is not shared). No reply path → no call ctx.
    void tick(clockwork::Timetag now) {
        std::vector<Command> batch;
        { std::lock_guard<std::mutex> lk(mMx); batch.swap(mAssets); }
        for (auto& c : batch) {
            if (c.kind == Command::Flush) {
                mCore.flush(c.tag);
            } else {
                mCore.add(c.when, c.tag, c.bytes.data(), static_cast<uint32_t>(c.bytes.size()));
            }
        }
        clockwork_fire_due(mCore, now, /*blockTime*/ clockwork::Timetag(),
            [this](const uint8_t* d, uint32_t n, clockwork::Timetag, clockwork::Timetag) {
                mRoutes.ingest(d, n, /*callCtx*/ nullptr);
            });
    }

    // Returns the timetag of the earliest held event as of the last tick(), if
    // one is held. Tick thread only.
    std::optional<clockwork::Timetag> nextDue() const { return mCore.nextTime(); }

    // ingest() bumps this for every message it queues. The tick thread waits
    // on it between ticks, with nextDue() as the deadline.
    ClockworkWaitWord& arrivals() { return mArrivals; }

    // The longest the tick thread sleeps while it holds an event. Timetags
    // use the wall clock, which a user or a first NTP sync can set, but the
    // sleep uses the steady clock. This bound is how soon the thread notices
    // a wall clock set forward.
    static constexpr std::chrono::seconds kWallClockFollow{1};

    // Runs the tick thread until `stop`: tick at the wall clock's time
    // (`wall` returns the current timetag), then sleep until the next event
    // is due or a message arrives (run_worker).
    template <class Wall = clockwork::Timetag (*)()>
    void run(const std::stop_token& stop, Wall wall = &osc_now) {
        clockwork::run_worker(mArrivals, stop, [this, &wall] {
            // Read both clocks together, before the tick, so the sleep is
            // measured from the moment `now` names, not from after the sends.
            const auto steady = std::chrono::steady_clock::now();
            const clockwork::Timetag now = wall();
            tick(now);
            const auto next = nextDue();
            if (!next) return clockwork::NextPass::woken();
            const std::chrono::microseconds until =
                std::min<std::chrono::microseconds>(osc_interval(*next - now), kWallClockFollow);
            return clockwork::NextPass::until(steady + until);
        });
    }

    int pending() const { return mCore.size(); }

private:
    static constexpr int kSlots    = 256;
    static constexpr int kDataPool = kSlots * 1024;
    using Core = Scheduler<kSlots, kDataPool>;

    struct Command {
        enum Kind { Schedule, Flush } kind;
        clockwork::Timetag   when;
        uint32_t             tag;
        std::vector<uint8_t> bytes;
    };

    void push(Command&& c) {
        {
            std::lock_guard<std::mutex> lk(mMx);
            mAssets.push_back(std::move(c));
        }
        clockwork::bump(mArrivals);
    }

    Core                 mCore;
    ClockworkSysRoutes&        mRoutes;
    std::mutex           mMx;
    std::vector<Command> mAssets;
    ClockworkWaitWord    mArrivals;
};

}  // namespace clockwork_host
