// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//  Shared memory IPC interface to clockwork.
//
//  A fixed-layout segment other processes map. It is ANONYMOUS: the engine
//  creates it with no name (memfd on Linux, an unlinked POSIX object on macOS,
//  an unnamed file mapping on Windows) and hands a duplicate of the handle to
//  each reader over the attach endpoint (shm_attach.hpp). Nothing is looked
//  up by name, so there is no namespace to collide in, nothing another user
//  can plant, and nothing left behind when the engine dies — the memory goes
//  with its last mapping. It holds a clock view, ring views, a node-tree
//  mirror, native stats and the peer plane.
//
//  The named create/open pair below still exists for the plugin bridge
//  (plugin_bridge.h), whose segment is pid-named and shared with a child the
//  engine itself spawns.

#pragma once

#include "shm_audio_buffer.hpp"
#include "clockwork_product.h"
#include "shm_scope_stream.hpp"
#include "clock/clock_math.h"   // wallClockNTP, kNtpEpochOffset
#include "shared_memory.h"
#include "shm_peer_plane.h"
#include "clockwork_path.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif
#include <cstdio>
#include <random>

namespace detail_shm_segment {

// 64 random bits from the OS, for the throwaway name a non-Linux POSIX
// segment carries for the instant before it is unlinked.
inline uint64_t detail_random_u64() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
}

// The sample clock is declared with the layout it reads (shared_memory.h);
// named here too because that is where callers have always found it.
using ::sample_clock_view;
using ::read_sample_clock;

using std::string;


// The public segment is a small handshake header followed by the unified
// shared_memory.h arena blob — the SAME layout the engine uses in
// ring_buffer_storage (native) / the WASM SAB. One layout, one source of
// truth; no separate native layout to drift. The peer command plane
// (shm_peer_plane.h) sits after the blob: it is native-segment-only (its
// consumers are the native host and an external peer), so the arena layout —
// and with it the web SAB and embedded profiles — is untouched by it.
// Raised from 128 to 256 when the guest-memory fields were added: the header
// had grown to exactly 128 bytes, so the next two words overflowed it and the
// static_assert below caught it. Headroom, not a snug fit — every field added
// here is a region someone needs to find without compile-time knowledge, and
// running out of room is not a reason to make one harder to find.
static constexpr size_t SHM_BLOB_OFFSET = 256;  // aligned, >= sizeof(shm_segment_header)
// Rounded up to 8: the arena total is only guaranteed 4-aligned, and the peer
// plane header is alignas(8).
static constexpr size_t SHM_PEER_OFFSET = (SHM_BLOB_OFFSET + TOTAL_BUFFER_SIZE + 7u) & ~size_t{7};
// The guest's two bulk regions, last because they are by far the largest and
// because nothing before them should move when a size changes. Each is
// 16-aligned so a guest can place anything it likes at a base without
// re-aligning first.
//
// ONE WRITER EACH: the inbox is written only by the client, the outbox only by
// the guest. Keeping them apart is what makes that enforceable rather than
// merely agreed.
//
// The guest's arena is not among them: no client maps it, so it is the
// engine's own allocation rather than a region of this segment.
static constexpr size_t SHM_INBOX_OFFSET =
    (SHM_PEER_OFFSET + SHM_PEER_PLANE_TOTAL_SIZE + 15u) & ~size_t{15};
static constexpr size_t SHM_INBOX_SIZE   = CLOCKWORK_INBOX_BYTES;

static constexpr size_t SHM_OUTBOX_OFFSET =
    (SHM_INBOX_OFFSET + SHM_INBOX_SIZE + 15u) & ~size_t{15};
static constexpr size_t SHM_OUTBOX_SIZE   = CLOCKWORK_OUTBOX_BYTES;

static constexpr size_t SEGMENT_SIZE = SHM_OUTBOX_OFFSET + SHM_OUTBOX_SIZE;

// The lanes are sized by the host at launch (ClockworkEngine::Config), the
// constants above being the defaults; the segment grows to fit, and a reader
// takes the geometry from the header. Everything before the inbox is fixed.
struct shm_lane_geometry {
    size_t inbox_offset, inbox_size, outbox_offset, outbox_size, segment_size;
    static constexpr size_t align16(size_t v) { return (v + 15u) & ~size_t{15}; }
    static constexpr shm_lane_geometry of(size_t inbox_bytes, size_t outbox_bytes) {
        const size_t in  = align16(inbox_bytes);
        const size_t out = align16(outbox_bytes);
        const size_t out_off = align16(SHM_INBOX_OFFSET + in);
        return { SHM_INBOX_OFFSET, in, out_off, out, out_off + out };
    }
};
static_assert(shm_lane_geometry::of(SHM_INBOX_SIZE, SHM_OUTBOX_SIZE).segment_size == SEGMENT_SIZE,
              "the default geometry is the constant one");

// ──── Platform shared memory primitives ─────────────────────────────────

struct shm_handle {
    void*  ptr  = nullptr;
    size_t size = 0;
#ifdef _WIN32
    HANDLE mapping = nullptr;
#else
    int    fd  = -1;
#endif
};

// The OS object behind a mapping, as it travels between processes: a file
// descriptor over a Unix socket, a HANDLE duplicated into the receiver.
#ifdef _WIN32
using shm_native_handle = HANDLE;
static const shm_native_handle shm_invalid_handle = nullptr;
inline bool shm_handle_valid(shm_native_handle h) { return h != nullptr; }
#else
using shm_native_handle = int;
static const shm_native_handle shm_invalid_handle = -1;
inline bool shm_handle_valid(shm_native_handle h) { return h >= 0; }
#endif

// Close a bare native handle that is not (or no longer) mapped.
inline void shm_close_native(shm_native_handle h) {
#ifdef _WIN32
    if (h) CloseHandle(h);
#else
    if (h >= 0) ::close(h);
#endif
}

// A second reference to the same object, for a reader in this process (a
// test, an in-process GUI) that wants a mapping of its own. Owned by the
// caller; shm_segment_client takes it.
inline shm_native_handle shm_dup_handle(shm_native_handle h) {
#ifdef _WIN32
    HANDLE out = nullptr;
    if (!h || !DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out,
                               0, FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
    return out;
#else
    if (h < 0) return -1;
    const int out = ::fcntl(h, F_DUPFD_CLOEXEC, 0);
    return out;
#endif
}

// The handle as the C ABI carries it: clockwork_client.h speaks intptr_t,
// because a HANDLE and a file descriptor have to cross the same signature.
// Which cast is legal differs — a HANDLE is a pointer, a descriptor an int —
// so the choice is made here rather than at each call.
inline intptr_t shm_handle_to_intptr(shm_native_handle h) {
#ifdef _WIN32
    return reinterpret_cast<intptr_t>(h);
#else
    return static_cast<intptr_t>(h);
#endif
}

#ifdef _WIN32
// The kernel object name. "Local\" pins it to this logon session's
// namespace — where the engine and everything that joins it live — rather
// than the global one, which needs a privilege to create in and is shared
// with every session on the machine. The name is UTF-8 in (it may carry the
// product name) and UTF-16 to the API.
inline std::wstring shm_object_name(const string& name) {
    return clockwork_path::to_wide("Local\\" + name);
}
#endif

inline shm_handle shm_create(const string& name, size_t size) {
    shm_handle h;
    h.size = size;
#ifdef _WIN32
    // The same rule as O_EXCL below, spelled the Windows way: CreateFileMapping
    // opens an existing mapping of that name and reports ERROR_ALREADY_EXISTS
    // rather than failing, so the check is on the error code after success. A
    // mapping that already exists is one another process still holds (Windows
    // has no stale segments — the object goes with its last handle), and
    // adopting it would mean dereferencing ring cursors somebody else owns.
    // The default security descriptor grants the creating user; that is the
    // 0600 of the POSIX branch.
    const std::wstring wname = shm_object_name(name);
    const ULARGE_INTEGER sz { { static_cast<DWORD>(size & 0xffffffffu),
                                static_cast<DWORD>(size >> 32) } };
    h.mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        sz.HighPart, sz.LowPart, wname.c_str());
    if (!h.mapping)
        throw std::runtime_error("CreateFileMapping failed for " + name + ": " +
                                 clockwork_path::last_error_text(GetLastError()));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h.mapping);
        h.mapping = nullptr;
        throw std::runtime_error("shared memory " + name + " already exists (another process holds it)");
    }
    h.ptr = MapViewOfFile(h.mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!h.ptr) {
        const std::string why = clockwork_path::last_error_text(GetLastError());
        CloseHandle(h.mapping);
        h.mapping = nullptr;
        throw std::runtime_error("MapViewOfFile failed for " + name + ": " + why);
    }
#else
    string posix_name = "/" + name;
    // 0600 + O_EXCL: the segment carries ring cursors the engine dereferences,
    // so only the owning user may map it, and a pre-existing segment (stale or
    // planted by another local user under the predictable name) is never
    // silently adopted — creation fails instead. Callers remove their own
    // leftovers first via shm_remove()/cleanup().
    h.fd = ::shm_open(posix_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (h.fd < 0)
        throw std::runtime_error("shm_open(create) failed for " + name);
    if (ftruncate(h.fd, static_cast<off_t>(size)) < 0) {
        ::close(h.fd);
        ::shm_unlink(posix_name.c_str());
        throw std::runtime_error("ftruncate failed for " + name);
    }
    h.ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) {
        ::close(h.fd);
        ::shm_unlink(posix_name.c_str());
        throw std::runtime_error("mmap failed for " + name);
    }
#endif
    return h;
}

// Create a segment with no name at all.
//
// Linux: memfd_create, the object that exists for exactly this purpose. Other
// POSIX systems (macOS has no memfd): a POSIX object under a throwaway random
// name, unlinked the moment it is open — the descriptor keeps it alive and
// nothing can ever open it by name again. Windows: an unnamed file mapping.
// Every handle is close-on-exec: the plugin bridge and any other child gets
// its memory by an explicit hand-off, never by inheritance.
inline shm_handle shm_create_anonymous(size_t size) {
    shm_handle h;
    h.size = size;
#ifdef _WIN32
    const ULARGE_INTEGER sz { { static_cast<DWORD>(size & 0xffffffffu),
                                static_cast<DWORD>(size >> 32) } };
    h.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   sz.HighPart, sz.LowPart, nullptr);
    if (!h.mapping)
        throw std::runtime_error("CreateFileMapping (anonymous) failed: " +
                                 clockwork_path::last_error_text(GetLastError()));
    h.ptr = MapViewOfFile(h.mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!h.ptr) {
        const std::string why = clockwork_path::last_error_text(GetLastError());
        CloseHandle(h.mapping);
        h.mapping = nullptr;
        throw std::runtime_error("MapViewOfFile (anonymous) failed: " + why);
    }
#else
    h.fd = -1;
#  if defined(__linux__)
    h.fd = ::memfd_create("clockwork-shm", MFD_CLOEXEC);
    // memfd_create takes no mode argument: the file lands at 0777 & ~umask, so
    // a caller with a permissive umask gets a group- and world-readable
    // segment. The named path above states 0600 outright and the non-Linux
    // fallback below does too; state it here rather than inherit whatever
    // umask the host process happened to be started with.
    if (h.fd >= 0 && ::fchmod(h.fd, 0600) != 0) {
        ::close(h.fd);
        throw std::runtime_error("fchmod(0600) failed for the anonymous segment");
    }
#  endif
    if (h.fd < 0) {
        // A name nobody can guess, alive for the few instructions between
        // create and unlink. O_EXCL still guards the create: on a collision
        // (or a planted name) the loop simply tries another.
        for (int attempt = 0; attempt < 16 && h.fd < 0; ++attempt) {
            const uint64_t r = detail_random_u64();
            char name[48];
            std::snprintf(name, sizeof name, "/cw-%d-%08x%08x",
                          static_cast<int>(::getpid()),
                          static_cast<unsigned>(r >> 32), static_cast<unsigned>(r));
            h.fd = ::shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
            if (h.fd >= 0) ::shm_unlink(name);
            else if (errno != EEXIST) break;
        }
        if (h.fd < 0)
            throw std::runtime_error("anonymous shm_open failed");
        ::fcntl(h.fd, F_SETFD, FD_CLOEXEC);
    }
    if (ftruncate(h.fd, static_cast<off_t>(size)) < 0) {
        ::close(h.fd);
        throw std::runtime_error("ftruncate (anonymous segment) failed");
    }
    h.ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) {
        ::close(h.fd);
        throw std::runtime_error("mmap (anonymous segment) failed");
    }
#endif
    return h;
}

// Map a segment from a handle received from its creator. Takes ownership of
// the handle on every path, including failure.
inline shm_handle shm_map_handle(shm_native_handle native) {
    shm_handle h;
    if (!shm_handle_valid(native))
        throw std::runtime_error("shm_map_handle: no handle");
#ifdef _WIN32
    h.mapping = native;
    h.ptr = MapViewOfFile(h.mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!h.ptr) {
        const std::string why = clockwork_path::last_error_text(GetLastError());
        CloseHandle(h.mapping);
        h.mapping = nullptr;
        throw std::runtime_error("MapViewOfFile (received handle) failed: " + why);
    }
    // As in shm_open_existing: the size to within a page, which is what the
    // callers' "not smaller than" checks need.
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(h.ptr, &info, sizeof(info)) == sizeof(info))
        h.size = info.RegionSize;
#else
    h.fd = native;
    struct stat st {};
    if (fstat(h.fd, &st) != 0) {
        ::close(h.fd);
        throw std::runtime_error("fstat (received handle) failed");
    }
    h.size = static_cast<size_t>(st.st_size);
    h.ptr = ::mmap(nullptr, h.size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) {
        ::close(h.fd);
        throw std::runtime_error("mmap (received handle) failed");
    }
#endif
    return h;
}

inline shm_handle shm_open_existing(const string& name) {
    shm_handle h;
#ifdef _WIN32
    const std::wstring wname = shm_object_name(name);
    h.mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
    if (!h.mapping)
        throw std::runtime_error("OpenFileMapping failed for " + name + ": " +
                                 clockwork_path::last_error_text(GetLastError()));
    h.ptr = MapViewOfFile(h.mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!h.ptr) {
        const std::string why = clockwork_path::last_error_text(GetLastError());
        CloseHandle(h.mapping);
        h.mapping = nullptr;
        throw std::runtime_error("MapViewOfFile failed for " + name + ": " + why);
    }
    // Windows has no fstat for a mapping. A whole-object view is one region,
    // and VirtualQuery reports its size rounded up to a page — so this is the
    // mapping's size to within 4 KB, which is what the callers need: they
    // check it is not SMALLER than the layout they expect, never that it is
    // equal. A failed query leaves size 0, and that check then refuses.
    MEMORY_BASIC_INFORMATION info {};
    if (VirtualQuery(h.ptr, &info, sizeof(info)) == sizeof(info))
        h.size = info.RegionSize;
#else
    string posix_name = "/" + name;
    h.fd = ::shm_open(posix_name.c_str(), O_RDWR, 0);
    if (h.fd < 0)
        throw std::runtime_error("shm_open(open) failed for " + name);
    struct stat st;
    fstat(h.fd, &st);
    h.size = static_cast<size_t>(st.st_size);
    h.ptr = ::mmap(nullptr, h.size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) {
        ::close(h.fd);
        throw std::runtime_error("mmap failed for " + name);
    }
#endif
    return h;
}

inline void shm_close(shm_handle& h) {
#ifdef _WIN32
    if (h.ptr)     UnmapViewOfFile(h.ptr);
    if (h.mapping) CloseHandle(h.mapping);
#else
    if (h.ptr && h.ptr != MAP_FAILED) ::munmap(h.ptr, h.size);
    if (h.fd >= 0)                    ::close(h.fd);
#endif
    h.ptr = nullptr;
}

inline void shm_remove(const string& name) {
#ifdef _WIN32
    // Nothing to unlink: a named mapping lives exactly as long as a handle to
    // it does, so there is no leftover to remove and no stale-segment problem
    // to solve. A create() that finds the name taken is the live-owner case,
    // and it refuses.
    (void)name;
#else
    ::shm_unlink(("/" + name).c_str());
#endif
}

// ──── Segment handshake header ──────────────────────────────────────────
//
// Segment layout (unified, MAGIC 0x5C09E00B):
//
//   shm_segment_header        (MAGIC + the self-describing geometry below;
//                              184 B today, inside the 256 B reserved)
//   <pad to SHM_BLOB_OFFSET>
//   shared_memory.h arena blob (TOTAL_BUFFER_SIZE) — the *same* layout the
//   engine uses in ring_buffer_storage / the WASM SAB. The engine points its
//   `shared_memory` base at this blob, so rings, control pointers, metrics,
//   node-tree, audio taps and scope streams are all addressed by their
//   shared_memory.h offsets and observable cross-process for free.
//
// MAGIC history:
//   0x5C09E001  initial (scope + control busses only)
//   0x5C09E002  added metrics + node tree mirror
//   0x5C09E003  added shm_audio_buffer multi-slot ring
//   0x5C09E004  unified: segment == shared_memory.h arena blob (rings in
//               segment; scope fixed-inline; TLSF pool + control busses removed)
//   0x5C09E006  + Link metrics fields (METRICS_SIZE 184→232)
//   0x5C09E007  appended the peer command plane after the arena blob
//               (shm_peer_plane.h: SPSC command + reply rings)
//   0x5C09E008  scope slots became lossless cursor-ring streams
//               (shm_scope_stream.hpp) + ClockworkClock sample-clock region appended
//               to the arena (engine-frames ↔ DAC-NTP mapping)
//   0x5C09E009  guest memory regions (inbox/outbox) after the plane
//   0x5C09E00A  channel map region
//   0x5C09E00B  self-describing for readers: control/clock-state/native-stats/
//               sample-clock/channel-map sizes and the audio tap geometry
//               appended, and readers (shm_segment_client, clockwork_client)
//               address every region through the header (ShmReaderLayout)
//               instead of their own build's constants. The segment is also
//               anonymous from here: no name, handed over by shm_attach.hpp.
//   0x5C09E00C  the arena describes itself (clockwork_arena.h): the blob's
//               regions left this header for the arena's own, which every
//               reader — in-process, segment, worklet — takes the same way.
//               This header keeps only what is segment-relative.
//
// Publication: the creator zeroes the whole segment and writes the header
// geometry, but defers the MAGIC store. The engine then populates the arena
// (init_memory(): node-tree empty-slot markers = 0xFF, metrics, scope headers,
// …) and only afterwards calls publish(), which stores MAGIC last behind a
// release fence. So a reader that observes MAGIC (after an acquire fence) sees
// both the geometry AND a fully-populated arena — never a half-zeroed one where
// e.g. node entries read id == 0 (aliasing the real root group) instead of the
// id == -1 empty marker. MAGIC visible ⇒ segment ready.

// Self-describing handshake: the creator publishes every region's offset and
// geometry, so a reader needs no compile-time knowledge of the
// arena layout — it locates metrics, rings, node-tree, audio taps and scope
// purely from these fields. This makes the consumer drift-proof: layout/size
// changes propagate through the header rather than requiring a hand-synced copy.
// All offsets are relative to the arena blob base (segment + blob_offset).
struct shm_segment_header {
    // E00C: the arena describes itself. Every region INSIDE the blob is in the
    // arena's own header (clockwork_arena.h, at blob_offset), read by every
    // kind of reader the same way; this header names only what is
    // segment-relative — where the blob is, and the regions that sit after it.
    static constexpr uint32_t MAGIC = 0x5C09E00C;
    uint32_t magic;
    uint32_t blob_offset;          // segment base → arena blob (its header is at offset 0 of it)
    uint32_t blob_size;            // == the arena's arena_bytes
    // Peer command plane (shm_peer_plane.h). SEGMENT-relative: the plane sits
    // after the arena blob.
    uint32_t peer_offset;
    uint32_t peer_header_bytes;    // sizeof(ShmPeerPlaneHeader)
    uint32_t peer_cmd_ring_bytes;  // SHM_PEER_CMD_RING_SIZE
    uint32_t peer_rep_ring_bytes;  // SHM_PEER_REP_RING_SIZE
    // The guest's two bulk regions, one direction each: a client writes a
    // sample into the inbox and sends a short message saying where; a guest
    // writes a rendered blob into the outbox and does the same. Nothing large
    // goes through a ring, and nothing is written from both ends. The guest's
    // arena is not in this segment — see SHM_INBOX_OFFSET.
    //
    // SEGMENT-relative, like peer_offset and for the same reason.
    //
    // EVERYTHING INSIDE IT IS ADDRESSED RELATIVE TO ITS BASE, and that is not
    // a stylistic choice. A reader mmaps this segment at whatever address the
    // kernel gives it, which is not the address the engine sees, so a raw
    // pointer in a message would be meaningless across the boundary — and
    // meaningless in a way that reads as valid.
    uint32_t inbox_offset;
    uint32_t inbox_size;
    uint32_t outbox_offset;
    uint32_t outbox_size;
};

static_assert(sizeof(shm_segment_header) <= SHM_BLOB_OFFSET,
              "shm_segment_header must fit within SHM_BLOB_OFFSET");
static_assert(SHM_PEER_OFFSET % 8 == 0,
              "peer plane must be 8-byte aligned (ShmPeerPlaneHeader is alignas(8))");

// ──── shm_segment ──────────────────────────────────────────────
//
// Process-local view: holds the arena blob base and exposes the observable
// regions by their shared_memory.h offsets. NOT itself in shared memory.

class shm_segment {
public:
    shm_segment(void* segment_base, bool init,
                shm_lane_geometry lanes = shm_lane_geometry::of(SHM_INBOX_SIZE, SHM_OUTBOX_SIZE)) {
        char* seg = static_cast<char*>(segment_base);
        auto* base8 = reinterpret_cast<uint8_t*>(seg);
        header_ = reinterpret_cast<shm_segment_header*>(seg);
        if (init) {
            // The creator lays the segment out from this build's constants
            // (and the lane sizes its caller chose) and publishes that layout
            // in the header.
            L_    = ShmReaderLayout::from_constants();
            blob_ = base8 + SHM_BLOB_OFFSET;
            peer_ = reinterpret_cast<ShmPeerPlaneHeader*>(seg + SHM_PEER_OFFSET);
            inbox_       = base8 + lanes.inbox_offset;
            inbox_size_  = lanes.inbox_size;
            outbox_      = base8 + lanes.outbox_offset;
            outbox_size_ = lanes.outbox_size;

            header_->blob_offset = static_cast<uint32_t>(SHM_BLOB_OFFSET);
            header_->blob_size   = static_cast<uint32_t>(TOTAL_BUFFER_SIZE);
            // The arena's own table, at the front of the blob. init_memory
            // writes the same bytes again when the engine takes the arena;
            // writing it here too means a segment is complete without one,
            // which is what a reader opened against a bare creator sees.
            writeArenaHeader(blob_);

            header_->peer_offset         = static_cast<uint32_t>(SHM_PEER_OFFSET);
            header_->peer_header_bytes   = static_cast<uint32_t>(sizeof(ShmPeerPlaneHeader));
            header_->peer_cmd_ring_bytes = SHM_PEER_CMD_RING_SIZE;
            header_->peer_rep_ring_bytes = SHM_PEER_REP_RING_SIZE;
            shm_peer_plane_init(peer_);

            header_->inbox_offset  = static_cast<uint32_t>(lanes.inbox_offset);
            header_->inbox_size    = static_cast<uint32_t>(lanes.inbox_size);
            header_->outbox_offset = static_cast<uint32_t>(lanes.outbox_offset);
            header_->outbox_size   = static_cast<uint32_t>(lanes.outbox_size);

            // NOTE: MAGIC is deliberately NOT stored here. The geometry fields
            // above are written now (before init_memory()), but the arena blob
            // is still zeroed — and zero is NOT a safe default for every region
            // (the node-tree empty-slot marker is id == -1, i.e. 0xFF, not 0).
            // Publishing MAGIC now would expose a window where a reader observes
            // MAGIC but sees an unpopulated arena (e.g. node entries with id == 0
            // aliasing the real root group). The creator publishes MAGIC via
            // publish() only after the engine's init_memory() has populated the
            // arena, so "MAGIC visible ⇒ fully-populated segment" holds.
        } else {
            // A reader takes EVERYTHING from the arena's table — its own
            // build may size the arena differently, and the table is the
            // cross-build contract. shm_segment_client has checked it against
            // the mapping before this runs, so this cannot fail here.
            blob_ = base8 + header_->blob_offset;
            const char* why = "";
            if (!ShmReaderLayout::from_arena(blob_, header_->blob_size, L_, &why))
                L_ = ShmReaderLayout{};
            peer_ = reinterpret_cast<ShmPeerPlaneHeader*>(seg + header_->peer_offset);
            inbox_       = base8 + header_->inbox_offset;
            inbox_size_  = header_->inbox_size;
            outbox_      = base8 + header_->outbox_offset;
            outbox_size_ = header_->outbox_size;
        }
    }

    // Publish the segment: release-fence, then store MAGIC last. Called once the
    // arena has been populated (after init_memory()). A reader that observes
    // MAGIC behind an acquire fence then sees both the header geometry and the
    // live arena structures — no publish-before-populate window.
    void publish() {
        std::atomic_thread_fence(std::memory_order_release);
        header_->magic = shm_segment_header::MAGIC;
    }

    // Arena blob base. The engine points `shared_memory` here; observers
    // address every region by the layout's offset from this pointer.
    uint8_t* get_base() const { return blob_; }

    // Where everything is, as the header says (the creator's constants, or
    // the engine's published values on the reader side).
    const ShmReaderLayout& layout() const { return L_; }

    PerformanceMetrics* get_metrics() {
        return reinterpret_cast<PerformanceMetrics*>(blob_ + L_.metrics_offset);
    }
    // The guest's publish window, as bytes. What it contains is the guest's
    // and a reader that knows the shape casts it there.
    uint8_t* get_window()      { return blob_ + L_.window_offset; }
    size_t   get_window_size() { return L_.window_bytes; }
    shm_audio_buffer* get_audio_buffers() {
        return reinterpret_cast<shm_audio_buffer*>(blob_ + L_.audio_offset);
    }
    // Slot `index`, at the engine's stride — not sizeof(shm_audio_buffer),
    // which is THIS build's slot and may be another size.
    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        if (index < L_.audio_slot_count)
            return reinterpret_cast<shm_audio_buffer*>(
                blob_ + L_.audio_offset + static_cast<size_t>(index) * L_.audio_slot_bytes);
        return nullptr;
    }

    // Base of a scope-stream slot, or nullptr if out of range.
    // The guest's slots, then the engine's track taps, as one index space.
    uint8_t* get_scope_slot(unsigned int index) { return L_.scopeSlotAt(blob_, index); }

    // Peer command plane (after the arena blob; see shm_peer_plane.h).
    ShmPeerPlaneHeader* get_peer_plane() { return peer_; }

    // The guest's three regions, one writer each. Callers address INTO them by
    // offset from a base — never by a pointer taken from a message, which would
    // belong to the engine's mapping and not to theirs.
    uint8_t* get_inbox()       { return inbox_; }        // client writes
    size_t   get_inbox_size()  { return inbox_size_; }
    uint8_t* get_outbox()      { return outbox_; }       // guest writes
    size_t   get_outbox_size() { return outbox_size_; }

private:
    shm_segment_header* header_;
    ShmReaderLayout     L_;
    uint8_t*            blob_;
    ShmPeerPlaneHeader* peer_;
    uint8_t*            inbox_       = nullptr;
    size_t              inbox_size_  = 0;
    uint8_t*            outbox_      = nullptr;
    size_t              outbox_size_ = 0;
};

// ──── Sample-clock view ─────────────────────────────────────────────────
//
// Seqlock-consistent snapshot of ClockworkClock's sample clock: the mapping
// from sample position to wall-clock DAC time (see shared_memory.h and
// docs/PORTS.md). `valid` is false until the driver has
// published at least once (headless/test runs may never publish).


// ──── Observer views (GUI / passive reader side) ────────────────────────
//
// Byte-level views onto observable regions, for consumers that render them
// generically (e.g. a GUI's panels). The observer tails rings with
// its own local cursor — head/tail here are the engine's, for reference.

struct ring_view {
    uint8_t* base = nullptr;
    uint32_t size = 0;
    std::atomic<int32_t>* head = nullptr;
    std::atomic<int32_t>* tail = nullptr;
};

// The guest's publish window as bytes and a length. A reader that knows what
// the guest put there casts it; clockwork hands over the extent and no more.
struct window_view {
    uint8_t* bytes = nullptr;
    uint32_t size  = 0;
};

// Native-only live engine stats snapshot.
struct native_stats {
    uint32_t cpu_load_avg_centi  = 0;  // DSP load, percent * 100 (smoothed average)
    uint32_t cpu_load_peak_centi = 0;  // DSP load, percent * 100 (decaying peak)
    uint32_t callback_overruns   = 0;  // audio callbacks that overran their budget
    uint32_t nrt_max_pass_us     = 0;  // longest NRT control-drain pass (high-water)
    uint32_t nrt_in_flight_us    = 0;  // NRT control-drain pass blocked right now
    uint32_t nrt_recent_worst_us = 0;  // worst pass in the trailing ~60 s window
};

// ──── Creator (audio engine side) ───────────────────────────────────────

class shm_segment_creator {
public:
    // control_busses is accepted for call-site compatibility but unused: the
    // unified arena is fixed-size and control busses are process-local (heap),
    // so they don't live in the segment.
    // The lanes are the caller's to size (a host's launch config); the
    // segment is as large as they need. NOT cleared here: fresh anonymous
    // memory is zero on every platform, and clearing it would commit every
    // page of a lane whose whole point is to cost nothing until written.
    explicit shm_segment_creator(size_t inbox_bytes = SHM_INBOX_SIZE,
                                 size_t outbox_bytes = SHM_OUTBOX_SIZE):
        lanes_(shm_lane_geometry::of(inbox_bytes, outbox_bytes)),
        handle(shm_create_anonymous(lanes_.segment_size))
    {
        if (lanes_.inbox_size > 0xFFFFFFFFull || lanes_.segment_size > 0xFFFFFFFFull) {
            shm_close(handle);
            throw std::runtime_error("shared memory lanes exceed what the header can address (4 GB)");
        }
        shm = new shm_segment(handle.ptr, true, lanes_);
    }

    ~shm_segment_creator() {
        if (shm)
            disconnect();
    }

    // Unmap and close. Readers that already hold a duplicate keep their pages;
    // the memory itself goes when the last of them does.
    void disconnect() {
        shm_close(handle);
        delete shm;
        shm = nullptr;
    }

    // The object itself, for the attach endpoint to duplicate into readers
    // and for an in-process reader to shm_dup_handle. Not owned by the caller.
    shm_native_handle native_handle() const {
#ifdef _WIN32
        return handle.mapping;
#else
        return handle.fd;
#endif
    }
    size_t segment_size() const { return lanes_.segment_size; }

    // Arena blob base — the engine points `shared_memory` here.
    uint8_t* get_base() { return shm ? shm->get_base() : nullptr; }

    // The guest's regions, handed to the DSP as DspConfig::arena / ::inbox /
    // ::outbox. Bulk travels through the last two, one direction each, so
    // nothing large has to travel as a message.
    uint8_t* get_inbox()       { return shm ? shm->get_inbox() : nullptr; }
    size_t   get_inbox_size()  { return shm ? shm->get_inbox_size() : 0; }
    uint8_t* get_outbox()      { return shm ? shm->get_outbox() : nullptr; }
    size_t   get_outbox_size() { return shm ? shm->get_outbox_size() : 0; }

    // Store MAGIC, making the segment visible to readers. Call once the engine
    // has populated the arena (after init_memory()) so observers never see a
    // published-but-unpopulated segment.
    void publish() { if (shm) shm->publish(); }

    PerformanceMetrics* get_metrics() { return shm ? shm->get_metrics() : nullptr; }
    uint8_t*            get_window()            { return shm ? shm->get_window()            : nullptr; }
    size_t              get_window_size()       { return shm ? shm->get_window_size()       : 0; }
    shm_audio_buffer*   get_audio_buffers()     { return shm ? shm->get_audio_buffers()     : nullptr; }
    ShmPeerPlaneHeader* get_peer_plane()        { return shm ? shm->get_peer_plane()        : nullptr; }

    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        return shm ? shm->get_audio_buffer(index) : nullptr;
    }

    shm_audio_buffer_writer get_audio_buffer_writer(unsigned int index) {
        return shm_audio_buffer_writer(get_audio_buffer(index));
    }

private:
    shm_lane_geometry lanes_;   // declared before handle: it sizes the mapping
    shm_handle            handle;
    shm_segment* shm = nullptr;
};


// ──── Client (GUI / reader side) ────────────────────────────────────────

class shm_segment_client {
public:
    // From a handle the engine handed over: shm_attach::receive() across
    // processes, shm_dup_handle() within one. Owned here from this point,
    // including when the constructor throws.
    explicit shm_segment_client(shm_native_handle received)
    {
        shm_handle handle = shm_map_handle(received);
        // The mapping is owned by a shared_ptr so readers minted from this
        // client can pin it beyond the client's own lifetime (a GUI destroys
        // and re-opens its client on engine cold swap while reader copies
        // live on in widgets). Until map_ owns it, close on any throw — the
        // destructor doesn't run for a throwing constructor.
        try {
            map_ = std::make_shared<mapping>(handle);
        } catch (...) {
            shm_close(handle);
            throw;
        }

        // Size check before touching any field: a truncated segment must not
        // be dereferenced even for its header.
        if (handle.size < sizeof(shm_segment_header))
            throw std::runtime_error(
                "Shared memory segment smaller than its header — stale or foreign segment?");

        auto* header = static_cast<shm_segment_header*>(handle.ptr);
        if (header->magic != shm_segment_header::MAGIC)
            throw std::runtime_error(
                "Invalid shared memory magic — is the audio engine running?");

        // Acquire pairs with the creator's release before the MAGIC store, so
        // observing MAGIC implies a fully-published header.
        std::atomic_thread_fence(std::memory_order_acquire);

        // The header is the contract. This reader addresses every region
        // through it, so the engine's memory profile need not be this build's
        // — what it must be is CONSISTENT: every region inside the mapping,
        // and every fixed-shape struct at least as large as the one this
        // reader casts to. Anything else is refused here rather than read
        // as garbage.
        const uint64_t size = handle.size;
        const auto within = [size](uint64_t off, uint64_t len) {
            return off <= size && len <= size - off;
        };
        if (!within(header->blob_offset, header->blob_size))
            throw std::runtime_error(
                "Shared memory segment smaller than its header claims — truncated or foreign segment?");
        if (!within(header->peer_offset,
                    uint64_t(header->peer_header_bytes) + header->peer_cmd_ring_bytes
                        + header->peer_rep_ring_bytes)
            || header->peer_header_bytes < sizeof(ShmPeerPlaneHeader))
            throw std::runtime_error("Shared memory peer plane outside the segment");
        if (!within(header->inbox_offset, header->inbox_size)
            || !within(header->outbox_offset, header->outbox_size))
            throw std::runtime_error("Shared memory guest regions outside the segment");
        const char* why = "";
        ShmReaderLayout L;
        if (!ShmReaderLayout::from_arena(static_cast<const uint8_t*>(handle.ptr) + header->blob_offset,
                                         header->blob_size, L, &why))
            throw std::runtime_error(std::string("Shared memory layout unusable by this reader: ") + why);

        shm.reset(new shm_segment(handle.ptr, false));
    }
    // Only a real handle. Before the segment was anonymous this took a PORT,
    // and a port converts silently to an int — which then fails at fstat
    // rather than at compile time. Any other integer type is refused here.
    template <typename T>
    explicit shm_segment_client(T) = delete;

    uint8_t*            get_base() { return shm->get_base(); }
    PerformanceMetrics* get_metrics() { return shm->get_metrics(); }
    uint8_t*            get_window()            { return shm->get_window(); }
    size_t              get_window_size()       { return shm->get_window_size(); }
    ShmPeerPlaneHeader* get_peer_plane()        { return shm->get_peer_plane(); }

    /*
     * The guest's region — where bulk lives, in both directions.
     *
     * This is the inverse of loading a sample and it is deliberately the SAME
     * region: a client writes bytes here and sends a short message saying
     * where they are; a guest writes bytes here and sends a short message
     * saying where they are. Neither direction puts a large payload in a ring,
     * and neither asks the audio thread to copy one.
     *
     * ADDRESS BY OFFSET FROM THIS BASE. The engine's mapping and this one are
     * at different addresses, so a pointer that arrived in a message belongs to
     * the other process and means nothing here — and would not fault, it would
     * simply read the wrong bytes. Every message about this region carries an
     * offset, and the bounds check below is why a corrupt or hostile one
     * yields nullptr rather than a read outside the mapping.
     */
    uint8_t* get_inbox()       { return shm->get_inbox(); }
    size_t   get_inbox_size()  { return shm->get_inbox_size(); }
    const uint8_t* get_outbox()      { return shm->get_outbox(); }
    size_t         get_outbox_size() { return shm->get_outbox_size(); }

    // Bounds-checked views of [offset, offset + len) inside a region. nullptr
    // when it does not lie wholly within — including on overflow, which is why
    // the comparison is written as a subtraction rather than as
    // offset + len > size.
    static uint8_t* region_at(uint8_t* base, size_t size,
                              uint32_t offset, uint32_t len) {
        if (!base || offset > size || len > size - offset) return nullptr;
        return base + offset;
    }
    uint8_t* inbox_at(uint32_t offset, uint32_t len) {
        return region_at(shm->get_inbox(), shm->get_inbox_size(), offset, len);
    }
    // Const on this side: the guest writes the outbox, a client only reads it,
    // which is the mirror of DspConfig::inbox being const to the guest.
    const uint8_t* outbox_at(uint32_t offset, uint32_t len) {
        return region_at(shm->get_outbox(), shm->get_outbox_size(), offset, len);
    }

    // Flat atomic u32 view of PerformanceMetrics, for observers that render
    // the fields generically.
    // Everything below addresses the segment through the engine's published
    // layout, never through this build's constants — see ShmReaderLayout.
    const ShmReaderLayout& layout() const { return shm->layout(); }

    const std::atomic<uint32_t>* get_metrics_flat() {
        return reinterpret_cast<const std::atomic<uint32_t>*>(
            shm->get_base() + layout().metrics_offset);
    }
    // The engine's count — a newer engine may publish more fields than this
    // reader's PerformanceMetrics names, and a generic renderer shows them all.
    uint32_t metrics_field_count() const { return layout().metrics_bytes / 4; }

    ring_view get_in_ring() {
        const auto& L = layout();
        return { shm->get_base() + L.in_ring_offset, L.in_ring_size,
                 &control()->in_head, &control()->in_tail };
    }
    ring_view get_out_ring() {
        const auto& L = layout();
        return { shm->get_base() + L.out_ring_offset, L.out_ring_size,
                 &control()->out_head, &control()->out_tail };
    }
    ring_view get_nrt_out_ring() {
        const auto& L = layout();
        return { shm->get_base() + L.nrt_out_ring_offset, L.nrt_out_ring_size,
                 &control()->nrt_out_head, &control()->nrt_out_tail };
    }

    window_view get_window_view() {
        return { shm->get_base() + layout().window_offset, layout().window_bytes };
    }

    const ClockworkClockState* get_clock_state() {
        return reinterpret_cast<const ClockworkClockState*>(
            shm->get_base() + layout().clock_state_offset);
    }

    native_stats get_native_stats() {
        auto field = [this](uint32_t off) {
            return reinterpret_cast<const std::atomic<uint32_t>*>(
                shm->get_base() + layout().native_stats_offset + off)
                ->load(std::memory_order_relaxed);
        };
        return { field(NATIVE_STAT_CPU_AVG_CENTI),
                 field(NATIVE_STAT_CPU_PEAK_CENTI), field(NATIVE_STAT_CB_OVERRUNS),
                 field(NATIVE_STAT_NRT_MAX_PASS_US),
                 field(NATIVE_STAT_NRT_IN_FLIGHT_US),
                 field(NATIVE_STAT_NRT_RECENT_WORST_US) };
    }
    // Native segments always carry the stats region; a web-origin arena has no
    // shm client at all. Kept for observer-API symmetry.
    bool has_native_stats() const { return true; }

    // The reader clamps a slot's self-declared capacity to the ENGINE's ring
    // size, not this build's, which is what keeps a corrupt slot inside the
    // engine's actual allocation.
    shm_scope_stream_reader get_scope_stream_reader(unsigned int index) {
        return shm_scope_stream_reader(
            reinterpret_cast<shm_scope_stream*>(shm->get_scope_slot(index)), map_,
            layout().scope_ring_frames);
    }

    sample_clock_view get_sample_clock() {
        return read_sample_clock(shm->get_base() + layout().sample_clock_offset);
    }

    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        return shm->get_audio_buffer(index);
    }

    shm_audio_buffer_reader get_audio_buffer_reader(unsigned int index) {
        return shm_audio_buffer_reader(shm->get_audio_buffer(index), map_,
                                       layout().audio_frames);
    }

private:
    struct mapping {
        explicit mapping(const shm_handle& h): handle(h) {}
        mapping(const mapping&) = delete;
        mapping& operator=(const mapping&) = delete;
        ~mapping() { shm_close(handle); }
        shm_handle handle;
    };

    ControlPointers* control() {
        return reinterpret_cast<ControlPointers*>(shm->get_base() + layout().control_offset);
    }

    std::shared_ptr<mapping>              map_;
    std::unique_ptr<shm_segment> shm;
};

} /* namespace detail_shm_segment */

using detail_shm_segment::shm_segment_client;
using detail_shm_segment::shm_segment_creator;
using detail_shm_segment::ring_view;
using detail_shm_segment::window_view;
using detail_shm_segment::native_stats;
using detail_shm_segment::sample_clock_view;
// shm_audio_buffer + SHM_AUDIO_* names are exported by shm_audio_buffer.hpp.
