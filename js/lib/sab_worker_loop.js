// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * Shared infrastructure for SAB ring-buffer reader workers.
 *
 * Each worker calls runSabWorker() with configuration specifying which
 * ring buffer to read, how to process messages, and what to post back.
 */
import { WasmClient } from './wasm_client.js';

export function runSabWorker(config) {
    const {
        name,
        calculateControlIndices,
        headIndex,          // (CONTROL_INDICES) => int32 array index for head
        tailIndex,          // (CONTROL_INDICES) => int32 array index for tail
        readMessages,       // (ctx) => array of results (empty if none)
        postResults,        // (results) => void — sends results to main thread
        initMetrics = true,
        onInit,             // optional (ctx) => void — runs after ring buffer setup
        extraHandlers,      // optional { [type]: (data, ctx) => void }
        // Where the init/start/stop protocol is spoken. A worker's own global by default; a MessagePort when
        // the reader is embedded in a worker that is already something else — the ring does not care which
        // thread drains it, and a host may want the frames where it uses them rather than a hop away.
        endpoint = self,
        // Atomics.wait BLOCKS the thread it runs on. That is right for a worker whose whole job is this ring
        // — it costs nothing and wakes instantly — and wrong for a reader embedded in a thread that is also
        // something else, because nothing else on that thread ever runs again, not even its own messages.
        // Embedded readers wait without blocking instead. Default: blocking when we own the thread.
        blocking = !endpoint || endpoint === globalThis,
    } = config;

    // Mutable shared state — updated by initRingBuffer, read by callbacks via ctx
    const ctx = {
        sharedBuffer: null,
        ringBufferBase: null,
        bufferConstants: null,
        atomicView: null,
        dataView: null,
        uint8View: null,
        metricsView: null,
        CONTROL_INDICES: {},
        // This worker's way into the rings: the engine's own C, running on
        // this worker's own stack. See js/lib/wasm_client.js.
        client: null,
    };

    let running = false;

    async function initRingBuffer(buffer, base, constants, wasmModule, wasmMemory) {
        ctx.sharedBuffer = buffer;
        ctx.ringBufferBase = base;
        ctx.bufferConstants = constants;
        ctx.atomicView = new Int32Array(buffer);
        ctx.dataView = new DataView(buffer);
        ctx.uint8View = new Uint8Array(buffer);
        ctx.CONTROL_INDICES = calculateControlIndices(base, constants.CONTROL_START);
        if (initMetrics) {
            const metricsBase = base + constants.METRICS_START;
            ctx.metricsView = new Uint32Array(buffer, metricsBase, constants.METRICS_SIZE / 4);
        }

        if (wasmModule && wasmMemory) {
            // Throws if no slot is free; the handler below turns that into an
            // error posted to the main thread rather than a worker that runs
            // for ever delivering nothing.
            ctx.client = await WasmClient.open({
                wasmModule, wasmMemory, ringBufferBase: base,
                bufferConstants: constants, label: name,
            });
        }

        await onInit?.(ctx);
    }

    function waitLoop() {
        const hIdx = headIndex(ctx.CONTROL_INDICES);
        // A reader that CONSUMES leaves its position in the ring's own tail, so
        // "nothing to do" is head === tail. A reader that only WATCHES keeps its
        // cursor to itself — the ring's tail belongs to whoever consumes — so it
        // idles on the head not having moved since it last looked.
        const tIdx = tailIndex ? tailIndex(ctx.CONTROL_INDICES) : -1;
        let lastHead = -1;

        while (running) {
            try {
                const currentHead = Atomics.load(ctx.atomicView, hIdx);
                const idle = tIdx >= 0
                    ? currentHead === Atomics.load(ctx.atomicView, tIdx)
                    : currentHead === lastHead;

                if (idle) {
                    Atomics.wait(ctx.atomicView, hIdx, currentHead);
                }
                lastHead = Atomics.load(ctx.atomicView, hIdx);

                const results = readMessages(ctx);
                if (results && results.length > 0) {
                    postResults(results);
                }
            } catch (error) {
                console.error(`[${name}] Error in wait loop:`, error);
                endpoint.postMessage({ type: 'error', error: error.message });
                Atomics.wait(ctx.atomicView, 0, ctx.atomicView[0], 10);
            }
        }
    }

    // The same loop, yielding rather than blocking: Atomics.waitAsync hands back a promise instead of
    // parking the thread, so the host's own work — its messages, its timers — keeps running between frames.
    // Where it is missing, a short sleep stands in: slower to wake, but it still yields.
    async function waitLoopAsync() {
        const hIdx = headIndex(ctx.CONTROL_INDICES);
        const tIdx = tailIndex ? tailIndex(ctx.CONTROL_INDICES) : -1;
        let lastHead = -1;

        while (running) {
            try {
                const currentHead = Atomics.load(ctx.atomicView, hIdx);
                const idle = tIdx >= 0
                    ? currentHead === Atomics.load(ctx.atomicView, tIdx)
                    : currentHead === lastHead;

                if (idle) {
                    if (typeof Atomics.waitAsync === 'function') {
                        const w = Atomics.waitAsync(ctx.atomicView, hIdx, currentHead);
                        if (w.async) await w.value;
                    } else {
                        await new Promise((r) => setTimeout(r, 1));
                    }
                    if (!running) return;
                }
                lastHead = Atomics.load(ctx.atomicView, hIdx);

                const results = readMessages(ctx);
                if (results && results.length > 0) {
                    postResults(results);
                }
            } catch (error) {
                console.error(`[${name}] Error in wait loop:`, error);
                endpoint.postMessage({ type: 'error', error: error.message });
                await new Promise((r) => setTimeout(r, 10));
            }
        }
    }

    function start() {
        if (!ctx.sharedBuffer) {
            console.error(`[${name}] Cannot start - not initialized`);
            return;
        }
        if (running) {
            if (__DEV__) console.warn(`[${name}] Already running`);
            return;
        }
        running = true;
        if (blocking) waitLoop(); else waitLoopAsync();
    }

    function stop() {
        running = false;
    }

    endpoint.addEventListener('message', async (event) => {
        const { data } = event;
        try {
            if (extraHandlers?.[data.type]) {
                extraHandlers[data.type](data, ctx);
                return;
            }
            switch (data.type) {
                case 'init':
                    if (data.sharedBuffer) {
                        await initRingBuffer(data.sharedBuffer, data.ringBufferBase,
                                             data.bufferConstants, data.wasmModule,
                                             data.wasmMemory);
                    }
                    // The slot this worker's client claimed: the transport releases it
                    // when it terminates the worker (a worker blocked in Atomics.wait never
                    // runs a 'stop', so it cannot give the slot back itself).
                    endpoint.postMessage({ type: 'initialized', slot: ctx.client?.slotIndex ?? -1 });
                    break;
                case 'start':
                    if (ctx.sharedBuffer) start();
                    break;
                case 'stop':
                    stop();
                    break;
                default:
                    console.warn(`[${name}] Unknown message type:`, data.type);   // a mismatch between versions: said in every build
            }
        } catch (error) {
            console.error(`[${name}] Error:`, error);
            endpoint.postMessage({ type: 'error', error: error.message });
        }
    });

    endpoint.start?.();   // a MessagePort delivers nothing until it is started
    if (__DEV__) console.log(`[${name}] Script loaded`);
}
