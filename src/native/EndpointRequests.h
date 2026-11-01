// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * EndpointRequests.h — lets a worker that feeds the audio thread ask the
 * audio thread to wake it, and the audio thread answer.
 *
 * Only the audio thread sees some of the things such a worker waits for:
 * room in a ring the worker fills, or a block having run. Waking the worker
 * every block would wake it for nothing most of the time, and never waking
 * it would leave it polling on a timer. So the worker asks, and the first
 * block that can answer wakes it, once.
 *
 * After a pass that did nothing, the worker asks (ask) and sleeps on its
 * wake word (run_worker). After a new request it makes one more pass first:
 * a block that ran between the worker's look and its ask found no request to
 * answer, so that pass sees the block's room instead.
 *
 * Each block, the audio thread answers (answer) and, if that takes the
 * request, bumps the worker's word. With nothing asked that costs one load.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace clockwork {

class EndpointRequests {
public:
    static constexpr uint32_t kBlock = 1;   // the next block
    static constexpr uint32_t kRoom  = 2;   // room in one of the pairs asked about

    // The worker states what would let it do something: `wants`, and with
    // kRoom a bit for each pair of lanes it found full. 0 withdraws the
    // request. Returns true if the request changed, and then the worker
    // passes again before it sleeps.
    bool ask(uint32_t wants, uint32_t pairs) noexcept {
        const uint32_t hadPairs = mPairs.exchange(pairs, std::memory_order_seq_cst);
        const uint32_t hadWants = mWants.exchange(wants, std::memory_order_seq_cst);
        return hadWants != wants || hadPairs != pairs;
    }

    // The audio thread calls this once per block. Returns true if this block
    // answers the request; the request is then taken, and the caller wakes the
    // worker. `roomIn(pair)` says whether that pair has room now.
    template <class RoomIn>
    bool answer(RoomIn&& roomIn) noexcept {
        const uint32_t wants = mWants.load(std::memory_order_seq_cst);
        if (wants == 0) return false;
        bool answered = (wants & kBlock) != 0;
        const uint32_t pairs = mPairs.load(std::memory_order_relaxed);
        for (uint32_t i = 0; !answered && i < 32; ++i)
            if (pairs & (1u << i)) answered = roomIn(i);
        return answered && mWants.exchange(0, std::memory_order_seq_cst) != 0;
    }

private:
    std::atomic<uint32_t> mWants{0};
    std::atomic<uint32_t> mPairs{0};
};

}  // namespace clockwork
