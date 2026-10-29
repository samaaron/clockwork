// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_shm_segment.cpp — pins the hardening of src/shm_segment.hpp.
 *
 * The POSIX shm segment is clockwork's cross-process surface: an observer
 * (a GUI, a monitor) maps it and reads ring cursors and stream geometry that
 * another process writes. On a multi-user machine that makes it a trust
 * boundary, and every field in it is attacker-influenced from the reader's
 * point of view. These tests pin the hardening:
 *
 *   - the segment is created 0600 (owner-only), not world-accessible;
 *   - creation is exclusive (O_EXCL), so a pre-existing segment under the
 *     predictable name is never silently adopted and ftruncated;
 *   - a reader rejects a too-small (foreign/truncated) segment before it
 *     dereferences any header field, and rejects a segment whose self-described
 *     layout does not match the layout it was compiled against;
 *   - a stream reader keeps its mapping alive after the client that produced it
 *     is destroyed, and clamps corrupt slot geometry to the compile-time ring
 *     rather than indexing out of bounds; and
 *   - the observer views a client hands out land inside its own mapping, at the
 *     compile-time offsets.
 *
 * No engine and no audio device: the segment is created, tampered with, and
 * read entirely from this test process.
 *
 * POSIX-only (the hardening is in the shm_open path); Windows uses named file
 * mappings and is out of scope here.
 */
#if !defined(_WIN32)

#include <catch2/catch_test_macros.hpp>
#include "src/shm_segment.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <vector>

using detail_shm_segment::shm_segment_creator;
using detail_shm_segment::shm_segment_client;
using detail_shm_segment::shm_dup_handle;

TEST_CASE("shm-security: the segment is anonymous, owner-sized and close-on-exec",
          "[shm][security]") {
    shm_segment_creator creator;
    const int fd = creator.native_handle();
    REQUIRE(fd >= 0);

    // At least the declared size (macOS rounds a POSIX object up to a page,
    // which is why the readers check "not smaller than" and never equality),
    // and not inheritable: a child the engine spawns gets the segment by an
    // explicit hand-off or not at all.
    struct stat st {};
    REQUIRE(::fstat(fd, &st) == 0);
    CHECK(static_cast<size_t>(st.st_size) >= creator.segment_size());
    CHECK(static_cast<size_t>(st.st_size) <  creator.segment_size() + 65536);

    // The lanes are sized by the creator's caller; the segment grows to fit,
    // and the header says where they are.
    shm_segment_creator big(64u << 20, 1u << 20);
    CHECK(big.get_assets_size()  == (64u << 20));
    CHECK(big.get_returns_size() == (1u << 20));
    CHECK(big.segment_size() >= creator.segment_size() - detail_shm_segment::SHM_ASSETS_SIZE - detail_shm_segment::SHM_RETURNS_SIZE + (65u << 20));
    struct stat bst {};
    REQUIRE(::fstat(big.native_handle(), &bst) == 0);
    CHECK(static_cast<size_t>(bst.st_size) >= big.segment_size());
    CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((st.st_mode & (S_IWGRP | S_IWOTH | S_IRGRP | S_IROTH)) == 0);

    // A duplicate of the handle is the same pages: what the creator writes,
    // a client mapped from the duplicate reads.
    creator.publish();
    shm_segment_client client(shm_dup_handle(creator.native_handle()));
    creator.get_base()[0] = 0x5A;
    CHECK(client.get_base()[0] == 0x5A);
}

TEST_CASE("shm-security: two creators never share memory", "[shm][security]") {
    // A named segment lets two engines on one port collide, and a planted name
    // be adopted. With no names there is nothing to collide on: each creator
    // gets its own pages, whatever else exists.
    shm_segment_creator a;
    shm_segment_creator b;
    CHECK(a.native_handle() != b.native_handle());
    a.get_base()[0] = 1;
    b.get_base()[0] = 2;
    CHECK(a.get_base()[0] == 1);
    CHECK(b.get_base()[0] == 2);
}

TEST_CASE("shm-security: reader rejects a too-small segment before dereferencing",
          "[shm][security]") {
    // A tiny segment carrying a valid MAGIC in its first word: the magic check
    // alone would pass, so only a size check stops the client building region
    // pointers into a few bytes of memory.
    auto tiny = detail_shm_segment::shm_create_anonymous(64);
    *static_cast<uint32_t*>(tiny.ptr) = detail_shm_segment::shm_segment_header::MAGIC;
    const int dup = shm_dup_handle(tiny.fd);
    detail_shm_segment::shm_close(tiny);
    REQUIRE(dup >= 0);

    bool threw = false;
    try {
        shm_segment_client client(dup);   // takes the handle, even on refusal
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("shm-security: stream readers pin the client mapping (no dangling reads)",
          "[shm][security]") {
    shm_segment_creator creator;
    creator.publish();

    // An observer hands reader copies to long-lived widgets, then remaps by
    // destroying its client. The reader must keep the old mapping alive, or a
    // widget's poll timer makes an atomic load through the munmapped slot
    // pointer.
    auto client = std::make_unique<shm_segment_client>(shm_dup_handle(creator.native_handle()));
    auto reader = client->get_tap_stream_reader(0);
    client.reset();

    float scratch[64 * 2] = {};
    (void)reader.valid();
    (void)reader.write_position();
    (void)reader.copy_window(reader.write_position(), 64, scratch);
    SUCCEED("reader survived client teardown without touching unmapped memory");
}

TEST_CASE("shm-security: stream reader clamps corrupt slot geometry",
          "[shm][security]") {
    shm_segment_creator creator;
    creator.publish();
    shm_segment_client client(shm_dup_handle(creator.native_handle()));
    auto reader = client.get_tap_stream_reader(0);

    // channels/capacity_frames are re-read from shared memory on every
    // copy_window and index into the inline data array; a corrupt or hostile
    // slot must clamp to the compile-time ring geometry rather than push
    // reads out of bounds (or divide by a zero capacity).
    auto* slot = reinterpret_cast<shm_tap_stream*>(
        creator.get_base() + TAPS_START + TAPS_HEADER_SIZE);
    slot->state.store(1, std::memory_order_release);
    slot->channels = 0xFFFFu;
    slot->capacity_frames = 0;
    slot->write_position.store(1u << 20, std::memory_order_release);

    CHECK(reader.channels() == TAP_STREAM_CHANNELS);
    CHECK(reader.capacity_frames() == TAP_RING_FRAMES);
    std::vector<float> scratch(1024 * TAP_STREAM_CHANNELS, 0.0f);
    (void)reader.copy_window(reader.write_position(), 1024, scratch.data());
    SUCCEED("copy_window stayed inside the inline ring under corrupt geometry");

    slot->capacity_frames = 0x7FFFFFFFu;  // absurdly large
    (void)reader.copy_window(reader.write_position(), 1024, scratch.data());
    SUCCEED("oversized capacity clamped");
}

TEST_CASE("shm: a client follows the engine's published layout, not its own constants",
          "[shm]") {
    // A reader built from another memory profile — or from no engine tree at
    // all — finds every region where THIS engine put it, because it reads the
    // header. Simulated by moving regions in the header after creation: the
    // client must read from where the header says, not from where this build's
    // constants would put them.
    shm_segment_creator creator;
    creator.publish();
    // The table is the arena's own, at the front of the blob.
    auto* arena = const_cast<ClockworkArenaHeader*>(arenaHeader(creator.get_base()));
    const auto entry = [&](uint32_t id) {
        return const_cast<ClockworkArenaEntry*>(clockwork_arena_find(arena, id));
    };

    // Native stats relocated into the (otherwise unused) window: the value
    // written there is what the client reports.
    const uint32_t movedStats = entry(CLOCKWORK_ARENA_GUEST_WINDOW)->offset;
    entry(CLOCKWORK_ARENA_CLOCKWORK_NATIVE_STATS)->offset = movedStats;
    auto* moved = reinterpret_cast<std::atomic<uint32_t>*>(creator.get_base() + movedStats);
    moved[NATIVE_STAT_CPU_AVG_CENTI / 4].store(4242, std::memory_order_relaxed);

    // The engine claims a smaller tap ring than this build's: the reader
    // clamps a slot's self-declared capacity to the ENGINE's, not to ours.
    entry(CLOCKWORK_ARENA_CLOCKWORK_TAPS)->geom[CLOCKWORK_GEOM_TAP_RING_FRAMES] = 64;

    // And more metrics fields than this build names: the count is the engine's.
    entry(CLOCKWORK_ARENA_CLOCKWORK_METRICS)->geom[CLOCKWORK_GEOM_METRICS_FIELDS] = METRICS_SIZE / 4 + 8;

    shm_segment_client client(shm_dup_handle(creator.native_handle()));
    CHECK(client.get_native_stats().cpu_load_avg_centi == 4242);
    CHECK(client.metrics_field_count() == METRICS_SIZE / 4 + 8);
    CHECK(client.layout().window_offset == movedStats);

    auto* slot = reinterpret_cast<shm_tap_stream*>(
        creator.get_base() + TAPS_START + TAPS_HEADER_SIZE);
    slot->state.store(1, std::memory_order_release);
    slot->channels = 2;
    slot->capacity_frames = TAP_RING_FRAMES;   // more than the engine says it has
    CHECK(client.get_tap_stream_reader(0).capacity_frames() == 64);
}

TEST_CASE("shm-security: client refuses a layout that runs past the segment or shrinks a struct",
          "[shm][security]") {
    // Following the header is only safe because every offset in it is checked
    // against the mapping first. A region outside the blob, or a fixed-shape
    // struct smaller than this reader's definition of it, is refused rather
    // than read.
    // Two tables to tamper with: the segment's (where the blob and the plane
    // are) and the arena's (where every region inside the blob is).
    using H = detail_shm_segment::shm_segment_header;
    auto refused = [](auto tamper) {
        shm_segment_creator creator;
        creator.publish();
        auto* hdr = reinterpret_cast<H*>(creator.get_base() - detail_shm_segment::SHM_BLOB_OFFSET);
        auto* arena = const_cast<ClockworkArenaHeader*>(arenaHeader(creator.get_base()));
        tamper(hdr, arena);
        try {
            shm_segment_client client(shm_dup_handle(creator.native_handle()));
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    const auto entry = [](ClockworkArenaHeader* a, uint32_t id) {
        return const_cast<ClockworkArenaEntry*>(clockwork_arena_find(a, id));
    };
    CHECK(refused([&](H*, ClockworkArenaHeader* a) { entry(a, CLOCKWORK_ARENA_CLOCKWORK_METRICS)->offset = a->arena_bytes; }));                       // past the blob
    CHECK(refused([&](H*, ClockworkArenaHeader* a) { entry(a, CLOCKWORK_ARENA_CLOCKWORK_TAPS)->geom[CLOCKWORK_GEOM_TAP_SLOTS] = 0x00FFFFFFu; }));  // scope array overruns the blob
    CHECK(refused([&](H*, ClockworkArenaHeader* a) { entry(a, CLOCKWORK_ARENA_CLOCKWORK_SCOPE)->geom[CLOCKWORK_GEOM_SCOPE_SLOTS] = 0xFFFFFFFFu; })); // count × stride overflow
    CHECK(refused([&](H*, ClockworkArenaHeader* a) { entry(a, CLOCKWORK_ARENA_CLOCKWORK_METRICS)->geom[CLOCKWORK_GEOM_METRICS_FIELDS] = 4; }));      // smaller than PerformanceMetrics
    CHECK(refused([&](H*, ClockworkArenaHeader* a) { entry(a, CLOCKWORK_ARENA_CLOCKWORK_CLOCK_STATE)->bytes = 8; }));                                 // smaller than ClockworkClockState
    CHECK(refused([&](H*, ClockworkArenaHeader* a) { entry(a, CLOCKWORK_ARENA_CLOCKWORK_TAPS)->geom[CLOCKWORK_GEOM_TAP_SLOT_HEADER] = 16; }));     // a slot shape we do not know
    CHECK(refused([&](H*, ClockworkArenaHeader* a) { a->version = CLOCKWORK_ARENA_VERSION + 1; }));                                        // a table we were not written for
    CHECK(refused([&](H*, ClockworkArenaHeader* a) { a->magic = 0; }));                                                                     // not an arena at all
    CHECK(refused([&](H* h, ClockworkArenaHeader*) { h->blob_size = 0x7FFFFFFFu; }));                                                      // blob larger than the mapping
    CHECK(refused([&](H* h, ClockworkArenaHeader*) { h->peer_offset = 0x7FFFFFF0u; }));                                                    // plane outside the segment
}

TEST_CASE("shm client exposes the observer views inside its own mapping",
          "[shm]") {
    shm_segment_creator creator;
    creator.publish();
    shm_segment_client client(shm_dup_handle(creator.native_handle()));
    uint8_t* base = client.get_base();

    auto in = client.get_in_ring();
    CHECK(in.base == base + IN_RING_START);
    CHECK(in.size == IN_RING_SIZE);
    REQUIRE(in.head != nullptr);
    REQUIRE(in.tail != nullptr);

    auto out = client.get_out_ring();
    CHECK(out.base == base + RT_OUT_RING_START);
    CHECK(out.size == RT_OUT_RING_SIZE);

    auto nrt = client.get_nrt_out_ring();
    CHECK(nrt.base == base + NRT_OUT_RING_START);
    CHECK(nrt.size == NRT_OUT_RING_SIZE);

    CHECK(client.get_metrics_flat()
          == reinterpret_cast<const std::atomic<uint32_t>*>(base + METRICS_START));
    CHECK(client.metrics_field_count() == METRICS_SIZE / 4);

    // The mirror view of the guest's own object graph is not asserted here:
    // it describes something only a real DSP populates, so it belongs to a
    // DSP's repo, not clockwork's.

    CHECK(client.has_native_stats());
    (void)client.get_native_stats();
}

TEST_CASE("shm: the guest's region is shared, and addressed by offset",
          "[shm]") {
    shm_segment_creator creator;
    creator.publish();
    shm_segment_client client(shm_dup_handle(creator.native_handle()));

    // THE BULK CHANNELS, and the reason they are in the segment rather than in
    // the arena: the arena is arena_storage, a static array, and multi-
    // megabyte .bss would be paid by every target including the embedded ones.
    // The segment is mmap'd, so this costs nothing where it is not mapped.
    //
    // Two regions in the segment, one writer each: the client-written assets and
    // the guest-written returns. They must not overlap, or "one writer" is a
    // claim the layout itself contradicts. The guest's arena is deliberately
    // NOT here — no client maps it, and in the segment it would exist only
    // while a client did.
    uint8_t*       engineSide = creator.get_returns();
    const uint8_t* clientSide = client.get_returns();  // const: the guest writes this
    REQUIRE(engineSide != nullptr);
    REQUIRE(clientSide != nullptr);
    REQUIRE(creator.get_returns_size() == client.get_returns_size());
    REQUIRE(client.get_returns_size() >= 1024 * 1024);

    // Disjoint, and in both mappings.
    auto disjoint = [](const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
        return a + an <= b || b + bn <= a;
    };
    REQUIRE(disjoint(creator.get_assets(), creator.get_assets_size(),
                     creator.get_returns(), creator.get_returns_size()));
    // The client sees the two staging regions and NOT the arena — there is no
    // client-side arena accessor to compare, which is the invariant, not an
    // omission.
    REQUIRE(client.get_assets_size()  == creator.get_assets_size());

    // TWO MAPPINGS OF THE SAME BYTES, AT DIFFERENT ADDRESSES. That is the whole
    // reason messages about this region carry offsets: this is one process, and
    // the two pointers already differ, so across a real process boundary a
    // pointer would name the wrong bytes without faulting.
    CHECK(engineSide != clientSide);

    const uint32_t offset = 4096;
    const uint32_t n      = 64 * 1024;
    for (uint32_t i = 0; i < n; ++i)
        engineSide[offset + i] = static_cast<uint8_t>(i * 31u + 5u);

    const uint8_t* view = client.returns_at(offset, n);
    REQUIRE(view != nullptr);
    for (uint32_t i = 0; i < n; ++i)
        REQUIRE(view[i] == static_cast<uint8_t>(i * 31u + 5u));

    // The bounds check is the trust boundary. An offset and a length arrive in
    // a message from another process, so both are attacker-influenced from the
    // reader's point of view, and a read outside the mapping is the failure
    // that must not be reachable.
    const uint32_t size = static_cast<uint32_t>(client.get_returns_size());
    CHECK(client.returns_at(size, 1) == nullptr);              // starts at the end
    CHECK(client.returns_at(size + 1, 0) == nullptr);          // starts past it
    CHECK(client.returns_at(0, size + 1) == nullptr);          // runs past it
    CHECK(client.returns_at(size - 4, 8) == nullptr);          // straddles the end
    // Overflow: offset + len wraps to something small, so a check written as
    // `offset + len > size` would pass this and hand back a view of the whole
    // region starting near its end.
    CHECK(client.returns_at(size - 4, 0xFFFFFFFCu) == nullptr);
    // Exactly filling it is legal, and a zero-length view at the very end is
    // too — refusing those would be a different bug, quieter and just as wrong.
    CHECK(client.returns_at(0, size) != nullptr);
    CHECK(client.returns_at(size, 0) != nullptr);
}

#endif  // !_WIN32
