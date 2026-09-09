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
                self.postMessage({ type: 'error', error: error.message });
                Atomics.wait(ctx.atomicView, 0, ctx.atomicView[0], 10);
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
        waitLoop();
    }

    function stop() {
        running = false;
    }

    self.addEventListener('message', async (event) => {
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
                    self.postMessage({ type: 'initialized' });
                    break;
                case 'start':
                    if (ctx.sharedBuffer) start();
                    break;
                case 'stop':
                    stop();
                    break;
                default:
                    if (__DEV__) console.warn(`[${name}] Unknown message type:`, data.type);
            }
        } catch (error) {
            console.error(`[${name}] Error:`, error);
            self.postMessage({ type: 'error', error: error.message });
        }
    });

    if (__DEV__) console.log(`[${name}] Script loaded`);
}
