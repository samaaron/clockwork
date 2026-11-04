// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * os_pages.cpp — see os_pages.h.
 */
#include "os_pages.h"
#include "clockwork_heap.h"   // CLOCKWORK_HEAP_CHUNK_BYTES

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace clockwork::pages {

namespace {
constexpr size_t kChunk = CLOCKWORK_HEAP_CHUNK_BYTES;

size_t whole_pages(size_t bytes) {
    const size_t page = page_bytes();
    return (bytes + page - 1) / page * page;
}
} // namespace

size_t whole_chunks(size_t bytes) {
    return (bytes + kChunk - 1) & ~(kChunk - 1);
}

#ifdef _WIN32

size_t page_bytes() {
    SYSTEM_INFO si {};
    GetSystemInfo(&si);
    return si.dwPageSize;
}

// The system's allocation granularity is 64 KiB, a chunk: a reservation is on
// a chunk already.
void* map(size_t bytes) {
    static_assert(kChunk == 65536, "VirtualAlloc reserves on 64 KiB, which must be a chunk");
    if (bytes == 0) return nullptr;
    return VirtualAlloc(nullptr, whole_chunks(bytes), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

void unmap(void* p, size_t) {
    if (p) VirtualFree(p, 0, MEM_RELEASE);
}

// Decommitted pages are gone; committed again, they read zero.
Zero zero(void* p, size_t bytes) {
    const size_t len = whole_pages(bytes);
    if (!VirtualFree(p, len, MEM_DECOMMIT)) return Zero::Kept;
    return VirtualAlloc(p, len, MEM_COMMIT, PAGE_READWRITE) == p ? Zero::Done : Zero::Lost;
}

#else

size_t page_bytes() {
    return static_cast<size_t>(sysconf(_SC_PAGESIZE));
}

// A chunk more than asked, then the ends past the chunk-aligned run given
// back: mmap places on a page, not a chunk.
void* map(size_t bytes) {
    if (bytes == 0) return nullptr;
    const size_t size = whole_chunks(bytes);
    const size_t over = size + kChunk;
    void* raw = mmap(nullptr, over, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) return nullptr;
    const uintptr_t at = reinterpret_cast<uintptr_t>(raw);
    const uintptr_t aligned = (at + kChunk - 1) & ~uintptr_t(kChunk - 1);
    if (aligned > at) munmap(raw, aligned - at);
    const uintptr_t end = aligned + size;
    if (at + over > end) munmap(reinterpret_cast<void*>(end), at + over - end);
    return reinterpret_cast<void*>(aligned);
}

void unmap(void* p, size_t bytes) {
    if (p) munmap(p, whole_chunks(bytes));
}

// Either way the range is as it was when the system refuses: a refused
// madvise changes nothing, and a refused mmap leaves the old mapping.
Zero zero(void* p, size_t bytes) {
    const size_t len = whole_pages(bytes);
#if defined(__linux__)
    // A private anonymous mapping's pages, dropped, are zero-filled when next
    // touched. Refused for pages a host locked (mlockall).
    return madvise(p, len, MADV_DONTNEED) == 0 ? Zero::Done : Zero::Kept;
#else
    // Elsewhere the dropped pages' contents are not promised (macOS's
    // MADV_FREE may keep them): a fresh mapping over the range is.
    return mmap(p, len, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0) == p ? Zero::Done : Zero::Kept;
#endif
}

#endif

} // namespace clockwork::pages
