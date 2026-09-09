// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_dsp_blobs.cpp — large payloads across the boundary, both ways.
 *
 * THERE IS NO BLOB DOOR, and that is the claim under test. A blob is an OSC
 * argument, so it arrives as an argument of the message dsp_osc is handed and
 * leaves as an argument of the message DspHost::emit_osc is given. Neither
 * direction needs clockwork to know what the bytes are.
 *
 * That is worth pinning precisely because it was not always so. A definition
 * used to have a path of its own: a verb declared in DspInfo so clockwork
 * could recognise the message carrying one, an entry point asking the guest to
 * read a name out of the bytes, and a cache keyed on that name. Samples — the
 * other obvious large payload — never got one, and were left to the client to
 * re-send. Removing the special case removed no capability, because the
 * general path was already carrying the other half of the traffic.
 *
 * SO WHY IS THERE A CEILING AT ALL, if the mechanism is symmetric? Because a
 * message is the wrong vehicle for bulk in EITHER direction, and the boundary is
 * arranged so that saying so is enforceable.
 *
 * Every entry point in dsp_api.h is called on the audio thread, and on web
 * that is forced rather than chosen: the guest lives in the AudioWorklet, so
 * there is no other thread it can be reached from. An off-thread ingress door
 * is not a thing that could be built. What that means for bulk is not "give up
 * and copy on the audio thread" — it is that bulk must not travel as a message
 * at all. A client writes the bytes into DspConfig::inbox from its own
 * thread and sends a short message saying where they are; the guest gets an
 * OFFSET. js/clockwork.js says it outright: "bulk never rides OSC ingress in either
 * mode."
 *
 * These cases therefore pin the small path and its bound, not the bulk path:
 *
 *   IN   the drain hands dsp_osc a pointer into the ring, so clockwork
 *        copies nothing — but the bytes die with the call, so a guest keeping
 *        them memcpy's on the audio thread. A TIMED message is copied twice:
 *        once by clockwork into its 512 KB scheduler pool during the drain,
 *        once by the guest at fire time.
 *
 *   OUT  emit_osc copies into the 128 KB egress ring, on whichever thread
 *        called it — the audio thread, for a reply from dsp_osc.
 *
 * Ingress is the more generous of the two (768 KB) and so tolerates a large
 * payload rather than refusing it; egress refuses. The refusal is the useful
 * behaviour and the last case pins it.
 *
 * The cases below therefore assert an asymmetric policy on purpose: as a
 * message, a large blob must arrive intact, a small blob may leave, and a
 * large one must be REFUSED with the guest told so. Bulk in either direction
 * then appears only in the last two cases, which do not use a message to carry
 * it: one region for each direction, one writer each, and two integers on the
 * wire.
 */
#include "LanesFixture.h"
#include "OscTestUtils.h"

#include "lanes/lanes.h"
#include "shared_memory.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kClient = 0xb10b;

const lanes_test::EgressFrame* findAddress(
    const std::vector<lanes_test::EgressFrame>& frames, const char* address) {
    for (const auto& f : frames) {
        if (osc_test::parseAddress(f.data.data(),
                                   static_cast<uint32_t>(f.data.size())) == address)
            return &f;
    }
    return nullptr;
}

// The same function the placeholder computes its bytes from, written out
// independently. Sharing a header with the guest would make the assertion
// "a function equals itself"; transcribing it makes it "the bytes that arrived
// are the bytes that were sent".
uint8_t blobByteAt(uint32_t seed, uint32_t i) {
    return static_cast<uint8_t>(seed * 31u + i * 7u + (i >> 8));
}

uint32_t fnv1a(const uint8_t* p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

std::vector<uint8_t> makeBlob(uint32_t seed, uint32_t n) {
    std::vector<uint8_t> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = blobByteAt(seed, i);
    return v;
}

} // namespace

// ── In ──────────────────────────────────────────────────────────────────────

TEST_CASE("blobs: a blob arrives at the guest whole, as an ordinary argument",
          "[boundary][blobs]") {
    lanes_test::boot();
    lanes_test::drainRt();

    const uint32_t n = 64 * 1024;
    const auto payload = makeBlob(0xABCD, n);

    const auto msg = osc_test::messageWithBlob("/dummy/blob", payload.data(), payload.size());
    REQUIRE(lanes_test::ingress(msg.ptr(), msg.size(), kClient));

    const auto frames = lanes_test::tickUntil(8, "/dummy/blob-is");
    const auto* got = findAddress(frames, "/dummy/blob-is");
    REQUIRE(got != nullptr);
    const auto r = osc_test::parseReply(got->data.data(),
                                        static_cast<uint32_t>(got->data.size()));

    REQUIRE(static_cast<uint32_t>(r.argInt(0)) == n);
    // The hash, not just the length. A blob truncated on the way in would
    // still report whatever length the message claimed, because the length
    // travels in the same message as the bytes.
    REQUIRE(static_cast<uint32_t>(r.argInt(1)) == fnv1a(payload.data(), n));
}

TEST_CASE("blobs: a blob far larger than the egress ring still arrives",
          "[boundary][blobs]") {
    // ARRIVES, which is not the same as arrives well. This is delivered on the
    // audio thread like every other message, and a guest that kept it would
    // memcpy 160 KB there. The case fixes what the path DOES rather than
    // recommending it: a sample this size belongs in the inbox, written by
    // the client, with a short message carrying the pointer.
    lanes_test::boot();
    lanes_test::drainRt();

    // Bigger than OUT_BUFFER_SIZE, comfortably inside IN_BUFFER_SIZE. This is
    // the asymmetry stated as a fact rather than as a header constant: the
    // inbound path carries payloads the outbound path cannot.
    const uint32_t n = OUT_BUFFER_SIZE + 32 * 1024;
    REQUIRE(n < IN_BUFFER_SIZE / 2);
    const auto payload = makeBlob(0x1234, n);

    const auto msg = osc_test::messageWithBlob("/dummy/blob", payload.data(), payload.size());
    REQUIRE(lanes_test::ingress(msg.ptr(), msg.size(), kClient));

    const auto frames = lanes_test::tickUntil(8, "/dummy/blob-is");
    const auto* got = findAddress(frames, "/dummy/blob-is");
    REQUIRE(got != nullptr);
    const auto r = osc_test::parseReply(got->data.data(),
                                        static_cast<uint32_t>(got->data.size()));
    REQUIRE(static_cast<uint32_t>(r.argInt(0)) == n);
    REQUIRE(static_cast<uint32_t>(r.argInt(1)) == fnv1a(payload.data(), n));
}

// ── Out ─────────────────────────────────────────────────────────────────────

TEST_CASE("blobs: a SMALL blob leaves the same way it arrived",
          "[boundary][blobs]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // Small, and the size is the point of the case rather than an incidental
    // choice. A short blob out is an ordinary message — a handle, an
    // identifier, a bit of state — and costs the audio thread a memcpy of a
    // few hundred bytes. Bulk does not belong here at all; see the file
    // header, and the refusal case below for what clockwork does about it.
    const uint32_t n = 512;
    const uint32_t seed = 0x77;

    const auto ask = osc_test::message("/dummy/blob/emit",
                                       static_cast<int32_t>(n),
                                       static_cast<int32_t>(seed));
    REQUIRE(lanes_test::ingress(ask.ptr(), ask.size(), kClient));

    const auto frames = lanes_test::tickUntil(8, "/dummy/blob-emitted");
    const auto* ack = findAddress(frames, "/dummy/blob-emitted");
    REQUIRE(ack != nullptr);
    const auto a = osc_test::parseReply(ack->data.data(),
                                        static_cast<uint32_t>(ack->data.size()));
    REQUIRE(a.argInt(0) == 1);   // emit_osc accepted it

    const auto* out = findAddress(frames, "/dummy/blob-out");
    REQUIRE(out != nullptr);
    // Routed to the asking client, like any other reply — a blob is not a
    // different kind of message and does not get different routing.
    REQUIRE(out->sourceId == kClient);

    const auto r = osc_test::parseReply(out->data.data(),
                                        static_cast<uint32_t>(out->data.size()));
    const auto bytes = r.argBlob(0);
    REQUIRE(bytes.size() == n);
    REQUIRE(fnv1a(bytes.data(), n) == fnv1a(makeBlob(seed, n).data(), n));
}

TEST_CASE("blobs: a blob too big for the egress ring is refused, and the guest is told",
          "[boundary][blobs]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // Larger than the ring can ever hold, so this is not a full-ring race: it
    // cannot fit at any moment, with any cursor positions.
    const uint32_t n = OUT_BUFFER_SIZE + 16 * 1024;
    REQUIRE(n < lanes_test::kGuestMemoryBytes);   // the guest can still build it

    const auto ask = osc_test::message("/dummy/blob/emit",
                                       static_cast<int32_t>(n), 0x99);
    REQUIRE(lanes_test::ingress(ask.ptr(), ask.size(), kClient));

    const auto frames = lanes_test::tickUntil(8, "/dummy/blob-emitted");
    const auto* ack = findAddress(frames, "/dummy/blob-emitted");
    REQUIRE(ack != nullptr);
    const auto a = osc_test::parseReply(ack->data.data(),
                                        static_cast<uint32_t>(ack->data.size()));

    // TOLD NO, which is the whole case. emit_osc returned void until
    // 2026-09-01, so this answered 1 whether the frame was queued or dropped —
    // and the guest's only other signal, STATUS_BUFFER_FULL, is a global flag
    // it would have to poll and could not attribute to its own message.
    REQUIRE(a.argInt(0) == 0);

    // And nothing arrived, so the refusal is a refusal rather than a truncation.
    REQUIRE(findAddress(frames, "/dummy/blob-out") == nullptr);
}

// ── Bulk in, through the inbox ──────────────────────────────────────────────

TEST_CASE("blobs: a client stages a multi-megabyte payload and the guest reads it",
          "[boundary][blobs]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // THIS IS WHAT BULK-IN IS, and it is the case the cases above cannot be:
    // ingress tolerates a large blob but still copies it, and does so on the
    // audio thread. Here the client writes the bytes itself, from its own
    // thread, and the message that crosses is two integers long.
    //
    // 2 MB, which is more than twice the ingress ring and unremarkable here
    // for the same reason as in the outbound case: nothing about the size
    // touches a ring or the audio thread's copying.
    const uint32_t n = 2 * 1024 * 1024;
    const uint32_t seed = 0x3C;
    const uint32_t offset = 1024;   // NOT zero: an offset that is ignored
                                    // rather than resolved passes at base.
    REQUIRE(n > IN_BUFFER_SIZE);
    REQUIRE(offset + n <= lanes_test::kInboxBytes);

    // The client is the region's only writer. lanes_test::inbox() hands back a
    // mutable pointer and lanes_test::outbox() a const one, which is the same
    // asymmetry DspConfig states from the other side.
    uint8_t* base = lanes_test::inbox();
    REQUIRE(base != nullptr);
    const auto staged = makeBlob(seed, n);
    std::memcpy(base + offset, staged.data(), n);

    const auto ask = osc_test::message("/dummy/inbox/hash",
                                       static_cast<int32_t>(offset),
                                       static_cast<int32_t>(n));
    REQUIRE(lanes_test::ingress(ask.ptr(), ask.size(), kClient));

    const auto frames = lanes_test::tickUntil(8, "/dummy/inbox/is");
    const auto* is = findAddress(frames, "/dummy/inbox/is");
    REQUIRE(is != nullptr);
    const auto r = osc_test::parseReply(is->data.data(),
                                        static_cast<uint32_t>(is->data.size()));

    // The guest read the range it was named, and the bytes it found there are
    // the bytes the client wrote. A length alone would not say so: it travels
    // in the message, not in the region.
    REQUIRE(static_cast<uint32_t>(r.argInt(0)) == n);
    REQUIRE(static_cast<uint32_t>(r.argInt(1)) == fnv1a(staged.data(), n));

    // AND NOTHING LARGE CROSSED. Two integers went in and two came back; the
    // 2 MB never entered a ring in either direction.
    REQUIRE(ask.size() < 64);
    for (const auto& f : frames) {
        REQUIRE(f.data.size() < 256);
    }
}

// ── The inverse of loading a buffer ─────────────────────────────────────────

TEST_CASE("blobs: the guest produces a multi-megabyte blob and a client takes it out",
          "[boundary][blobs]") {
    lanes_test::boot();
    lanes_test::drainRt();

    // THIS IS WHAT BULK-OUT IS. Not a large OSC reply — the previous case
    // shows those refused — but the exact mirror of loading a sample: bytes in
    // DspConfig::outbox, and a short message saying where they are. The guest
    // is the only writer of that region; the client only ever reads it.
    //
    // 2 MB, which is sixteen times what the egress ring can carry and is not
    // remarkable here, because nothing about the size touches a ring or the
    // audio thread's copying. That is the property being tested; the hash is
    // just how it is checked.
    const uint32_t n = 2 * 1024 * 1024;
    const uint32_t seed = 0x5A;
    REQUIRE(n > OUT_BUFFER_SIZE);
    REQUIRE(n <= lanes_test::kOutboxBytes);

    const auto ask = osc_test::message("/dummy/blob/produce",
                                       static_cast<int32_t>(n),
                                       static_cast<int32_t>(seed));
    REQUIRE(lanes_test::ingress(ask.ptr(), ask.size(), kClient));

    const auto frames = lanes_test::tickUntil(8, "/dummy/blob/at");
    const auto* at = findAddress(frames, "/dummy/blob/at");
    REQUIRE(at != nullptr);
    const auto r = osc_test::parseReply(at->data.data(),
                                        static_cast<uint32_t>(at->data.size()));

    const uint32_t offset = static_cast<uint32_t>(r.argInt(0));
    const uint32_t length = static_cast<uint32_t>(r.argInt(1));
    const uint32_t hash   = static_cast<uint32_t>(r.argInt(2));
    REQUIRE(length == n);

    // The message carried an OFFSET, and the client resolves it against ITS
    // OWN base. On native those two mappings are at different addresses in
    // different processes, so a pointer would have read the wrong bytes
    // without faulting; here the fixture and the guest share an address space,
    // and the offset is still the only thing that crossed.
    const uint8_t* base = lanes_test::outbox();
    REQUIRE(base != nullptr);
    REQUIRE(offset <= lanes_test::kOutboxBytes);
    REQUIRE(length <= lanes_test::kOutboxBytes - offset);

    const auto expected = makeBlob(seed, n);
    REQUIRE(fnv1a(base + offset, length) == hash);
    REQUIRE(std::memcmp(base + offset, expected.data(), n) == 0);

    // AND IT DID NOT GO THROUGH EGRESS. The only frames on the ring are the
    // short "here is where it is" reply — 2 MB of payload never entered it,
    // which is the difference between this case and the refused one above.
    for (const auto& f : frames) {
        REQUIRE(f.data.size() < 256);
    }
}
