// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * AudioWorklet Processor for the clockwork WASM
 * Runs in AudioWorkletGlobalScope with real-time priority
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { REGION_INGRESS, CLIENT_MESSAGE_BYTES } from '../lib/wasm_client.js';
import { calculateAllControlIndices } from '../lib/control_offsets.js';
import { readArena } from '../lib/arena.js';
import {
  ClockworkClockMessageType,
  retempoClock, writeClockOrigin, writeClockTransport, writeClockMeter, isValidMeter,
} from '../lib/clockwork_clock_protocol.js';

// PM Mode Pool Configuration - pre-allocated buffers for allocation-free process()
const PM_POOL_CONFIG = {
    MAX_REPLY_MESSAGES: 64,
    MAX_LOG_ENTRIES: 100,

    REPLY_BUFFER_SIZE: 128 * 1024,   // 128KB - matches OUT_BUFFER_SIZE
    LOG_BUFFER_SIZE: 256 * 1024,     // 256KB for log entries

    LOG_MAX_MESSAGE_SIZE: 16 * 1024, // 16KB - truncate larger messages

};

class ClockworkProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        // Transport mode: 'sab' or 'postMessage'
        this.mode = 'sab';

        this.sharedBuffer = null;
        this.wasmModule = null;
        this.wasmInstance = null;
        this.isInitialized = false;
        this.processCallCount = 0;
        this.lastStatusCheck = 0;
        this.lastInTail = 0;
        this.ringBufferBase = null;
        this.pendingClearSched = false;

        // Pre-allocated audio view to avoid per-frame allocations
        this.audioView = null;
        this.lastAudioBufferPtr = 0;
        this.lastWasmBufferSize = 0;

        this.lastTreeVersion = -1;
        this.treeSnapshotsSent = 0;
        this.lastTreeSendTime = -1; // AudioContext time of last send (-1 = never)
        this.treeSnapshotMinInterval = 0.150; // Default, can be overridden by snapshotIntervalMs config

        // Views into SharedArrayBuffer (or WASM memory in postMessage mode)
        this.atomicView = null;
        this.uint8View = null;
        this.dataView = null;
        this.localClockOffsetView = null;  // Float64Array for reading local clock offset

        // Buffer constants (loaded from WASM at initialization)
        this.bufferConstants = null;

        // Control region indices (Int32Array indices) - will be calculated dynamically
        this.CONTROL_INDICES = null;

        this.metricsView = null;

        this.STATUS_FLAGS = {
            OK: 0,
            BUFFER_FULL: 1 << 0,
            OVERRUN: 1 << 1,
            WASM_ERROR: 1 << 2,
            FRAGMENTED_MSG: 1 << 3
        };

        // Additional OSC input ports (for prescheduler and user workers)
        // These allow workers to send OSC directly to the worklet
        this.oscPorts = [];

        // Map of port -> sourceId for worker ports (postMessage mode)
        this.portSourceIds = new Map();

        // Pre-allocated channel views to avoid per-frame subarray() allocations
        this.channelViews = null;
        this.lastNumSamples = 0;
        this.lastNumChannels = 0;

        // PM Mode pools - pre-allocated for allocation-free process()
        // Initialized in initPMPools() after mode is known
        this.pmPools = null;

        // Node ID allocation (PM mode only — SAB mode uses shared atomic counter)
        // NODE_ID_COUNTER is an allocation cursor living in WASM memory,
        // bumped by an atomic increment by whoever hands out node IDs (in SAB
        // mode that is OscChannel — js/lib/osc_channel.js).
        // In PM mode, that memory is local, so we seed it with a range from the
        // main thread and top up asynchronously via a MessagePort.
        //
        // Two pre-allocated range slots (no allocation during process):
        //   nodeIdRanges[0] = current range being consumed
        //   nodeIdRanges[1] = pre-fetched next range (disjoint)
        // After process_audio(), we read the counter back. If it's past
        // range[0].to, we trim range[0] and jump to range[1].from.
        this.nodeIdRanges = [
            { from: 0, to: 0 },  // slot 0: current
            { from: 0, to: 0 },  // slot 1: prefetch
        ];
        this.nodeIdRangeCount = 0;     // Active slots (0, 1, or 2)
        this.nodeIdRefillRequested = false; // True while waiting for async refill
        this.nodeIdPort = null;        // MessagePort for requesting more ranges
        this.nodeIdCounterView = null; // Int32Array(1) view into WASM NODE_ID_COUNTER

        // Pre-allocated objects for checkStatus() to avoid allocation on audio thread
        this._statusObj = {
            bufferFull: false,
            overrun: false,
            wasmError: false,
            fragmented: false
        };
        this._metricsObj = {
            processCount: 0,
            messagesProcessed: 0,
            messagesDropped: 0,
            schedulerQueueDepth: 0,
            schedulerQueueMax: 0,
            schedulerQueueDropped: 0
        };
        this._statusMessage = {
            type: 'status',
            flags: 0,
            status: this._statusObj,
            metrics: this._metricsObj
        };

        // postMessage mode drains the egress on the main thread's behalf, and
        // a batch posted before the transport has a handler on this port is
        // delivered to nobody. The engine speaks as it boots — the banner,
        // the version, "engine ready" — and every one of those lines was
        // lost that way. So the frames stay in the ring until the transport
        // says it is listening; the ring is theirs to wait in.
        this.egressListening = false;
        this.port.onmessage = this.handleMessage.bind(this);
    }

    // The arena's table of contents (js/lib/arena.js, src/clockwork_arena.h):
    // read from the front of the arena, by id, never by position. The
    // constants the rest of the runtime consumes are derived from it.
    loadBufferConstants() {
        const memory = this.wasmMemory;
        if (!memory) {
            throw new Error('WASM memory not available');
        }
        if (!this.wasmInstance || !this.wasmInstance.exports.get_ring_buffer_base) {
            throw new Error('WASM instance does not export get_ring_buffer_base');
        }
        const base = this.wasmInstance.exports.get_ring_buffer_base();
        this.arena = readArena(memory.buffer, base);
        this.bufferConstants = this.arena.constants;
    }

    // Calculate buffer indices based on dynamic ring buffer base address
    // Uses constants loaded from WASM via loadBufferConstants()
    calculateBufferIndices(ringBufferBase) {
        if (!this.bufferConstants) {
            throw new Error('Buffer constants not loaded. Call loadBufferConstants() first.');
        }

        const CONTROL_START = this.bufferConstants.CONTROL_START;
        const METRICS_START = this.bufferConstants.METRICS_START;
        const CLOCK_STATE_START = this.bufferConstants.CLOCK_STATE_START;
        const CLOCK_STATE_SIZE  = this.bufferConstants.CLOCK_STATE_SIZE;

        // Calculate Int32Array indices (divide byte offsets by 4)
        this.CONTROL_INDICES = calculateAllControlIndices(ringBufferBase, CONTROL_START);

        const clockworkClockBuf = (this.mode === 'sab') ? this.sharedBuffer : this.wasmMemory.buffer;
        const clockworkClockBase = ringBufferBase + CLOCK_STATE_START;
        this.clockworkClockStateBigInt = new BigInt64Array(clockworkClockBuf, clockworkClockBase, CLOCK_STATE_SIZE / 8);
        this.clockworkClockStateInt32  = new Int32Array(clockworkClockBuf, clockworkClockBase, CLOCK_STATE_SIZE / 4);
        if (this.mode === 'sab') {
            // SAB mode: views into SharedArrayBuffer
            const metricsBase = ringBufferBase + METRICS_START;
            this.metricsView = new Uint32Array(this.sharedBuffer, metricsBase, this.bufferConstants.METRICS_SIZE / 4);
        } else {
            // PostMessage mode: views into WASM memory
            // Note: atomicView/uint8View/dataView are set up here for ring buffer access
            this.atomicView = new Int32Array(this.wasmMemory.buffer);
            this.uint8View = new Uint8Array(this.wasmMemory.buffer);
            this.dataView = new DataView(this.wasmMemory.buffer);
            const metricsBase = ringBufferBase + METRICS_START;
            this.metricsView = new Uint32Array(this.wasmMemory.buffer, metricsBase, this.bufferConstants.METRICS_SIZE / 4);

        }
    }

    // Set up Int32Array view for NODE_ID_COUNTER in WASM memory.
    // In PM mode, this view is used to seed the counter with range-based
    // allocation values before process_audio() and read it back after.
    // In SAB mode, the counter is shared and managed atomically — no seeding needed.
    initNodeIdCounter() {
        if (!this.wasmMemory || !this.bufferConstants || !this.ringBufferBase) return;
        if (this.mode !== 'postMessage') return;  // SAB mode uses shared atomic

        const counterBase = this.ringBufferBase + this.bufferConstants.NODE_ID_COUNTER_START;
        this.nodeIdCounterView = new Int32Array(this.wasmMemory.buffer, counterBase, 1);

        // If ranges were received before WASM loaded, seed counter with first range
        if (this.nodeIdRangeCount > 0) {
            Atomics.store(this.nodeIdCounterView, 0, this.nodeIdRanges[0].from);
        }
    }

    // Push a node ID range into the pre-allocated slots.
    // Called from message handler — never during process().
    pushNodeIdRange(from, to) {
        if (this.nodeIdRangeCount < 2) {
            const slot = this.nodeIdRanges[this.nodeIdRangeCount];
            slot.from = from;
            slot.to = to;
            this.nodeIdRangeCount++;
            this.nodeIdRefillRequested = false;

            // If this is the first range and counter view is ready, seed it
            if (this.nodeIdRangeCount === 1 && this.nodeIdCounterView) {
                Atomics.store(this.nodeIdCounterView, 0, from);
            }
        }
    }

    /*
     * Copy the guest's own config block into the region reserved for it.
     *
     * CLOCKWORK DOES NOT KNOW WHAT IS IN THESE BYTES. Writing named slots here
     * — one engine's config struct, field by field — means a second guest
     * cannot be configured at all without editing this file.
     *
     * The product encodes its own block and clockwork only moves it.
     * The one thing clockwork contributes is the transport mode, which it
     * alone knows, and it contributes it as a VALUE passed to the encoder
     * rather than as a slot written here.
     */
    writeGuestConfigToMemory() {
        if (!this.guestConfigBytes || !this.wasmMemory) {
            return;
        }
        const start = this.bufferConstants?.GUEST_CONFIG_START;
        if (start === undefined) {
            console.error('GUEST_CONFIG_START not available in bufferConstants');
            return;
        }
        const src = new Uint8Array(this.guestConfigBytes);
        const cap = this.bufferConstants?.GUEST_CONFIG_SIZE ?? src.byteLength;
        if (src.byteLength > cap) {
            /*
             * THROW, do not just log.
             *
             * Refused whole rather than truncated is right — a half-written config block
             * is a different config and the guest could not tell. But returning quietly
             * means the guest boots with a ZEROED config: no buses, no nodes, no sample
             * rate, and a death somewhere unrelated. Growing the block by one slot
             * without growing GUEST_CONFIG_SIZE produces exactly that.
             */
            throw new Error(`guest config is ${src.byteLength} bytes but the `
                          + `region holds ${cap}: GUEST_CONFIG_SIZE and the `
                          + `encoder disagree`);
        }
        new Uint8Array(this.wasmMemory.buffer, this.ringBufferBase + start, src.byteLength)
            .set(src);
    }

    // Atomic-safe load - uses Atomics in SAB mode, regular access in postMessage mode
    atomicLoad(index) {
        if (this.mode === 'sab') {
            return Atomics.load(this.atomicView, index);
        } else {
            return this.atomicView[index];
        }
    }

    // Atomic-safe store - uses Atomics in SAB mode, regular access in postMessage mode
    atomicStore(index, value) {
        if (this.mode === 'sab') {
            Atomics.store(this.atomicView, index, value);
        } else {
            this.atomicView[index] = value;
        }
    }

    // Initialize pre-allocated pools for allocation-free PM mode
    // Called once after mode is set and bufferConstants are loaded
    initPMPools() {
        if (this.mode !== 'postMessage') return;

        const C = PM_POOL_CONFIG;

        this.pmPools = {
            // === OUTGOING POOLS ===

            // OSC replies from the DSP
            replies: {
                message: { type: 'oscReplies', messages: null, count: 0 },
                buffer: new ArrayBuffer(C.REPLY_BUFFER_SIZE),
                bufferView: null,
                entries: new Array(C.MAX_REPLY_MESSAGES).fill(null).map(() => ({
                    offset: 0,
                    length: 0,
                    sequence: 0
                })),
            },

            // Metrics + node tree snapshot
            snapshot: {
                message: { type: 'snapshot', buffer: null, snapshotsSent: 0 },
                buffer: null,       // Sized after bufferConstants known
                bufferView: null,
                size: 0,
            },

            // OSC log entries
            log: {
                message: { type: 'oscLog', entries: null, count: 0, buffer: null },
                buffer: new ArrayBuffer(C.LOG_BUFFER_SIZE),
                bufferView: null,
                entries: new Array(C.MAX_LOG_ENTRIES).fill(null).map(() => ({
                    offset: 0,
                    length: 0,
                    originalLength: 0,
                    sourceId: 0,
                    sequence: 0
                })),
            },

        };

        const p = this.pmPools;

        p.replies.bufferView = new Uint8Array(p.replies.buffer);
        p.log.bufferView = new Uint8Array(p.log.buffer);

        p.replies.message.messages = p.replies.entries;
        p.log.message.entries = p.log.entries;

        if (this.bufferConstants && this.wasmMemory) {
            const bc = this.bufferConstants;
            const size = bc.METRICS_SIZE + bc.SHM_WINDOW_SIZE;
            p.snapshot.buffer = new ArrayBuffer(size);
            p.snapshot.bufferView = new Uint8Array(p.snapshot.buffer);
            p.snapshot.size = size;
            p.snapshot.message.buffer = p.snapshot.buffer;

            // Pre-allocated source views: the metrics (in the clockwork
            // block) and the guest's window (in the guest region), copied
            // into one snapshot as metrics then window — the shape the
            // client reads back (readWindow: the window at METRICS_SIZE).
            // Valid as long as WASM memory does not grow.
            p.snapshot.metricsView = new Uint8Array(this.wasmMemory.buffer,
                this.ringBufferBase + bc.METRICS_START, bc.METRICS_SIZE);
            p.snapshot.windowView = new Uint8Array(this.wasmMemory.buffer,
                this.ringBufferBase + bc.SHM_WINDOW_START, bc.SHM_WINDOW_SIZE);
        }
    }

    // Write a single OSC message directly to the IN ring buffer (postMessage mode)
    // Called from onmessage handlers
    // Note: new Uint8Array(oscData) creates a view (no copy), which is required to access ArrayBuffer bytes
    // Open the client boundary over the engine's own heap.
    //
    // A browser client is a client like any other; it simply happens to share
    // an address space with the engine, so it opens by address rather than by
    // segment. Everything after this — sending, draining, reading a region —
    // is the same code a GUI in another process runs.
    openClientBoundary() {
        this.wasmExports = this.wasmInstance.exports;
        if (!this.wasmExports.clockwork_client_open_memory) return;   // older module

        const base  = this.wasmExports.get_ring_buffer_base();
        const bytes = this.bufferConstants.TOTAL_BUFFER_SIZE;

        // The status is written into scratch memory rather than returned,
        // because C reports through an out-parameter and wasm has no other way
        // to hand back two values.
        const stPtr = this.wasmExports.malloc(4);
        this.clientHandle = this.wasmExports.clockwork_client_open_memory(base, bytes, stPtr);

        if (!this.clientHandle) {
            const status = new Int32Array(this.wasmMemory.buffer, stPtr, 1)[0];
            console.error('[AudioWorklet] client boundary unavailable, status', status);
            this.wasmExports.free(stPtr);
            return;
        }

        // Room for the messages a drain hands back, and the tap that watches
        // what was sent. Claimed once, here, so neither drain allocates on the
        // audio thread.
        const C = PM_POOL_CONFIG;
        this.pollMax = Math.max(C.MAX_REPLY_MESSAGES, C.MAX_LOG_ENTRIES);
        this.pollMessages = this.wasmExports.malloc(this.pollMax * CLIENT_MESSAGE_BYTES);

        // The log shows what was SENT, which is the ingress ring — and the
        // engine is that ring's consumer, so this watches rather than drains.
        const tapBytes = this.wasmExports.clockwork_client_tap_sizeof();
        this.logTap = this.wasmExports.clockwork_client_tap_open_in(
            this.wasmExports.malloc(tapBytes), tapBytes,
            this.clientHandle, REGION_INGRESS, stPtr);
        if (!this.logTap) {
            const status = new Int32Array(this.wasmMemory.buffer, stPtr, 1)[0];
            console.error('[AudioWorklet] could not watch the ingress ring, status', status);
        }

        this.wasmExports.free(stPtr);
    }

    // Read one ClockworkClientMessage out of the array a drain just filled.
    #readPolled(view, index) {
        const at = this.pollMessages + index * CLIENT_MESSAGE_BYTES;
        return {
            ptr: view.getUint32(at, true),
            length: view.getUint32(at + 4, true),
            origin: view.getUint32(at + 8, true),
            sequence: view.getUint32(at + 16, true),
        };
    }

    writeOscToRingBuffer(oscData, sourceId = 0) {
        // THROUGH THE CLIENT BOUNDARY (src/clockwork_client.h), not around it.
        //
        // This used to frame the message here: read the head and tail, decide
        // whether it fits before the end or after a wrap, bump the sequence,
        // lay out the header, copy the payload, publish the head. That is the
        // same page of arithmetic the engine has in C, and keeping a second
        // copy of it in JavaScript meant the two could drift — silently, since
        // a mistake in it yields a torn frame rather than an error.
        //
        // Now the module does it. The handle is opened once over the same heap
        // the engine addresses, so there is nothing to marshal: the bytes are
        // already where the C will read them.
        if (!this.clientHandle) return false;

        const len = oscData.byteLength;
        if (len === 0) return false;

        // One scratch buffer, grown when a message needs more. The alternative
        // is a malloc per message on the path a client sends every note down.
        if (!this.sendScratch || this.sendScratchBytes < len) {
            if (this.sendScratch) this.wasmExports.free(this.sendScratch);
            this.sendScratchBytes = Math.max(len, 4096);
            this.sendScratch = this.wasmExports.malloc(this.sendScratchBytes);
        }
        new Uint8Array(this.wasmMemory.buffer, this.sendScratch, len)
            .set(new Uint8Array(oscData));

        const status = this.wasmExports.clockwork_client_send(
            this.clientHandle, this.sendScratch, len, sourceId);

        if (status !== 0) {
            // 6 is CLOCKWORK_E_FULL: a moment, not a verdict. A postMessage
            // sender has already been told its send succeeded, so the drop is
            // only visible in the metrics — which is why it is logged here.
            console.error('[AudioWorklet] send refused, status', status);
            return false;
        }
        return true;
    }

    // Note: SAB mode OSC logging is now handled by osc_out_log_sab_worker
    // The worker uses Atomics.wait() on IN_HEAD for instant wake when messages arrive

    // Read OSC replies from OUT ring buffer and send via postMessage
    // Uses pre-allocated pools for allocation-free operation
    readOscReplies() {
        if (!this.pmPools || !this.clientHandle) return;

        const pool = this.pmPools.replies;
        const C = PM_POOL_CONFIG;

        // The frames, the magic, the padding marker at the end of the ring and
        // the tail that has to be published after them are all in the C now.
        // What is left here is the copy into the pool that postMessage sends.
        const got = this.wasmExports.clockwork_client_poll(
            this.clientHandle, this.pollMessages,
            Math.min(C.MAX_REPLY_MESSAGES, this.pollMax));
        if (!got) return;

        const view = new DataView(this.wasmMemory.buffer);
        const heap = new Uint8Array(this.wasmMemory.buffer);
        let count = 0;
        let bufferOffset = 0;

        for (let i = 0; i < got; i++) {
            const m = this.#readPolled(view, i);
            if (!m.length) continue;
            if (bufferOffset + m.length > C.REPLY_BUFFER_SIZE) break;

            // The route word is already off; m.ptr is the OSC itself.
            pool.bufferView.set(heap.subarray(m.ptr, m.ptr + m.length), bufferOffset);

            const entry = pool.entries[count];
            entry.offset = bufferOffset;
            entry.length = m.length;
            entry.sequence = m.sequence;

            bufferOffset += m.length;
            count++;
        }

        // Send via postMessage (structured clone - pool remains valid for reuse)
        if (count > 0) {
            pool.message.count = count;
            pool.message.buffer = pool.buffer;
            this.port.postMessage(pool.message);
        }
    }


    // Read metrics from WASM memory as raw Uint32Array
    // Returns null if not ready, otherwise a copy of the metrics buffer
    // Same layout as SAB - can be used directly with MetricsOffsets
    readMetrics() {
        if (!this.metricsView) {
            return null;
        }
        // Return a copy of the raw buffer (same layout as SAB)
        return new Uint32Array(this.metricsView);
    }

    // Record OSC message received (for postMessage mode metrics).
    // In PM mode, we track what the worklet receives since there's no shared memory.
    recordOscReceived(byteLength) {
        if (!this.metricsView) return;

        if (this.mode === 'sab') {
            // SAB mode: use atomic operations
            Atomics.add(this.metricsView, MetricsOffsets.OSC_OUT_MESSAGES_SENT, 1);
            Atomics.add(this.metricsView, MetricsOffsets.OSC_OUT_BYTES_SENT, byteLength);
        } else {
            // PM mode: direct increment (single-threaded context)
            this.metricsView[MetricsOffsets.OSC_OUT_MESSAGES_SENT]++;
            this.metricsView[MetricsOffsets.OSC_OUT_BYTES_SENT] += byteLength;
        }
    }

    // Record an inbound OSC message dropped at the IN ring (ring full),
    // into the same counter the native engine's ingest uses for ingress
    // drops (messages_dropped), so the loss appears in metrics snapshots.
    recordOscDropped() {
        if (!this.metricsView) return;
        // PM-mode only: SAB senders write the ring themselves and see the
        // failure synchronously.
        this.metricsView[MetricsOffsets.ENGINE_MESSAGES_DROPPED]++;
    }

    // Read metrics + the guest's window from WASM memory and send via
    // postMessage: immediately when the window's version word moves, otherwise
    // on interval. Two copies into one pre-allocated pool, metrics then
    // window. Nothing here reads INSIDE the window — the version stamp is the
    // whole of the contract (dsp_api.h shm_window).
    // Returns true if a snapshot was sent, so log entries batch on the same tick.
    checkAndSendSnapshot(audioTime) {
        const bc = this.bufferConstants;
        if (!bc || !this.wasmMemory || this.ringBufferBase === null || !this.pmPools) return false;

        const windowBase = this.ringBufferBase + bc.SHM_WINDOW_START;
        // Reuse existing atomicView to avoid creating new typed array
        const versionOffset = windowBase / 4;   // the guest's version stamp, word 0
        const currentVersion = this.atomicView[versionOffset];
        const versionChanged = currentVersion !== this.lastTreeVersion;

        if (versionChanged) {
            this.lastTreeVersion = currentVersion;
            this.lastTreeSendTime = audioTime;
        } else {
            if (this.lastTreeSendTime >= 0 && audioTime - this.lastTreeSendTime < this.treeSnapshotMinInterval) {
                return false; // Skip this frame, will send on next interval
            }
            this.lastTreeSendTime = audioTime;
        }

        const pool = this.pmPools.snapshot;
        if (!pool.buffer || !pool.metricsView) return false;

        // Metrics, then the window — pre-allocated views, no allocation.
        pool.bufferView.set(pool.metricsView, 0);
        pool.bufferView.set(pool.windowView, pool.metricsView.length);

        // Send via postMessage (structured clone - pool remains valid for reuse)
        this.treeSnapshotsSent++;
        pool.message.snapshotsSent = this.treeSnapshotsSent;
        this.port.postMessage(pool.message);
        return true;
    }

    // Read metrics + the guest's window as one snapshot: metrics first, then
    // the window at offset METRICS_SIZE, which is where readWindow() on the
    // client side reads it back. Returns a raw ArrayBuffer (transferable) or
    // null if not ready. Used only for the initial snapshot.
    readMetricsAndTreeBuffer() {
        if (!this.bufferConstants || !this.wasmMemory || this.ringBufferBase === null) {
            return null;
        }
        const bc = this.bufferConstants;
        const buffer = new ArrayBuffer(bc.METRICS_SIZE + bc.SHM_WINDOW_SIZE);
        const out = new Uint8Array(buffer);
        out.set(new Uint8Array(this.wasmMemory.buffer, this.ringBufferBase + bc.METRICS_START, bc.METRICS_SIZE), 0);
        out.set(new Uint8Array(this.wasmMemory.buffer, this.ringBufferBase + bc.SHM_WINDOW_START, bc.SHM_WINDOW_SIZE), bc.METRICS_SIZE);
        return buffer;
    }

    // Read and send OSC log entries from the IN ring (postMessage mode). Called
    // on the snapshot heartbeat (~150ms) to batch entries; pre-allocated pools,
    // with truncation for large messages.
    sendLogEntries() {
        if (!this.pmPools || !this.logTap) return;

        const pool = this.pmPools.log;
        const C = PM_POOL_CONFIG;

        // A TAP, not a drain: the engine consumes this ring, so the log reads
        // the same bytes without moving anyone's cursor. Falling behind is
        // ordinary — the tap resynchronises to the newest traffic on its own.
        const got = this.wasmExports.clockwork_client_tap_poll(
            this.logTap, this.pollMessages,
            Math.min(C.MAX_LOG_ENTRIES, this.pollMax));
        if (!got) return;

        const view = new DataView(this.wasmMemory.buffer);
        const heap = new Uint8Array(this.wasmMemory.buffer);
        let count = 0;
        let bufferOffset = 0;

        for (let i = 0; i < got; i++) {
            const m = this.#readPolled(view, i);
            if (!m.length) continue;

            // Truncate large messages (e.g., buffer dumps) to LOG_MAX_MESSAGE_SIZE
            const actualLength = Math.min(m.length, C.LOG_MAX_MESSAGE_SIZE);
            if (bufferOffset + actualLength > C.LOG_BUFFER_SIZE) break;

            pool.bufferView.set(heap.subarray(m.ptr, m.ptr + actualLength), bufferOffset);

            const entry = pool.entries[count];
            entry.offset = bufferOffset;
            entry.length = actualLength;
            entry.originalLength = m.length;   // Receiver can detect truncation
            entry.sourceId = m.origin;
            entry.sequence = m.sequence;

            bufferOffset += actualLength;
            count++;
        }

        // Send via postMessage (structured clone - pool remains valid for reuse)
        if (count > 0) {
            pool.message.count = count;
            pool.message.buffer = pool.buffer;
            this.port.postMessage(pool.message);
        }
    }

    async handleMessage(event) {
        const { data } = event;


        try {
            if (data.type === 'listening') {
                this.egressListening = true;
                return;
            }

            if (data.type === 'osc') {
                if (this.mode === 'postMessage') {
                    // Write OSC message directly to ring buffer (no allocation
                    // in process()). Count it as received only if it landed;
                    // a full ring counts as a drop instead.
                    if (data.oscData) {
                        if (this.writeOscToRingBuffer(data.oscData, data.sourceId ?? 0)) {
                            this.recordOscReceived(data.oscData.byteLength);
                        } else {
                            this.recordOscDropped();
                        }
                    }
                }
                return;
            }

            // Handle adding a new OSC input port (for user workers).
            // This allows workers to send OSC directly to the worklet via MessageChannel
            if (data.type === 'addOscPort') {
                const port = event.ports[0];
                if (port) {
                    // Extract sourceId from message (assigned by transport when creating OscChannel)
                    const portSourceId = data.sourceId ?? 0;
                    this.portSourceIds.set(port, portSourceId);

                    port.onmessage = (e) => {
                        if (e.data.type === 'osc' && e.data.oscData) {
                            // Write OSC message directly to ring buffer (no
                            // allocation in process()). sourceId comes from the
                            // message or the port's assignment. As above: only
                            // a landed write counts as received.
                            const msgSourceId = e.data.sourceId ?? this.portSourceIds.get(port) ?? 0;
                            if (this.writeOscToRingBuffer(e.data.oscData, msgSourceId)) {
                                this.recordOscReceived(e.data.oscData.byteLength);
                            } else {
                                this.recordOscDropped();
                            }
                        }
                    };
                    this.oscPorts.push(port);
                }
                return;
            }

            if (data.type === 'clearSched') {
                // Drain the IN ring buffer immediately — discard all stale messages
                // that accumulated while the AudioContext was suspended (e.g. mobile
                // tab switch). This must happen eagerly (not deferred to process())
                // so that new messages sent after purge() resolves are not affected.
                // handleMessage and process() both run on the audio thread, so there
                // is no race with the C++ ring buffer consumer.
                if (this.CONTROL_INDICES) {
                    const head = this.atomicLoad(this.CONTROL_INDICES.IN_HEAD);
                    this.atomicStore(this.CONTROL_INDICES.IN_TAIL, head);
                }

                // Set flag to clear the WASM scheduler on next process() call.
                // The scheduler may contain bundles already dequeued from the ring
                // buffer before the drain — these must also be discarded.
                this.pendingClearSched = true;

                if (data.ack) {
                    this.port.postMessage({ type: 'clearSchedAck' });
                }
                return;
            }

            if (data.type === 'nodeIdRange') {
                // PM mode: receive node ID range (initial or refill)
                if (data.from !== undefined && data.to !== undefined) {
                    this.pushNodeIdRange(data.from, data.to);
                }

                // Accept refill port (transferred via ports array on initial message)
                const port = event.ports[0];
                if (port) {
                    this.nodeIdPort = port;
                    this.nodeIdPort.onmessage = (e) => {
                        if (e.data.type === 'nodeIdRange') {
                            this.pushNodeIdRange(e.data.from, e.data.to);
                        }
                    };
                }
                return;
            }

            if (data.type === 'init') {
                this.mode = data.mode || 'sab';

                // Set snapshot interval (postMessage mode) - convert ms to seconds for AudioContext time
                if (data.snapshotIntervalMs) {
                    this.treeSnapshotMinInterval = data.snapshotIntervalMs / 1000;
                }

                if (this.mode === 'sab' && data.sharedBuffer) {
                    this.sharedBuffer = data.sharedBuffer;
                    this.atomicView = new Int32Array(this.sharedBuffer);
                    this.uint8View = new Uint8Array(this.sharedBuffer);
                    this.dataView = new DataView(this.sharedBuffer);
                }
                // PostMessage mode: memory will be created locally in loadWasm
            }

            if (data.type === 'loadWasm') {
                if (data.wasmBytes) {
                    let memory;

                    if (this.mode === 'sab') {
                        // SAB mode: use the memory passed from orchestrator
                        memory = data.wasmMemory;
                        if (!memory) {
                            this.port.postMessage({
                                type: 'error',
                                error: 'No WASM memory provided!'
                            });
                            return;
                        }
                    } else {
                        // PostMessage mode: create memory locally
                        // Note: WASM was compiled with --shared-memory, so we must use shared: true
                        // The memory just isn't shared with the main thread in this mode
                        const memoryPages = data.memoryPages || 1280;  // 80MB default
                        const maxMemoryPages = data.maxMemoryPages || memoryPages;
                        memory = new WebAssembly.Memory({
                            initial: memoryPages,
                            maximum: maxMemoryPages,
                            shared: true
                        });
                    }

                    // Save memory reference for later use (WASM imports memory, doesn't export it)
                    this.wasmMemory = memory;

                    // Store the guest's encoded config and sampleRate for C++ initialization
                    this.guestConfigBytes = data.guestConfigBytes || null;
                    // Clockwork's own number, in its own word: how many
                    // channels to read from the device. It used to come out
                    // of an input bus-channel count in one engine's config.
                    this.inputChannels = data.inputChannels ?? 0;
                    this.outputChannels = data.outputChannels ?? 2;
                    this.sampleRate = data.sampleRate || 48000;  // Fallback to 48000 if not provided
                    // Where the guest's memory region sits in the wasm heap,
                    // and how big it is. An ABSOLUTE address, measured from
                    // zero — clockwork proves its own heap stops short of it
                    // and then hands it to the guest as DspConfig::arena.
                    this.guestMemoryOffset = data.guestMemoryOffset ?? 0;
                    this.guestMemorySize = data.guestMemorySize ?? 0;
                    // Bulk staging, one writer each. Absolute offsets in the
                    // wasm heap, below the guest's arena because the arena
                    // grows upward into reserved space.
                    this.inboxOffset  = data.inboxOffset ?? 0;
                    this.inboxSize    = data.inboxSize ?? 0;
                    this.outboxOffset = data.outboxOffset ?? 0;
                    this.outboxSize   = data.outboxSize ?? 0;
                    // The span clockwork::mem allocates from. The guest's real-time
                    // pool comes out of it, so the host sizes it from what the
                    // guest asked for; 0 leaves the module on its build-time
                    // default.
                    this.memArenaSize = data.memArenaSize ?? 0;

                    // Import object for WASM
                    // a pthread-enabled WASM build requires these imports
                    // (pthread stubs are no-ops - AudioWorklet is single-threaded)
                    const imports = {
                        env: {
                            memory: memory,

                            /*
                             * SAFE_HEAP's two reporters.
                             *
                             * A -sSAFE_HEAP build instruments every load and
                             * store and calls these when one is out of bounds
                             * or misaligned. Emscripten's own JS glue supplies
                             * them; this worklet instantiates the module
                             * itself, so without these the build will not
                             * INSTANTIATE at all — which is how the strongest
                             * memory diagnostic came to be unusable exactly
                             * when it was needed. They cost nothing in a
                             * normal build, where nothing imports them.
                             */
                            segfault: (...a) => {
                                // The stack is the point: it names the wasm
                                // function that made the bad access, which the
                                // address alone does not.
                                throw new Error("SAFE_HEAP OOB args=" + JSON.stringify(a)
                                    + " STACK " + (new Error().stack || "").replace(/\n/g, " | "));
                            },
                            alignfault: (...a) => {
                                throw new Error("SAFE_HEAP MISALIGNED args=" + JSON.stringify(a)
                                    + " STACK " + (new Error().stack || "").replace(/\n/g, " | "));
                            },
                            emscripten_asm_const_double: () => Date.now() * 1000,
                            // Filesystem syscalls. The build has no filesystem,
                            // but the standard libraries carry references to
                            // these; an unresolved import fails instantiation
                            // of the whole module, with an error that names the
                            // symbol and nothing about where it came from.
                            __syscall_getdents64: () => 0,
                            __syscall_unlinkat: () => 0,
                            __syscall_getcwd: () => -52,  // -ENOSYS
                            // pthread stubs (no-ops - AudioWorklet doesn't support threading)
                            _emscripten_init_main_thread_js: () => {},
                            _emscripten_thread_mailbox_await: () => {},
                            _emscripten_thread_set_strongref: () => {},
                            emscripten_exit_with_live_runtime: () => {},
                            _emscripten_receive_on_main_thread_js: () => {},
                            emscripten_check_blocking_allowed: () => {},
                            _emscripten_thread_cleanup: () => {},
                            emscripten_num_logical_cores: () => 1,  // Report 1 core
                            _emscripten_notify_mailbox_postmessage: () => {},
                            emscripten_notify_memory_growth: () => {}  // Called on memory.grow() — no-op (we manage views ourselves)
                        },
                        wasi_snapshot_preview1: {
                            clock_time_get: (clockid, precision, timestamp_ptr) => {
                                const view = new DataView(memory.buffer);
                                const nanos = BigInt(Math.floor(Date.now() * 1000000));
                                view.setBigUint64(timestamp_ptr, nanos, true);
                                return 0;
                            },
                            // Rust's standard library seeds its hash maps from
                            // here. AudioWorkletGlobalScope does not reliably
                            // expose crypto, so this falls back to a cheap
                            // generator — which is sound for the only use the
                            // engine has: hash seeding affects iteration order
                            // and nothing that reaches the audio.
                            random_get: (buf, len) => {
                                const bytes = new Uint8Array(memory.buffer, buf, len);
                                if (typeof crypto !== 'undefined' && crypto.getRandomValues) {
                                    crypto.getRandomValues(bytes);
                                } else {
                                    let x = (Date.now() ^ 0x9e3779b9) >>> 0;
                                    for (let i = 0; i < len; i++) {
                                        x ^= x << 13; x >>>= 0;
                                        x ^= x >> 17;
                                        x ^= x << 5;  x >>>= 0;
                                        bytes[i] = x & 0xff;
                                    }
                                }
                                return 0;
                            },
                            environ_sizes_get: () => 0,
                            environ_get: () => 0,
                            fd_close: () => 0,
                            /*
                             * THE MODULE'S ONLY WAY TO SPEAK BEFORE THE ENGINE
                             * EXISTS, and it used to return 0 and discard the
                             * bytes.
                             *
                             * clockwork_log writes an OSC message into the debug ring
                             * for a worker to drain, which is worth nothing
                             * during boot — nothing drains that ring until the
                             * engine is running. Everything else the module
                             * prints (scprintf, an abort, a failed placement)
                             * arrives here as a write to fd 2, and a stub
                             * returning 0 told it the bytes were written.
                             *
                             * Measured 2026-09-01: a boot that ended in
                             * init_memory surfaced to the client as
                             * "AudioWorklet initialization timeout" with the
                             * reason discarded at this line.
                             */
                            fd_write: (fd, iov, iovcnt, pnum) => {
                                const view = new DataView(memory.buffer);
                                const u8 = new Uint8Array(memory.buffer);
                                let written = 0, text = '';
                                for (let i = 0; i < iovcnt; i++) {
                                    const ptr = view.getUint32(iov + i * 8, true);
                                    const len = view.getUint32(iov + i * 8 + 4, true);
                                    // Decoded a byte at a time on purpose:
                                    // AudioWorkletGlobalScope has no
                                    // TextDecoder, and a diagnostic channel
                                    // that throws while reporting a failure is
                                    // worse than the silence it replaced.
                                    for (let j = 0; j < len; j++)
                                        text += String.fromCharCode(u8[ptr + j]);
                                    written += len;
                                }
                                if (pnum) view.setUint32(pnum, written, true);
                                const line = text.replace(/\n+$/, '');
                                if (line)
                                    (fd === 2 ? console.error : console.log)(
                                        '[wasm] ' + line);
                                return 0;
                            },
                            fd_seek: () => 0,
                            fd_read: () => 0,
                            proc_exit: (code) => {
                                console.error('[AudioWorklet] WASM tried to exit with code:', code);
                            }
                        }
                    };

                    const module = await WebAssembly.compile(data.wasmBytes);
                    this.wasmInstance = await WebAssembly.instantiate(module, imports);

                    if (this.wasmInstance.exports.get_ring_buffer_base) {
                        this.ringBufferBase = this.wasmInstance.exports.get_ring_buffer_base();

                        // Load buffer constants from WASM (single source of truth)
                        this.loadBufferConstants();

                        this.calculateBufferIndices(this.ringBufferBase);

                        this.initPMPools();

                        this.writeGuestConfigToMemory();

                        // Boot the engine. Clockwork's geometry goes as
                        // arguments; the guest's config block is already in
                        // its region (writeGuestConfigToMemory above), so the
                        // pointer pair is null — JavaScript has no C pointer
                        // to hand over.
                        //
                        // Block size is 0 for "platform default", which on web
                        // is the 128-sample render quantum and cannot be
                        // anything else.
                        if (this.wasmInstance.exports.clockwork_init) {
                            console.log(`[clockwork] transport: ${this.mode === 'sab' ? 'SAB' : 'PM'}`);
                            this.wasmInstance.exports.clockwork_init(
                                this.sampleRate,
                                0,
                                this.inputChannels,
                                this.outputChannels,
                                0,
                                this.guestMemoryOffset,
                                this.guestMemorySize,
                                0, 0,
                                this.memArenaSize,
                                this.inboxOffset,  this.inboxSize,
                                this.outboxOffset, this.outboxSize);

                            this.initNodeIdCounter();
                            this.openClientBoundary();

                            this.isInitialized = true;

                            const initialSnapshot = this.mode === 'postMessage' ? this.readMetricsAndTreeBuffer() : undefined;

                            const msg = {
                                type: 'initialized',
                                success: true,
                                ringBufferBase: this.ringBufferBase,
                                bufferConstants: this.bufferConstants,
                                exports: Object.keys(this.wasmInstance.exports),
                                initialSnapshot
                            };
                            this.port.postMessage(msg, initialSnapshot ? [initialSnapshot] : []);
                        } else {
                            // Nothing can boot without it, so say so rather
                            // than falling out of the handler and letting the
                            // client time out with no reason.
                            const why = 'clockwork_init is not exported by the module - '
                                      + 'the engine cannot be booted. Add _clockwork_init to '
                                      + 'EXPORTED_FUNCTIONS in scripts/build-web.sh.';
                            console.error('[AudioWorklet] ' + why);
                            this.port.postMessage({ type: 'error', error: why });
                        }
                    }
                } else if (data.wasmInstance) {
                    // Pre-instantiated WASM (from Emscripten)
                    this.wasmInstance = data.wasmInstance;

                    if (this.wasmInstance.exports.get_ring_buffer_base) {
                        this.ringBufferBase = this.wasmInstance.exports.get_ring_buffer_base();

                        // Load buffer constants from WASM (single source of truth)
                        this.loadBufferConstants();

                        this.calculateBufferIndices(this.ringBufferBase);

                        this.initPMPools();

                        this.writeGuestConfigToMemory();

                        // Boot the engine. Clockwork's geometry goes as
                        // arguments; the guest's config block is already in
                        // its region (writeGuestConfigToMemory above), so the
                        // pointer pair is null — JavaScript has no C pointer
                        // to hand over.
                        //
                        // Block size is 0 for "platform default", which on web
                        // is the 128-sample render quantum and cannot be
                        // anything else.
                        if (this.wasmInstance.exports.clockwork_init) {
                            console.log(`[clockwork] transport: ${this.mode === 'sab' ? 'SAB' : 'PM'}`);
                            this.wasmInstance.exports.clockwork_init(
                                this.sampleRate,
                                0,
                                this.inputChannels,
                                this.outputChannels,
                                0,
                                this.guestMemoryOffset,
                                this.guestMemorySize,
                                0, 0,
                                this.memArenaSize,
                                this.inboxOffset,  this.inboxSize,
                                this.outboxOffset, this.outboxSize);

                            this.initNodeIdCounter();
                            this.openClientBoundary();

                            this.isInitialized = true;

                            const initialSnapshot = this.mode === 'postMessage' ? this.readMetricsAndTreeBuffer() : undefined;

                            const msg = {
                                type: 'initialized',
                                success: true,
                                ringBufferBase: this.ringBufferBase,
                                bufferConstants: this.bufferConstants,
                                exports: Object.keys(this.wasmInstance.exports),
                                initialSnapshot
                            };
                            this.port.postMessage(msg, initialSnapshot ? [initialSnapshot] : []);
                        }
                    }
                }
            }

            if (data.type === 'callExport') {
                // Invoke a named export on the audio thread and post the result
                // back. A guest calls its own exports here to reach the live
                // engine (rerezzed stages its pipeline this way).
                let result;
                try {
                    const fn = this.wasmExports && this.wasmExports[data.name];
                    result = fn ? fn(...(data.args || [])) : undefined;
                } catch (e) {
                    if (__DEV__) console.error('[AudioWorklet] callExport', data.name, e);
                    result = undefined;
                }
                this.port.postMessage({ type: 'exportCalled', callId: data.callId, result });
                return;
            }

            if (data.type === 'getTimeOffset') {
                // Return time offset (NTP seconds when AudioContext was at 0)
                if (this.wasmInstance && this.wasmInstance.exports.get_time_offset) {
                    const offset = this.wasmInstance.exports.get_time_offset();
                    this.port.postMessage({
                        type: 'timeOffset',
                        offset: offset
                    });
                } else {
                    console.error('[AudioWorklet] get_time_offset not available! wasmInstance:', !!this.wasmInstance);
                    this.port.postMessage({
                        type: 'error',
                        error: 'get_time_offset function not available in WASM exports'
                    });
                }
            }

            if (data.type === 'setNTPStartTime') {
                // Write NTP start time to WASM memory (Float64)
                if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
                    const offset = this.ringBufferBase + this.bufferConstants.NTP_START_TIME_START;
                    const view = new Float64Array(this.wasmMemory.buffer, offset, 1);
                    view[0] = data.ntpStartTime;
                }
            }

            if (data.type === 'setDriftOffset') {
                // Write drift offset to WASM memory (Int32, microseconds)
                if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
                    const offset = this.ringBufferBase + this.bufferConstants.DRIFT_OFFSET_START;
                    const view = new Int32Array(this.wasmMemory.buffer, offset, 1);
                    view[0] = data.driftOffsetUs;
                }
            }

            if (data.type === 'setClockOffset') {
                // Write clock offset to WASM memory (Int32, milliseconds)
                if (this.wasmMemory && this.ringBufferBase !== null && this.bufferConstants) {
                    const offset = this.ringBufferBase + this.bufferConstants.GLOBAL_OFFSET_START;
                    const view = new Int32Array(this.wasmMemory.buffer, offset, 1);
                    view[0] = data.clockOffsetMs;
                }
            }

            // ClockworkClock session-state writes (PM mode). The worklet's
            // ClockworkClockState region is private memory in PM mode; these
            // handlers apply main-thread mutations into it. SAB mode
            // bypasses this entirely — JS writes the shared region.

            if (this.clockworkClockStateBigInt) {
                const views = { bigInt: this.clockworkClockStateBigInt, int32: this.clockworkClockStateInt32 };
                if (data.type === ClockworkClockMessageType.SET_SESSION_BPM) {
                    retempoClock(views, data.bpm, data.nowNtp);
                } else if (data.type === ClockworkClockMessageType.SET_SESSION_IS_PLAYING) {
                    writeClockTransport(views, data.isPlaying, data.atNtpSeconds);
                } else if (data.type === ClockworkClockMessageType.SET_SESSION_BEAT_ORIGIN_NTP) {
                    writeClockOrigin(views, data.beatOriginNtp);
                } else if (data.type === ClockworkClockMessageType.SET_SESSION_METER) {
                    if (isValidMeter(data.num, data.den)) writeClockMeter(views, data.num, data.den);
                }
            }

            if (data.type === 'getMetrics') {
                // Return raw metrics buffer for postMessage mode
                // Same layout as SAB - can be used directly with MetricsOffsets
                const metrics = this.metricsView ? new Uint32Array(this.metricsView) : null;
                this.port.postMessage({
                    type: 'metricsSnapshot',
                    requestId: data.requestId,
                    metrics: metrics
                });
            }

            if (data.type === 'copyBufferData') {
                try {
                    const { copyId, ptr, data: bufferData } = data;

                    if (!this.wasmMemory || !this.wasmMemory.buffer) {
                        throw new Error('WASM memory not initialized');
                    }

                    // Copy the payload to WASM memory at the given offset.
                    /*
                     * BYTES, not samples. writeInbox promises OPAQUE bytes — a
                     * definition, a lookup table, six bytes of anything. Reading the payload
                     * as a Float32Array throws "byte length of Float32Array should be a
                     * multiple of 4" for any length that is not, and copies the right bytes
                     * only by coincidence of the view width for any length that is.
                     */
                    const src = new Uint8Array(bufferData);
                    new Uint8Array(this.wasmMemory.buffer, ptr, src.byteLength).set(src);

                    if (__DEV__) {
                        console.log(`[AudioWorklet] Copied ${src.byteLength} bytes to WASM memory at offset ${ptr}`);
                    }

                    this.port.postMessage({
                        type: 'bufferCopied',
                        copyId: copyId,
                        success: true
                    });
                } catch (copyError) {
                    console.error('[AudioWorklet] Buffer copy failed:', copyError);
                    this.port.postMessage({
                        type: 'bufferCopied',
                        copyId: data.copyId,
                        success: false,
                        error: copyError.message
                    });
                }
            }

            /*
             * The other half of copyBufferData, and the reason it exists.
             *
             * In postMessage mode the client has no view of the worklet's heap
             * at all, so a guest that produced a blob there had no way to hand
             * it back — readOutbox simply threw, and the bulk channel was
             * one-directional on this transport alone. The worklet CAN see the
             * heap, so it copies the range out and transfers the buffer, which
             * is the same trick copyBufferData plays going the other way.
             *
             * TRANSFERRED, not structured-cloned: the copy below is the only
             * one, and handing the buffer over detaches it here rather than
             * duplicating a multi-megabyte payload on the way past.
             */
            if (data.type === 'readBufferData') {
                try {
                    const { readId, ptr, len } = data;

                    if (!this.wasmMemory || !this.wasmMemory.buffer) {
                        throw new Error('WASM memory not initialized');
                    }
                    // Bounds are checked on the client too, against the region
                    // it was told about. Checked again here against the heap
                    // that actually exists, because this side is the one that
                    // would fault — and because a worklet must not trust a
                    // length it did not compute.
                    const heap = this.wasmMemory.buffer.byteLength;
                    if (!Number.isInteger(ptr) || !Number.isInteger(len)
                        || ptr < 0 || len < 0 || ptr > heap || len > heap - ptr) {
                        throw new Error(`read [${ptr}, ${ptr + len}) is outside the heap (${heap})`);
                    }

                    const out = new Uint8Array(len);
                    out.set(new Uint8Array(this.wasmMemory.buffer, ptr, len));

                    this.port.postMessage({
                        type: 'bufferRead',
                        readId,
                        success: true,
                        data: out.buffer,
                    }, [out.buffer]);
                } catch (readError) {
                    console.error('[AudioWorklet] Buffer read failed:', readError);
                    this.port.postMessage({
                        type: 'bufferRead',
                        readId: data.readId,
                        success: false,
                        error: readError.message
                    });
                }
            }

            if (data.type === 'growMemory') {
                try {
                    const { growId, pages } = data;
                    if (!this.wasmMemory) {
                        throw new Error('WASM memory not initialized');
                    }
                    const result = this.wasmMemory.grow(pages);
                    const success = result !== -1;
                    if (__DEV__ && success) {
                        const newSize = this.wasmMemory.buffer.byteLength;
                        console.log(`[AudioWorklet] Memory grown by ${pages} pages, new size: ${(newSize / (1024 * 1024)).toFixed(0)}MB`);
                    }
                    this.port.postMessage({
                        type: 'memoryGrown',
                        growId,
                        success,
                        newBufferSize: this.wasmMemory.buffer.byteLength,
                    });
                } catch (growError) {
                    console.error('[AudioWorklet] Memory grow failed:', growError);
                    this.port.postMessage({
                        type: 'memoryGrown',
                        growId: data.growId,
                        success: false,
                        error: growError.message,
                    });
                }
            }

        } catch (error) {
            console.error('[AudioWorklet] Error handling message:', error);
            this.port.postMessage({
                type: 'error',
                error: error.message,
                stack: error.stack
            });
        }
    }

    process(inputs, outputs, parameters) {
        this.processCallCount++;

        if (!this.isInitialized) {
            return true;
        }

        try {
            if (this.wasmInstance && this.wasmInstance.exports.clockwork_tick) {

                // Clear WASM scheduler if flagged (before the tick runs scheduled bundles)
                if (this.pendingClearSched) {
                    this.pendingClearSched = false;
                    if (this.wasmInstance.exports.clear_scheduler) {
                        this.wasmInstance.exports.clear_scheduler();
                    }
                }

                // In AudioWorkletGlobalScope, currentTime is an attribute of the global
                // scope; a different local name avoids shadowing it.
                const audioContextTime = currentTime;  // Access the global currentTime directly

                // Copy WebAudio input to the DSP input buses (before processing)
                const inputChannels = inputs[0]?.length || 0;
                const outputChannels = outputs[0]?.length || 0;

                if (inputChannels > 0 && this.wasmInstance?.exports?.get_audio_input_bus) {
                    try {
                        const inputBusPtr = this.wasmInstance.exports.get_audio_input_bus();
                        const numSamples = this.wasmInstance.exports.get_audio_buffer_samples();

                        if (inputBusPtr && inputBusPtr > 0) {
                            const memBuffer = this.sharedBuffer || this.wasmMemory?.buffer;
                            if (memBuffer) {
                                const configuredChannels = this.inputChannels || 2;
                                const effectiveChannels = Math.min(inputChannels, configuredChannels);

                                // Reuse input view if possible to avoid allocation in hot path
                                if (!this.inputView ||
                                    this.lastInputBusPtr !== inputBusPtr ||
                                    this.lastInputChannels !== configuredChannels) {
                                    this.inputView = new Float32Array(memBuffer, inputBusPtr, numSamples * configuredChannels);
                                    this.lastInputBusPtr = inputBusPtr;
                                    this.lastInputChannels = configuredChannels;
                                }

                                for (let ch = 0; ch < effectiveChannels; ch++) {
                                    if (inputs[0]?.[ch]) {
                                        this.inputView.set(inputs[0][ch], ch * numSamples);
                                    }
                                }
                            }
                        }
                    } catch (err) {
                        // Silently fail in real-time audio context
                    }
                }

                // clockwork_tick (the lanes tick, src/lanes/lanes.h) calculates NTP
                // time internally from:
                // - NTP_START_TIME (write-once, set during initialization)
                // - DRIFT_OFFSET (microseconds, updated every 1s by main thread)
                // - GLOBAL_OFFSET (milliseconds, written by the setClockOffset
                //   handler above; for multi-system sync)

                const keepAlive = this.wasmInstance.exports.clockwork_tick(
                    audioContextTime,
                    outputChannels,
                    inputChannels
                );

                // PM mode: check whether the counter advanced past our current range.
                // With halfway pre-fetch (request at 50% remaining), the refill arrives
                // long before exhaustion — a postMessage round-trip is ~microseconds while
                // burning 5000+ IDs requires thousands of synth creations per audio block.
                if (this.nodeIdCounterView && this.nodeIdRangeCount > 0) {
                    const current = Atomics.load(this.nodeIdCounterView, 0);

                    if (current >= this.nodeIdRanges[0].to) {
                        if (this.nodeIdRangeCount > 1) {
                            // Promote range[1] to range[0], jump counter to new range
                            const next = this.nodeIdRanges[1];
                            this.nodeIdRanges[0].from = next.from;
                            this.nodeIdRanges[0].to = next.to;
                            next.from = 0;
                            next.to = 0;
                            this.nodeIdRangeCount = 1;
                            Atomics.store(this.nodeIdCounterView, 0, this.nodeIdRanges[0].from);
                        }
                        if (!this.nodeIdRefillRequested && this.nodeIdPort) {
                            this.nodeIdRefillRequested = true;
                            this.nodeIdPort.postMessage({ type: 'requestNodeIdRange' });
                        }
                    } else if (!this.nodeIdRefillRequested && this.nodeIdRangeCount < 2 && this.nodeIdPort) {
                        // Pre-fetch at halfway through the range
                        const remaining = this.nodeIdRanges[0].to - current;
                        const rangeSize = this.nodeIdRanges[0].to - this.nodeIdRanges[0].from;
                        if (remaining <= (rangeSize >>> 1)) {
                            this.nodeIdRefillRequested = true;
                            this.nodeIdPort.postMessage({ type: 'requestNodeIdRange' });
                        }
                    }
                }

                // Copy DSP audio output to AudioWorklet outputs
                if (this.wasmInstance.exports.get_audio_output_bus && outputs[0] && outputs[0].length >= 1) {
                    try {
                        const audioBufferPtr = this.wasmInstance.exports.get_audio_output_bus();
                        const numSamples = this.wasmInstance.exports.get_audio_buffer_samples();

                        if (audioBufferPtr && audioBufferPtr > 0) {
                            const wasmMemory = this.wasmInstance.exports.memory || this.wasmMemory;

                            if (!wasmMemory || !wasmMemory.buffer) {
                                return true;
                            }

                            const currentBuffer = wasmMemory.buffer;
                            const bufferSize = currentBuffer.byteLength;
                            const configuredOutputChannels = this.outputChannels || 2;
                            const effectiveOutputChannels = Math.min(outputs[0].length, configuredOutputChannels);
                            const requiredBytes = audioBufferPtr + (numSamples * effectiveOutputChannels * 4);

                            if (audioBufferPtr < 0 || audioBufferPtr > bufferSize || requiredBytes > bufferSize) {
                                return true;
                            }

                            // Reuse Float32Array view if possible (avoid allocation in hot path)
                            if (!this.audioView ||
                                this.lastAudioBufferPtr !== audioBufferPtr ||
                                this.lastWasmBufferSize !== bufferSize ||
                                this.lastNumChannels !== effectiveOutputChannels ||
                                currentBuffer !== this.audioView.buffer) {
                                this.audioView = new Float32Array(currentBuffer, audioBufferPtr, numSamples * effectiveOutputChannels);
                                this.lastAudioBufferPtr = audioBufferPtr;
                                this.lastWasmBufferSize = bufferSize;
                            }

                            // Recreate channel views only when parameters change
                            // (avoids per-frame subarray() allocation)
                            if (!this.channelViews ||
                                this.lastNumSamples !== numSamples ||
                                this.lastNumChannels !== effectiveOutputChannels ||
                                this.channelViews[0].buffer !== this.audioView.buffer) {
                                this.channelViews = new Array(effectiveOutputChannels);
                                for (let ch = 0; ch < effectiveOutputChannels; ch++) {
                                    this.channelViews[ch] = this.audioView.subarray(ch * numSamples, (ch + 1) * numSamples);
                                }
                                this.lastNumSamples = numSamples;
                                this.lastNumChannels = effectiveOutputChannels;
                            }

                            for (let ch = 0; ch < effectiveOutputChannels; ch++) {
                                outputs[0][ch].set(this.channelViews[ch]);
                            }
                        }
                    } catch (err) {
                        // Silently fail in real-time audio context
                    }
                }

                if (this.mode === 'postMessage') {
                    if (this.egressListening) this.readOscReplies();
                    // Batch log entries with snapshot heartbeat (~150ms) to reduce postMessage frequency
                    if (this.checkAndSendSnapshot(audioContextTime)) {
                        this.sendLogEntries();
                    }
                } else {
                    // SAB mode: Notify waiting workers when there's data to read
                    // Atomics.notify() is cheap when no one is waiting, so notify every frame
                    if (this.atomicView) {
                        const outHead = this.atomicLoad(this.CONTROL_INDICES.OUT_HEAD);
                        const outTail = this.atomicLoad(this.CONTROL_INDICES.OUT_TAIL);
                        if (outHead !== outTail) {
                            Atomics.notify(this.atomicView, this.CONTROL_INDICES.OUT_HEAD, 1);
                        }
                        // Notify any IN-ring producer waiting on space when C++ consumes
                        // messages (tail moved = space freed)
                        const inTail = this.atomicLoad(this.CONTROL_INDICES.IN_TAIL);
                        if (inTail !== this.lastInTail) {
                            Atomics.notify(this.atomicView, this.CONTROL_INDICES.IN_TAIL, 1);
                            this.lastInTail = inTail;
                        }
                    }
                    // SAB mode: OSC logging is handled by osc_out_log_sab_worker
                }

                if (this.processCallCount % 3750 === 0) {  // Every ~10 seconds instead of 1
                    this.checkStatus();
                }

                return keepAlive !== 0;
            }
        } catch (error) {
            console.error('[AudioWorklet] process() error:', error);
            console.error('[AudioWorklet] Stack:', error.stack);
            if (this.atomicView && this.mode === 'sab') {
                Atomics.or(this.atomicView, this.CONTROL_INDICES.STATUS_FLAGS, this.STATUS_FLAGS.WASM_ERROR);
            }
            if (this.metricsView) {
                if (this.mode === 'sab') {
                    Atomics.add(this.metricsView, MetricsOffsets.ENGINE_WASM_ERRORS, 1);
                } else {
                    this.metricsView[MetricsOffsets.ENGINE_WASM_ERRORS]++;
                }
            }
        }

        return true;
    }

    checkStatus() {
        if (!this.atomicView) return;

        const statusFlags = this.atomicLoad(this.CONTROL_INDICES.STATUS_FLAGS);

        if (statusFlags !== this.STATUS_FLAGS.OK) {
            // Update pre-allocated status object (avoids allocation on audio thread)
            this._statusObj.bufferFull = !!(statusFlags & this.STATUS_FLAGS.BUFFER_FULL);
            this._statusObj.overrun = !!(statusFlags & this.STATUS_FLAGS.OVERRUN);
            this._statusObj.wasmError = !!(statusFlags & this.STATUS_FLAGS.WASM_ERROR);
            this._statusObj.fragmented = !!(statusFlags & this.STATUS_FLAGS.FRAGMENTED_MSG);

            // Update pre-allocated metrics object (avoids allocation on audio thread)
            this._metricsObj.processCount = this.metricsView[MetricsOffsets.ENGINE_PROCESS_COUNT];
            this._metricsObj.messagesProcessed = this.metricsView[MetricsOffsets.ENGINE_MESSAGES_PROCESSED];
            this._metricsObj.messagesDropped = this.metricsView[MetricsOffsets.ENGINE_MESSAGES_DROPPED];
            this._metricsObj.schedulerQueueDepth = this.metricsView[MetricsOffsets.ENGINE_SCHEDULER_DEPTH];
            this._metricsObj.schedulerQueueMax = this.metricsView[MetricsOffsets.ENGINE_SCHEDULER_PEAK_DEPTH];
            this._metricsObj.schedulerQueueDropped = this.metricsView[MetricsOffsets.ENGINE_SCHEDULER_DROPPED];

            // Update pre-allocated message object and send
            // Note: postMessage does structured clone, so reusing the object is safe
            this._statusMessage.flags = statusFlags;
            this.port.postMessage(this._statusMessage);

            const persistentFlags = statusFlags & (this.STATUS_FLAGS.BUFFER_FULL);
            this.atomicStore(this.CONTROL_INDICES.STATUS_FLAGS, persistentFlags);
        }
    }
}

registerProcessor('clockwork-processor', ClockworkProcessor);
