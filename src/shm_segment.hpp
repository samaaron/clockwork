// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
//  Shared memory IPC interface to clockwork.
//
//  Two fixed-layout segments other processes map (docs/SEGMENTS.md):
//
//    the PUBLISHED segment — a small header and the arena blob: clock,
//    metrics, scopes, taps, the node tree, the engine's rings. The engine
//    writes it; every reader maps it READ-ONLY, from a read-only descriptor
//    the engine makes for the purpose, so a reader cannot write it however
//    it tries.
//
//    the COMMAND segment — the peer plane (shm_peer_plane.h) and the two
//    bulk lanes (the assets area a client fills, the returns area the guest
//    fills). It exists only when the host asked for the shared-memory
//    command plane, and one peer at a time maps it read-write.
//
//  Both are ANONYMOUS: the engine creates them with no name (memfd on Linux,
//  an unlinked POSIX object on macOS, an unnamed file mapping on Windows)
//  and hands a duplicate of the handle to each reader over the attach
//  endpoint (shm_attach.hpp). Nothing is looked up by name, so there is no
//  namespace to collide in, nothing another user can plant, and nothing left
//  behind when the engine dies — the memory goes with its last mapping.
//
//  The named create/open pair below still exists for the plugin bridge
//  (plugin_bridge.h), whose segment is pid-named and shared with a child the
//  engine itself spawns.

#pragma once

#include "shm_audio_buffer.hpp"
#include "clockwork_product.h"
#include "shm_tap_stream.hpp"
#include "clock/clock_math.h"   // wallClockNTP, kNtpEpochOffset
#include "shared_memory.h"
#include "shm_peer_plane.h"
#include "clockwork_path.h"
#include "workers/wait_word.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <array>
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
    return (static_cast<uint64_t>(rd()) << 32u) ^ static_cast<uint64_t>(rd());
}

// The sample clock is declared with the layout it reads (shared_memory.h)
// and re-exported here.
using ::sample_clock_view;
using ::read_sample_clock;

using std::string;


// ──── The published segment's layout ────────────────────────────────────
//
// A small handshake header followed by the unified shared_memory.h arena
// blob — the SAME layout the engine uses in a core's own arena (native) /
// the WASM SAB. One layout, one source of truth; no separate native layout to
// drift. Headroom, not a snug fit: every field added to the header is a
// region a reader needs to find without compile-time knowledge, and the
// static_assert below fails the build if the header outgrows this.
static constexpr size_t SHM_BLOB_OFFSET = 256;  // aligned, >= sizeof(shm_segment_header)
// A segment for a core laid out as `L`: the header, then its arena.
inline size_t segment_bytes(const clockwork::Layout& L) { return SHM_BLOB_OFFSET + L.arena_bytes; }

// ──── The command segment's layout ──────────────────────────────────────
//
// Its own header, then the peer plane, then the guest's two bulk regions,
// last because they are by far the largest and because nothing before them
// should move when a size changes. Each lane is 16-aligned so a guest can
// place anything it likes at a base without re-aligning first.
//
// ONE WRITER EACH: the plane's command ring and the assets area are written
// only by the peer, its reply ring and the returns area only by the engine
// and the guest. Keeping them apart is what makes that enforceable rather
// than merely agreed. The guest's arena is not here: no client maps it, so
// it is the engine's own allocation rather than a region of a segment.
static constexpr size_t SHM_COMMAND_HEADER_BYTES = 256;   // aligned, >= sizeof(shm_command_header)
static constexpr size_t SHM_PEER_OFFSET          = SHM_COMMAND_HEADER_BYTES;   // alignas(8) holds: 256 % 8 == 0
static constexpr size_t SHM_ASSETS_OFFSET =
    (SHM_PEER_OFFSET + SHM_PEER_PLANE_TOTAL_SIZE + 15u) & ~size_t{15};
static constexpr size_t SHM_ASSETS_SIZE   = static_cast<size_t>(CLOCKWORK_ASSETS_BYTES);
static constexpr size_t SHM_RETURNS_OFFSET =
    (SHM_ASSETS_OFFSET + SHM_ASSETS_SIZE + 15u) & ~size_t{15};
static constexpr size_t SHM_RETURNS_SIZE   = static_cast<size_t>(CLOCKWORK_RETURNS_BYTES);
static constexpr size_t COMMAND_SEGMENT_SIZE = SHM_RETURNS_OFFSET + SHM_RETURNS_SIZE;

// The lanes are sized by the host at launch (ClockworkEngine::Config), the
// constants above being the defaults; the command segment grows to fit, and
// a peer takes the geometry from the header. Everything before the assets
// area is fixed.
struct shm_lane_geometry {
    size_t assets_offset, assets_size, returns_offset, returns_size, segment_size;
    static constexpr size_t align16(size_t v) { return (v + 15u) & ~size_t{15}; }
    static constexpr shm_lane_geometry of(size_t assets_bytes, size_t returns_bytes) {
        const size_t in  = align16(assets_bytes);
        const size_t out = align16(returns_bytes);
        const size_t out_off = align16(SHM_ASSETS_OFFSET + in);
        return { SHM_ASSETS_OFFSET, in, out_off, out, out_off + out };
    }
};
static_assert(shm_lane_geometry::of(SHM_ASSETS_SIZE, SHM_RETURNS_SIZE).segment_size == COMMAND_SEGMENT_SIZE,
              "the default geometry is the constant one");

// ──── Platform shared memory primitives ─────────────────────────────────

#ifdef _WIN32
using shm_native_handle = HANDLE;
static const shm_native_handle shm_invalid_handle = nullptr;
inline bool shm_handle_valid(shm_native_handle h) { return h != nullptr; }
#else
using shm_native_handle = int;
static const shm_native_handle shm_invalid_handle = -1;
inline bool shm_handle_valid(shm_native_handle h) { return h >= 0; }
#endif

struct shm_handle {
    void*  ptr  = nullptr;
    size_t size = 0;
#ifdef _WIN32
    HANDLE mapping = nullptr;
#else
    int    fd  = -1;
#endif
    // A read-only handle onto the same object, when the creator asked for
    // one (shm_create_anonymous): what the published segment is handed out
    // as. Invalid otherwise.
    shm_native_handle read_only = shm_invalid_handle;

    shm_native_handle native() const {
#ifdef _WIN32
        return mapping;
#else
        return fd;
#endif
    }
};

// Close a bare native handle that is not (or no longer) mapped.
inline void shm_close_native(shm_native_handle h) {
#ifdef _WIN32
    if (h) CloseHandle(h);
#else
    if (h >= 0) ::close(h);
#endif
}

// A second reference to the same object, with the same access, for a reader
// in this process (a test, an in-process GUI) that wants a mapping of its
// own. Owned by the caller; the client classes take it.
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
inline shm_native_handle shm_handle_from_intptr(intptr_t v) {
#ifdef _WIN32
    return reinterpret_cast<shm_native_handle>(v);
#else
    return static_cast<shm_native_handle>(v);
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

// Create a segment with no name at all, mapped read-write for its creator.
// With `with_read_only`, also a READ-ONLY handle onto the same object
// (shm_handle::read_only): the one a published segment is handed out as, so
// a reader's mapping can only ever be PROT_READ / FILE_MAP_READ — the kernel
// refuses anything else.
//
// Linux: memfd_create, the object that exists for exactly this purpose; the
// read-only handle is the memfd opened again through /proc/self/fd. Other
// POSIX systems (macOS has no memfd): a POSIX object under a throwaway
// random name, opened a second time O_RDONLY and unlinked the moment both
// are open — the descriptors keep it alive and nothing can ever open it by
// name again. Windows: an unnamed file mapping, its read-only handle a
// duplicate with FILE_MAP_READ as its whole access. Every handle is
// close-on-exec: the plugin bridge and any other child gets its memory by an
// explicit hand-off, never by inheritance.
inline shm_handle shm_create_anonymous(size_t size, bool with_read_only = false) {
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
    if (with_read_only) {
        HANDLE ro = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), h.mapping, GetCurrentProcess(), &ro,
                             FILE_MAP_READ, FALSE, 0)) {
            const std::string why = clockwork_path::last_error_text(GetLastError());
            UnmapViewOfFile(h.ptr);
            CloseHandle(h.mapping);
            h.mapping = nullptr;
            throw std::runtime_error("DuplicateHandle (read-only) failed: " + why);
        }
        h.read_only = ro;
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
    if (h.fd >= 0 && with_read_only) {
        std::array<char, 64> path{};
        std::snprintf(path.data(), path.size(), "/proc/self/fd/%d", h.fd);
        h.read_only = ::open(path.data(), O_RDONLY | O_CLOEXEC);
        if (h.read_only < 0) {
            ::close(h.fd);
            throw std::runtime_error("open (read-only) failed for the anonymous segment");
        }
    }
#  endif
    if (h.fd < 0) {
        // A name nobody can guess, alive for the few instructions between
        // create and unlink. O_EXCL still guards the create: on a collision
        // (or a planted name) the loop simply tries another.
        for (int attempt = 0; attempt < 16 && h.fd < 0; ++attempt) {
            const uint64_t r = detail_random_u64();
            std::array<char, 48> name{};
            std::snprintf(name.data(), name.size(), "/cw-%d-%08x%08x",
                          static_cast<int>(::getpid()),
                          static_cast<unsigned>(r >> 32u), static_cast<unsigned>(r));
            h.fd = ::shm_open(name.data(), O_CREAT | O_EXCL | O_RDWR, 0600);
            if (h.fd < 0) {
                if (errno != EEXIST) break;
                continue;
            }
            if (with_read_only) {
                h.read_only = ::shm_open(name.data(), O_RDONLY, 0);
                if (h.read_only < 0) {
                    ::close(h.fd);
                    ::shm_unlink(name.data());
                    throw std::runtime_error("shm_open (read-only) failed for the anonymous segment");
                }
                ::fcntl(h.read_only, F_SETFD, FD_CLOEXEC);
            }
            ::shm_unlink(name.data());
        }
        if (h.fd < 0)
            throw std::runtime_error("anonymous shm_open failed");
        ::fcntl(h.fd, F_SETFD, FD_CLOEXEC);
    }
    if (ftruncate(h.fd, static_cast<off_t>(size)) < 0) {
        ::close(h.fd);
        shm_close_native(h.read_only);
        throw std::runtime_error("ftruncate (anonymous segment) failed");
    }
    h.ptr = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) {
        ::close(h.fd);
        shm_close_native(h.read_only);
        throw std::runtime_error("mmap (anonymous segment) failed");
    }
#endif
    return h;
}

// Map a segment from a handle received from its creator, writable or not.
// A read-only handle maps only read-only; asking for more is the kernel's
// refusal, reported here. Takes ownership of the handle on every path,
// including failure.
inline shm_handle shm_map_handle(shm_native_handle native, bool writable) {
    shm_handle h;
    if (!shm_handle_valid(native))
        throw std::runtime_error("shm_map_handle: no handle");
#ifdef _WIN32
    h.mapping = native;
    h.ptr = MapViewOfFile(h.mapping, writable ? FILE_MAP_ALL_ACCESS : FILE_MAP_READ, 0, 0, 0);
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
    h.ptr = ::mmap(nullptr, h.size, PROT_READ | (writable ? PROT_WRITE : 0), MAP_SHARED, h.fd, 0);
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
    struct stat st{};
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
    h.mapping = nullptr;
#else
    if (h.ptr && h.ptr != MAP_FAILED) ::munmap(h.ptr, h.size);
    if (h.fd >= 0)                    ::close(h.fd);
    h.fd = -1;
#endif
    shm_close_native(h.read_only);
    h.read_only = shm_invalid_handle;
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

// ──── The published segment's header ────────────────────────────────────
//
//   shm_segment_header        (MAGIC + where the blob is)
//   <pad to SHM_BLOB_OFFSET>
//   shared_memory.h arena blob (the core's layout) — the *same* layout the
//   engine uses in a core's own arena / the WASM SAB. The engine points its
//   `shared_memory` base at this blob, so rings, control pointers, metrics,
//   node-tree, scope and tap streams are all addressed by their
//   shared_memory.h offsets and observable cross-process for free.
//
// MAGIC changes with the layout; a reader accepts the current value only. The
// blob's regions are described by the arena's own header (clockwork_arena.h),
// which every reader (in-process, segment, worklet) takes the same way; this
// header keeps only what is segment-relative.
//
// Publication: the creator zeroes the whole segment and writes the header
// geometry, but defers the MAGIC store. The engine then populates the arena
// (init_memory(): node-tree empty-slot markers = 0xFF, metrics, tap headers,
// …) and only afterwards calls publish(), which stores MAGIC last behind a
// release fence. So a reader that observes MAGIC (after an acquire fence) sees
// both the geometry AND a fully-populated arena — never a half-zeroed one where
// e.g. node entries read id == 0 (aliasing the real root group) instead of the
// id == -1 empty marker. MAGIC visible ⇒ segment ready.
struct shm_segment_header {
    static constexpr uint32_t MAGIC = 0x5C09E00D;
    uint32_t magic;
    uint32_t blob_offset;          // segment base → arena blob (its header is at offset 0 of it)
    uint32_t blob_size;            // == the arena's arena_bytes
};
static_assert(sizeof(shm_segment_header) <= SHM_BLOB_OFFSET,
              "shm_segment_header must fit within SHM_BLOB_OFFSET");

// ──── The command segment's header ──────────────────────────────────────
//
//   shm_command_header        (MAGIC + where everything in it is)
//   the control pass's wake word (ClockworkWaitWord, at wake_offset)
//   <pad to SHM_COMMAND_HEADER_BYTES>
//   ShmPeerPlaneHeader, command ring, reply ring   (shm_peer_plane.h)
//   assets area                                    (the peer writes)
//   returns area                                   (the guest writes)
//
// EVERYTHING INSIDE IT IS ADDRESSED RELATIVE TO ITS BASE, and that is not a
// stylistic choice. A peer mmaps this segment at whatever address the
// kernel gives it, which is not the address the engine sees, so a raw
// pointer in a message would be meaningless across the boundary — and
// meaningless in a way that reads as valid.
//
// Published at creation: there is no populate step, the plane and the lanes
// start zeroed and the plane's geometry is the only thing written.
struct shm_command_header {
    static constexpr uint32_t MAGIC = 0x5C09C00D;
    uint32_t magic;
    uint32_t peer_offset;
    uint32_t peer_header_bytes;    // sizeof(ShmPeerPlaneHeader)
    uint32_t peer_cmd_ring_bytes;  // SHM_PEER_CMD_RING_SIZE
    uint32_t peer_rep_ring_bytes;  // SHM_PEER_REP_RING_SIZE
    uint32_t assets_offset;
    uint32_t assets_size;
    uint32_t returns_offset;
    uint32_t returns_size;
    // The word the engine's control pass sleeps on (src/workers/wait_word.h):
    // the peer bumps it with WaitScope::Shared after each write to the command
    // ring, so the pass wakes with no audio block and no timer. On Windows,
    // which has no cross-process address wait, the bump also signals the event
    // the attach hand-off gave the peer.
    uint32_t wake_offset;
};
static constexpr size_t SHM_COMMAND_WAKE_OFFSET = 128;   // inside the header's reserved span, 8-aligned
static_assert(sizeof(shm_command_header) <= SHM_COMMAND_WAKE_OFFSET,
              "shm_command_header runs into the wake word");
static_assert(SHM_COMMAND_WAKE_OFFSET + sizeof(ClockworkWaitWord) <= SHM_COMMAND_HEADER_BYTES,
              "the wake word must fit the header's reserved span");
static_assert(sizeof(shm_command_header) <= SHM_COMMAND_HEADER_BYTES,
              "shm_command_header must fit within SHM_COMMAND_HEADER_BYTES");
static_assert(SHM_PEER_OFFSET % 8 == 0,
              "peer plane must be 8-byte aligned (ShmPeerPlaneHeader is alignas(8))");

// ──── shm_segment ──────────────────────────────────────────────
//
// Process-local view of the published segment: holds the arena blob base and
// exposes the observable regions by their shared_memory.h offsets. NOT itself
// in shared memory. A reader's mapping is read-only; the pointers here are
// its addresses and nothing a reader writes through.

class shm_segment {
public:
    // The creator's: lays the segment out as its core's `layout` says and
    // publishes that layout in the header.
    shm_segment(void* segment_base, const clockwork::Layout& layout)
        : header_(static_cast<shm_segment_header*>(segment_base)),
          blob_(static_cast<uint8_t*>(segment_base) + SHM_BLOB_OFFSET),
          L_(ShmReaderLayout::from_layout(layout)) {
        header_->blob_offset = static_cast<uint32_t>(SHM_BLOB_OFFSET);
        header_->blob_size   = layout.arena_bytes;
        // The arena's own table, at the front of the blob. init_memory
        // writes the same bytes again when the engine takes the arena;
        // writing it here too means a segment is complete without one,
        // which is what a reader opened against a bare creator sees.
        writeArenaHeader(blob_, layout);
        // MAGIC is deliberately NOT stored here: the arena blob is still
        // zeroed, and zero is not a safe default for every region. The
        // creator publishes it via publish() once the engine's
        // init_memory() has populated the arena.
    }
    // A reader's: takes EVERYTHING from the arena's table — the engine may
    // size the arena differently from this build's default, and the table
    // is the cross-build contract. shm_segment_client has checked it
    // against the mapping before this runs, so this cannot fail here.
    explicit shm_segment(void* segment_base)
        : header_(static_cast<shm_segment_header*>(segment_base)),
          blob_(static_cast<uint8_t*>(segment_base) + header_->blob_offset) {
        const char* why = "";
        if (!ShmReaderLayout::from_arena(blob_, header_->blob_size, L_, &why))
            L_ = ShmReaderLayout{};
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

    // Where everything is, as the header says (the creator's layout, or the
    // engine's published values on the reader side).
    const ShmReaderLayout& layout() const { return L_; }

    PerformanceMetrics* get_metrics() const {
        return reinterpret_cast<PerformanceMetrics*>(blob_ + L_.metrics_offset);
    }
    // The guest's publish window, as bytes. What it contains is the guest's
    // and a reader that knows the shape casts it there.
    uint8_t* get_window() const      { return blob_ + L_.window_offset; }
    size_t   get_window_size() const { return L_.window_bytes; }
    // Slot `index`, at the engine's stride.
    shm_audio_buffer* get_audio_buffer(unsigned int index) const {
        if (index < L_.audio_slot_count)
            return reinterpret_cast<shm_audio_buffer*>(
                blob_ + L_.audio_offset + static_cast<size_t>(index) * L_.audio_slot_bytes);
        return nullptr;
    }

    // Base of a tap-stream slot, or nullptr if out of range.
    // The guest's slots, then the engine's track taps, as one index space.
    uint8_t* get_tap_slot(unsigned int index) const { return L_.tapSlotAt(blob_, index); }

private:
    shm_segment_header* header_;
    uint8_t*            blob_;
    ShmReaderLayout     L_;
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
// generically (e.g. a GUI's panels). Read-only: the rings and their cursors
// are the engine's, and a reader's mapping could not take a write anyway.

struct ring_view {
    const uint8_t* base = nullptr;
    uint32_t size = 0;
    const std::atomic<int32_t>* head = nullptr;
    const std::atomic<int32_t>* tail = nullptr;
};

// The guest's publish window as bytes and a length. A reader that knows what
// the guest put there casts it; clockwork hands over the extent and no more.
struct window_view {
    const uint8_t* bytes = nullptr;
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

// ──── Creator of the published segment (audio engine side) ──────────────

class shm_segment_creator {
public:
    // A segment for a core laid out as `layout`. NOT cleared here: fresh
    // anonymous memory is zero on every platform.
    explicit shm_segment_creator(const clockwork::Layout& layout):
        handle(shm_create_anonymous(segment_bytes(layout), /*with_read_only=*/true)),
        bytes(segment_bytes(layout))
    {
        shm = std::make_unique<shm_segment>(handle.ptr, layout);
    }
    // The segment's one creator: its handle and mapping are not things to copy.
    shm_segment_creator(const shm_segment_creator&) = delete;
    shm_segment_creator& operator=(const shm_segment_creator&) = delete;

    ~shm_segment_creator() {
        if (shm)
            disconnect();
    }

    // Unmap and close. Readers that already hold a duplicate keep their pages;
    // the memory itself goes when the last of them does.
    void disconnect() {
        shm_close(handle);
        shm.reset();
    }

    // The engine's own, read-write handle. Not owned by the caller, and not
    // what a reader is given.
    shm_native_handle native_handle() const { return handle.native(); }
    // The READ-ONLY handle onto the same object: what the attach endpoint
    // duplicates into readers and an in-process reader shm_dup_handles. A
    // mapping made from it can only be read. Not owned by the caller.
    shm_native_handle reader_handle() const { return handle.read_only; }
    size_t segment_size() const { return bytes; }

    // Arena blob base — the engine points `shared_memory` here.
    uint8_t* get_base() { return shm ? shm->get_base() : nullptr; }

    // Store MAGIC, making the segment visible to readers. Call once the engine
    // has populated the arena (after init_memory()) so observers never see a
    // published-but-unpopulated segment.
    void publish() { if (shm) shm->publish(); }

    PerformanceMetrics* get_metrics() { return shm ? shm->get_metrics() : nullptr; }
    uint8_t*            get_window()            { return shm ? shm->get_window()            : nullptr; }
    size_t              get_window_size()       { return shm ? shm->get_window_size()       : 0; }

    shm_audio_buffer* get_audio_buffer(unsigned int index) {
        return shm ? shm->get_audio_buffer(index) : nullptr;
    }

private:
    shm_handle                   handle;
    size_t                       bytes;
    std::unique_ptr<shm_segment> shm;
};

// ──── Creator of the command segment (audio engine side) ────────────────
//
// Only with Config::shmCommands: the peer plane and the lanes, for the one
// peer the attach endpoint lets hold it. Read-write for both sides.

class shm_command_segment_creator {
public:
    // The lanes are the caller's to size (a host's launch config); the
    // segment is as large as they need. NOT cleared: fresh anonymous memory
    // is zero, and clearing it would commit every page of a lane whose whole
    // point is to cost nothing until written.
    explicit shm_command_segment_creator(size_t assets_bytes = SHM_ASSETS_SIZE,
                                         size_t returns_bytes = SHM_RETURNS_SIZE):
        lanes_(shm_lane_geometry::of(assets_bytes, returns_bytes)),
        handle(shm_create_anonymous(lanes_.segment_size))
    {
        if (lanes_.assets_size > 0xFFFFFFFFull || lanes_.segment_size > 0xFFFFFFFFull) {
            shm_close(handle);
            throw std::runtime_error("shared memory lanes exceed what the header can address (4 GB)");
        }
        auto* base = static_cast<uint8_t*>(handle.ptr);
        header_ = reinterpret_cast<shm_command_header*>(base);
        peer_   = reinterpret_cast<ShmPeerPlaneHeader*>(base + SHM_PEER_OFFSET);
        header_->peer_offset         = static_cast<uint32_t>(SHM_PEER_OFFSET);
        header_->peer_header_bytes   = static_cast<uint32_t>(sizeof(ShmPeerPlaneHeader));
        header_->peer_cmd_ring_bytes = SHM_PEER_CMD_RING_SIZE;
        header_->peer_rep_ring_bytes = SHM_PEER_REP_RING_SIZE;
        header_->assets_offset  = static_cast<uint32_t>(lanes_.assets_offset);
        header_->assets_size    = static_cast<uint32_t>(lanes_.assets_size);
        header_->returns_offset = static_cast<uint32_t>(lanes_.returns_offset);
        header_->returns_size   = static_cast<uint32_t>(lanes_.returns_size);
        header_->wake_offset    = static_cast<uint32_t>(SHM_COMMAND_WAKE_OFFSET);
        wake_ = new (base + SHM_COMMAND_WAKE_OFFSET) ClockworkWaitWord();
#ifdef _WIN32
        // Windows has no cross-process address wait: the peer wakes the pass
        // through this auto-reset event, a handle to which the attach hand-off
        // duplicates into it.
        wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!wake_event_) {
            const std::string why = clockwork_path::last_error_text(GetLastError());
            shm_close(handle);
            throw std::runtime_error("CreateEvent for the command segment's wake failed: " + why);
        }
#endif
        shm_peer_plane_init(peer_);
        std::atomic_thread_fence(std::memory_order_release);
        header_->magic = shm_command_header::MAGIC;
    }
    shm_command_segment_creator(const shm_command_segment_creator&) = delete;
    shm_command_segment_creator& operator=(const shm_command_segment_creator&) = delete;
    ~shm_command_segment_creator() {
#ifdef _WIN32
        if (wake_event_) CloseHandle(wake_event_);
#endif
        shm_close(handle);
    }

    // The word the engine's control pass sleeps on, in this segment so the
    // peer wakes it; on Windows also the event a peer signals (null elsewhere).
    ClockworkWaitWord*   wake_word() const { return wake_; }
    clockwork::WaitEvent wake_event() const {
#ifdef _WIN32
        return wake_event_;
#else
        return nullptr;
#endif
    }

    // The read-write handle the attach endpoint duplicates into the one peer.
    // Not owned by the caller.
    shm_native_handle native_handle() const { return handle.native(); }
    size_t segment_size() const { return lanes_.segment_size; }
    uint8_t* get_base() const { return static_cast<uint8_t*>(handle.ptr); }

    ShmPeerPlaneHeader* get_peer_plane() const { return peer_; }

    // The guest's two lanes, handed to the DSP as DspConfig::assets /
    // ::returns. Bulk travels through them, one direction each, so nothing
    // large has to travel as a message.
    uint8_t* get_assets()       const { return get_base() + lanes_.assets_offset; }
    size_t   get_assets_size()  const { return lanes_.assets_size; }
    uint8_t* get_returns()      const { return get_base() + lanes_.returns_offset; }
    size_t   get_returns_size() const { return lanes_.returns_size; }
    size_t   assets_offset()    const { return lanes_.assets_offset; }
    size_t   returns_offset()   const { return lanes_.returns_offset; }

    // Tell an arena where the lanes are: offsets from this segment's base,
    // in the command segment's space (clockwork_arena.h), which every reader
    // of that arena can see and only the peer can reach.
    void describe_in(uint8_t* arena_blob) const {
        arenaSetArea(arena_blob, CLOCKWORK_AREA_ASSETS,  CLOCKWORK_SPACE_COMMAND_SEGMENT,
                     lanes_.assets_offset,  lanes_.assets_size);
        arenaSetArea(arena_blob, CLOCKWORK_AREA_RETURNS, CLOCKWORK_SPACE_COMMAND_SEGMENT,
                     lanes_.returns_offset, lanes_.returns_size);
    }

private:
    shm_lane_geometry   lanes_;   // declared before handle: it sizes the mapping
    shm_handle          handle;
    shm_command_header* header_ = nullptr;
    ShmPeerPlaneHeader* peer_   = nullptr;
    ClockworkWaitWord*  wake_   = nullptr;
#ifdef _WIN32
    HANDLE              wake_event_ = nullptr;
#endif
};


// ──── Reader of the published segment (GUI / observer side) ─────────────
//
// Maps READ-ONLY and hands out only const views. The handle it is given is
// the read-only one, so the kernel refuses a writable mapping; the class
// asks for none.

class shm_segment_client {
public:
    // From a handle the engine handed over: shm_attach::receive() across
    // processes, shm_dup_handle(reader_handle()) within one. Owned here from
    // this point, including when the constructor throws.
    explicit shm_segment_client(shm_native_handle received)
    {
        shm_handle handle = shm_map_handle(received, /*writable=*/false);
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

        const auto* header = static_cast<const shm_segment_header*>(handle.ptr);
        if (header->magic != shm_segment_header::MAGIC)
            throw std::runtime_error(
                "Invalid shared memory magic — is the audio engine running, and is this its published segment?");

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
        const char* why = "";
        ShmReaderLayout L;
        if (!ShmReaderLayout::from_arena(static_cast<const uint8_t*>(handle.ptr) + header->blob_offset,
                                         header->blob_size, L, &why))
            throw std::runtime_error(std::string("Shared memory layout unusable by this reader: ") + why);

        shm = std::make_unique<shm_segment>(handle.ptr);
    }
    // Only a real handle. Any other integer (a port number, say) would convert
    // silently to an int and fail at fstat rather than at compile time, so
    // every other integer type is refused here.
    template <typename T>
    explicit shm_segment_client(T) = delete;

    const uint8_t*            get_base() const        { return shm->get_base(); }
    const PerformanceMetrics* get_metrics() const     { return shm->get_metrics(); }
    const uint8_t*            get_window() const      { return shm->get_window(); }
    size_t                    get_window_size() const { return shm->get_window_size(); }

    // Flat atomic u32 view of PerformanceMetrics, for observers that render
    // the fields generically.
    // Everything below addresses the segment through the engine's published
    // layout, never through this build's constants — see ShmReaderLayout.
    const ShmReaderLayout& layout() const { return shm->layout(); }

    const std::atomic<uint32_t>* get_metrics_flat() const {
        return reinterpret_cast<const std::atomic<uint32_t>*>(
            shm->get_base() + layout().metrics_offset);
    }
    // The engine's count — a newer engine may publish more fields than this
    // reader's PerformanceMetrics names, and a generic renderer shows them all.
    uint32_t metrics_field_count() const { return layout().metrics_bytes / 4; }

    // The engine's rings: where they are and how full they are. Nothing a
    // reader writes — the in ring is the engine process's, and taking from
    // an egress ring is a peer's (clockwork_client.h).
    ring_view get_in_ring() const {
        const auto& L = layout();
        return { shm->get_base() + L.in_ring_offset, L.in_ring_size,
                 &control()->in_head, &control()->in_tail };
    }
    ring_view get_out_ring() const {
        const auto& L = layout();
        return { shm->get_base() + L.out_ring_offset, L.out_ring_size,
                 &control()->out_head, &control()->out_tail };
    }
    ring_view get_nrt_out_ring() const {
        const auto& L = layout();
        return { shm->get_base() + L.nrt_out_ring_offset, L.nrt_out_ring_size,
                 &control()->nrt_out_head, &control()->nrt_out_tail };
    }

    window_view get_window_view() const {
        return { shm->get_base() + layout().window_offset, layout().window_bytes };
    }

    const ClockworkClockState* get_clock_state() const {
        return reinterpret_cast<const ClockworkClockState*>(
            shm->get_base() + layout().clock_state_offset);
    }

    native_stats get_native_stats() const {
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

    // The stream readers take a slot pointer and only ever read through it
    // (loads of the slot's atomics, copies of its frames); the mapping behind
    // it is read-only, so the pointer's type gives them nothing to use.
    //
    // The reader clamps a slot's self-declared capacity to the ENGINE's ring
    // size, not this build's, which is what keeps a corrupt slot inside the
    // engine's actual allocation.
    shm_tap_stream_reader get_tap_stream_reader(unsigned int index) const {
        return shm_tap_stream_reader(
            reinterpret_cast<shm_tap_stream*>(shm->get_tap_slot(index)), layout().tap_ring_frames, map_);
    }

    sample_clock_view get_sample_clock() const {
        return read_sample_clock(shm->get_base() + layout().sample_clock_offset);
    }

    shm_audio_buffer_reader get_audio_buffer_reader(unsigned int index) const {
        return shm_audio_buffer_reader(shm->get_audio_buffer(index), layout().audio_frames,
                                       layout().audio_channels, map_);
    }

private:
    struct mapping {
        explicit mapping(const shm_handle& h): handle(h) {}
        mapping(const mapping&) = delete;
        mapping& operator=(const mapping&) = delete;
        ~mapping() { shm_close(handle); }
        shm_handle handle;
    };

    const RingCursors* control() const {
        return reinterpret_cast<const RingCursors*>(shm->get_base() + layout().control_offset);
    }

    std::shared_ptr<mapping>     map_;
    std::unique_ptr<shm_segment> shm;
};

// ──── The peer's mapping of the command segment ─────────────────────────
//
// Read-write: the one peer writes the plane's command ring and the assets
// area, and reads the reply ring and the returns area.

class shm_command_client {
public:
    // From a handle the engine handed over: shm_attach::receive_command_plane()
    // across processes, shm_dup_handle() within one. Owned here from this
    // point, including when the constructor throws. `wake_event` is the
    // Windows event the hand-off gave with it (null elsewhere), owned here too.
    explicit shm_command_client(shm_native_handle received, clockwork::WaitEvent wake_event = nullptr)
        : handle_(shm_map_handle(received, /*writable=*/true)), wake_event_(wake_event)
    {
        try {
            if (handle_.size < sizeof(shm_command_header))
                throw std::runtime_error(
                    "Shared memory command segment smaller than its header — stale or foreign segment?");
            const auto* header = static_cast<const shm_command_header*>(handle_.ptr);
            if (header->magic != shm_command_header::MAGIC)
                throw std::runtime_error(
                    "Invalid shared memory magic — is this the engine's command segment?");
            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t size = handle_.size;
            const auto within = [size](uint64_t off, uint64_t len) {
                return off <= size && len <= size - off;
            };
            if (!within(header->peer_offset,
                        uint64_t(header->peer_header_bytes) + header->peer_cmd_ring_bytes
                            + header->peer_rep_ring_bytes)
                || header->peer_header_bytes < sizeof(ShmPeerPlaneHeader)
                || header->peer_offset % alignof(ShmPeerPlaneHeader) != 0)
                throw std::runtime_error("Shared memory peer plane outside the segment or not one this build knows");
            if (!within(header->assets_offset, header->assets_size)
                || !within(header->returns_offset, header->returns_size))
                throw std::runtime_error("Shared memory lanes outside the segment");
            if (!within(header->wake_offset, sizeof(ClockworkWaitWord))
                || header->wake_offset % alignof(ClockworkWaitWord) != 0)
                throw std::runtime_error("Shared memory wake word outside the segment");
            auto* base = static_cast<uint8_t*>(handle_.ptr);
            peer_         = reinterpret_cast<ShmPeerPlaneHeader*>(base + header->peer_offset);
            assets_       = base + header->assets_offset;
            assets_size_  = header->assets_size;
            returns_      = base + header->returns_offset;
            returns_size_ = header->returns_size;
            wake_         = reinterpret_cast<ClockworkWaitWord*>(base + header->wake_offset);
        } catch (...) {
            shm_close(handle_);
            close_wake_event();
            throw;
        }
    }
    template <typename T>
    explicit shm_command_client(T) = delete;
    shm_command_client(const shm_command_client&) = delete;
    shm_command_client& operator=(const shm_command_client&) = delete;
    ~shm_command_client() {
        shm_close(handle_);
        close_wake_event();
    }

    ShmPeerPlaneHeader* get_peer_plane() const { return peer_; }

    // Wake the engine's control pass: after each write to the command ring, so
    // it is read with no audio block and no timer. Never blocks.
    void wake_engine() const { clockwork::bump(*wake_, clockwork::WaitScope::Shared, wake_event_); }
    ClockworkWaitWord* wake_word() const { return wake_; }

    /*
     * The lanes — where bulk lives, in both directions.
     *
     * This is the inverse of loading a sample and it is deliberately the SAME
     * mechanism: the peer writes bytes into the assets area and sends a short
     * message saying where; the guest writes bytes into the returns area and
     * does the same. Neither direction puts a large payload in a ring, and
     * neither asks the audio thread to copy one.
     *
     * ADDRESS BY OFFSET FROM A BASE. The engine's mapping and this one are
     * at different addresses, so a pointer that arrived in a message belongs to
     * the other process and means nothing here — and would not fault, it would
     * simply read the wrong bytes. Every message about a lane carries an
     * offset, and the bounds check below is why a corrupt or hostile one
     * yields nullptr rather than a read outside the mapping.
     */
    uint8_t*       get_assets()       const { return assets_; }        // the peer writes
    size_t         get_assets_size()  const { return assets_size_; }
    const uint8_t* get_returns()      const { return returns_; }       // the guest writes
    size_t         get_returns_size() const { return returns_size_; }

    // Bounds-checked views of [offset, offset + len) inside a lane. nullptr
    // when it does not lie wholly within — including on overflow, which is why
    // the comparison is written as a subtraction rather than as
    // offset + len > size.
    static uint8_t* region_at(uint8_t* base, size_t size,
                              uint32_t offset, uint32_t len) {
        if (!base || offset > size || len > size - offset) return nullptr;
        return base + offset;
    }
    uint8_t* assets_at(uint32_t offset, uint32_t len) const {
        return region_at(assets_, assets_size_, offset, len);
    }
    const uint8_t* returns_at(uint32_t offset, uint32_t len) const {
        return region_at(returns_, returns_size_, offset, len);
    }

private:
    void close_wake_event() {
#ifdef _WIN32
        if (wake_event_) CloseHandle(static_cast<HANDLE>(wake_event_));
#endif
        wake_event_ = nullptr;
    }

    shm_handle           handle_;
    clockwork::WaitEvent wake_event_   = nullptr;
    ShmPeerPlaneHeader*  peer_         = nullptr;
    uint8_t*             assets_       = nullptr;
    size_t               assets_size_  = 0;
    uint8_t*             returns_      = nullptr;
    size_t               returns_size_ = 0;
    ClockworkWaitWord*   wake_         = nullptr;
};

} /* namespace detail_shm_segment */

using detail_shm_segment::shm_segment_client;
using detail_shm_segment::shm_segment_creator;
using detail_shm_segment::shm_command_client;
using detail_shm_segment::shm_command_segment_creator;
using detail_shm_segment::ring_view;
using detail_shm_segment::window_view;
using detail_shm_segment::native_stats;
using detail_shm_segment::sample_clock_view;
// shm_audio_buffer + SCOPE_* names are exported by shm_audio_buffer.hpp.
