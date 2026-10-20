// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * ring_marks.h — where in a ring the messages written for a generation end
 * (docs/RESTARTS.md, rule 1).
 *
 * A ring that carries messages for the guest — the in ring, a shm peer's
 * command ring — is kept across a rebuild, and what was in it for the old
 * instance is answered by refusal, not delivered to the next. A mark holds
 * the ring's sequence at the moment a generation's messages ended, the
 * generation they were written for, and why they are refused; a frame
 * sequenced below it is refused, naming that generation, by whichever drain
 * comes to it next. Several can wait for one drain — an instance built and
 * torn down again before anything drained leaves the mark of the generation
 * before it and its own — and a frame is refused for the earliest it falls
 * below.
 *
 * The marks belong to the ring's one consumer: set where its drain is not
 * running (on that thread, or with it stopped), so nothing here is atomic.
 * No allocation: a fixed handful, the two oldest folded together when full.
 */
#pragma once

#include <cstdint>

struct RingMark {
    uint32_t    below;        // the first sequence written after it
    uint32_t    generation;   // the generation its refusals name
    const char* reason;       // a CLOCKWORK_REFUSED_* phrase: static storage
};

class RingMarks {
public:
    // Everything written so far, below `below`, is refused for `reason`,
    // naming `generation`.
    void set(uint32_t below, uint32_t generation, const char* reason) {
        // Nothing written since the last: it would mark no message.
        if (mCount > 0 && mMarks[mCount - 1].below == below) return;
        // Full — eight replacements without a drain between them: the two
        // oldest become one, and what was written for the oldest generation
        // is refused naming the one after it. Answered still, which is what
        // a client waits on.
        if (mCount == kMarks) {
            for (uint32_t i = 1; i < kMarks; ++i) mMarks[i - 1] = mMarks[i];
            --mCount;
        }
        mMarks[mCount++] = RingMark { below, generation, reason };
    }

    // The mark a frame falls below, if any: the earliest, every mark it has
    // passed done with. A drain meets frames in sequence order, so a mark
    // once passed is passed for good. The signed delta stays correct across
    // uint32 sequence rollover (pending frames are always far fewer than
    // 2^31 apart).
    const RingMark* of(uint32_t seq) {
        uint32_t passed = 0;
        while (passed < mCount && static_cast<int32_t>(seq - mMarks[passed].below) >= 0) ++passed;
        if (passed > 0) {
            for (uint32_t i = passed; i < mCount; ++i) mMarks[i - passed] = mMarks[i];
            mCount -= passed;
        }
        return mCount > 0 ? &mMarks[0] : nullptr;
    }

    void clear() { mCount = 0; }

private:
    static constexpr uint32_t kMarks = 8;
    RingMark mMarks[kMarks] {};
    uint32_t mCount = 0;
};
