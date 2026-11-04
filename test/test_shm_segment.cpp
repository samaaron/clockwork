// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * test_shm_segment.cpp — the two segments of src/shm_segment.hpp
 * (docs/SEGMENTS.md), created, tampered with and read from this process.
 *
 *   - the published segment is anonymous, owner-only and close-on-exec, and
 *     the handle a reader is given is READ-ONLY: a writable mapping of it is
 *     refused by the kernel;
 *   - the command segment holds the peer plane and the two bulk lanes, one
 *     writer each, and the published segment holds none of them;
 *   - the arena's table says where the lanes are, by space;
 *   - a reader rejects a too-small segment before it dereferences a header
 *     field, and a layout that runs past the mapping or shrinks a struct;
 *   - a stream reader keeps its mapping alive after its client is destroyed,
 *     and clamps corrupt slot geometry rather than reading out of bounds;
 *   - the observer views land inside the reader's own mapping.
 *
 * POSIX-only: the read-only descriptor and the mapping checks are stated in
 * POSIX terms; Windows takes the same code through its own handles.
 */
#if !defined(_WIN32)

#include <catch2/catch_test_macros.hpp>
#include "src/shm_segment.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using detail_shm_segment::shm_segment_creator;
using detail_shm_segment::shm_segment_client;
using detail_shm_segment::shm_command_segment_creator;
using detail_shm_segment::shm_command_client;
using detail_shm_segment::shm_dup_handle;

namespace {

// Whether a writable mapping of `fd` is possible. A reader's descriptor must
// say no, and say it with EACCES: the object was opened read-only.
bool mapsWritable(int fd, size_t bytes) {
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return false;
    ::munmap(p, bytes);
    return true;
}

} // namespace

TEST_CASE("shm-security: the published segment is anonymous, owner-only, close-on-exec and handed out read-only",
          "[shm][security]") {
    shm_segment_creator creator{clockwork::kDefaultLayout};
    const int rw = creator.native_handle();
    const int ro = creator.reader_handle();
    REQUIRE(rw >= 0);
    REQUIRE(ro >= 0);
    CHECK(rw != ro);

    // At least the declared size (macOS rounds a POSIX object up to a page,
    // which is why the readers check "not smaller than" and never equality),
    // and not inheritable: a child the engine spawns gets the segment by an
    // explicit hand-off or not at all.
    struct stat st {};
    REQUIRE(::fstat(rw, &st) == 0);
    CHECK(static_cast<size_t>(st.st_size) >= creator.segment_size());
    CHECK(static_cast<size_t>(st.st_size) <  creator.segment_size() + 65536);
    CHECK((::fcntl(rw, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(ro, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((st.st_mode & (S_IWGRP | S_IWOTH | S_IRGRP | S_IROTH)) == 0);

    // The same object, opened read-only: a reader can map it to read and
    // nothing else, and the kernel is what says so.
    struct stat rst {};
    REQUIRE(::fstat(ro, &rst) == 0);
    CHECK(rst.st_ino == st.st_ino);
    CHECK((::fcntl(ro, F_GETFL) & O_ACCMODE) == O_RDONLY);
    CHECK(mapsWritable(rw, creator.segment_size()));
    CHECK_FALSE(mapsWritable(ro, creator.segment_size()));
    // POSIX says EACCES; XNU's POSIX shm says EPERM. Either is the kernel refusing.
    CHECK((errno == EACCES || errno == EPERM));

    // What the creator writes, a client mapped from the read-only duplicate
    // reads.
    creator.publish();
    shm_segment_client client(shm_dup_handle(creator.reader_handle()));
    creator.get_base()[0] = 0x5A;
    CHECK(client.get_base()[0] == 0x5A);
    // And a reader's view is const: it can be read, never written.
    static_assert(std::is_const_v<std::remove_pointer_t<decltype(client.get_base())>>,
                  "a reader's base is const");
    static_assert(std::is_const_v<std::remove_pointer_t<decltype(client.get_metrics())>>,
                  "a reader's metrics are const");
}

TEST_CASE("shm: the published segment is the header and the arena, and nothing a client writes",
          "[shm]") {
    shm_segment_creator creator{clockwork::kDefaultLayout};
    // The whole of it: the header's reserved span, then the arena blob.
    CHECK(creator.segment_size() == detail_shm_segment::SHM_BLOB_OFFSET + clockwork::kDefaultLayout.arena_bytes);
    auto* hdr = reinterpret_cast<const detail_shm_segment::shm_segment_header*>(
        creator.get_base() - detail_shm_segment::SHM_BLOB_OFFSET);
    CHECK(hdr->blob_offset == detail_shm_segment::SHM_BLOB_OFFSET);
    CHECK(hdr->blob_size   == clockwork::kDefaultLayout.arena_bytes);
    // Unpublished until the engine says so.
    CHECK(hdr->magic == 0);
    creator.publish();
    CHECK(hdr->magic == detail_shm_segment::shm_segment_header::MAGIC);

    // Without a command segment the lanes are nowhere a reader can see: the
    // arena's table says so rather than pointing into memory that is not
    // there.
    const auto* arena = arenaHeader(creator.get_base());
    const auto* assets = clockwork_arena_area(arena, CLOCKWORK_AREA_ASSETS);
    REQUIRE(assets != nullptr);
    CHECK(assets->space == CLOCKWORK_SPACE_UNKNOWN);
}

TEST_CASE("shm: the command segment holds the peer plane and the two lanes, one writer each",
          "[shm][peer]") {
    shm_command_segment_creator cmd(64u << 20, 1u << 20);
    REQUIRE(cmd.native_handle() >= 0);
    CHECK((::fcntl(cmd.native_handle(), F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(cmd.native_handle(), F_GETFL) & O_ACCMODE) == O_RDWR);   // the one peer writes it

    // The header names everything in it, and it is published at once: there
    // is no populate step, the plane and the lanes start zeroed.
    using H = detail_shm_segment::shm_command_header;
    const auto* hdr = reinterpret_cast<const H*>(cmd.get_base());
    CHECK(hdr->magic == H::MAGIC);
    CHECK(hdr->peer_offset == detail_shm_segment::SHM_COMMAND_HEADER_BYTES);
    CHECK(hdr->peer_header_bytes == sizeof(ShmPeerPlaneHeader));
    CHECK(hdr->peer_cmd_ring_bytes == SHM_PEER_CMD_RING_SIZE);
    CHECK(hdr->peer_rep_ring_bytes == SHM_PEER_REP_RING_SIZE);
    CHECK(hdr->assets_size  == (64u << 20));
    CHECK(hdr->returns_size == (1u << 20));
    CHECK(cmd.segment_size() == hdr->returns_offset + hdr->returns_size);
    struct stat st {};
    REQUIRE(::fstat(cmd.native_handle(), &st) == 0);
    CHECK(static_cast<size_t>(st.st_size) >= cmd.segment_size());

    // The plane carries the geometry a peer reads (it has no compile-time
    // view of this build).
    ShmPeerPlaneHeader* plane = cmd.get_peer_plane();
    REQUIRE(plane != nullptr);
    CHECK(plane->cmd_ring_size == SHM_PEER_CMD_RING_SIZE);
    CHECK(plane->rep_ring_size == SHM_PEER_REP_RING_SIZE);

    // The lanes: the sizes asked for, 16-aligned, disjoint from each other
    // and from the plane, inside the segment.
    auto disjoint = [](const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
        return a + an <= b || b + bn <= a;
    };
    const auto* planeBytes = reinterpret_cast<const uint8_t*>(plane);
    CHECK(cmd.get_assets_size()  == (64u << 20));
    CHECK(cmd.get_returns_size() == (1u << 20));
    CHECK(reinterpret_cast<uintptr_t>(cmd.get_assets())  % 16 == 0);
    CHECK(reinterpret_cast<uintptr_t>(cmd.get_returns()) % 16 == 0);
    CHECK(disjoint(planeBytes, SHM_PEER_PLANE_TOTAL_SIZE, cmd.get_assets(), cmd.get_assets_size()));
    CHECK(disjoint(cmd.get_assets(), cmd.get_assets_size(), cmd.get_returns(), cmd.get_returns_size()));
    CHECK(cmd.get_returns() + cmd.get_returns_size() <= cmd.get_base() + cmd.segment_size());

    // A peer's mapping sees the same plane and the same lanes, at its own
    // addresses.
    shm_command_client peer(shm_dup_handle(cmd.native_handle()));
    REQUIRE(peer.get_peer_plane() != nullptr);
    CHECK(peer.get_peer_plane() != plane);
    CHECK(peer.get_peer_plane()->cmd_ring_size == SHM_PEER_CMD_RING_SIZE);
    CHECK(peer.get_assets_size()  == cmd.get_assets_size());
    CHECK(peer.get_returns_size() == cmd.get_returns_size());
    peer.get_assets()[123] = 0xA5;                        // the peer writes the assets
    CHECK(cmd.get_assets()[123] == 0xA5);
    cmd.get_returns()[77] = 0x3C;                         // the guest writes the returns
    CHECK(peer.get_returns()[77] == 0x3C);
    static_assert(std::is_const_v<std::remove_pointer_t<decltype(peer.get_returns())>>,
                  "a peer reads the returns area");
}

TEST_CASE("shm: the arena's table says where the lanes are", "[shm][peer]") {
    shm_segment_creator published{clockwork::kDefaultLayout};
    shm_command_segment_creator cmd;
    // The engine tells the published arena where the command segment put
    // them: offsets from the command segment's base, in that space.
    cmd.describe_in(published.get_base());
    published.publish();
    const auto* arena = arenaHeader(published.get_base());
    const auto* assets  = clockwork_arena_area(arena, CLOCKWORK_AREA_ASSETS);
    const auto* returns = clockwork_arena_area(arena, CLOCKWORK_AREA_RETURNS);
    REQUIRE(assets != nullptr);
    REQUIRE(returns != nullptr);
    CHECK(assets->space  == CLOCKWORK_SPACE_COMMAND_SEGMENT);
    CHECK(returns->space == CLOCKWORK_SPACE_COMMAND_SEGMENT);
    CHECK(assets->base_lo  == static_cast<uint32_t>(cmd.get_assets()  - cmd.get_base()));
    CHECK(returns->base_lo == static_cast<uint32_t>(cmd.get_returns() - cmd.get_base()));
    CHECK(assets->bytes_lo  == cmd.get_assets_size());
    CHECK(returns->bytes_lo == cmd.get_returns_size());

    // A reader of the published segment sees the same table and can tell it
    // has no way to those bytes: they are not in its segment.
    shm_segment_client reader(shm_dup_handle(published.reader_handle()));
    CHECK(clockwork_arena_area(arenaHeader(reader.get_base()), CLOCKWORK_AREA_ASSETS)->space
          == CLOCKWORK_SPACE_COMMAND_SEGMENT);
}

TEST_CASE("shm-security: two creators never share memory", "[shm][security]") {
    // Nothing is named, so there is nothing to collide on: each creator gets
    // its own pages, whatever else exists.
    shm_segment_creator a{clockwork::kDefaultLayout};
    shm_segment_creator b{clockwork::kDefaultLayout};
    CHECK(a.native_handle() != b.native_handle());
    a.get_base()[0] = 1;
    b.get_base()[0] = 2;
    CHECK(a.get_base()[0] == 1);
    CHECK(b.get_base()[0] == 2);
    shm_command_segment_creator c;
    shm_command_segment_creator d;
    CHECK(c.native_handle() != d.native_handle());
    c.get_assets()[0] = 3;
    d.get_assets()[0] = 4;
    CHECK(c.get_assets()[0] == 3);
    CHECK(d.get_assets()[0] == 4);
}

TEST_CASE("shm-security: reader rejects a too-small segment before dereferencing",
          "[shm][security]") {
    // A tiny segment carrying a valid MAGIC in its first word: the magic check
    // alone would pass, so only a size check stops the client building region
    // pointers into a few bytes of memory. The same for the command segment.
    auto tiny = detail_shm_segment::shm_create_anonymous(64);
    *static_cast<uint32_t*>(tiny.ptr) = detail_shm_segment::shm_segment_header::MAGIC;
    const int dup = shm_dup_handle(tiny.fd);
    REQUIRE(dup >= 0);
    bool threw = false;
    try {
        shm_segment_client client(dup);   // takes the handle, even on refusal
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    *static_cast<uint32_t*>(tiny.ptr) = detail_shm_segment::shm_command_header::MAGIC;
    const int dup2 = shm_dup_handle(tiny.fd);
    detail_shm_segment::shm_close(tiny);
    REQUIRE(dup2 >= 0);
    threw = false;
    try {
        shm_command_client peer(dup2);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("shm-security: a reader's handle given to the peer's class is refused, and the other way round",
          "[shm][security]") {
    // The two segments have different magics: a client that asked for one
    // and was handed the other must say so rather than read the wrong
    // layout.
    shm_segment_creator published{clockwork::kDefaultLayout};
    published.publish();
    shm_command_segment_creator cmd;
    bool threw = false;
    try { shm_command_client wrong(shm_dup_handle(published.reader_handle())); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { shm_segment_client wrong(shm_dup_handle(cmd.native_handle())); }
    catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("shm-security: stream readers pin the client mapping (no dangling reads)",
          "[shm][security]") {
    shm_segment_creator creator{clockwork::kDefaultLayout};
    creator.publish();

    // An observer hands reader copies to long-lived widgets, then remaps by
    // destroying its client. The reader must keep the old mapping alive, or a
    // widget's poll timer makes an atomic load through the munmapped slot
    // pointer.
    auto client = std::make_unique<shm_segment_client>(shm_dup_handle(creator.reader_handle()));
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
    shm_segment_creator creator{clockwork::kDefaultLayout};
    creator.publish();
    shm_segment_client client(shm_dup_handle(creator.reader_handle()));
    auto reader = client.get_tap_stream_reader(0);

    // channels/capacity_frames are re-read from shared memory on every
    // copy_window and index into the inline data array; a corrupt or hostile
    // slot must clamp to the compile-time ring geometry rather than push
    // reads out of bounds (or divide by a zero capacity).
    auto* slot = reinterpret_cast<shm_tap_stream*>(
        creator.get_base() + clockwork::kDefaultLayout.taps_start + TAPS_HEADER_SIZE);
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
    shm_segment_creator creator{clockwork::kDefaultLayout};
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

    shm_segment_client client(shm_dup_handle(creator.reader_handle()));
    CHECK(client.get_native_stats().cpu_load_avg_centi == 4242);
    CHECK(client.metrics_field_count() == METRICS_SIZE / 4 + 8);
    CHECK(client.layout().window_offset == movedStats);

    auto* slot = reinterpret_cast<shm_tap_stream*>(
        creator.get_base() + clockwork::kDefaultLayout.taps_start + TAPS_HEADER_SIZE);
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
    // Two tables to tamper with: the segment's (where the blob is) and the
    // arena's (where every region inside the blob is).
    using H = detail_shm_segment::shm_segment_header;
    auto refused = [](auto tamper) {
        shm_segment_creator creator{clockwork::kDefaultLayout};
        creator.publish();
        auto* hdr = reinterpret_cast<H*>(creator.get_base() - detail_shm_segment::SHM_BLOB_OFFSET);
        auto* arena = const_cast<ClockworkArenaHeader*>(arenaHeader(creator.get_base()));
        tamper(hdr, arena);
        try {
            shm_segment_client client(shm_dup_handle(creator.reader_handle()));
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

    // The command segment's own header, the same way.
    using C = detail_shm_segment::shm_command_header;
    auto peerRefused = [](auto tamper) {
        shm_command_segment_creator cmd;
        tamper(reinterpret_cast<C*>(cmd.get_base()));
        try {
            shm_command_client peer(shm_dup_handle(cmd.native_handle()));
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    CHECK(peerRefused([](C* h) { h->peer_offset = 0x7FFFFFF0u; }));          // plane outside the segment
    CHECK(peerRefused([](C* h) { h->peer_header_bytes = 16; }));             // a plane header we do not know
    CHECK(peerRefused([](C* h) { h->assets_size = 0xFFFFFFFFu; }));          // lane past the mapping
    CHECK(peerRefused([](C* h) { h->returns_offset = 0xFFFFFFF0u; }));       // lane past the mapping
    CHECK(peerRefused([](C* h) { h->magic = 0; }));                          // not a command segment
}

TEST_CASE("shm client exposes the observer views inside its own mapping",
          "[shm]") {
    shm_segment_creator creator{clockwork::kDefaultLayout};
    creator.publish();
    shm_segment_client client(shm_dup_handle(creator.reader_handle()));
    const uint8_t* base = client.get_base();

    // The rings are the engine's; a reader sees where they are and how full
    // they are, and can write none of it.
    auto in = client.get_in_ring();
    CHECK(in.base == base + clockwork::kDefaultLayout.in_ring_start);
    CHECK(in.size == clockwork::kDefaultLayout.profile.in_ring_bytes);
    REQUIRE(in.head != nullptr);
    REQUIRE(in.tail != nullptr);
    static_assert(std::is_const_v<std::remove_pointer_t<decltype(in.head)>>, "a reader's cursors are const");
    static_assert(std::is_const_v<std::remove_pointer_t<decltype(in.base)>>, "a reader's ring bytes are const");

    auto out = client.get_out_ring();
    CHECK(out.base == base + clockwork::kDefaultLayout.rt_out_ring_start);
    CHECK(out.size == clockwork::kDefaultLayout.profile.rt_out_ring_bytes);

    auto nrt = client.get_nrt_out_ring();
    CHECK(nrt.base == base + clockwork::kDefaultLayout.nrt_out_ring_start);
    CHECK(nrt.size == clockwork::kDefaultLayout.profile.nrt_out_ring_bytes);

    CHECK(client.get_metrics_flat()
          == reinterpret_cast<const std::atomic<uint32_t>*>(base + METRICS_START));
    CHECK(client.metrics_field_count() == METRICS_SIZE / 4);

    // The mirror view of the guest's own object graph is not asserted here:
    // it describes something only a real DSP populates, so it belongs to a
    // DSP's repo, not clockwork's.

    CHECK(client.has_native_stats());
    (void)client.get_native_stats();
}

TEST_CASE("shm: the returns area is shared with the peer, and addressed by offset",
          "[shm][peer]") {
    shm_command_segment_creator cmd;
    shm_command_client peer(shm_dup_handle(cmd.native_handle()));

    // THE BULK LANES, and the reason they are in a segment rather than in the
    // arena: an arena is a core's own memory or a device's static storage,
    // and multi-megabyte lanes there would be paid by every target including
    // the embedded ones. The
    // segment is mmap'd, so this costs nothing where it is not mapped.
    uint8_t*       engineSide = cmd.get_returns();
    const uint8_t* peerSide   = peer.get_returns();  // const: the guest writes this
    REQUIRE(engineSide != nullptr);
    REQUIRE(peerSide != nullptr);
    REQUIRE(cmd.get_returns_size() == peer.get_returns_size());
    REQUIRE(peer.get_returns_size() >= 1024 * 1024);

    // TWO MAPPINGS OF THE SAME BYTES, AT DIFFERENT ADDRESSES. That is the whole
    // reason messages about this region carry offsets: this is one process, and
    // the two pointers already differ, so across a real process boundary a
    // pointer would name the wrong bytes without faulting.
    CHECK(engineSide != peerSide);

    const uint32_t offset = 4096;
    const uint32_t n      = 64 * 1024;
    for (uint32_t i = 0; i < n; ++i)
        engineSide[offset + i] = static_cast<uint8_t>(i * 31u + 5u);

    const uint8_t* view = peer.returns_at(offset, n);
    REQUIRE(view != nullptr);
    for (uint32_t i = 0; i < n; ++i)
        REQUIRE(view[i] == static_cast<uint8_t>(i * 31u + 5u));

    // The bounds check is the trust boundary. An offset and a length arrive in
    // a message from another process, so both are attacker-influenced from the
    // reader's point of view, and a read outside the mapping is the failure
    // that must not be reachable.
    const uint32_t size = static_cast<uint32_t>(peer.get_returns_size());
    CHECK(peer.returns_at(size, 1) == nullptr);              // starts at the end
    CHECK(peer.returns_at(size + 1, 0) == nullptr);          // starts past it
    CHECK(peer.returns_at(0, size + 1) == nullptr);          // runs past it
    CHECK(peer.returns_at(size - 4, 8) == nullptr);          // straddles the end
    // Overflow: offset + len wraps to something small, so a check written as
    // `offset + len > size` would pass this and hand back a view of the whole
    // region starting near its end.
    CHECK(peer.returns_at(size - 4, 0xFFFFFFFCu) == nullptr);
    // Exactly filling it is legal, and a zero-length view at the very end is
    // too — refusing those would be a different bug, quieter and just as wrong.
    CHECK(peer.returns_at(0, size) != nullptr);
    CHECK(peer.returns_at(size, 0) != nullptr);
    // The same check guards the assets area.
    const uint32_t asize = static_cast<uint32_t>(peer.get_assets_size());
    CHECK(peer.assets_at(asize - 4, 8) == nullptr);
    CHECK(peer.assets_at(0, asize) != nullptr);
}

#endif  // !_WIN32
