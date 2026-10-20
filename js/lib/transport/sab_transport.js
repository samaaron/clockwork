// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

import { Transport } from './transport.js';
import { createWorker } from '../worker_loader.js';
import { OscChannel } from '../osc_channel.js';
import { calculateInControlIndices, IN_HEAD, OUT_HEAD } from '../ring_cursor_offsets.js';
import { engineRings } from '../ring.js';

/**
 * SAB (SharedArrayBuffer) Transport
 *
 * Uses ring buffers in shared memory for high-performance, low-latency
 * OSC message passing. Requires crossOriginIsolated context.
 *
 * Architecture:
 * - OSC OUT: producer frames messages directly onto the IN ring (OscChannel)
 * - OSC IN: Worker waits on OUT ring buffer (Atomics.wait), forwards to callbacks
 *   (debug rides this path too, as /clockwork/debug)
 */
export class SABTransport extends Transport {
    #sharedBuffer;
    #arenaBase;
    #bufferConstants;

    // The engine's ingress ring, which every channel on this thread writes
    // (js/lib/ring.js). The source id travels with each send.
    #ingress = null;

    // Cached views for ring buffer access
    #atomicView;
    #dataView;
    #uint8View;
    #controlIndices;

    // Workers
    #oscInWorker;
    #oscInIsOurs = true;   // false when the host named its own reader: it is not ours to terminate
    #oscOutLogWorker;
    #workerBaseURL;
    #oscInEndpoint;

    // The readers' stop, in memory they read every time they wake (sab_worker_loop.js): 1 = stop.
    #stop = new Int32Array(new SharedArrayBuffer(4));
    #disposing = null;

    // Lazily-created main-thread channel for the transport's own send()
    #mainChannel = null;

    // Callbacks
    #onReplyCallback;
    #onDebugCallback;
    #onErrorCallback;
    #onOscLogCallback;

    // Source ID tracking (0 = main thread, 1+ = workers)
    #nextSourceId = 1;

    // State
    #initialized = false;

    // Metrics (using canonical names matching metrics_offsets.js)
    #oscOutMessagesSent = 0;
    #oscOutMessagesDropped = 0;
    #oscOutBytesSent = 0;

    /**
     * @param {Object} config
     * @param {SharedArrayBuffer} config.sharedBuffer
     * @param {number} config.arenaBase
     * @param {Object} config.bufferConstants
     * @param {string} config.workerBaseURL
     * @param {Function} config.getAudioContextTime
     * @param {Function} config.getNTPStartTime
     */
    constructor(config) {
        super({ ...config, mode: 'sab' });

        this.#sharedBuffer = config.sharedBuffer;
        this.#arenaBase = config.arenaBase;
        this.#bufferConstants = config.bufferConstants;
        this.#workerBaseURL = config.workerBaseURL;
        // A host may drain the egress itself, in a worker where it can act on the frames. It speaks the same
        // init/start/stop protocol over this endpoint and posts the frames back as { type: 'messages' } — the
        // ones it did not keep. The ring has one reader, so naming one here means the default pump is not
        // spawned at all.
        this.#oscInEndpoint = config.oscInEndpoint ?? null;

        // Validate SAB requirements
        if (!(this.#sharedBuffer instanceof SharedArrayBuffer)) {
            throw new Error('SABTransport requires a SharedArrayBuffer');
        }

        // Initialize typed array views
        this.#initializeViews();
    }

    /**
     * Initialize the transport - spawns workers
     */
    async initialize() {
        if (this.#initialized) {
            if (__DEV__) console.warn('[SABTransport] Already initialized');
            return;
        }

        // Spawn workers (uses Blob URL if cross-origin). OSC OUT is gone: the
        // producer writes the IN ring directly (OscChannel); nothing sits in
        // between.
        const [oscInWorker, oscOutLogWorker] = await Promise.all([
            this.#oscInEndpoint ?? createWorker(this.#workerBaseURL + 'osc_in_worker.js', { type: 'module' }),
            createWorker(this.#workerBaseURL + 'osc_out_log_sab_worker.js', { type: 'module' })
        ]);
        this.#oscInWorker = oscInWorker;
        this.#oscInIsOurs = !this.#oscInEndpoint;
        this.#oscInWorker.start?.();   // a MessagePort delivers nothing until it is started
        this.#oscOutLogWorker = oscOutLogWorker;

        // Set up message handlers
        this.#setupWorkerHandlers();

        // Initialize workers with shared buffer
        await Promise.all([
            this.#initWorker(this.#oscInWorker, 'OSC IN'),
            this.#initWorker(this.#oscOutLogWorker, 'OSC OUT LOG'),
        ]);

        // Start polling workers
        this.#oscInWorker.postMessage({ type: 'start' });
        this.#oscOutLogWorker.postMessage({ type: 'start' });

        this.#initialized = true;
    }

    /**
     * Send an OSC message by framing it onto the IN ring (via an internal
     * main-thread channel). The audio thread classifies + schedules.
     */
    send(message) {
        if (!this.#initialized || this._disposed) {
            return false;
        }
        if (!this.#mainChannel) this.#mainChannel = this.createOscChannel({ sourceId: 0 });
        const ok = this.#mainChannel.send(message);
        if (ok) {
            this.#oscOutMessagesSent++;
            this.#oscOutBytesSent += message.length;
        }
        return ok;
    }

    /**
     * Create an OscChannel for direct worker-to-worklet communication
     *
     * Returns an OscChannel backed by the SharedArrayBuffer that can be
     * transferred to a Web Worker, allowing that worker to send OSC messages
     * directly to the AudioWorklet's ring buffer.
     *
     * Usage:
     *   const channel = transport.createOscChannel();
     *   myWorker.postMessage({ channel: channel.transferable }, channel.transferList);
     *
     * In worker:
     *   const channel = OscChannel.fromTransferable(event.data.channel);
     *   channel.send(oscBytes);
     *
     * @param {Object} [options]
     * @param {number} [options.sourceId] - Override sourceId (default: auto-assign)
     * @returns {OscChannel}
     */
    createOscChannel(options = {}) {
        if (!this.#initialized) {
            throw new Error('Transport not initialized');
        }

        // Use provided sourceId or auto-assign
        // sourceId 0 is reserved for main thread, 1+ for workers
        const sourceId = options.sourceId ?? this.#nextSourceId++;

        return OscChannel.createSAB({
            sharedBuffer: this.#sharedBuffer,
            arenaBase: this.#arenaBase,
            bufferConstants: this.#bufferConstants,
            controlIndices: this.#controlIndices,
            ingress: this.#ingress,
            sourceId,
        });
    }

    onReply(callback) {
        this.#onReplyCallback = callback;
    }

    onDebug(callback) {
        this.#onDebugCallback = callback;
    }

    onError(callback) {
        this.#onErrorCallback = callback;
    }

    onOscLog(callback) {
        this.#onOscLogCallback = callback;
    }

    /**
     * Handle OSC log entries from worklet
     * In SAB mode, Clockwork forwards these from the worklet port
     * @param {Array} entries - Array of {sourceId, oscData, timestamp}
     */
    handleOscLog(entries) {
        if (this.#onOscLogCallback) {
            this.#onOscLogCallback(entries);
        }
    }

    getMetrics() {
        return {
            oscOutMessagesSent: this.#oscOutMessagesSent,
            oscOutMessagesDropped: this.#oscOutMessagesDropped,
            oscOutBytesSent: this.#oscOutBytesSent,
        };
    }

    get ready() {
        return this.#initialized && !this._disposed;
    }

    // The readers stopped, and heard to have stopped, before they are let go: a reader that read on — parked in
    // Atomics.wait, where neither a message nor terminate() reaches it at once — would take what the next engine
    // says first (sab_worker_loop.js, the stop a parked thread sees). Told in memory and by message, its ring's
    // waiters woken, and waited for, within a bound a reader that is already gone reaches.
    dispose() {
        if (this._disposed) return Promise.resolve();
        this.#disposing ??= this.#stopReaders();
        return this.#disposing;
    }

    async #stopReaders() {
        const readers = [this.#oscInWorker, this.#oscOutLogWorker].filter(Boolean);
        const heard = readers.map((reader) => new Promise((resolve) => {
            const done = () => { clearTimeout(timer); reader.removeEventListener('message', onMessage); resolve(); };
            const onMessage = (event) => { if (event.data?.type === 'stopped') done(); };
            const timer = setTimeout(done, 500);
            reader.addEventListener('message', onMessage);
        }));
        Atomics.store(this.#stop, 0, 1);
        const cursors = this.#arenaBase + this.#bufferConstants.RING_CURSORS_START;
        Atomics.notify(this.#atomicView, (cursors + OUT_HEAD) / 4);
        Atomics.notify(this.#atomicView, (cursors + IN_HEAD) / 4);
        for (const reader of readers) reader.postMessage({ type: 'stop' });
        await Promise.all(heard);

        if (this.#oscInWorker && this.#oscInIsOurs) this.#oscInWorker.terminate();
        this.#oscInWorker = null;
        this.#oscOutLogWorker?.terminate();
        this.#oscOutLogWorker = null;

        this.#initialized = false;
        super.dispose();
    }

    // =========================================================================
    // Private Methods
    // =========================================================================

    #initializeViews() {
        this.#atomicView = new Int32Array(this.#sharedBuffer);
        this.#dataView = new DataView(this.#sharedBuffer);
        this.#uint8View = new Uint8Array(this.#sharedBuffer);

        this.#controlIndices = calculateInControlIndices(
            this.#arenaBase,
            this.#bufferConstants.RING_CURSORS_START
        );
        this.#ingress = engineRings(this.#sharedBuffer, this.#arenaBase, this.#bufferConstants).ingress;
    }

    #initWorker(worker, name, extraConfig = {}) {
        return new Promise((resolve, reject) => {
            const timeout = setTimeout(() => {
                reject(new Error(`${name} worker initialization timeout`));
            }, 5000);

            const handler = (event) => {
                if (event.data.type === 'error') {
                    // e.g. rings in a format it does not read: say so now rather than time out
                    clearTimeout(timeout);
                    worker.removeEventListener('message', handler);
                    reject(new Error(`${name} worker failed to initialize: ${event.data.error}`));
                } else if (event.data.type === 'initialized') {
                    clearTimeout(timeout);
                    worker.removeEventListener('message', handler);
                    resolve();
                }
            };

            worker.addEventListener('message', handler);
            worker.postMessage({
                type: 'init',
                stopBuffer: this.#stop.buffer,
                sharedBuffer: this.#sharedBuffer,
                arenaBase: this.#arenaBase,
                bufferConstants: this.#bufferConstants,
                ...extraConfig,
            });
        });
    }

    #setupWorkerHandlers() {
        // OSC IN worker - receives replies from the engine
        this.#oscInWorker.onmessage = (event) => {
            const data = event.data;
            if (data.type === 'messages') {
                if (this.#onReplyCallback) {
                    data.messages.forEach(msg => {
                        if (msg.oscData) {
                            this.#onReplyCallback(msg.oscData, msg.sequence, msg.timestamp);
                        }
                    });
                }
            } else if (data.type === 'error') {
                console.error('[SABTransport] OSC IN error:', data.error);
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'oscIn');
                }
            }
        };

        // OSC OUT LOG worker - receives logged OSC messages
        this.#oscOutLogWorker.onmessage = (event) => {
            const data = event.data;
            if (data.type === 'oscLog' && this.#onOscLogCallback) {
                this.#onOscLogCallback(data.entries);
            } else if (data.type === 'error') {
                console.error('[SABTransport] OSC OUT LOG error:', data.error);
                if (this.#onErrorCallback) {
                    this.#onErrorCallback(data.error, 'oscOutLog');
                }
            }
        };
    }

}
