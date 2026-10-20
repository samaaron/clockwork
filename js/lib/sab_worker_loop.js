// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * Shared infrastructure for SAB ring-buffer reader workers.
 *
 * Each worker calls runSabWorker() with configuration specifying which
 * ring buffer to read, how to process messages, and what to post back.
 */
import { engineRings } from './ring.js';

export function runSabWorker(config) {
    const {
        name,
        calculateControlIndices,
        headIndex,          // (RING_CURSOR_INDICES) => int32 array index for head
        tailIndex,          // (RING_CURSOR_INDICES) => int32 array index for tail
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
        arenaBase: null,
        bufferConstants: null,
        atomicView: null,
        dataView: null,
        uint8View: null,
        metricsView: null,
        RING_CURSOR_INDICES: {},
        // The engine's rings, for js/lib/ring.js to read. Nothing compiled
        // runs here (docs/THREADS.md).
        rings: null,
    };

    let running = false;
    // THE STOP A PARKED THREAD SEES. A worker blocked in Atomics.wait runs no message until it wakes, and a
    // terminate() does not stop it at once either: Chromium runs a terminated worker on for about two seconds, and
    // one parked on a ring wakes with every frame written and reads it — the next engine's setup, its first
    // replies, gone to a thread no one listens to. So the transport says stop in shared memory as well, wakes the
    // ring's waiters, and waits to hear 'stopped': the loop reads this word every time it wakes, before it reads
    // the ring.
    let stopWord = null;
    const toldToStop = () => stopWord !== null && Atomics.load(stopWord, 0) !== 0;
    // Counted up by every start() and stop(). A yielding loop parked in Atomics.waitAsync when the reader is stopped
    // and started again, with nothing written in between, wakes to a running reader — and without this read on beside
    // the loop that replaced it: one more loop for each time, every one woken by every frame (and a watcher, which
    // keeps its own place, shown each frame by each of them).
    let generation = 0;

    async function initRingBuffer(buffer, base, constants) {
        ctx.sharedBuffer = buffer;
        ctx.arenaBase = base;
        ctx.bufferConstants = constants;
        ctx.atomicView = new Int32Array(buffer);
        ctx.dataView = new DataView(buffer);
        ctx.uint8View = new Uint8Array(buffer);
        ctx.RING_CURSOR_INDICES = calculateControlIndices(base, constants.RING_CURSORS_START);
        if (initMetrics) {
            const metricsBase = base + constants.METRICS_START;
            ctx.metricsView = new Uint32Array(buffer, metricsBase, constants.METRICS_SIZE / 4);
        }

        // Throws if the engine's rings are not in a format ring.js reads; the
        // handler below turns that into an error posted to the main thread
        // rather than a worker that runs for ever delivering nothing.
        ctx.rings = engineRings(buffer, base, constants);

        await onInit?.(ctx);
    }

    // Out of the loop for good, said once to whoever waits on it.
    function stopped() {
        running = false;
        generation++;
        endpoint.postMessage({ type: 'stopped' });
    }

    function waitLoop(mine) {
        const hIdx = headIndex(ctx.RING_CURSOR_INDICES);
        // A reader that CONSUMES leaves its position in the ring's own tail, so
        // "nothing to do" is head === tail. A reader that only WATCHES keeps its
        // cursor to itself — the ring's tail belongs to whoever consumes — so it
        // idles on the head not having moved since it last looked.
        const tIdx = tailIndex ? tailIndex(ctx.RING_CURSOR_INDICES) : -1;
        let lastHead = -1;

        while (running && mine === generation) {
            try {
                if (toldToStop()) { stopped(); return; }
                const currentHead = Atomics.load(ctx.atomicView, hIdx);
                const idle = tIdx >= 0
                    ? currentHead === Atomics.load(ctx.atomicView, tIdx)
                    : currentHead === lastHead;

                if (idle) {
                    Atomics.wait(ctx.atomicView, hIdx, currentHead);
                    if (toldToStop()) { stopped(); return; }
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
    async function waitLoopAsync(mine) {
        const hIdx = headIndex(ctx.RING_CURSOR_INDICES);
        const tIdx = tailIndex ? tailIndex(ctx.RING_CURSOR_INDICES) : -1;
        let lastHead = -1;

        while (running && mine === generation) {
            try {
                if (toldToStop()) { stopped(); return; }
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
                    if (toldToStop()) { stopped(); return; }
                    if (!running || mine !== generation) return;   // stopped, or replaced, while it waited
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
        const mine = ++generation;
        if (blocking) waitLoop(mine); else waitLoopAsync(mine);
    }

    function stop() {
        running = false;
        generation++;
    }

    endpoint.addEventListener('message', async (event) => {
        // A worker's messages carry no origin; a window's must be our own.
        if (event.origin && event.origin !== self.location.origin) return;
        const { data } = event;
        try {
            if (extraHandlers?.[data.type]) {
                extraHandlers[data.type](data, ctx);
                return;
            }
            switch (data.type) {
                case 'init':
                    stopWord = data.stopBuffer ? new Int32Array(data.stopBuffer) : null;
                    if (data.sharedBuffer) {
                        await initRingBuffer(data.sharedBuffer, data.arenaBase,
                                             data.bufferConstants);
                    }
                    endpoint.postMessage({ type: 'initialized' });
                    break;
                case 'start':
                    if (ctx.sharedBuffer) start();
                    break;
                case 'stop':
                    // Not while a read is under way: this handler runs between the loop's awaits, or not at all
                    // while it is blocked (the stop word is for that one).
                    stop();
                    endpoint.postMessage({ type: 'stopped' });
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
