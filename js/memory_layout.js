// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron
/*
    Clockwork Memory Layout

    Defines the WASM memory regions. The initial committed memory is small
    (heap + ring buffers + the guest's initial region). The guest region
    The inbox grows on demand up to maxInboxSize by extending WASM memory.

    Both inboxSize and maxInboxSize can be overridden at runtime
    via Clockwork constructor options:
      memory: { guestMemorySize: 64 * 1024 * 1024 }  // 64MB committed at boot
      memory: { maxInboxSize: 512 * 1024 * 1024 }  // 512MB ceiling

    Memory Layout:
      0-14MB:   WASM Heap (emscripten malloc, static data, stack) — and the
                ring buffers, which are a static array in the data segment and
                have always lived here rather than in a region of their own
      14MB+:    Guest memory — opaque to clockwork, grows on demand

    Clockwork DOES NOT KNOW WHAT IS IN THE GUEST REGION. Until 2026-08-31
    this file named two regions above the heap — an "RT Pool (engine's
    real-time allocator)" and a "Buffer Pool (audio samples)" — and sized the
    first from a real-time-memory field inside one engine's own config. That
    is one guest's memory plan written into clockwork, and a guest that has
    neither an RT allocator nor audio samples had to pretend.

    So there is one region now, and the guest subdivides it. Clockwork
    reserves the address space, commits the initial slice, grows it on
    request, and moves opaque bytes into it. What those bytes mean — an
    allocator arena, sample frames, anything else — is the guest's business
    and is decided by the guest's own client code.

    THE RINGS ARE INSIDE THE HEAP, not above it. `ring_buffer_storage` is a
    static array, so the linker places it in the data segment; measured
    2026-08-22 the regions span 6.72MB from `ringBufferBase`, all of it inside
    what was then an 8MB heap — leaving about 1MB for malloc and the stack.

    The number that matters is guestMemoryOffset, because emscripten's malloc
    does not know about any of these regions: it starts after the static data
    and grows upward until it runs into whatever we put next.
    wasmHeapSize, ringBufferReserved and memArenaSize are only names for parts
    of that gap — malloc gets the whole of it, and only their SUM decides
    anything.

    Measured 2026-08-22: static data ends at ~7.4MB, so an 11MB
    guestMemoryOffset left ~3.6MB for malloc — and a client asking for a large
    guest region used very nearly all of it, so raising the OSC IN ring by
    256KB was enough to push malloc into the guest's region and crash the
    renderer. The gap is 14MB now.

    Nothing measures this margin from a booted engine any more; two specs that
    did (rt_pool_isolation, memory_headroom) were cited here long after they
    stopped existing. What catches it now is in C++: clockwork_init refuses to boot
    when clockwork heap's end reaches the guest region base, with the two
    addresses in the message (see clockwork_heap_backing_end).
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
     * Reduced from implicit 16MB since the guest's region became a region of
     * its own, then given back the 3MB reserved next door but never used.
     * Holds the static ring-buffer array (6.72MB measured) plus malloc and
     * the stack, so treat the difference as the real malloc budget and
     * re-measure with `sonic.bufferConstants` after changing any region size.
     * Only the sum of this, ringBufferReserved and memArenaSize matters; see
     * the header.
     * Current: 24MB
     */
    wasmHeapSize: 24 * 1024 * 1024,  // 24MB — see the measurement below

    /*
     * WHY 24MB, MEASURED 2026-08-31.
     *
     * This said 12MB, and that raising it to 20MB "made things worse" and
     * must not be repeated. The reasoning in that note was right about the
     * pressure and wrong about the cause, so it is replaced rather than kept:
     * raising the floor was never the problem, and the arithmetic here has
     * never actually fit.
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
     * against a floor of 12MB + 2MB. It does not fit and never did. clockwork_heap
     * is claimed with malloc, so it simply ran past the line: measured, its
     * backing ended at 0x01113F48 (17.08 MB) while the guest's region began
     * at 0x00E00000 (14.00 MB). Clockwork's heap and the guest's memory
     * overlapped by 3.08 MB on EVERY boot, and nothing anywhere said so.
     *
     * That is what the tests of the time were reacting to. They did not break
     * because the gap grew; they broke because moving the gap moved which
     * bytes two allocators shared, and any change to this number reshuffled a
     * corruption that was always present.
     *
     * So the floor is now big enough to hold what is measured above, and
     * the boot REFUSES TO RUN if the heap's end reaches the guest region
     * (see clockwork_heap_backing_end). If that ever fires, raise this number —
     * do not go looking for which test is flaky.
     */

    /**
     * Ring buffer reserved space (between the WASM heap and the guest's
     * region).
     *
     * DO NOT LIST THE FIGURES HERE OR RE-DERIVE THE TOTAL BY HAND — a written
     * total drifts from the build it describes, and SHM_SCOPE, the largest
     * region of the lot, is the one most easily forgotten. The regions are
     * laid out by
     * src/shared_memory.h from the build's own -D flags, and the module
     * exports every start and size at runtime. Read them from a booted engine
     * (`sonic.bufferConstants`) and measure.
     *
     * Measured on the web build, 2026-08-22: the regions span 6.474MB from
     * `ringBufferBase`, of which SHM_SCOPE is 4.097MB, SHM_AUDIO 1.500MB and
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
     * 32MB because that is exactly what the guest region used to lend back for
     * this. Until 2026-09-01 the client carved an RT arena (RT_ARENA_MIN, 32MB)
     * out of the FRONT of its own region and passed the offset to the engine
     * through a config-block slot; guestMemorySize was 36MB for that reason,
     * "32MB RT pool + 4MB buffers". The 32MB moved here and the guest region
     * kept the 4MB it was actually using, so the total is unchanged and the
     * engine's pool now comes from the same clockwork::mem every other target uses.
     *
     * It bounds the engine: a real-time pool larger than this fails at boot
     * with both numbers in the message, rather than being served from memory
     * clockwork does not own.
     */
    memArenaSize: 32 * 1024 * 1024,  // 32MB

    ringBufferReserved: 2 * 1024 * 1024,  // 2MB — see the header: the rings do
                                          // not live here, and this only sets
                                          // where the guest's region starts

    /**
     * Guest memory, committed at boot, in bytes.
     *
     * One opaque region handed to the guest, which subdivides it however its
     * own design requires. This replaced `rtPoolSize` (32MB, sized from a
     * real-time-memory field in one engine's own config) and `bufferPoolSize`
     * (4MB, audio samples) on 2026-08-31; the default is their sum, so a
     * guest that splits it the old way gets exactly what it had before.
     *
     * Override at runtime: memory: { guestMemorySize: N }
     */
    guestMemorySize: 4 * 1024 * 1024,  // 4MB — fixed at boot; see memArenaSize

    /**
     * Bulk staging, committed at boot. ONE WRITER EACH: the client writes the
     * inbox and the guest only reads it; the guest writes the outbox and the
     * client only reads it. Neither is the guest's arena, and nothing is
     * written from both ends — which is what removes the need for the two
     * sides to agree at runtime about which bytes belong to whom.
     *
     * The INBOX sits at the top of the memory and is the only region that
     * grows, because bulk arriving from the client is the only thing whose
     * size a session discovers as it runs. See inboxOffset.
     *
     * Override at runtime: memory: { inboxSize: N, outboxSize: N }
     */
    inboxSize:  4 * 1024 * 1024,   // client -> guest, grows to maxInboxSize
    outboxSize: 4 * 1024 * 1024,   // guest -> client, fixed

    /**
     * Maximum inbox size in bytes — the hard ceiling for growth.
     *
     * WASM address space is RESERVED up to inboxOffset +
     * maxInboxSize at construction and can never be raised afterwards;
     * only the committed size grows into it. That is why this is a build-time
     * cap rather than something a caller can lift at runtime.
     *
     * Measured 2026-08-31: growth works because the memory is created with
     * `shared: true`. A non-shared WebAssembly.Memory DETACHES its old
     * ArrayBuffer on grow() and every view over it throws; a shared one does
     * not, and pre-existing views keep reading. The growable allocator relies
     * on this, because it appends a new pool per growth and keeps the earlier
     * pools' buffer references alive.
     */
    maxInboxSize: 768 * 1024 * 1024,  // 768MB — bulk in; see memArenaSize

    /**
     * Guest memory byte offset from the start of the SharedArrayBuffer.
     *
     * Everything below this is the module's own: emscripten's static data,
     * malloc and stack, clockwork's ring buffers (a static array inside that
     * same heap) and the placement arena. Everything at or above it is the
     * guest's and clockwork never interprets it.
     */
    get outboxOffset() {
        return this.wasmHeapSize + this.ringBufferReserved + this.memArenaSize;
    },

    get guestMemoryOffset() {
        return this.outboxOffset + this.outboxSize;
    },

    /**
     * THE INBOX IS LAST, because it is the one region that grows.
     *
     * Only the region at the top of the memory can grow in place — everything
     * above it would have to move, and a published range that moved would be a
     * pointer the guest already holds pointing at something else. The thing
     * that grows is bulk arriving from the client: samples, wavetables,
     * impulse responses, however many of them a session turns out to load. The
     * guest's arena is sized once at boot, as an arena is.
     */
    get inboxOffset() {
        return this.guestMemoryOffset + this.guestMemorySize;
    },

    /**
     * Total committed memory (derived)
     * inboxOffset + inboxSize
     */
    get totalMemory() {
        return this.inboxOffset + this.inboxSize;
    },

    /**
     * Maximum total WASM memory (derived, used by build.sh for -sMAXIMUM_MEMORY).
     * Computed as: inboxOffset + maxInboxSize.
     *
     * This is the RESERVATION. It fixes the address space at construction and
     * cannot be raised later, so it is the one number a build must get right
     * up front; everything else is committed on demand beneath it.
     */
    get maxTotalMemory() {
        return this.inboxOffset + Math.max(this.maxInboxSize, this.inboxSize);
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
