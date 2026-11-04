// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * os_pages.h — memory straight from the system, natively: whole chunks of the
 * address map's on a chunk (CLOCKWORK_HEAP_CHUNK_BYTES, clockwork_heap.h),
 * zero when mapped, and made to read zero again by giving its pages back
 * rather than by writing them. A span taken this way costs the RAM its user
 * touches and no more (docs/ENGINES.md): a core's guest spans, which
 * are zeroed at every build (dsp_api.h), and its own pool.
 *
 * Linux, macOS, iOS, Windows and the other POSIX systems. Not the web, whose
 * memory is the module's, nor a device, which places its spans by hand.
 * Control threads only: every call is a system call.
 */
#pragma once
#include <cstddef>

namespace clockwork::pages {

// `bytes` rounded up to whole chunks.
size_t whole_chunks(size_t bytes);

// The system's page, in bytes.
size_t page_bytes();

// whole_chunks(bytes) of fresh memory on a chunk, reading zero; null when
// the system will not give it.
void* map(size_t bytes);

// Give a mapping back whole: `p` from map, `bytes` as map was asked.
void unmap(void* p, size_t bytes);

// Make [p, p + bytes) read zero again without writing it: its pages go back
// to the system and come back zero when next touched. `p` on a page, `bytes`
// counted to the end of its last page, and the whole range inside one
// mapping from map. Kept: the system refused and the range is as it was, for
// the caller to write. Lost: the pages went and could not come back (Windows,
// under commit pressure); the range cannot be touched.
enum class Zero { Done, Kept, Lost };
Zero zero(void* p, size_t bytes);

} // namespace clockwork::pages
