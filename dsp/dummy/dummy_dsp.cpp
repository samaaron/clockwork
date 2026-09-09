// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * dummy_dsp.cpp — a placeholder DSP, meant to be deleted.
 *
 * This exists so clockwork can build, test and make a noise with no real
 * engine attached. It is NOT a reference implementation of anything musical:
 * there is no graph, no voice allocation, no definitions. It is the smallest
 * thing that can honestly satisfy dsp_api.h.
 *
 * It emits signal rather than silence, and that is a testing decision rather
 * than a whim: silence cannot distinguish a working audio path from an
 * unwired one, because a test asserting zeros passes just as happily when
 * nothing is connected at all.
 *
 * TWO GENERATORS AND AN ECHO, AND WHY
 *
 *   PULSE (default) — n ms of signal every m ms, 10 ms every 500 ms by
 *   default, so it reads as a 2 Hz click track. This is the timing
 *   instrument. Every edge is a function of the ABSOLUTE frame index, so a
 *   test can assert that the k-th pulse begins at exactly k * period frames,
 *   whatever block size the host chose and however clockwork sliced it.
 *   Drift shows up as an edge in the wrong place; a dropped block shows up as
 *   a missing pulse; a duplicated one shows up as a doubled pulse. None of
 *   that is visible in a continuous tone.
 *
 *   TONE — 440 Hz. Proves the audio path carries samples, and is the
 *   pleasanter thing to listen to when checking that a real device works.
 *
 *   ECHO — output channel c is input channel c, verbatim. This is the INPUT
 *   instrument: the other two generate, so they can say nothing about whether
 *   input arrives, arrives on the right channel, or arrives at all beyond the
 *   first two. Feed each input channel a distinguishable signal, run blocks,
 *   and the outputs say exactly which input landed where. An input channel
 *   the host does not supply reads as silence rather than as stale memory.
 *
 * In both generators, the right channel is the left INVERTED: same waveform, same
 * instant, opposite sign. So left[i] + right[i] == 0 exactly, at every
 * sample, and that invariant catches inter-channel skew (one sample of delay
 * spikes the sum at every edge), duplication (sums to 2*left), and a swap or
 * lost sign (assert which channel is positive). Per-channel comparison alone
 * catches none of the first.
 *
 * Neither generator accumulates phase: output for frame N is a pure function
 * of N, so blocks join seamlessly and two runs agree exactly.
 *
 * Switch and configure over OSC — which also exercises the message path:
 *
 *   /dummy/pulse ,ii <width_ms> <period_ms>
 *   /dummy/tone
 *   /dummy/echo
 *   /dummy/ping                 -> /dummy/pong
 *   /dummy/when                 -> /dummy/when-was ,h <timetag>
 *   /dummy/log ,s <text>        -> DspHost::log, framed as /clockwork/debug
 *
 * IT ALSO REPORTS ON ITS REGIONS, and that is not about making a noise either.
 * DspConfig hands a guest seven blocks of shared memory with different sets of
 * promises attached — zeroed or not, published or private, readable or
 * writable, surviving a rebuild or cleared by one — and NONE of that is
 * observable from outside the guest. That is what makes them regions. So the placeholder answers for them,
 * and test_dsp_regions.cpp asserts on the answers:
 *
 *   /dummy/region/config          -> ,iiiii <bytes> <slot0..3>
 *   /dummy/region/geometry        -> ,iiii  <rate> <block> <in> <out>
 *   /dummy/region/memory          -> ,iii   <bytes> <was_zero> <read_write_ok>
 *   /dummy/region/clock           -> ,iii   <present> <millibeats> <playing>
 *   /dummy/region/window ,ii <v> <_>        publish v through shm_window
 *   /dummy/region/persist ,ii <v> <_>    -> /dummy/region/persist-gen ,i <gen>
 *   /dummy/region/persist-at-birth -> /dummy/region/persist-was
 *                                        ,iiii <bytes> <found> <gen> <value>
 *
 * The window has no report on purpose: a client polling those bytes from
 * outside is the region's entire reason to exist, so the test reads the arena
 * rather than asking. Asking would prove only that the guest remembers what it
 * wrote.
 *
 * IT ALSO MOVES BULK, one way down each lane. The inbox verb only reads and
 * the outbox verb only writes, which is the whole invariant stated as two
 * messages — a client can prove the guest saw what it staged, and read back
 * what the guest produced, without either side touching the other's region:
 *
 *   /dummy/inbox/hash ,ii <offset> <len> -> /dummy/inbox/is ,ii <read> <hash>
 *   /dummy/blob/produce ,ii <len> <seed>
 *                          -> /dummy/blob/at ,iii <offset> <wrote> <hash>
 *
 * Both report a hash rather than the bytes: a length alone cannot tell an
 * intact region from a shifted or truncated one, and echoing the payload back
 * would prove only that the message path can carry it.
 *
 * IT ALSO READS WHICH CHANNEL IS WHICH, and that is not about making a noise
 * at all. DspConfig::channel_map (shared_memory.h) says what every channel is:
 * the device's, a port's, or nothing. The placeholder keeps no copy and does
 * nothing with it — no processes, no modelling, no routing — because the map
 * IS the record. A rebuilt instance is caught up the moment it is handed the
 * pointer, with nothing to replay.
 *
 * It can be read back out for a test through:
 *
 *   /dummy/stream/dump          -> /dummy/stream/entry ,isiii per attachment
 *                                  (port, name, direction, first, chans)
 *
 * A count alone could not catch a swapped first_chan and chans, or a name
 * that arrived truncated, and those are exactly the mistakes an announcement
 * shape invites.
 *
 * AND IT SENDS TO EVENT SINKS, for the same reason it listens for stream
 * identity: so the path is proven end to end rather than only unit-tested.
 * dsp_api.h hands a DSP `open_sink` and `send_sink` (clockwork_event_sink.h), which
 * is how a DSP that is its own scheduler gets MIDI and OSC out without going
 * back through ingress. The placeholder is not a scheduler, so it does the
 * smallest honest version:
 *
 *   /dummy/sink/open ,sii <target> <kind> <capacity>
 *                               -> /dummy/sink/opened ,i <sink>   (0 = refused)
 *   /dummy/sink/send ,isi <sink> <text> <delay_ms>
 *   /dummy/sink/pending          -> /dummy/sink/still ,i <queued>
 *
 * A send is QUEUED by dsp_osc and emitted at the top of the next
 * dsp_process, and that is the whole point of the exercise rather than an
 * implementation detail: `when` is computed from THAT BLOCK'S block_time
 * (block_time + delay_ms, or 1 when the delay is zero), which is the exact
 * arithmetic a real scheduling DSP does, against the block the send is
 * actually emitted in. Sending from dsp_osc instead would prove nothing about
 * the audio thread, which is the thread the contract is really about.
 */

#include "dsp_api.h"
#include "shared_memory.h"   // readClockworkClock — how a DSP consumes the clock; this one never asks

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <new>

namespace {

constexpr double kToneHz  = 440.0;
constexpr float  kAmp     = 0.2f;
constexpr double kTwoPi   = 6.283185307179586476925286766559;

constexpr uint32_t kDefaultPulseWidthMs  = 10;
constexpr uint32_t kDefaultPulsePeriodMs = 500;

enum class Mode { Pulse, Tone, Echo };

// What clockwork told us about one port. Kept as flat bytes: the placeholder
// has nowhere to put a stream and nothing to do with one, so this is a record
// and not a model.
// One send the DSP has been asked to make, waiting for a block_time to date
// it by. Flat bytes, fixed size, no allocation: this is emitted from
// dsp_process, and dsp_process may not allocate.
constexpr uint32_t kMaxPendingSends = 8;
constexpr uint32_t kSendMax         = 64;

struct PendingSend {
    ClockworkSink  sink     = CLOCKWORK_SINK_NONE;
    uint32_t len      = 0;
    uint32_t delay_ms = 0;
    uint8_t  bytes[kSendMax] = {};
};

// What dsp_new found in each region it was handed, recorded at construction
// because that is the only moment some of it is observable. The arena is
// zeroed by clockwork before every build, and persistent deliberately is
// not — so "what was here when I arrived" is the whole answer to whether
// clockwork kept its side of DspConfig's promises, and it cannot be asked later.
struct RegionsAtBirth {
    uint32_t config_bytes    = 0;
    uint32_t config_slots[4] = {};   // the first four words the host wrote

    uint32_t memory_bytes    = 0;
    uint32_t memory_was_zero = 0;    // 1 = every byte zero, as promised
    uint32_t memory_rw_ok    = 0;    // 1 = wrote a pattern and read it back

    uint32_t bulk_bytes      = 0;    // DspConfig::arena_bulk — 0 on a single-region host
    uint32_t bulk_was_zero   = 0;    // 1 = every byte zero (1 when there is no bulk tier)
    uint32_t bulk_rw_ok      = 0;    // 1 = pattern read back   (1 when there is no bulk tier)
    uint32_t fp_env          = 0;    // DspConfig::fp_env, verbatim

    uint32_t window_bytes    = 0;

    uint32_t inbox_bytes     = 0;
    uint32_t outbox_bytes    = 0;
    uint32_t inbox_readable  = 0;    // 1 = first, middle and last byte read back
    uint32_t outbox_rw_ok    = 0;    // 1 = wrote a pattern into it and read it back

    uint32_t persist_bytes   = 0;
    uint32_t persist_found   = 0;    // 1 = a valid record was already there
    uint32_t persist_gen     = 0;    // its generation, 0 if none
    uint32_t persist_value   = 0;    // its payload, 0 if none
};

// The record the placeholder keeps in DspConfig::persistent. Stamped and
// checksummed because dsp_api.h says these bytes are hostile: a death mid-write
// leaves half an update, and believing half of one is the failure the stamp
// exists to prevent.
constexpr uint32_t kPersistMagic = 0x50455253u;   // 'PERS'
struct PersistRecord {
    uint32_t magic;
    uint32_t generation;
    uint32_t value;
    uint32_t check;      // magic ^ generation ^ value
};
inline uint32_t persistCheck(const PersistRecord& r) {
    return r.magic ^ r.generation ^ r.value;
}

// The record the placeholder publishes through DspConfig::shm_window. A client
// polls this while it is being written, so it carries a sequence number that is
// odd for the duration of a write — the standard seqlock, which is what makes a
// torn read detectable rather than merely unlikely.
constexpr uint32_t kWindowMagic = 0x57494e44u;   // 'WIND'
struct WindowRecord {
    uint32_t magic;
    uint32_t seq;
    uint32_t value;
    uint32_t writes;
};

struct DummyDsp {
    DspConfig config{};
    DspHost   host{};
    RegionsAtBirth born{};
    uint32_t  persist_gen = 0;    // generation of the record WE last wrote
    uint32_t  window_writes = 0;
    uint64_t  frame = 0;      // absolute frames rendered; the phase clock
    uint32_t  messages = 0;   // how many OSC messages have arrived

    Mode      mode = Mode::Pulse;
    uint64_t  pulse_width  = 0;   // frames, computed from ms at the sample rate
    uint64_t  pulse_period = 1;   // frames; never 0, so the modulo is safe

    PendingSend pending[kMaxPendingSends] = {};
    uint32_t    n_pending = 0;

    // The last inbound event dsp_osc was handed (DspInfo::wants_events), and
    // how many so far — reported by /dummy/events/get, so a test can see the
    // guest heard the keyboard.
    char     last_event[64] = {0};
    uint32_t n_events = 0;

    // ── Assets (dsp_api.h): what we were handed, and what we still hold ──
    // Recorded so a test can see, from the client's side, that the pointer
    // landed on its bytes: the checksum is over the inbox as the guest reads
    // it. Held ids are released on /dummy/asset/release, which is the only
    // way a placeholder ever lets go.
    struct AssetRec { uint32_t id, kind, bytes, channels, frames, rate_milli, sum; };
    AssetRec  last_asset {};
    bool      has_asset = false;
    static constexpr uint32_t kMaxHeld = 64;
    uint32_t  held[kMaxHeld] = {};
    uint32_t  n_held = 0;

    // Returns whether it was taken. A refusal leaves n_pending where it was,
    // which /dummy/sink/pending reports, so a caller that asked for more than
    // the placeholder can hold can see that it did.
    bool queueSend(ClockworkSink sink, const char* text, uint32_t delay_ms) {
        if (!text) return false;
        return queueSendBytes(sink, reinterpret_cast<const uint8_t*>(text),
                              static_cast<uint32_t>(std::strlen(text)), delay_ms);
    }
    bool queueSendBytes(ClockworkSink sink, const uint8_t* bytes, uint32_t n, uint32_t delay_ms) {
        if (n_pending >= kMaxPendingSends || !bytes) return false;
        if (n == 0 || n > kSendMax) return false;
        PendingSend& p = pending[n_pending++];
        p.sink = sink;
        p.len = n;
        p.delay_ms = delay_ms;
        std::memcpy(p.bytes, bytes, n);
        return true;
    }

    // ── The regions, read once, at the only moment they can be ──────────────
    void inspectRegions() {
        // guest_config: opaque to clockwork, ours to read. Take the first
        // four words so a test can assert on exactly what the host wrote and
        // on the fact that clockwork carried them unchanged.
        born.config_bytes = config.guest_config_bytes;
        if (config.guest_config && config.guest_config_bytes >= 16) {
            const auto* w = static_cast<const uint32_t*>(config.guest_config);
            for (int i = 0; i < 4; ++i) born.config_slots[i] = w[i];
        }

        // arena: clockwork promises it is ours alone and that it arrives
        // zeroed. Check both — the second by writing a pattern across the whole
        // region and reading it back, which is also the only way to find out
        // whether the bytes are really there.
        // The bulk lanes, probed the same way and for the same reason: a
        // region that is shorter than it claims shows up here rather than as
        // a corruption somewhere else. THE INBOX IS ONLY READ — it is const,
        // and a guest that writes it is the bug this arrangement removes.
        born.inbox_bytes  = config.inbox_bytes;
        born.outbox_bytes = config.outbox_bytes;
        if (config.inbox && config.inbox_bytes >= 16) {
            const auto* p = static_cast<const uint8_t*>(config.inbox);
            const uint32_t n = config.inbox_bytes;
            // All this can honestly claim is that the three reads happened
            // without faulting — the inbox's CONTENT is the client's business
            // and may be anything. Accumulated through a volatile sink so the
            // reads survive the optimiser; a bad pointer of the stated length
            // dies here rather than somewhere unrelated later.
            volatile uint32_t sink = 0;
            sink += p[0]; sink += p[n / 2u]; sink += p[n - 1u];
            born.inbox_readable = 1;
        }
        if (config.outbox && config.outbox_bytes >= 16) {
            auto* p = static_cast<uint8_t*>(config.outbox);
            const uint32_t n = config.outbox_bytes;
            const uint32_t probes[3] = { 0u, n / 2u, n - 1u };
            born.outbox_rw_ok = 1;
            for (uint32_t i = 0; i < 3; ++i) p[probes[i]] = static_cast<uint8_t>(0x5A + i);
            for (uint32_t i = 0; i < 3; ++i)
                if (p[probes[i]] != static_cast<uint8_t>(0x5A + i)) born.outbox_rw_ok = 0;
        }

        born.memory_bytes = config.arena_bytes;
        if (config.arena && config.arena_bytes >= 16) {
            auto* p = static_cast<uint8_t*>(config.arena);
            const uint32_t n = config.arena_bytes;
            born.memory_was_zero = 1;
            for (uint32_t i = 0; i < n; ++i)
                if (p[i] != 0) { born.memory_was_zero = 0; break; }
            // First byte, last byte and one in the middle: enough to catch a
            // region that is shorter than it says it is, without touching every
            // page of what may be tens of megabytes.
            const uint32_t probes[3] = { 0u, n / 2u, n - 1u };
            born.memory_rw_ok = 1;
            for (uint32_t i = 0; i < 3; ++i) p[probes[i]] = static_cast<uint8_t>(0xA5 + i);
            for (uint32_t i = 0; i < 3; ++i)
                if (p[probes[i]] != static_cast<uint8_t>(0xA5 + i)) born.memory_rw_ok = 0;
            for (uint32_t i = 0; i < 3; ++i) p[probes[i]] = 0;
        }

        // The bulk tier, by the same probes. A single-region host hands NULL/0
        // and that is a working state, reported as "nothing to check" rather
        // than as a failure — test arena_bulk for NULL, never the platform.
        born.bulk_bytes = config.arena_bulk_bytes;
        if (config.arena_bulk && config.arena_bulk_bytes >= 16) {
            auto* p = static_cast<uint8_t*>(config.arena_bulk);
            const uint32_t n = config.arena_bulk_bytes;
            born.bulk_was_zero = 1;
            for (uint32_t i = 0; i < n; ++i)
                if (p[i] != 0) { born.bulk_was_zero = 0; break; }
            const uint32_t probes[3] = { 0u, n / 2u, n - 1u };
            born.bulk_rw_ok = 1;
            for (uint32_t i = 0; i < 3; ++i) p[probes[i]] = static_cast<uint8_t>(0xB5 + i);
            for (uint32_t i = 0; i < 3; ++i)
                if (p[probes[i]] != static_cast<uint8_t>(0xB5 + i)) born.bulk_rw_ok = 0;
            for (uint32_t i = 0; i < 3; ++i) p[probes[i]] = 0;
        } else {
            born.bulk_was_zero = 1;
            born.bulk_rw_ok    = 1;
        }

        born.fp_env = static_cast<uint32_t>(config.fp_env);

        born.window_bytes = config.shm_window_bytes;

        // persistent: what the LAST instance left, if anything. Validated
        // before it is believed, exactly as dsp_api.h asks.
        born.persist_bytes = config.persistent_bytes;
        if (config.persistent && config.persistent_bytes >= sizeof(PersistRecord)) {
            PersistRecord r{};
            std::memcpy(&r, config.persistent, sizeof r);
            if (r.magic == kPersistMagic && r.check == persistCheck(r)) {
                born.persist_found = 1;
                born.persist_gen   = r.generation;
                born.persist_value = r.value;
                persist_gen        = r.generation;
            }
        }
    }

    // Write a value into the persistent region, one generation on from
    // whatever was there. Returns the generation written, or 0 for no region.
    uint32_t writePersist(uint32_t value) {
        if (!config.persistent || config.persistent_bytes < sizeof(PersistRecord))
            return 0;
        PersistRecord r{};
        r.magic      = kPersistMagic;
        r.generation = ++persist_gen;
        r.value      = value;
        r.check      = persistCheck(r);
        std::memcpy(config.persistent, &r, sizeof r);
        return r.generation;
    }

    // Publish a value through the window, under a seqlock. Returns the
    // sequence number left behind (always even), or 0 for no region.
    uint32_t publishWindow(uint32_t value) {
        if (!config.shm_window || config.shm_window_bytes < sizeof(WindowRecord))
            return 0;
        auto* w = static_cast<WindowRecord*>(config.shm_window);
        const uint32_t seq = (w->magic == kWindowMagic ? w->seq : 0u) + 1u;  // odd: writing
        w->seq    = seq;
        w->magic  = kWindowMagic;
        w->value  = value;
        w->writes = ++window_writes;
        w->seq    = seq + 1u;                                                // even: settled
        return seq + 1u;
    }

    void setPulse(uint32_t width_ms, uint32_t period_ms) {
        const double sr = config.sample_rate > 0.0 ? config.sample_rate : 48000.0;
        // Round to nearest frame once, here. Everything downstream counts
        // frames, so the pulse train can never drift against the audio clock
        // no matter how long it runs.
        uint64_t period = static_cast<uint64_t>(period_ms * sr / 1000.0 + 0.5);
        uint64_t width  = static_cast<uint64_t>(width_ms  * sr / 1000.0 + 0.5);
        if (period < 1) period = 1;
        if (width > period) width = period;   // fully on, rather than nonsense
        pulse_period = period;
        pulse_width  = width;
    }
};

// Sample value at an absolute frame, per channel. A pure function of `n`,
// which is what makes the output assertable.
//
// The right channel is the left's POLARITY INVERSION — same waveform, same
// instant, opposite sign — in both modes. That makes `left[i] + right[i] == 0`
// an exact invariant at every sample, and an exact invariant catches things
// that per-channel comparison cannot:
//
//   - inter-channel skew. One sample of delay on either channel makes the sum
//     spike to +/-2*kAmp at every pulse edge, where each channel on its own
//     still looks perfectly plausible.
//   - duplication. A channel copied rather than inverted sums to 2*left, not
//     zero.
//   - a swap, or a lost sign, by asserting which channel is positive.
//
// It is also the classic mono-fold check: sum the pair and it should vanish.
inline float sampleAt(const DummyDsp& d, uint64_t n, uint32_t channel) {
    const bool right = (channel % 2) != 0;
    const float sign = right ? -1.0f : 1.0f;
    if (d.mode == Mode::Tone) {
        const double sr = d.config.sample_rate;
        return sign * kAmp *
               static_cast<float>(std::sin(kTwoPi * kToneHz * (static_cast<double>(n) / sr)));
    }
    const uint64_t phase = n % d.pulse_period;
    return phase < d.pulse_width ? sign * kAmp : 0.0f;
}

// ── Just enough OSC to be configurable ──────────────────────────────────────
// Not a codec. Clockwork has a real one and a real DSP brings its own; this
// reads exactly the shapes documented at the top of the file.

inline uint32_t padded(uint32_t n) { return ((n + 4) / 4) * 4; }

bool addressIs(const uint8_t* b, uint32_t len, const char* addr) {
    const uint32_t n = static_cast<uint32_t>(std::strlen(addr));
    return len > n && std::memcmp(b, addr, n) == 0 && b[n] == '\0';
}

// Read two big-endian int32 arguments after a ",ii" type tag. Returns false
// unless the message really is that shape — a malformed message is ignored,
// not guessed at.
bool readTwoInts(const uint8_t* b, uint32_t len, const char* addr,
                 uint32_t* a, uint32_t* c) {
    // padded() TAKES strlen, NOT strlen + 1 — it already accounts for the
    // terminator (padded(n) rounds n+1 up to a multiple of 4), which is how
    // every other caller here uses it. Passing strlen + 1 double-counts, and
    // does so INVISIBLY unless the address length is 3 mod 4: for those, and
    // only those, n + 1 is already 4-aligned and the extra rounding steps a
    // whole word past the type tag. "/dummy/blob/produce" is 19 characters and
    // was the first address in this file to land on it — every earlier verb
    // happened to have a length that hid the arithmetic.
    const uint32_t astart = padded(static_cast<uint32_t>(std::strlen(addr)));
    if (len < astart + 4 + 8) return false;
    if (std::memcmp(b + astart, ",ii", 3) != 0) return false;
    const uint8_t* p = b + astart + 4;
    *a = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    *c = (uint32_t(p[4]) << 24) | (uint32_t(p[5]) << 16) | (uint32_t(p[6]) << 8) | uint32_t(p[7]);
    return true;
}

// A cursor over a message's arguments, for the two announcement shapes and
// nothing else. Every read is bounds-checked and any failure latches `ok`
// false, so a malformed announcement is ignored rather than half-recorded.
struct ArgReader {
    const uint8_t* b;
    uint32_t       len;
    uint32_t       off;
    bool           ok = true;

    ArgReader(const uint8_t* bytes, uint32_t n, uint32_t start)
        : b(bytes), len(n), off(start) {}

    const char* str() {
        if (!ok || off >= len) { ok = false; return ""; }
        const char* s = reinterpret_cast<const char*>(b + off);
        uint32_t i = off;
        while (i < len && b[i] != '\0') ++i;
        if (i >= len) { ok = false; return ""; }   // unterminated: not a string
        off += padded(i - off);
        return s;
    }

    uint32_t i32() {
        if (!ok || off + 4 > len) { ok = false; return 0; }
        const uint8_t* p = b + off;
        off += 4;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
               (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
    }

    // An OSC blob: int32 size, then that many bytes, padded to 4. Returned IN
    // PLACE — a blob is the one argument big enough that copying it to look at
    // it would be the expensive part, and the bytes are valid for the duration
    // of the call like every other argument.
    const uint8_t* blob(uint32_t* size_out) {
        const uint32_t n = i32();
        if (!ok || off + n > len) { ok = false; return nullptr; }
        const uint8_t* p = b + off;
        off += (n + 3u) & ~3u;
        if (size_out) *size_out = n;
        return p;
    }
};

// A blob's content, as a pure function of its position, so both sides compute
// it independently and a test asserting on it is asserting transport rather
// than agreement.
inline uint8_t blobByteAt(uint32_t seed, uint32_t i) {
    return static_cast<uint8_t>(seed * 31u + i * 7u + (i >> 8));
}

// FNV-1a over the bytes. Cheap, order-sensitive, and different for a blob that
// arrived truncated — which a length check alone would not catch when the
// length is carried in the same message as the bytes.
inline uint32_t blobHash(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

uint32_t putStr(uint8_t* buf, uint32_t off, const char* s) {
    const uint32_t n = static_cast<uint32_t>(std::strlen(s));
    const uint32_t b = padded(n);
    std::memcpy(buf + off, s, n);
    std::memset(buf + off + n, 0, b - n);
    return off + b;
}

uint32_t putInt(uint8_t* buf, uint32_t off, uint32_t v) {
    buf[off + 0] = static_cast<uint8_t>(v >> 24);
    buf[off + 1] = static_cast<uint8_t>(v >> 16);
    buf[off + 2] = static_cast<uint8_t>(v >> 8);
    buf[off + 3] = static_cast<uint8_t>(v);
    return off + 4;
}

// "/dummy/stream/entry" ,isiii <port> <name> <direction> <first> <chans> —
// one attachment, exactly as it was announced.
uint32_t writeEntry(uint8_t* buf, uint32_t cap, const ClockworkStreamEntry& e) {
    const char* addr = "/dummy/stream/entry";
    const uint32_t need = padded(static_cast<uint32_t>(std::strlen(addr)))
                        + padded(6)                       // ",isiii"
                        + 4 + padded(static_cast<uint32_t>(std::strlen(e.name)))
                        + 12;
    if (need > cap) return 0;
    uint32_t off = 0;
    off = putStr(buf, off, addr);
    off = putStr(buf, off, ",isiii");
    off = putInt(buf, off, e.port);
    off = putStr(buf, off, e.name);
    off = putInt(buf, off, e.direction);
    off = putInt(buf, off, e.first);
    off = putInt(buf, off, e.count);
    return off;
}

// The same, carrying one int64 argument. Used by /dummy/when to hand back the
// timetag dsp_osc was called with: whether a timed message reached the DSP
// with its time intact, or flattened to "now", is otherwise unobservable from
// outside — and it is exactly the difference between a harness that holds
// messages and one that cannot.
uint32_t writeReplyInt64(uint8_t* buf, uint32_t cap, const char* address, int64_t v) {
    const uint32_t need = padded(static_cast<uint32_t>(std::strlen(address)))
                        + padded(2)               // ",h"
                        + 8;
    if (need > cap) return 0;
    uint32_t off = putStr(buf, 0, address);
    off = putStr(buf, off, ",h");
    const uint64_t u = static_cast<uint64_t>(v);  // big-endian, as OSC requires
    for (int i = 0; i < 8; ++i)
        buf[off + i] = static_cast<uint8_t>((u >> (8 * (7 - i))) & 0xFFu);
    return off + 8;
}

// The same, carrying one int32. Used to hand back a sink handle and a pending
// count: both are numbers a test must read to say anything at all about the
// sink path, and neither is visible from outside any other way.
// "<address> ,si <text> <v>" — a string and an int, big-endian, padded.
uint32_t writeReplyStrInt(uint8_t* buf, uint32_t cap, const char* address,
                          const char* text, uint32_t v) {
    const uint32_t alen = padded(static_cast<uint32_t>(std::strlen(address)));
    const uint32_t tlen = padded(static_cast<uint32_t>(std::strlen(text)));
    const uint32_t need = alen + 4 + tlen + 4;
    if (need > cap) return 0;
    std::memset(buf, 0, need);
    std::memcpy(buf, address, std::strlen(address));
    std::memcpy(buf + alen, ",si", 3);
    std::memcpy(buf + alen + 4, text, std::strlen(text));
    uint8_t* p = buf + alen + 4 + tlen;
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
    return need;
}

uint32_t writeReplyInt32(uint8_t* buf, uint32_t cap, const char* address, uint32_t v) {
    const uint32_t need = padded(static_cast<uint32_t>(std::strlen(address)))
                        + padded(2)               // ",i"
                        + 4;
    if (need > cap) return 0;
    uint32_t off = putStr(buf, 0, address);
    off = putStr(buf, off, ",i");
    return putInt(buf, off, v);
}

// N int32 arguments. The region reports below answer with several numbers at
// once so that a test sees ONE consistent snapshot: asking for them one verb at
// a time would let a block run in between and compare figures from two moments.
uint32_t writeReplyInts(uint8_t* buf, uint32_t cap, const char* address,
                        const uint32_t* v, uint32_t n) {
    const uint32_t need = padded(static_cast<uint32_t>(std::strlen(address)))
                        + padded(1 + n)
                        + 4 * n;
    if (cap < need) return 0;
    uint32_t off = putStr(buf, 0, address);
    char tags[16];
    tags[0] = ',';
    for (uint32_t i = 0; i < n && i + 1 < sizeof tags; ++i) tags[i + 1] = 'i';
    tags[n + 1] = '\0';
    off = putStr(buf, off, tags);
    for (uint32_t i = 0; i < n; ++i) {
        buf[off++] = static_cast<uint8_t>((v[i] >> 24) & 0xFF);
        buf[off++] = static_cast<uint8_t>((v[i] >> 16) & 0xFF);
        buf[off++] = static_cast<uint8_t>((v[i] >>  8) & 0xFF);
        buf[off++] = static_cast<uint8_t>( v[i]        & 0xFF);
    }
    return off;
}

// One blob argument, generated straight into the caller's buffer. No staging
// copy: the blob IS the message, so building it anywhere else would mean
// holding two of them.
uint32_t writeReplyBlob(uint8_t* buf, uint32_t cap, const char* address,
                        uint32_t seed, uint32_t n) {
    const uint32_t need = padded(static_cast<uint32_t>(std::strlen(address)))
                        + padded(2)          // ",b"
                        + 4 + ((n + 3u) & ~3u);
    if (cap < need) return 0;
    uint32_t off = putStr(buf, 0, address);
    off = putStr(buf, off, ",b");
    buf[off++] = static_cast<uint8_t>((n >> 24) & 0xFF);
    buf[off++] = static_cast<uint8_t>((n >> 16) & 0xFF);
    buf[off++] = static_cast<uint8_t>((n >>  8) & 0xFF);
    buf[off++] = static_cast<uint8_t>( n        & 0xFF);
    for (uint32_t i = 0; i < n; ++i) buf[off + i] = blobByteAt(seed, i);
    const uint32_t padTo = (n + 3u) & ~3u;
    for (uint32_t i = n; i < padTo; ++i) buf[off + i] = 0;
    return off + padTo;
}

uint32_t writeReply(uint8_t* buf, uint32_t cap, const char* address) {
    // padded() already accounts for the NUL — writeEntry above relies on that
    // and so does putStr. This used to say padded(alen + 1), counting the NUL
    // twice and leaving four spare zero bytes before the type tag. Nothing
    // caught it because the only user is /dummy/pong, which carries no
    // arguments and so has nothing after the tag for anyone to fail to find.
    const uint32_t need = padded(static_cast<uint32_t>(std::strlen(address)))
                        + padded(1);              // ","
    if (need > cap) return 0;
    uint32_t off = putStr(buf, 0, address);
    return putStr(buf, off, ",");
}

} // namespace

extern "C" {

const DspInfo* dsp_describe(void) {
    static const DspInfo info = {
        /* name                    */ "dummy",
        /* version                 */ "0",
        /* holds_schedule          */ 0,   // let clockwork hold timed messages
        // Wants that EVERY host in this tree can meet — the smallest arena any
        // of them reserves is test_attach.cpp's 1 MB — declared so the check
        // that meets them runs on every boot rather than only in a guest that
        // is not here. On a single-region host the fast arena must hold both:
        // 640 KB of that 1 MB.
        /* arena_bytes_wanted      */ 512u * 1024u,
        /* arena_bulk_bytes_wanted */ 128u * 1024u,
        // Hear the host's inbound events (a keyboard, a pad) as they arrive,
        // so the path a self-directed guest takes them by is exercised here.
        /* wants_events            */ 1,
    };
    return &info;
}

struct Dsp* dsp_new(const DspConfig* config, const DspHost* host, const char** err) {
    if (!config || config->block_size == 0 || !(config->sample_rate > 0.0)) {
        if (err) *err = "dummy dsp: needs a sample rate and a block size";
        return nullptr;
    }
    auto* d = new (std::nothrow) DummyDsp();
    if (!d) {
        if (err) *err = "dummy dsp: out of memory";
        return nullptr;
    }
    d->config = *config;
    if (host) d->host = *host;
    d->setPulse(kDefaultPulseWidthMs, kDefaultPulsePeriodMs);
    d->inspectRegions();
    return reinterpret_cast<struct Dsp*>(d);
}

void dsp_free(struct Dsp* dsp) {
    delete reinterpret_cast<DummyDsp*>(dsp);
}

// Take an asset: record its shape and a checksum of the bytes AS THE GUEST
// READS THEM (through the pointer, in the inbox), and hold the id until asked
// to let go. A kind this placeholder does not know is refused, which is the
// path a real guest takes for bytes it cannot bind.
int dsp_asset(struct Dsp* dsp, const ClockworkAsset* asset) {
    auto* d = reinterpret_cast<DummyDsp*>(dsp);
    if (!d || !asset || asset->struct_bytes < sizeof(ClockworkAsset)) return 1;
    if (asset->kind != CLOCKWORK_ASSET_RAW && asset->kind != CLOCKWORK_ASSET_AUDIO_F32) return 2;
    if (d->n_held >= DummyDsp::kMaxHeld) return 3;
    const auto* p = static_cast<const uint8_t*>(asset->bytes);
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < asset->byte_count; ++i) { h ^= p[i]; h *= 16777619u; }
    d->last_asset = DummyDsp::AssetRec{ asset->id, asset->kind, asset->byte_count,
                                        asset->channels, asset->frames,
                                        static_cast<uint32_t>(asset->sample_rate * 1000.f + 0.5f), h };
    d->has_asset = true;
    d->held[d->n_held++] = asset->id;
    return 0;
}

void dsp_process(struct Dsp* dsp,
                 const float* const* in, uint32_t n_in,
                 float* const* out, uint32_t n_out,
                 uint32_t frames,
                 int64_t block_time) {
    auto* d = reinterpret_cast<DummyDsp*>(dsp);
    if (!d || !out) return;
    // `in` is read in Echo mode and ignored by the generators.
    // `block_time` is not ignored any more: the sink sends below are dated by
    // it. The audio itself is still a function of the frame index, which is
    // what keeps two runs byte-identical.

    // Anything /dummy/sink/send queued, dated by THIS block's start. An OSC
    // timetag is 32.32 fixed point, so a millisecond is 2^32/1000 units; a
    // zero delay becomes 1, OSC's "immediately", rather than a timetag that
    // happens to be now.
    for (uint32_t i = 0; i < d->n_pending; ++i) {
        const PendingSend& p = d->pending[i];
        if (!d->host.send_sink || p.sink == CLOCKWORK_SINK_NONE) continue;
        const int64_t when = p.delay_ms == 0
            ? 1
            : block_time + static_cast<int64_t>(
                  (static_cast<uint64_t>(p.delay_ms) * 4294967296ull) / 1000ull);
        d->host.send_sink(d->host.ctx, p.sink, p.bytes, p.len, when);
    }
    d->n_pending = 0;

    // The live counts, not the ceilings: Link peers and device switches move
    // them while running.
    const uint32_t channels = n_out;
    const uint32_t in_channels = n_in;

    for (uint32_t c = 0; c < channels; ++c) {
        float* dst = out[c];
        if (!dst) continue;

        if (d->mode == Mode::Echo) {
            // Straight through, channel for channel. A channel the host did
            // not supply is silence, never whatever was in the buffer before.
            const float* src = (in && c < in_channels) ? in[c] : nullptr;
            if (src) {
                std::memcpy(dst, src, frames * sizeof(float));
            } else {
                std::memset(dst, 0, frames * sizeof(float));
            }
            continue;
        }

        for (uint32_t i = 0; i < frames; ++i) {
            dst[i] = sampleAt(*d, d->frame + i, c);
        }
    }
    d->frame += frames;
}

// One element of a "#bundle": marker, 8-byte timetag, then [size][element]*.
// dsp_api.h hands a DSP "one OSC message OR BUNDLE", so a DSP that only ever
// compares the first bytes to an address silently ignores every bundle it is
// given — which this one did, and which made clockwork's whole timed path
// impossible to observe from out here. Unwrapping is one loop, and the
// placeholder exists to exercise the message path.
//
// The bundle's OWN timetag is not read: clockwork already handed it over as
// `when`, and a DSP reading it a second time would be two answers to one
// question. A nested bundle needs no special case: each element goes back
// through dsp_osc, which unwraps a bundle exactly as it unwrapped this one,
// and every element inherits the same `when`.
void dsp_osc(struct Dsp* dsp, const uint8_t* bytes, uint32_t len,
             int64_t when, uint32_t origin, int64_t block_time);

void dispatchBundle(struct Dsp* dsp, const uint8_t* b, uint32_t len,
                    int64_t when, uint32_t origin, int64_t block_time) {
    uint32_t off = 16;                       // "#bundle\0" + the 8-byte timetag
    while (off + 4 <= len) {
        const uint32_t n = (uint32_t(b[off]) << 24) | (uint32_t(b[off + 1]) << 16)
                         | (uint32_t(b[off + 2]) << 8) | uint32_t(b[off + 3]);
        off += 4;
        if (n == 0 || off + n > len) return;  // truncated or lying: stop, don't guess
        dsp_osc(dsp, b + off, n, when, origin, block_time);
        off += n;
    }
}

void dsp_osc(struct Dsp* dsp,
             const uint8_t* bytes,
             uint32_t len,
             int64_t when,
             uint32_t origin,
             int64_t block_time) {
    auto* d = reinterpret_cast<DummyDsp*>(dsp);
    if (!d || !bytes || len == 0) return;
    if (len >= 16 && std::memcmp(bytes, "#bundle", 8) == 0) {
        dispatchBundle(dsp, bytes, len, when, origin, block_time);
        return;
    }
    // `when` is not ignored any more: /dummy/when below reports it. Everything
    // else here is time-independent, which is why the placeholder can afford
    // to be — see holds_schedule, which is 0, so clockwork holds timed
    // messages in a build that has a store to hold them in.
    ++d->messages;

    // The one thing under /clockwork/ that DOES reach a DSP: an inbound
    // event, because this DSP asked for them (DspInfo::wants_events).
    // Remembered, not answered — a guest does not reply to a keyboard.
    if (len > 11 && std::memcmp(bytes, "/clockwork/", 11) == 0) {
        uint32_t a = 0; while (a < len && bytes[a] != '\0' && a < sizeof d->last_event - 1) ++a;
        std::memcpy(d->last_event, bytes, a);
        d->last_event[a] = '\0';
        ++d->n_events;
        return;
    }
    // /dummy/events/get — what the guest heard: ",si" the last address and the count.
    if (addressIs(bytes, len, "/dummy/events/get")) {
        if (d->host.emit_osc) {
            uint8_t reply[128];
            const uint32_t n = writeReplyStrInt(reply, sizeof reply, "/dummy/events",
                                                d->last_event, d->n_events);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }

    // These are NOT under /clockwork/ — that prefix is clockwork's own, claimed
    // before a top-level message ever reaches a DSP. (Inside a bundle it is not
    // claimed at all, so such an address does arrive here, and falls through
    // this dispatch unanswered — see clockwork_prefix.h.)
    // /dummy/asset/last — what the most recent dsp_asset carried.
    if (addressIs(bytes, len, "/dummy/asset/last")) {
        if (d->host.emit_osc) {
            uint8_t reply[96];
            const uint32_t v[7] = { d->last_asset.id, d->last_asset.kind, d->last_asset.bytes,
                                    d->last_asset.channels, d->last_asset.frames,
                                    d->last_asset.rate_milli, d->last_asset.sum };
            const uint32_t n = writeReplyInts(reply, sizeof reply, "/dummy/asset/last.reply", v, 7);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    // /dummy/asset/release ,i id — let go of a held asset (DspHost::asset_release).
    if (addressIs(bytes, len, "/dummy/asset/release")) {
        // ",i" after the padded address: one big-endian int32.
        uint32_t a = 0; while (a < len && bytes[a] != '\0') ++a;
        a = (a + 4) & ~3u;                       // past the address
        if (a + 4 <= len && bytes[a] == ',' && bytes[a + 1] == 'i') {
            a = (a + 4) & ~3u;                   // past ",i\0\0"
            if (a + 4 <= len) {
                const uint32_t id = (uint32_t(bytes[a]) << 24) | (uint32_t(bytes[a + 1]) << 16)
                                  | (uint32_t(bytes[a + 2]) << 8) | uint32_t(bytes[a + 3]);
                for (uint32_t i = 0; i < d->n_held; ++i) {
                    if (d->held[i] != id) continue;
                    d->held[i] = d->held[--d->n_held];
                    if (d->host.asset_release) d->host.asset_release(d->host.ctx, id);
                    break;
                }
            }
        }
        return;
    }
    if (addressIs(bytes, len, "/dummy/ping")) {
        if (d->host.emit_osc) {
            uint8_t reply[32];
            const uint32_t n = writeReply(reply, sizeof reply, "/dummy/pong");
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    // Hand back the timetag we were called with. dsp_api.h says a message
    // carries its own time and the DSP subtracts the block start itself, so
    // this is the placeholder doing the smallest possible version of that:
    // reporting the number rather than acting on it.
    if (addressIs(bytes, len, "/dummy/when")) {
        if (d->host.emit_osc) {
            uint8_t reply[48];
            const uint32_t n = writeReplyInt64(reply, sizeof reply,
                                               "/dummy/when-was", when);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    if (addressIs(bytes, len, "/dummy/tone")) {
        d->mode = Mode::Tone;
        return;
    }
    if (addressIs(bytes, len, "/dummy/echo")) {
        d->mode = Mode::Echo;
        return;
    }
    // What this guest can see of its own channels — read straight out of
    // DspConfig::channel_map, which clockwork keeps current. Nothing is
    // recorded here and nothing was announced: the map IS the record, so a
    // rebuilt instance is caught up the moment it is handed the pointer.
    if (addressIs(bytes, len, "/dummy/stream/dump")) {
        if (!d->host.emit_osc || !d->config.channel_map) return;
        const ClockworkChannelMapState* m = d->config.channel_map;
        for (uint32_t i = 0; i < CHANNEL_MAP_MAX_STREAMS; ++i) {
            if (m->streams[i].port == 0) continue;
            uint8_t buf[160];
            const uint32_t n = writeEntry(buf, sizeof buf, m->streams[i]);
            if (n) d->host.emit_osc(d->host.ctx, origin, buf, n);
        }
        return;
    }
    // ── Event sinks (dsp_api.h: open_sink / send_sink) ──────────────────────
    //
    // Sending is deferred to dsp_process, where block_time is. Opening is done
    // right here, and that is KNOWINGLY THE WRONG THREAD: dsp_api.h says
    // open_sink is control-thread-only because it allocates and may touch a
    // device, and it also says dsp_osc "may be called from the audio thread
    // (the ring drain runs there)" — which in clockwork it is.
    //
    // The placeholder does it anyway rather than hiding the problem, because
    // the problem is not the placeholder's. A DSP learns which destination to
    // open from a control message, and dsp_osc is the ONLY door it has; there
    // is no per-instance control tick in the boundary to defer the open to. So a
    // real DSP faces exactly this and has exactly these options: open in
    // dsp_new from something it was configured with, or open on the wrong
    // thread. That gap is reported, not papered over here — inventing an eighth
    // entry point on the sly is precisely what dsp_api.h says it will not do.
    // ── Blobs, both ways (dsp_api.h: dsp_osc in, DspHost::emit_osc out) ─────
    //
    // There is no separate blob door and there does not need to be one: a blob
    // is an OSC argument, so it arrives as an argument of the message dsp_osc
    // is handed and leaves as an argument of the message emit_osc is given.
    // These two verbs are that pair, and they are deliberately NOT an echo —
    // an echo would need the placeholder to hold a copy of the blob between
    // arrival and departure, which is the one thing neither direction requires.
    //
    // In: report the size and a hash, so a truncated or reordered blob is
    // distinguishable from an intact one (a length alone is not: the length
    // travels in the same message as the bytes).
    if (addressIs(bytes, len, "/dummy/blob")) {
        ArgReader r(bytes, len, padded(static_cast<uint32_t>(std::strlen("/dummy/blob"))));
        const char* tags = r.str();
        if (!r.ok || std::strcmp(tags, ",b") != 0) return;
        uint32_t n = 0;
        const uint8_t* p = r.blob(&n);
        if (!r.ok || !p) return;
        if (d->host.emit_osc) {
            const uint32_t v[2] = { n, blobHash(p, n) };
            uint8_t reply[64];
            const uint32_t m = writeReplyInts(reply, sizeof reply, "/dummy/blob-is", v, 2);
            if (m) d->host.emit_osc(d->host.ctx, origin, reply, m);
        }
        return;
    }
    // ── Bulk in: read what the client staged ────────────────────────────────
    //
    // The client wrote these bytes into DspConfig::inbox and sent a short
    // message saying where they are. THE GUEST ONLY READS THIS REGION — inbox
    // is a `const void*` precisely so that a guest which forgets fails to
    // compile rather than racing the client's next write.
    //
    // The offset is FROM THE REGION BASE, never a pointer: on native the
    // client has the segment mapped at its own address, so a pointer would be
    // meaningless to it in a way that still reads as valid.
    //
    // Reporting a size and a hash rather than echoing the bytes keeps the
    // reply small and still tells a truncated or shifted staging area from an
    // intact one.
    if (addressIs(bytes, len, "/dummy/inbox/hash")) {
        uint32_t offset = 0, want = 0;
        if (!readTwoInts(bytes, len, "/dummy/inbox/hash", &offset, &want)) return;
        if (!d->host.emit_osc) return;

        // Subtraction form: offset + want would wrap and let a crafted pair
        // past a naive bound into a read outside the region.
        uint32_t read = 0, hash = 0;
        if (d->config.inbox && offset <= d->config.inbox_bytes
                            && want <= d->config.inbox_bytes - offset) {
            const auto* p = static_cast<const uint8_t*>(d->config.inbox) + offset;
            read = want;
            hash = blobHash(p, want);
        }
        const uint32_t v[2] = { read, hash };
        uint8_t reply[80];
        const uint32_t m = writeReplyInts(reply, sizeof reply, "/dummy/inbox/is", v, 2);
        if (m) d->host.emit_osc(d->host.ctx, origin, reply, m);
        return;
    }

    // ── Bulk out: the inverse, through the other region ─────────────────────
    //
    // The guest writes bytes into DspConfig::outbox and sends a short message
    // saying where they are; a client reads them out on its own thread. Same
    // addressing as the inbox, opposite direction, and a different region.
    if (addressIs(bytes, len, "/dummy/blob/produce")) {
        uint32_t want = 0, seed = 0;
        if (!readTwoInts(bytes, len, "/dummy/blob/produce", &want, &seed)) return;
        if (!d->host.emit_osc) return;

        // Offset 0 is this placeholder's whole allocator. A real guest has one
        // and would hand back wherever it placed the blob.
        const uint32_t offset = 0;
        uint32_t wrote = 0, hash = 0;
        if (d->config.outbox && want <= d->config.outbox_bytes) {
            auto* p = static_cast<uint8_t*>(d->config.outbox) + offset;
            for (uint32_t i = 0; i < want; ++i) p[i] = blobByteAt(seed, i);
            wrote = want;
            hash  = blobHash(p, want);
        }
        const uint32_t v[3] = { offset, wrote, hash };
        uint8_t reply[80];
        const uint32_t m = writeReplyInts(reply, sizeof reply, "/dummy/blob/at", v, 3);
        if (m) d->host.emit_osc(d->host.ctx, origin, reply, m);
        return;
    }

    // Out: generate a blob of the requested size and send it.
    //
    // BUILT IN the arena, which is the point of having one. A blob big
    // enough to be interesting is too big for the stack and dsp_osc may not
    // reach the system allocator, so the region clockwork reserved and
    // promised to leave alone is exactly the right place — and using it here
    // means the outbound path is exercised at sizes a fixed buffer could not
    // reach.
    //
    // The reply to /dummy/blob/emit is the ACCEPTANCE, not the blob: emit_osc
    // answers whether the frame was queued, and a caller that asked for more
    // than the egress ring holds needs to be told that rather than left
    // waiting for a message clockwork dropped.
    if (addressIs(bytes, len, "/dummy/blob/emit")) {
        uint32_t want = 0, seed = 0;
        if (!readTwoInts(bytes, len, "/dummy/blob/emit", &want, &seed)) return;
        if (!d->host.emit_osc) return;

        uint32_t accepted = 0;
        const uint32_t need = padded(static_cast<uint32_t>(std::strlen("/dummy/blob-out")))
                            + padded(2) + 4 + ((want + 3u) & ~3u);
        if (d->config.arena && need <= d->config.arena_bytes) {
            auto* buf = static_cast<uint8_t*>(d->config.arena);
            const uint32_t m = writeReplyBlob(buf, d->config.arena_bytes,
                                              "/dummy/blob-out", seed, want);
            if (m) accepted = d->host.emit_osc(d->host.ctx, origin, buf, m) ? 1u : 0u;
        }
        uint8_t reply[64];
        const uint32_t m = writeReplyInt32(reply, sizeof reply,
                                           "/dummy/blob-emitted", accepted);
        if (m) d->host.emit_osc(d->host.ctx, origin, reply, m);
        return;
    }

    // The third destination a message can leave by. emit_osc answers a
    // client, send_sink reaches an external endpoint, and this reaches the
    // host's diagnostic channel — no origin, no handle, no reply. A DSP that
    // wants to say something to a human has nowhere else to say it.
    if (addressIs(bytes, len, "/dummy/log")) {
        ArgReader r(bytes, len, padded(static_cast<uint32_t>(std::strlen("/dummy/log"))));
        const char* tags = r.str();
        const char* text = r.str();
        if (!r.ok || std::strcmp(tags, ",s") != 0) return;
        // Level 6 = info, per dsp_api.h's syslog severities. The placeholder
        // does this from dsp_osc, which in clockwork is the audio thread —
        // knowingly, and for the same reason it opens sinks there: a DSP has
        // no other door. dsp_api.h says log is not RT-safe, and that mismatch
        // is reported rather than hidden. See the note at /dummy/sink/open.
        if (d->host.log) d->host.log(d->host.ctx, 6, text);
        return;
    }

    // ── The regions (dsp_api.h: guest_config, arena, inbox, outbox,
    // shm_window, persistent, clock) ────────────────────────────────────────
    //
    // Every one of these reports rather than acts. A region is not observable
    // from outside the guest — that is what makes it a region — so the only
    // way a test can hold clockwork to its promises about them is to ask the
    // guest what it was given and what it found there.
    if (addressIs(bytes, len, "/dummy/region/config")) {
        if (d->host.emit_osc) {
            const uint32_t v[5] = { d->born.config_bytes,
                                    d->born.config_slots[0], d->born.config_slots[1],
                                    d->born.config_slots[2], d->born.config_slots[3] };
            uint8_t reply[96];
            const uint32_t n = writeReplyInts(reply, sizeof reply,
                                              "/dummy/region/config-is", v, 5);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    if (addressIs(bytes, len, "/dummy/region/memory")) {
        if (d->host.emit_osc) {
            const uint32_t v[3] = { d->born.memory_bytes,
                                    d->born.memory_was_zero,
                                    d->born.memory_rw_ok };
            uint8_t reply[80];
            const uint32_t n = writeReplyInts(reply, sizeof reply,
                                              "/dummy/region/memory-is", v, 3);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    // What the bulk lanes looked like on arrival. Sizes AND usability: a lane
    // that is NULL and a lane that is a bad pointer are different bugs, and
    // only the first is visible from a size alone.
    if (addressIs(bytes, len, "/dummy/region/lanes")) {
        if (d->host.emit_osc) {
            const uint32_t v[4] = { d->born.inbox_bytes,    d->born.outbox_bytes,
                                    d->born.inbox_readable, d->born.outbox_rw_ok };
            uint8_t reply[96];
            const uint32_t n = writeReplyInts(reply, sizeof reply,
                                              "/dummy/region/lanes-is", v, 4);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    // Publish a value through the window. The test reads the bytes back from
    // the arena itself, not from a reply — a client polling shared memory is
    // the whole point of the region, and a reply would prove only that the DSP
    // remembers what it was told.
    if (addressIs(bytes, len, "/dummy/region/window")) {
        uint32_t value = 0, unused = 0;
        if (readTwoInts(bytes, len, "/dummy/region/window", &value, &unused))
            d->publishWindow(value);
        return;
    }
    // Write a value into the persistent region, and report the generation it
    // landed at. Survival is asserted by rebuilding and asking /dummy/region/
    // persist-at-birth, below.
    if (addressIs(bytes, len, "/dummy/region/persist")) {
        uint32_t value = 0, unused = 0;
        if (!readTwoInts(bytes, len, "/dummy/region/persist", &value, &unused))
            return;
        const uint32_t gen = d->writePersist(value);
        if (d->host.emit_osc) {
            uint8_t reply[64];
            const uint32_t n = writeReplyInt32(reply, sizeof reply,
                                               "/dummy/region/persist-gen", gen);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    // The bulk tier and the floating-point environment, as handed over. A
    // single-region host reports 0 bytes and "nothing to check" for the tier;
    // fp_env is whatever the host declared, verbatim, so a test can hold the
    // declaration to the guest.
    if (addressIs(bytes, len, "/dummy/region/tiers")) {
        if (d->host.emit_osc) {
            const uint32_t v[4] = { d->born.bulk_bytes, d->born.bulk_was_zero,
                                    d->born.bulk_rw_ok, d->born.fp_env };
            uint8_t reply[96];
            const uint32_t n = writeReplyInts(reply, sizeof reply,
                                              "/dummy/region/tiers-is", v, 4);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    if (addressIs(bytes, len, "/dummy/region/persist-at-birth")) {
        if (d->host.emit_osc) {
            const uint32_t v[4] = { d->born.persist_bytes, d->born.persist_found,
                                    d->born.persist_gen,   d->born.persist_value };
            uint8_t reply[96];
            const uint32_t n = writeReplyInts(reply, sizeof reply,
                                              "/dummy/region/persist-was", v, 4);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    // The clock region, clockwork to guest. Reported in millibeats per minute so
    // it fits an int32 and a test can assert an exact figure — the region
    // carries an IEEE-754 bit pattern and readClockworkClock is the only sanctioned
    // way to turn it back into a number.
    if (addressIs(bytes, len, "/dummy/region/clock")) {
        if (d->host.emit_osc) {
            const ClockworkClockSnapshot c = readClockworkClock(d->config.clock);
            const uint32_t v[3] = { d->config.clock ? 1u : 0u,
                                    static_cast<uint32_t>(c.bpm * 1000.0 + 0.5),
                                    c.is_playing ? 1u : 0u };
            uint8_t reply[80];
            const uint32_t n = writeReplyInts(reply, sizeof reply,
                                              "/dummy/region/clock-is", v, 3);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    // What clockwork said the audio geometry is. Not a region — it is the
    // half of DspConfig that arrives as plain numbers — but it is the half
    // that used to make a round trip through the guest's config block, so it
    // is worth being able to ask about.
    if (addressIs(bytes, len, "/dummy/region/geometry")) {
        if (d->host.emit_osc) {
            const uint32_t v[4] = { static_cast<uint32_t>(d->config.sample_rate + 0.5),
                                    d->config.block_size,
                                    d->config.max_input_channels,
                                    d->config.max_output_channels };
            uint8_t reply[96];
            const uint32_t n = writeReplyInts(reply, sizeof reply,
                                              "/dummy/region/geometry-is", v, 4);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    if (addressIs(bytes, len, "/dummy/sink/open")) {
        ArgReader r(bytes, len, padded(static_cast<uint32_t>(std::strlen("/dummy/sink/open"))));
        const char* tags   = r.str();
        const char* target = r.str();
        const uint32_t kind = r.i32();
        const uint32_t cap  = r.i32();
        if (!r.ok || std::strcmp(tags, ",sii") != 0) return;
        ClockworkSink sink = CLOCKWORK_SINK_NONE;
        if (d->host.open_sink) {
            sink = d->host.open_sink(d->host.ctx, static_cast<ClockworkSinkKind>(kind),
                                     target, cap);
        }
        // Reported either way. CLOCKWORK_SINK_NONE is an answer — a refusal a caller
        // must see — and not an error to swallow.
        if (d->host.emit_osc) {
            uint8_t reply[48];
            const uint32_t n = writeReplyInt32(reply, sizeof reply,
                                               "/dummy/sink/opened", sink);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    if (addressIs(bytes, len, "/dummy/sink/send")) {
        // ",isi" — the bytes as a string, for a C caller; ",ibi" — as a blob,
        // for a client whose strings are UTF-8 and cannot spell a MIDI status
        // byte (every one is 0x80 or above).
        ArgReader r(bytes, len, padded(static_cast<uint32_t>(std::strlen("/dummy/sink/send"))));
        const char* tags = r.str();
        if (!r.ok) return;
        const uint32_t sink = r.i32();
        if (std::strcmp(tags, ",isi") == 0) {
            const char* text = r.str();
            const uint32_t delay_ms = r.i32();
            if (!r.ok) return;
            d->queueSend(sink, text, delay_ms);
        } else if (std::strcmp(tags, ",ibi") == 0) {
            uint32_t n = 0;
            const uint8_t* p = r.blob(&n);
            const uint32_t delay_ms = r.i32();
            if (!r.ok || !p) return;
            d->queueSendBytes(sink, p, n, delay_ms);
        }
        return;
    }
    if (addressIs(bytes, len, "/dummy/sink/pending")) {
        if (d->host.emit_osc) {
            uint8_t reply[48];
            const uint32_t n = writeReplyInt32(reply, sizeof reply,
                                               "/dummy/sink/still", d->n_pending);
            if (n) d->host.emit_osc(d->host.ctx, origin, reply, n);
        }
        return;
    }
    if (addressIs(bytes, len, "/dummy/pulse")) {
        uint32_t width_ms = 0, period_ms = 0;
        if (readTwoInts(bytes, len, "/dummy/pulse", &width_ms, &period_ms)) {
            d->mode = Mode::Pulse;
            d->setPulse(width_ms, period_ms);
        }
        return;
    }
}

} // extern "C"
