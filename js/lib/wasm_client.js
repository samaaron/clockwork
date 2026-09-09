// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron

/**
 * wasm_client.js — the client boundary for a JavaScript context that is not
 * the audio worklet.
 *
 * The worklet holds the engine's own module instance, so it reaches
 * src/clockwork_client.h by calling it. Every other context — the main thread,
 * the reply pump, the traffic logger — used to reach the same rings by
 * reimplementing their arithmetic in JavaScript. This is what replaced that:
 * each context gets a module instance of its own over the engine's memory, and
 * then calls the same C.
 *
 *
 * ── WHY AN INSTANCE RATHER THAN A COPY OF THE CODE ──────────────────────────
 *
 * The ring's rules are subtle in the way that does not announce itself: a
 * frame that fits in the total free space but not contiguously before the
 * reader, a padding marker at the end, a sequence that has to be published
 * after the bytes and read before them. A second implementation does not fail
 * loudly when it disagrees — it tears one message, once, under load.
 *
 *
 * ── THE STACK ───────────────────────────────────────────────────────────────
 *
 * A second instance over a shared memory begins life pointing its stack at the
 * same place the first one does, because the stack pointer is a per-instance
 * global initialised from the same constant. Left alone, this context's first
 * call would write over the audio thread's frame.
 *
 * So a context claims a client slot (see "Client slots" in shared_memory.h),
 * aims its stack into that slot, and only then calls anything. The claim is a
 * compare-exchange in JavaScript rather than a call into wasm, because at that
 * moment there is nowhere safe to make a call from.
 *
 * For the same reason nothing here calls malloc: the handle and the tap live
 * in the claimed slot, and clockwork_client_open_memory_in is the door that
 * takes storage rather than allocating it.
 *
 *
 * ── NO STAGING BUFFER ───────────────────────────────────────────────────────
 *
 * A message is written straight into the ring. The bytes an encoder produces
 * live on the JavaScript heap, which the engine cannot address at all, so
 * sending used to mean copying them into a buffer inside the shared memory and
 * having the C copy them again into the ring. clockwork_client_send_begin
 * hands back a pointer to the frame's final position instead, so there is one
 * copy, no buffer to size, and no ceiling below the ring's own.
 */

// ClockworkRegionId, for the rings a tap can watch.
export const REGION_INGRESS = 7;
export const REGION_EGRESS = 8;

// ClockworkStatus values this module reports on.
export const CLOCKWORK_OK = 0;
export const CLOCKWORK_E_FULL = 6;

// ClockworkClientMessage: bytes, length, origin, route, sequence. Exported
// because the worklet reads the same array out of its own instance.
export const CLIENT_MESSAGE_BYTES = 20;
const MESSAGE_BYTES = CLIENT_MESSAGE_BYTES;

// Where things sit inside a slot's struct area. The sizes are checked against
// what the library reports at open, so a struct that grows is caught here
// rather than by overwriting its neighbour.
const HANDLE_OFFSET = 0;
const HANDLE_BYTES = 256;
const TAP_OFFSET = 256;
const TAP_BYTES = 256;
const OUTPARAM_OFFSET = 512;
const MESSAGES_OFFSET = 1024;

/**
 * Take one of the engine's client slots. Returns its index, or -1 when they
 * are all spoken for.
 *
 * A compare-exchange on one word: no allocator, no handshake, and no order in
 * which contexts have to start.
 */
export function claimClientSlot(atomicView, ringBufferBase, constants) {
    const word = (ringBufferBase + constants.CLIENT_SLOTS_START) >> 2;
    const count = constants.CLIENT_SLOT_COUNT;

    for (;;) {
        const seen = Atomics.load(atomicView, word);
        let index = -1;
        for (let i = 0; i < count; i++) {
            if ((seen & (1 << i)) === 0) { index = i; break; }
        }
        if (index < 0) return -1;
        const bit = 1 << index;
        if (Atomics.compareExchange(atomicView, word, seen, seen | bit) === seen)
            return index;
        // Another context took it between the load and the exchange; look again.
    }
}

export function releaseClientSlot(atomicView, ringBufferBase, constants, index) {
    if (index < 0) return;
    const word = (ringBufferBase + constants.CLIENT_SLOTS_START) >> 2;
    const bit = 1 << index;
    for (;;) {
        const seen = Atomics.load(atomicView, word);
        if (Atomics.compareExchange(atomicView, word, seen, seen & ~bit) === seen)
            return;
    }
}

/**
 * The imports a clockwork module needs. It is compiled standalone, so this is
 * a handful of WASI entry points rather than a runtime; the ones that matter
 * to a client are none of them, since a client calls ring code that does no
 * IO. fd_write is wired to the console so a failed assertion says something.
 */
function moduleImports(memory, label) {
    const noop = () => 0;
    return {
        env: {
            memory,
            emscripten_notify_memory_growth: noop,
            _emscripten_thread_set_strongref: noop,
            emscripten_exit_with_live_runtime: noop,
            __syscall_getcwd: noop,
            _emscripten_init_main_thread_js: noop,
            _emscripten_thread_mailbox_await: noop,
            _emscripten_receive_on_main_thread_js: noop,
            emscripten_check_blocking_allowed: noop,
            _emscripten_thread_cleanup: noop,
            _emscripten_notify_mailbox_postmessage: noop,
        },
        wasi_snapshot_preview1: {
            clock_time_get: noop,
            // Rust's std seeds its hash maps here, and a guest may use
            // randomness of its own. Present in every context that instantiates
            // the module, not only the worklet, or the instantiation fails when
            // the guest imports it.
            random_get: (buf, len) => {
                const bytes = new Uint8Array(memory.buffer, buf, len);
                if (typeof crypto !== 'undefined' && crypto.getRandomValues) {
                    crypto.getRandomValues(bytes);
                } else {
                    let x = (Date.now() ^ 0x9e3779b9) >>> 0;
                    for (let i = 0; i < len; i++) {
                        x ^= x << 13; x >>>= 0; x ^= x >> 17; x ^= x << 5; x >>>= 0;
                        bytes[i] = x & 0xff;
                    }
                }
                return 0;
            },
            fd_close: noop,
            environ_sizes_get: noop,
            environ_get: noop,
            fd_seek: noop,
            fd_read: noop,
            proc_exit: (code) => {
                console.error(`[${label}] wasm tried to exit with code`, code);
            },
            fd_write: (fd, iov, count, pnum) => {
                const view = new DataView(memory.buffer);
                const u8 = new Uint8Array(memory.buffer);
                let written = 0;
                let text = '';
                for (let i = 0; i < count; i++) {
                    const ptr = view.getUint32(iov + i * 8, true);
                    const len = view.getUint32(iov + i * 8 + 4, true);
                    for (let j = 0; j < len; j++) text += String.fromCharCode(u8[ptr + j]);
                    written += len;
                }
                if (pnum) view.setUint32(pnum, written, true);
                const line = text.replace(/\n+$/, '');
                if (line) (fd === 2 ? console.error : console.log)(`[${label}] ${line}`);
                return 0;
            },
        },
    };
}

/**
 * A handle onto a running engine, from a context that shares its memory.
 *
 * Open it once and keep it: opening claims a slot, and there are only a few.
 */
export class WasmClient {
    #exports;
    #memory;
    #handle = 0;
    #tap = 0;
    #slotIndex = -1;
    #atomicView;
    #ringBufferBase;
    #constants;
    #messagesPtr = 0;
    #maxMessages = 0;
    #outParamPtr = 0;
    #label;

    /**
     * @param {object}             opts
     * @param {WebAssembly.Module} opts.wasmModule      compiled once, shared by every context
     * @param {WebAssembly.Memory} opts.wasmMemory      the engine's, shared
     * @param {number}             opts.ringBufferBase  what get_ring_buffer_base() returned
     * @param {object}             opts.bufferConstants the BufferLayout, as JS reads it
     * @param {string}             [opts.label]         for diagnostics
     * @returns {Promise<WasmClient>}
     * @throws when no slot is free, or the engine will not open. Both are
     *         wiring faults rather than conditions to handle: a context that
     *         carried on without a client would look like a working transport
     *         that silently delivers nothing.
     */
    static async open({ wasmModule, wasmMemory, ringBufferBase, bufferConstants, label = 'WasmClient' }) {
        const c = new WasmClient();
        c.#memory = wasmMemory;
        c.#ringBufferBase = ringBufferBase;
        c.#constants = bufferConstants;
        c.#label = label;
        c.#atomicView = new Int32Array(wasmMemory.buffer);

        c.#slotIndex = claimClientSlot(c.#atomicView, ringBufferBase, bufferConstants);
        if (c.#slotIndex < 0) {
            throw new Error(`[${label}] no client slot free — all `
                + `${bufferConstants.CLIENT_SLOT_COUNT} are taken. Raise `
                + `CLIENT_SLOT_COUNT in shared_memory.h, or close a client.`);
        }

        const instance = await WebAssembly.instantiate(wasmModule, moduleImports(wasmMemory, label));
        c.#exports = instance.exports;

        const slotBase = ringBufferBase
            + bufferConstants.CLIENT_SLOTS_START
            + bufferConstants.CLIENT_SLOTS_HEADER_SIZE
            + c.#slotIndex * bufferConstants.CLIENT_SLOT_SIZE;

        // THE STACK, BEFORE ANYTHING ELSE. Until these two calls this instance
        // shares the worklet's stack, so nothing above a global read is safe.
        const stackLow = slotBase + bufferConstants.CLIENT_SLOT_STACK_OFFSET;
        const stackHigh = stackLow + bufferConstants.CLIENT_SLOT_STACK_SIZE;
        c.#exports.emscripten_stack_set_limits(stackHigh, stackLow);
        c.#exports._emscripten_stack_restore(stackHigh);

        const structs = slotBase + bufferConstants.CLIENT_SLOT_STRUCTS_OFFSET;
        c.#outParamPtr = structs + OUTPARAM_OFFSET;
        c.#messagesPtr = structs + MESSAGES_OFFSET;
        c.#maxMessages = Math.floor(
            (bufferConstants.CLIENT_SLOT_STRUCTS_SIZE - MESSAGES_OFFSET) / MESSAGE_BYTES);

        // The library's structs must still fit where this file puts them.
        const handleBytes = c.#exports.clockwork_client_sizeof();
        const tapBytes = c.#exports.clockwork_client_tap_sizeof();
        if (handleBytes > HANDLE_BYTES || tapBytes > TAP_BYTES) {
            c.close();
            throw new Error(`[${label}] client structs outgrew their slot: `
                + `handle ${handleBytes}/${HANDLE_BYTES}, tap ${tapBytes}/${TAP_BYTES}`);
        }

        c.#handle = c.#exports.clockwork_client_open_memory_in(
            structs + HANDLE_OFFSET, HANDLE_BYTES,
            ringBufferBase, bufferConstants.TOTAL_BUFFER_SIZE,
            c.#outParamPtr);

        if (!c.#handle) {
            const status = new Int32Array(wasmMemory.buffer, c.#outParamPtr, 1)[0];
            c.close();
            throw new Error(`[${label}] client boundary unavailable, status ${status}`);
        }
        return c;
    }

    get slotIndex() { return this.#slotIndex; }

    /**
     * Where this instance's stack pointer currently is.
     *
     * Diagnostic, and the one thing worth checking after open: it must be
     * inside this client's own slot. An instance that skipped the hand-off
     * still works right up until the audio thread is mid-call, so "it ran" is
     * no evidence at all — the address is.
     */
    stackPointer() {
        return this.#exports.emscripten_stack_get_current();
    }

    /** The half-open range this client's stack may occupy. */
    stackRange() {
        const base = this.#ringBufferBase
            + this.#constants.CLIENT_SLOTS_START
            + this.#constants.CLIENT_SLOTS_HEADER_SIZE
            + this.#slotIndex * this.#constants.CLIENT_SLOT_SIZE
            + this.#constants.CLIENT_SLOT_STACK_OFFSET;
        return { low: base, high: base + this.#constants.CLIENT_SLOT_STACK_SIZE };
    }

    /**
     * Frame an OSC message onto the ingress ring.
     *
     * The bytes go straight into the ring: begin reserves the frame's own
     * position, this copies into it once, commit publishes. The reservation
     * holds the ring's write lock, so the finally is not tidiness — a return
     * or a throw between the two would stop the ring for every producer.
     *
     * @returns {boolean} false when it was refused; the ring being momentarily
     *                    full is the ordinary reason.
     */
    send(oscData, sourceId = 0) {
        if (!this.#handle) return false;
        const bytes = oscData.byteLength ?? oscData.length;
        if (!bytes) return false;

        const ptr = this.#exports.clockwork_client_send_begin(
            this.#handle, bytes, this.#outParamPtr);
        if (!ptr) return false;

        let committed = false;
        try {
            const src = oscData instanceof Uint8Array ? oscData : new Uint8Array(oscData);
            new Uint8Array(this.#memory.buffer, ptr, bytes).set(src);
            committed = this.#exports.clockwork_client_send_commit(
                this.#handle, bytes, sourceId) === CLOCKWORK_OK;
            return committed;
        } finally {
            // A refused commit has already given the reservation back, and
            // abort with nothing open is accepted, so this is safe either way.
            if (!committed) this.#exports.clockwork_client_send_abort(this.#handle);
        }
    }

    /** Watch a ring rather than take from it. One tap per client. */
    openTap(ring) {
        if (!this.#handle || this.#tap) return false;
        const structs = this.#slotStructsBase();
        this.#tap = this.#exports.clockwork_client_tap_open_in(
            structs + TAP_OFFSET, TAP_BYTES, this.#handle, ring, this.#outParamPtr);
        if (!this.#tap) {
            const status = new Int32Array(this.#memory.buffer, this.#outParamPtr, 1)[0];
            console.error(`[${this.#label}] tap unavailable, status`, status);
            return false;
        }
        return true;
    }

    /** Frames written past this tap before it read them, since it opened. */
    tapMissed() {
        return this.#tap ? this.#exports.clockwork_client_tap_missed(this.#tap) : 0;
    }

    /**
     * Take replies from the egress ring. `onMessage(bytes, origin, route, seq)`
     * is called for each; `bytes` is a view into the ring, so a caller that
     * keeps it copies it.
     * @returns {number} how many were delivered
     */
    poll(onMessage, max = this.#maxMessages) {
        if (!this.#handle) return 0;
        return this.#deliver(
            this.#exports.clockwork_client_poll(this.#handle, this.#messagesPtr,
                                                Math.min(max, this.#maxMessages)),
            onMessage);
    }

    /** As poll, for the tap: reads without taking. */
    tapPoll(onMessage, max = this.#maxMessages) {
        if (!this.#tap) return 0;
        return this.#deliver(
            this.#exports.clockwork_client_tap_poll(this.#tap, this.#messagesPtr,
                                                    Math.min(max, this.#maxMessages)),
            onMessage);
    }

    close() {
        if (this.#tap) { this.#exports.clockwork_client_tap_close(this.#tap); this.#tap = 0; }
        if (this.#handle) { this.#exports.clockwork_client_close(this.#handle); this.#handle = 0; }
        if (this.#slotIndex >= 0) {
            releaseClientSlot(this.#atomicView, this.#ringBufferBase,
                              this.#constants, this.#slotIndex);
            this.#slotIndex = -1;
        }
    }

    #slotStructsBase() {
        return this.#ringBufferBase
            + this.#constants.CLIENT_SLOTS_START
            + this.#constants.CLIENT_SLOTS_HEADER_SIZE
            + this.#slotIndex * this.#constants.CLIENT_SLOT_SIZE
            + this.#constants.CLIENT_SLOT_STRUCTS_OFFSET;
    }

    // One view per call over the message array the C just filled. The buffer
    // is re-read each time because a growable memory detaches its old one.
    #deliver(count, onMessage) {
        if (!count) return 0;
        const view = new DataView(this.#memory.buffer);
        for (let i = 0; i < count; i++) {
            const at = this.#messagesPtr + i * MESSAGE_BYTES;
            const ptr = view.getUint32(at, true);
            const len = view.getUint32(at + 4, true);
            const origin = view.getUint32(at + 8, true);
            const route = view.getUint32(at + 12, true);
            const sequence = view.getUint32(at + 16, true);
            if (!ptr || !len) continue;
            onMessage(new Uint8Array(this.#memory.buffer, ptr, len), origin, route, sequence);
        }
        return count;
    }
}
