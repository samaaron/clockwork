// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    Clockwork Memory Layout

    Defines the WASM memory regions. The initial committed memory is small
    (heap + ring buffers + the guest's initial region). The guest region
    The assets area grows on demand up to maxAssetsSize by extending WASM memory.

    Both assetsSize and maxAssetsSize can be overridden at runtime
    via Clockwork constructor options:
      memory: { guestMemorySize: 64 * 1024 * 1024 }  // 64MB committed at boot
      memory: { maxAssetsSize: 512 * 1024 * 1024 }  // 512MB ceiling

    Memory Layout:
      0-14MB:   WASM Heap (emscripten malloc, static data, stack) — and the
                ring buffers, which are a static array in the data segment,
                not a region of their own
      14MB+:    Guest memory — opaque to clockwork, grows on demand

    Clockwork does not know what is in the guest region. There is one
    region and the guest subdivides it; naming sub-regions here (an RT pool,
    a sample buffer pool) would write one guest's memory plan into clockwork.
    Clockwork reserves the address space, commits the initial slice, grows
    it on request, and moves opaque bytes into it. What those bytes mean — an
    allocator arena, sample frames, anything else — is the guest's business
    and is decided by the guest's own client code.

    THE RINGS ARE INSIDE THE HEAP, not above it. `arena_storage` is a
    static array, so the linker places it in the data segment; the regions
    span about 6.72MB from `arenaBase`, all of it inside the heap.

    The number that matters is guestMemoryOffset, because emscripten's malloc
    does not know about any of these regions: it starts after the static data
    and grows upward until it runs into whatever we put next.
    wasmHeapSize, ringBufferReserved and memArenaSize are only names for parts
    of that gap — malloc gets the whole of it, and only their SUM decides
    anything.

    Static data ends at about 7.4MB, so the rest of the gap is malloc's whole
    budget. With a gap only a few MB past the static data, a few hundred KB
    more in a ring is enough to push malloc into the guest's region and
    corrupt it. The gap is guestMemoryOffset, the sum of the three.

    Nothing in JS measures this margin. clockwork_init refuses to boot when
    malloc's frontier reaches a guest region the host placed itself, with the
    two addresses in the message; and the guest's spans come out of the
    engine's own memory below that frontier, so there is nothing to reach.
*/

/**
 * Memory Layout Configuration
 *
 * Defines SharedArrayBuffer structure and WebAssembly memory allocation.
 * These values are read by build.sh to set emscripten's -sINITIAL_MEMORY flag.
 */
export const MemoryLayout = {
    /**
     * WASM heap size in bytes
     * Space for emscripten malloc, static data, and stack.
     * Holds the static ring-buffer array (6.72MB measured) plus malloc and
     * the stack, so treat the difference as the real malloc budget and
     * re-measure with `sonic.bufferConstants` after changing any region size.
     * Only the sum of this, ringBufferReserved and memArenaSize matters; see
     * the header.
     * Current: 24MB
     */
    wasmHeapSize: 24 * 1024 * 1024,  // 24MB — see the measurement below

    /*
     * WHY 24MB.
     *
     * What this space must hold, measured on the web build:
     *
     *     static data (incl. the 6.474MB of rings)    6.72 MB
     *     wasm stack (--stack-first)                  1.00 MB
     *     clockwork_heap's backing block (CLOCKWORK_HEAP_SIZE)    8.00 MB
     *     everything else emscripten mallocs          ?
     *                                                --------
     *                                                15.7 MB +
     *
     * A smaller floor does not fit: clockwork_heap is claimed with malloc, so
     * its backing runs past guestMemoryOffset and the two allocators share
     * bytes on every boot with nothing reporting it. Changing the floor then
     * only moves which bytes are shared, which shows up as unrelated tests
     * failing.
     *
     * So the floor holds what is measured above, and the boot refuses to run
     * if the heap's end reaches the guest region (see
     * clockwork_heap_backing_end). If that fires, raise this number.
     */

    /**
     * Ring buffer reserved space (between the WASM heap and the guest's
     * region).
     *
     * DO NOT LIST THE FIGURES HERE OR RE-DERIVE THE TOTAL BY HAND — a written
     * total drifts from the build it describes, and TAPS, the largest
     * region of the lot, is the one most easily forgotten. The regions are
     * laid out by
     * src/shared_memory.h from the build's own -D flags, and the module
     * exports every start and size at runtime. Read them from a booted engine
     * (`sonic.bufferConstants`) and measure.
     *
     * Measured on the web build: the regions span 6.474MB from
     * `arenaBase`, of which TAPS is 4.097MB, SCOPE 1.500MB and
     * the OSC rings 1.125MB (IN 1MB + OUT 128KB). They sit inside the WASM
     * heap below, not in the reservation named here.
     */
    /**
     * Clockwork's placement arena, in bytes — what clockwork::mem allocates from
     * on this target.
     *
     * A THIRD NAME FOR PART OF THE SAME GAP below guestMemoryOffset, like
     * wasmHeapSize and ringBufferReserved: the module claims it with one
     * malloc at init_memory and hands it to clockwork::mem::set_arena, so it has to
     * be budgeted here or that malloc pushes the break upward into the guest.
     *
     * 32MB: the guest's two spans come out of it — its fast memory, sized to what
     * it declares (dsp_memory_bytes: scsynth's tables and its pool), and its
     * bulk memory, which takes what is left after the RT router's block
     * (CLOCKWORK_GUEST_MEMORY_BULK_BYTES, CLOCKWORK_FAST_RESERVE in
     * src/memory_profile.h) — the same shape as on every other target.
     *
     * It bounds the engine: a span the guest declares that does not fit
     * fails the boot with the two figures in the message, rather than being
     * served from memory clockwork does not own. engineOverhead below is what
     * a client adds to a guest's declared total to size this.
     */
    memArenaSize: 32 * 1024 * 1024,  // 32MB

    /**
     * What the engine takes of memArenaSize beside the guest's two spans, in
     * bytes: the RT router's block (CLOCKWORK_ROUTER_BYTES, 1 MB) and the
     * placement arena's own headers. A client that sizes memArenaSize
     * for a guest's declared memory (SuperSonic does, from scsynth's
     * options) adds this, or the guest's bulk want does not fit and the
     * boot is refused with the two figures in the message. Mirrors
     * src/memory_profile.h's web figures; a generous round number, since
     * the headers are a few hundred bytes.
     */
    engineOverhead: 2 * 1024 * 1024,

    ringBufferReserved: 2 * 1024 * 1024,  // 2MB — see the header: the rings do
                                          // not live here, and this only sets
                                          // where the guest's region starts

    /**
     * A fixed slice for the guest's fast memory, in bytes. 0: none.
     *
     * The guest's fast memory (DspConfig::memory) is reserved by clockwork
     * itself, from the engine's memory (memArenaSize), sized to what the guest
     * asks — DspInfo::memory_bytes_wanted, and what its configuration adds
     * through dsp_memory_bytes (scsynth: its tables and its real-time pool) —
     * and so is its bulk memory, the span its allocations come from. The same
     * on every platform. A host that wants to place the fast slice itself may
     * still give one here; it is handed to the guest as is, and clockwork
     * reserves no fast span.
     *
     * Override at runtime: memory: { guestMemorySize: N }
     */
    guestMemorySize: 0,

    /**
     * Bulk staging, committed at boot. ONE WRITER EACH: the client writes the
     * assets and the guest only reads it; the guest writes the returns area and the
     * client only reads it. Neither is the guest's arena, and nothing is
     * written from both ends — which is what removes the need for the two
     * sides to agree at runtime about which bytes belong to whom.
     *
     * The ASSETS sits at the top of the memory and is the only region that
     * grows, because bulk arriving from the client is the only thing whose
     * size a session discovers as it runs. See assetsOffset.
     *
     * Override at runtime: memory: { assetsSize: N, returnsSize: N }
     */
    assetsSize:  4 * 1024 * 1024,   // client -> guest, grows to maxAssetsSize
    returnsSize: 4 * 1024 * 1024,   // guest -> client, fixed

    /**
     * Maximum assets size in bytes — the hard ceiling for growth.
     *
     * WASM address space is RESERVED up to assetsOffset +
     * maxAssetsSize at construction and can never be raised afterwards;
     * only the committed size grows into it. That is why this is a build-time
     * cap rather than something a caller can lift at runtime.
     *
     * Growth works because the memory is created with
     * `shared: true`. A non-shared WebAssembly.Memory DETACHES its old
     * ArrayBuffer on grow() and every view over it throws; a shared one does
     * not, and pre-existing views keep reading. The growable allocator relies
     * on this, because it appends a new pool per growth and keeps the earlier
     * pools' buffer references alive.
     */
    maxAssetsSize: 768 * 1024 * 1024,  // 768MB — bulk in; see memArenaSize

    /**
     * Guest memory byte offset from the start of the SharedArrayBuffer.
     *
     * Everything below this is the module's own: emscripten's static data,
     * malloc and stack, clockwork's ring buffers (a static array inside that
     * same heap) and the placement arena. Everything at or above it is the
     * guest's and clockwork never interprets it.
     */
    get returnsOffset() {
        return this.wasmHeapSize + this.ringBufferReserved + this.memArenaSize;
    },

    get guestMemoryOffset() {
        return this.returnsOffset + this.returnsSize;
    },

    /**
     * THE ASSETS IS LAST, because it is the one region that grows.
     *
     * Only the region at the top of the memory can grow in place — everything
     * above it would have to move, and a published range that moved would be a
     * pointer the guest already holds pointing at something else. The thing
     * that grows is bulk arriving from the client: samples, wavetables,
     * impulse responses, however many of them a session turns out to load. The
     * guest's arena is sized once at boot, as an arena is.
     */
    get assetsOffset() {
        return this.guestMemoryOffset + this.guestMemorySize;
    },

    /**
     * Total committed memory (derived)
     * assetsOffset + assetsSize
     */
    get totalMemory() {
        return this.assetsOffset + this.assetsSize;
    },

    /**
     * Maximum total WASM memory (derived, used by build.sh for -sMAXIMUM_MEMORY).
     * Computed as: assetsOffset + maxAssetsSize.
     *
     * This is the RESERVATION. It fixes the address space at construction and
     * cannot be raised later, so it is the one number a build must get right
     * up front; everything else is committed on demand beneath it.
     */
    get maxTotalMemory() {
        return this.assetsOffset + Math.max(this.maxAssetsSize, this.assetsSize);
    },

    /**
     * Total WebAssembly memory in pages (derived, 1 page = 64KB)
     * Used by build.sh to set -sINITIAL_MEMORY.
     */
    get totalPages() {
        return Math.ceil(this.totalMemory / 65536);
    },
};

export default MemoryLayout;
