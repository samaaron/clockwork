// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * Clockwork - hosts a guest DSP compiled to WebAssembly
 * Coordinates SharedArrayBuffer, WASM, AudioWorklet, and IO Workers
 */

import { createTransport, OscChannel } from "./lib/transport/index.js";
import { readTimetag, getCurrentNTPFromPerformance } from "./lib/osc_classifier.js";
import { clockworkSys } from "./lib/clockwork_sys.js";
import { HostFront } from "./lib/host_front.js";

export { OscChannel };
import { AssetLoader } from "./lib/asset_loader.js";
import { dspProfile, NO_DSP } from "./lib/dsp_profile.js";
import { ENGINE_PROCESS_COUNT, GUEST_METRICS_BASE } from "./lib/metrics_offsets.js";
import { EventEmitter } from "./lib/event_emitter.js";
import { MetricsReader } from "./lib/metrics_reader.js";
import { METRICS_SCHEMA } from "./lib/metrics_schema.js";
import { ClockworkClock } from "./lib/clockwork_clock.js";
import { AudioHealthMonitor } from "./lib/audio_health_monitor.js";
import { AudioCapture } from "./lib/audio_capture.js";
import * as oscFast from "./lib/osc_fast.js";
// Timeout waiting for the DSP profile's synced reply from the engine
const SYNC_TIMEOUT_MS = 10000;
// Timeout waiting for AudioWorklet initialization
const WORKLET_INIT_TIMEOUT_MS = 5000;
// Timeout waiting for the worklet's clearSched ack in purge(). A live worklet
// acks synchronously from its message handler, so this only elapses when the
// worklet is gone — at which point purge() resolves best-effort so recover()
// can fall back to reload() instead of hanging.
const PURGE_ACK_TIMEOUT_MS = 1000;
// Interval for metrics/tree snapshots in postMessage mode (ms)
const SNAPSHOT_INTERVAL_MS = 150;
import { MemoryLayout } from "./memory_layout.js";
import { addWorkletModule } from "./lib/worker_loader.js";

const HEX = [];
for (let i = 0; i < 256; i++) HEX[i] = i.toString(16).padStart(2, '0');

function formatUUID(bytes) {
  return HEX[bytes[0]] + HEX[bytes[1]] + HEX[bytes[2]] + HEX[bytes[3]] + '-' +
    HEX[bytes[4]] + HEX[bytes[5]] + '-' + HEX[bytes[6]] + HEX[bytes[7]] + '-' +
    HEX[bytes[8]] + HEX[bytes[9]] + '-' + HEX[bytes[10]] + HEX[bytes[11]] +
    HEX[bytes[12]] + HEX[bytes[13]] + HEX[bytes[14]] + HEX[bytes[15]];
}

function formatUUIDShort(bytes) {
  return '\u2026' + HEX[bytes[13]] + HEX[bytes[14]] + HEX[bytes[15]];
}

function formatOscArg(a, maxLen) {
  if (a && a.type === 'uuid' && a.value) return formatUUIDShort(a.value);
  if (a instanceof Uint8Array || a instanceof ArrayBuffer) return `<${a.byteLength || a.length} bytes>`;
  const str = JSON.stringify(a);
  return maxLen && str.length > maxLen ? str.slice(0, maxLen) + '...' : str;
}

function escapeHtml(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

function formatOscArgHtml(arg, address, argIndex) {
  if (arg && arg.type === 'uuid' && arg.value) {
    return `<span class="clockwork-osc-string" title="${formatUUID(arg.value)}">${formatUUIDShort(arg.value)}</span>`;
  }
  let value = arg, type = null;
  if (typeof arg === 'object' && arg !== null && arg.value !== undefined) {
    value = arg.value;
    type = arg.type;
  }
  if (type === 'b' || value instanceof Uint8Array || value instanceof ArrayBuffer) {
    const len = value.byteLength ?? value.length ?? '?';
    return `<span class="clockwork-osc-binary">&lt;${len} bytes&gt;</span>`;
  }
  const isFloat = type === 'f' || (type === null && typeof value === 'number' && !Number.isInteger(value));
  const isInt = type === 'i' || (type === null && Number.isInteger(value));
  if (isFloat) return `<span class="clockwork-osc-float">${parseFloat(value.toFixed(3))}</span>`;
  if (isInt) return `<span class="clockwork-osc-int">${value}</span>`;
  if (typeof value === 'string') return `<span class="clockwork-osc-string">${escapeHtml(JSON.stringify(value))}</span>`;
  return `<span class="clockwork-osc-string">${escapeHtml(value)}</span>`;
}

function formatOscLineHtml(msg, sequence, timestamp, initTime, sourceId) {
  const address = msg[0];
  const args = msg.slice(1);
  const relTime = initTime && timestamp ? (timestamp - initTime).toFixed(2) : '';
  let html = `<span class="clockwork-osc-seq">[${sequence}]</span>`;
  if (relTime) html += ` <span class="clockwork-osc-time">${relTime}</span>`;
  if (sourceId !== undefined) html += ` <span class="clockwork-osc-source">ch${sourceId}</span>`;
  html += ` <span class="clockwork-osc-address">${escapeHtml(address)}</span>`;
  if (args.length > 0) {
    const argsHtml = args.map((a, i) => formatOscArgHtml(a, address, i)).join(', ');
    html += ' ' + argsHtml;
  }
  return html;
}

function formatBundleHtml(decoded, sequence, timestamp, initTime, sourceId) {
  if (!decoded.packets) return formatOscLineHtml(decoded, sequence, timestamp, initTime, sourceId);
  if (decoded.packets.length === 1) return formatOscLineHtml(decoded.packets[0], sequence, timestamp, initTime, sourceId);
  const relTime = initTime && timestamp ? (timestamp - initTime).toFixed(2) : '';
  let html = `<span class="clockwork-osc-seq">[${sequence}]</span>`;
  if (relTime) html += ` <span class="clockwork-osc-time">${relTime}</span>`;
  if (sourceId !== undefined) html += ` <span class="clockwork-osc-source">ch${sourceId}</span>`;
  html += ` <span class="clockwork-osc-bundle">Bundle (${decoded.packets.length})</span>`;
  for (const pkt of decoded.packets) {
    const addr = pkt[0];
    const pktArgs = pkt.slice(1);
    html += `<br><span class="clockwork-osc-address">${escapeHtml(addr)}</span>`;
    if (pktArgs.length > 0) {
      html += ' ' + pktArgs.map((a, i) => formatOscArgHtml(a, addr, i)).join(', ');
    }
  }
  return html;
}

export class Clockwork {
  // Expose OSC utilities as static methods (uses plain args, not typed {type, value} format)
  static osc = {
    encodeMessage: (address, args) => oscFast.copyEncoded(oscFast.encodeMessage(address, args)),
    encodeBundle: (timeTag, packets) => oscFast.copyEncoded(oscFast.encodeBundle(timeTag, packets)),
    decode: (data) => oscFast.decodePacket(data),
    encodeSingleBundle: (timeTag, address, args) =>
      oscFast.copyEncoded(oscFast.encodeSingleBundle(timeTag, address, args)),
    readTimetag: (bundleData) => readTimetag(bundleData),
    ntpNow: () => getCurrentNTPFromPerformance(),
    NTP_EPOCH_OFFSET: oscFast.NTP_EPOCH_OFFSET,
  };

  /**
   * Schema describing every available metric — each key maps to
   * { offset, type, unit, description } for the merged Uint32Array — plus a
   * `layout` panel structure for rendering a metrics UI.
   */
  static getMetricsSchema() {
    return METRICS_SCHEMA;
  }

  /**
   * Schema describing the node tree structure.
   */


  #audioContext;
  #workletNode;
  #node = null;
  #osc;
  #wasmMemory;
  #syncListeners;
  #fetchRetryConfig;
  #assetLoader;
  #guestWriteSeq = 0;
  #guestReadSeq = 0;
  #guestGrowSeq = 0;
  #exportCallSeq = 0;
  #initialized;
  #initializing;
  #initPromise;
  #capabilities;
  #version;
  #dsp = NO_DSP;
  #config;

  #eventEmitter;
  #metricsReader;
  #clock;
  #audioCapture;
  #audioHealthMonitor;

  #oscChannel;

  // The host's front (js/lib/host_front.js): the far end of clockwork's
  // chain on the web. Every /clockwork/ verb the worklet's audio thread does
  // not answer itself comes back here to be answered — MIDI and gamepad,
  // when enabled — or refused by name.
  #front = null;

  // Node ID counter for PM mode (SAB mode uses shared memory).
  // Starts at 1000: 0 is the root group and
  // 1–999 are left free for the client to assign by hand.
  #nodeIdCounter = 1000;

  #previousAudioContextState = null;

  #cachedWasmBytes = null;

  #snapshotsSent = 0;

  #earlyDebugMessages = [];
  #debugRawHandler = null;

  // Cached TypedArray views for scope slots (lazily initialized, avoids per-frame allocations)
  #scopeViews = null;

  /* Guest options are the product's to validate — clockwork enforces its own
   * rules (128-frame blocks and the rest) through its own `audio` options.
   */

  /*
   * Build the memory config.
   *
   * CLOCKWORK NEVER LOOKS INSIDE GUEST CONFIG to size anything. A guest that
   * wants more memory says so the way any caller does, through
   * `memory: { guestMemorySize }`.
   */
  #buildMemoryConfig(overrides) {
    const mem = overrides ? { ...MemoryLayout, ...overrides } : { ...MemoryLayout };
    // Re-derive computed values, since spreading an object loses its getters.
    //
    // This has to stay the same sum as MemoryLayout.guestMemoryOffset. It did
    // not when memArenaSize was added: the region moved up by 32MB and this
    // copy of the formula did not, so every guest write was checked against a
    // base 32MB below the real one and refused. ?? rather than ||, so a
    // deliberate 0 for any part is not silently replaced by the default.
    // The INBOX IS LAST and everything else is fixed, because only the top
    // region can grow in place — see MemoryLayout.inboxOffset, which says why
    // bulk in is the thing that grows and the arena is not.
    mem.outboxOffset      = (mem.wasmHeapSize ?? MemoryLayout.wasmHeapSize)
                          + (mem.ringBufferReserved ?? MemoryLayout.ringBufferReserved)
                          + (mem.memArenaSize ?? MemoryLayout.memArenaSize);
    mem.guestMemoryOffset = mem.outboxOffset
                          + (mem.outboxSize ?? MemoryLayout.outboxSize);
    mem.inboxOffset       = mem.guestMemoryOffset
                          + (mem.guestMemorySize ?? MemoryLayout.guestMemorySize);
    mem.totalMemory    = mem.inboxOffset + mem.inboxSize;
    mem.maxTotalMemory = mem.inboxOffset + Math.max(mem.maxInboxSize,
                                                    mem.inboxSize);
    return mem;
  }

  constructor(options = {}) {
    this.#initialized = false;
    this.#initializing = false;
    this.#initPromise = null;
    this.#capabilities = {};
    this.#version = null;

    this.#eventEmitter = new EventEmitter();
    this.#audioCapture = new AudioCapture({});

    this.#audioContext = null;
    this.#workletNode = null;
    this.#osc = null;

    // Configuration
    // baseURL is a convenience shorthand when all assets are co-located
    // coreBaseURL is for the WASM + AudioWorklet assets
    // workerBaseURL is for the workers
    const baseURL = options.baseURL || null;
    const coreBaseURL = options.coreBaseURL || baseURL;
    const workerBaseURL = options.workerBaseURL || (baseURL ? `${baseURL}workers/` : null);
    const wasmBaseURL = options.wasmBaseURL || (coreBaseURL ? `${coreBaseURL}wasm/` : null);

    if (!workerBaseURL || !wasmBaseURL) {
      throw new Error(
        `Clockwork requires explicit URL configuration.\n\n` +
        `For CDN usage:\n` +
        `  import { Clockwork } from 'https://unpkg.com/clockwork@VERSION/dist/clockwork.js';\n` +
        `  new Clockwork({\n` +
        `    baseURL: 'https://unpkg.com/clockwork@VERSION/dist/',\n` +
        `  })\n\n` +
        `For local usage:\n` +
        `  new Clockwork({ baseURL: '/path/to/clockwork/dist/' })\n\n` +
        `See: docs/ in the clockwork repository`
      );
    }

    /* Guest config is OPAQUE. Clockwork asks for what it needs in its own
     * words, under `audio`: how many channels to open and how many to read.
     */
    const guestOptions = { ...options.guestOptions };
    const mode = options.mode || 'postMessage';

    // The DSP's vocabulary — see js/lib/dsp_profile.js. Clockwork holds
    // no opinion about which engine is underneath; without a profile it
    // simply does not cache definitions and cannot sync.
    this.#dsp = dspProfile(options.dsp);
    // After the profile is resolved, because the reader needs what this guest
    // says it exposes. None is a normal answer.
    this.#metricsReader = new MetricsReader({
      mode: options.mode || 'postMessage',
      guestMetrics: this.#dsp?.metrics || {},
    });

    this.#config = {
      mode: mode,
      snapshotIntervalMs: options.snapshotIntervalMs ?? SNAPSHOT_INTERVAL_MS,
      wasmBytes: options.wasmBytes ?? null,
      wasmUrl: options.wasmUrl || wasmBaseURL + "clockwork-engine.wasm",
      wasmBaseURL: wasmBaseURL,
      workletUrl: options.workletUrl || (coreBaseURL ? `${coreBaseURL}workers/clockwork_audio_worklet.js` : workerBaseURL + "clockwork_audio_worklet.js"),
      workerBaseURL: workerBaseURL,
      audioContext: options.audioContext || null,
      autoConnect: options.autoConnect !== false,
      audioContextOptions: {
        latencyHint: "interactive",
        sampleRate: 48000,
        ...options.audioContextOptions,
      },
      memory: this.#buildMemoryConfig(options.memory),
      guestOptions,
      audio: {
        outputChannels: options.audio?.outputChannels ?? 2,
        inputChannels: options.audio?.inputChannels ?? 0,
      },
      bypassLookaheadMs: options.bypassLookaheadMs ?? 500,
      activityEvent: {
        maxLineLength: options.activityEvent?.maxLineLength ?? 200,
        engineMaxLineLength: options.activityEvent?.engineMaxLineLength ?? null,
        oscInMaxLineLength: options.activityEvent?.oscInMaxLineLength ?? null,
        oscOutMaxLineLength: options.activityEvent?.oscOutMaxLineLength ?? null,
      },
      debug: options.debug ?? false,
      // The main-thread subsystems (js/lib/host_front.js). Off by default:
      // Web MIDI asks the user's permission, and a page that never sends a
      // /clockwork/midi/ verb should not. `true`, or an object of manager
      // options (a test injects its fake device access there).
      midi: options.midi ?? false,
      gamepad: options.gamepad ?? false,
      debugEngine: options.debugEngine ?? false,
      debugOscIn: options.debugOscIn ?? false,
      debugOscOut: options.debugOscOut ?? false,
      bufferGrowIncrement: options.bufferGrowIncrement ?? (32 * 1024 * 1024),
    };

    // The growth ceiling, resolved once: caller's option → layout default →
    // the committed size, which means "do not grow". It belongs to the INBOX,
    // which is the region that grows.
    this.#config.effectiveMaxInbox = options.maxInbox
        || this.#config.memory.maxInboxSize
        || this.#config.memory.inboxSize;


    this.#fetchRetryConfig = {
      maxRetries: options.fetchMaxRetries ?? 3,
      baseDelay: options.fetchRetryDelay ?? 1000,
    };

    this.#assetLoader = new AssetLoader({
      onLoadingEvent: (event, data) => this.#eventEmitter.emit(event, data),
      maxRetries: this.#fetchRetryConfig.maxRetries,
      baseDelay: this.#fetchRetryConfig.baseDelay,
      skipHeadRequests: options.skipHeadRequests ?? false,
    });

    this.bootStats = {
      initStartTime: null,
      initDuration: null,
    };
  }

  // ============================================================================
  // PUBLIC GETTERS
  // ============================================================================

  get initialized() { return this.#initialized; }
  get initializing() { return this.#initializing; }

  // Mirrors C++ ClockworkEngine::isRunning(). Same value as the
  // `initialized` getter, exposed as a method to match the C++ API shape.
  isRunning() { return this.#initialized; }

  // Mirrors C++ ClockworkEngine::engineState(). Returns 'stopped',
  // 'booting', or 'running'. The C++ enum also has 'restarting' and 'error'
  // values that JS does not currently distinguish — calls to recover/resume
  // do not surface a separate 'restarting' state from JS.
  getEngineState() {
    if (this.#initializing) return 'booting';
    if (this.#initialized) return 'running';
    return 'stopped';
  }
  get audioContext() { return this.#audioContext; }
  // ClockworkClock — engine-wide session-state + time authority.
  // Available after init() resolves. Read-only reference.
  get clock() { return this.#clock; }
  get mode() { return this.#config.mode; }
  get bufferConstants() { return this.#metricsReader.bufferConstants; }
  get ringBufferBase() { return this.#metricsReader.ringBufferBase; }
  get sharedBuffer() { return this.#metricsReader.sharedBuffer; }
  /*
   * The guest's memory object, for a client that maps its own region.
   *
   * `sharedBuffer` is not a substitute: growth leaves earlier ArrayBuffer
   * references valid but still sized to the OLD length, so a client holding
   * one cannot see memory it has just grown into. The Memory object always
   * yields the current buffer. Null in postMessage mode, where the client
   * has no heap of its own and must go through writeInbox.
   */
  get wasmMemory() { return this.#wasmMemory; }
  /*
   * Fetching, with the retry and progress policy clockwork already applies
   * to its own artifacts. A client fetching a definition or sample material
   * wants exactly that behaviour and should not build a second one beside it.
   */
  get assetLoader() { return this.#assetLoader; }
  get node() { return this.#node; }
  get osc() { return this.#osc; }
  /*
   * The main-thread subsystems, when enabled (`midi: true`, `gamepad: true`):
   * the MidiManager and GamepadManager the front answers /clockwork/midi/ and
   * /clockwork/gamepad/ with. Null when not enabled, and before init. A
   * client that wants the structured fast path (onMessage) rather than OSC
   * bytes reaches them here.
   */
  get midi() { return this.#front?.midi ?? null; }
  get gamepad() { return this.#front?.gamepad ?? null; }
  // Why `midi` / `gamepad` is null although it was asked for: the error its
  // init threw (a platform with no Web MIDI backend, a missing wasm). Null
  // when the subsystem is up or was never requested.
  get midiError() { return this.#front?.midiError ?? null; }
  get gamepadError() { return this.#front?.gamepadError ?? null; }

  /**
   * NTP time (seconds since 1900) when the AudioContext started.
   *
   * @deprecated Use `sonic.clock.getNTPStartTime()` for the same value, or
   *   `sonic.clock.now()` to get the current audio-thread NTP. This getter
   *   stays for backward compatibility with older callers that did
   *   `event.timestamp - sonic.initTime`.
   */
  get initTime() { return this.#clock?.getNTPStartTime() ?? 0; }

  // ============================================================================
  // EVENT EMITTER DELEGATION
  // ============================================================================

  /*
   * Raise an event on clockwork's own emitter.
   *
   * For a product built on clockwork: its events should reach listeners
   * through the same `on()` its clients already use, rather than a second
   * emitter beside it that callers have to know about separately.
   */
  emit(event, payload) { return this.#eventEmitter.emit(event, payload); }
  on(event, callback) { return this.#eventEmitter.on(event, callback); }
  off(event, callback) { this.#eventEmitter.off(event, callback); return this; }
  once(event, callback) { return this.#eventEmitter.once(event, callback); }
  removeAllListeners(event) { this.#eventEmitter.removeAllListeners(event); return this; }

  // ============================================================================
  // INITIALIZATION
  // ============================================================================

  async init() {
    if (this.#initialized) return;
    if (this.#initPromise) return this.#initPromise;

    this.#initPromise = this.#doInit();
    return this.#initPromise;
  }

  async #doInit() {
    this.#initializing = true;
    this.bootStats.initStartTime = performance.now();

    try {
      this.#setAndValidateCapabilities();
      this.#initializeMemory();
      this.#initializeAudioContext();
      const wasmBytes = await this.#loadWasm();
      await this.#initializeAudioWorklet(wasmBytes);
      await this.#initializeOSC();
      await this.#initializeFront();
      await this.#finishInitialization();
    } catch (error) {
      this.#initializing = false;
      this.#initPromise = null;
      console.error("[Clockwork] Initialization failed:", error);
      this.#eventEmitter.emit('error', error);
      throw error;
    }
  }

  // ============================================================================
  // METRICS API
  // ============================================================================

  getMetrics() {
    return this.#gatherMetrics();
  }

  /**
   * Get metrics as a flat Uint32Array for zero-allocation reading.
   * Returns the same array reference every call — values are updated in-place.
   * Slots 0-49: SAB/snapshot metrics (slot 50 is the C++ struct's alignment
   * padding, reused by the first context metric), 50-68: main-thread context
   * metrics, 69+: metrics the guest declared in its DSP profile.
   * Use getMetricsSchema().metrics for offset mappings.
   * @returns {Uint32Array}
   */
  getMetricsArray() {
    this.#updateMergedArray();
    return this.#metricsReader.getMergedArray();
  }

  /**
   * Get a diagnostic snapshot containing metrics and memory info.
   * Useful for debugging timing issues, capturing state for bug reports, etc.
   * @returns {Object} Snapshot with timestamp, metrics (with descriptions) and memory info
   */
  getSnapshot() {
    const rawMetrics = this.#gatherMetrics();
    const schemaMetrics = Clockwork.getMetricsSchema()?.metrics || {};

    const metricsWithDescriptions = {};
    for (const [key, value] of Object.entries(rawMetrics)) {
      const def = schemaMetrics[key];
      if (def?.description) {
        metricsWithDescriptions[key] = {
          value,
          description: def.description,
        };
      } else {
        metricsWithDescriptions[key] = { value };
      }
    }

    // Get JS heap memory info (Chrome only, non-standard API)
    let memory = null;
    if (typeof performance !== 'undefined' && performance.memory) {
      memory = {
        usedJSHeapSize: performance.memory.usedJSHeapSize,
        totalJSHeapSize: performance.memory.totalJSHeapSize,
        jsHeapSizeLimit: performance.memory.jsHeapSizeLimit,
      };
    }

    return {
      timestamp: new Date().toISOString(),
      metrics: metricsWithDescriptions,
      memory,
    };
  }

  /**
   * Get a comprehensive system performance report.
   *
   * Includes hardware info, audio configuration, Chrome playbackStats (if available),
   * a cross-browser audio health percentage, and a human-readable health assessment.
   * Useful for diagnosing audio crackling on constrained hardware.
   * @returns {Object} SystemReport
   */
  getSystemReport() {
    this.#ensureInitialized('get system report');

    const metrics = this.#gatherMetrics();
    const issues = [];

    const system = {
      userAgent: navigator.userAgent,
      hardwareConcurrency: navigator.hardwareConcurrency ?? null,
      deviceMemory: navigator.deviceMemory ?? null,
      platform: navigator.platform,
    };

    const audio = {
      sampleRate: this.#audioContext.sampleRate,
      baseLatency: this.#audioContext.baseLatency ?? null,
      outputLatency: this.#audioContext.outputLatency ?? null,
      state: this.#audioContext.state,
      channelCount: this.#config.audio.outputChannels,
    };

    // Playback stats (Chrome 146+)
    const pbStats = this.#capabilities.playbackStats ? this.#audioContext.playbackStats : null;
    const playbackStats = pbStats ? {
      glitchCount: pbStats.fallbackFramesEvents,
      glitchDurationS: pbStats.fallbackFramesDuration,
      totalDurationS: pbStats.totalFramesDuration,
      averageLatencyS: pbStats.averageLatency,
      maximumLatencyS: pbStats.maximumLatency,
    } : null;

    const healthPct = this.#audioHealthMonitor?.getHealth()?.healthPct ?? 100;

    if (healthPct < 95) {
      issues.push({
        severity: healthPct < 80 ? 'critical' : 'warning',
        message: `Audio health at ${healthPct}% — audio thread may be falling behind`,
      });
    }
    if (metrics.engineSchedulerLates > 0) {
      issues.push({
        severity: 'warning',
        message: `${metrics.engineSchedulerLates} late bundles in engine scheduler`,
      });
    }
    if (metrics.engineWasmErrors > 0) {
      issues.push({
        severity: 'error',
        message: `${metrics.engineWasmErrors} WASM errors detected`,
      });
    }
    if (pbStats?.fallbackFramesEvents > 0) {
      issues.push({
        severity: 'warning',
        message: `${pbStats.fallbackFramesEvents} audio glitch events (${(pbStats.fallbackFramesDuration * 1000).toFixed(1)}ms total silence)`,
      });
    }
    if (metrics.driftOffsetMs !== undefined && Math.abs(metrics.driftOffsetMs) > 10) {
      issues.push({
        severity: 'warning',
        message: `Clock drift: ${metrics.driftOffsetMs}ms between AudioContext and wall clock`,
      });
    }

    const summary = issues.length === 0
      ? `Audio health: ${healthPct}% — no issues detected`
      : `Audio health: ${healthPct}% — ${issues.length} issue(s): ${issues.map(i => i.message).join('; ')}`;

    return {
      timestamp: new Date().toISOString(),
      system,
      audio,
      playbackStats,
      engine: {
        mode: this.mode,
        version: this.#version,
        bootTimeMs: this.bootStats.initDuration,
      },
      health: {
        audioHealthPct: healthPct,
        issues,
        summary,
      },
      metrics,
    };
  }

  // ============================================================================
  // TIMING API
  // ============================================================================

  /**
   * Set clock offset for multi-system sync (e.g., Ableton Link, NTP server).
   * This shifts all scheduled bundle execution times by the specified offset.
   * Positive values mean the shared/server clock is ahead of local time —
   * bundles with shared-clock timetags are shifted earlier to compensate.
   * @param {number} offsetS - Offset in seconds
   */
  setClockOffset(offsetS) {
    this.#ensureInitialized('set clock offset');
    this.#clock?.setClockOffset(offsetS);
  }

  // ============================================================================
  // RECOVERY API
  // ============================================================================

  /**
   * Smart recovery - tries quick resume first, falls back to full reload.
   * Use this when you don't know if a quick resume will work.
   * @returns {Promise<boolean>} true if audio is running after recovery
   */
  async recover() {
    if (!this.#initialized) return false;

    if (__DEV__) console.log('[Dbg-Clockwork] Attempting recovery...');

    if (await this.resume()) {
      if (__DEV__) console.log('[Dbg-Clockwork] Quick resume succeeded');
      return true;
    }

    if (__DEV__) console.log('[Dbg-Clockwork] Resume failed, doing full reload');
    return await this.reload();
  }

  /**
   * Quick resume - just resumes AudioContext and resyncs timing.
   * Memory and node tree are preserved. Does NOT emit 'setup' event.
   * Use when you know the worklet is still running (e.g., tab was just backgrounded briefly).
   * @returns {Promise<boolean>} true if worklet is running after resume
   */
  async resume() {
    if (!this.#initialized || !this.#audioContext) return false;

    // Clear stale messages before resuming so scheduled events from
    // before the suspend (e.g. fade-outs) don't interfere with new work
    await this.purge();

    try {
      await this.#audioContext.resume();
    } catch (e) {
    }

    this.#clock?.startDriftTimer();

    const count1 = this.#readProcessCount();
    if (count1 === null) {
      // No metrics available yet — check AudioContext state instead
      const isRunning = this.#audioContext.state === 'running';
      if (isRunning) {
        this.#clock?.resync();
        this.#eventEmitter.emit('resumed');
      }
      return isRunning;
    }

    await new Promise(resolve => setTimeout(resolve, 200));
    const count2 = this.#readProcessCount();

    const isRunning = count2 !== null && count2 > count1;
    if (isRunning) {
      this.#clock?.resync();
      this.#eventEmitter.emit('resumed');
    }

    return isRunning;
  }

  /**
   * Suspend the AudioContext and stop the drift timer.
   * The worklet remains loaded but processing stops.
   * The audiocontext statechange listener handles emitting events.
   */
  async suspend() {
    if (!this.#initialized) return;
    this.#clock?.stopDriftTimer();
    try {
      await this.#audioContext?.suspend();
    } catch (e) {
      // Suspend may fail
    }
  }

  /**
   * Full reload - destroys and recreates worklet/WASM, then calls
   * restoreClientState() to put back whatever the product was holding.
   * Emits 'setup' event so you can rebuild groups, FX chains, bus routing.
   * Use when the worklet was killed (e.g., long background, browser reclaimed memory).
   * @returns {Promise<boolean>} true if reload succeeded
   */
  async reload() {
    if (!this.#initialized) return false;

    this.#eventEmitter.emit('reload:start');

    await this.#partialShutdown();
    await this.#partialInit();

    // Everything the engine was holding, put back by the only party that
    // knows what it was.
    //
    // Clockwork used to restore one category itself — definitions — by
    // watching for the verb that carried them, reading a name out of each blob
    // through the profile, and replaying the lot here. Two categories, two
    // mechanisms, and clockwork's one worked only for the guest that had
    // declared its vocabulary. Now there is one, and it covers definitions,
    // samples and whatever else a guest invents, because the product re-sends
    // rather than clockwork replaying.
    //
    // BEFORE the sync below, so one barrier covers all of it.
    try {
      await this.restoreClientState();
    } catch (e) {
      console.error("[Clockwork] client state did not survive the reload:", e);
    }

    // Verified, not assumed: the first message or two after a reload can
    // still go missing (observed on the reused-SAB path even with the ring
    // epoch, engine-pumping wait and writer-lock fixes — the pipe is
    // reliable from roughly half a second in). Recovery code retries with
    // short timeouts instead of betting ten seconds on the first attempt.
    await this.#syncWithRetry();

    this.#eventEmitter.emit('reload:complete', { success: true });
    return true;
  }

  async #syncWithRetry(attempts = 6, timeoutMs = 1500) {
    let lastErr = null;
    for (let i = 0; i < attempts; i++) {
      try {
        await this.sync(undefined, timeoutMs);
        return;
      } catch (e) {
        lastErr = e;
      }
    }
    throw lastErr ?? new Error("sync failed after reload");
  }

  /**
   * Zero the ring control words in a reused SAB, in the one window where it
   * is safe: after partialShutdown (workers terminated, worklet gone) and
   * before anything new attaches.
   *
   * A first boot gets this free (a fresh SAB is zeros); reload keeps the SAB,
   * so the old session's cursors survive. The engine's own epoch reset runs on
   * the audio thread's first process(), but the new OSC workers attach before
   * that — they would drink the stale ring and starve at a read index the
   * reborn writer won't reach for several messages.
   */
  #resetRingEpoch() {
    if (this.#config.mode !== 'sab' || !this.#wasmMemory) return;
    const bc = this.#metricsReader?.bufferConstants;
    const ringBufferBase = this.#metricsReader?.ringBufferBase;
    if (!bc || ringBufferBase == null || bc.CONTROL_START == null) return;
    const view = new Int32Array(this.#wasmMemory.buffer, ringBufferBase + bc.CONTROL_START, 12);
    // Heads, tails, sequences, the JS-side log tail — and the IN writer
    // lock. The engine's own epoch reset deliberately leaves that lock
    // alone, because on ITS path a live producer may be holding it. Here
    // the opposite invariant holds: partialShutdown has terminated every
    // worker and the worklet, so anything that held the lock is gone and a
    // surviving 1 is stale by construction — left in place, every write
    // after reload spins the lock's 10-second deadlock path and the
    // restore-phase traffic dies in exactly the /clockwork/synced-timeout shape.
    // NOT status_flags: the engine owns its lifecycle word.
    for (const idx of [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11]) {
      Atomics.store(view, idx, 0);
    }
  }

  async #awaitEngineProcessing(timeoutMs = 5000) {
    const view = this.#metricsReader?.getMetricsView?.() ?? null;
    const readCount = () => {
      if (view) return Atomics.load(view, ENGINE_PROCESS_COUNT);
      // postMessage mode: snapshots carry the counter with some lag, which
      // is fine — the wait only needs "the engine is demonstrably running".
      const snap = this.#metricsReader?.getSnapshotBuffer?.();
      return snap ? new Uint32Array(snap, 0, ENGINE_PROCESS_COUNT + 1)[ENGINE_PROCESS_COUNT] : null;
    };
    const t0 = performance.now();
    let last = null;
    let rises = 0;
    while (performance.now() - t0 < timeoutMs) {
      const n = readCount();
      if (n != null && last != null && n > last) {
        rises += 1;
        if (rises >= 2) return true;
      }
      if (n != null) last = n;
      await new Promise((r) => setTimeout(r, 25));
    }
    console.warn("[Clockwork] Engine did not start processing within", timeoutMs, "ms");
    return false;
  }

  async #partialShutdown() {
    this.#clock?.stopDriftTimer();
    this.#syncListeners?.clear();
    this.#syncListeners = null;

    if (this.#osc) {
      this.#osc.dispose();
      this.#osc = null;
    }
    // Stop forwarding worklet debug batches — the transport is gone. A live
    // handler here would deref the null #osc when a late batch arrives.
    this.#debugRawHandler = null;
    this.#front?.dispose();
    this.#front = null;

    if (this.#workletNode) {
      this.#workletNode.disconnect();
      this.#workletNode = null;
    }

    if (this.#audioContext) {
      await this.#audioContext.close();
      this.#audioContext = null;
    }

    this.#initialized = false;
    this.#scopeViews = null;
    this.#initPromise = null;
    this.#oscChannel = null;
    this.#clock?.reset();
    this.#audioHealthMonitor?.reset();
  }

  async #partialInit() {
    this.#initializing = true;
    this.bootStats.initStartTime = performance.now();

    try {
      this.#resetRingEpoch();
      this.#initializeAudioContext();
      const wasmBytes = await this.#loadWasm();
      await this.#initializeAudioWorklet(wasmBytes);
      await this.#initializeOSC();
      await this.#initializeFront();
      // The reborn engine runs its ring-epoch reset on the audio thread's
      // first process(), and anything already sitting in the IN ring at that
      // moment is discarded as belonging to the old epoch. First boot never
      // races this — the SAB is fresh and init's callers only send after
      // 'ready'. A reload reuses the SAB, and the definition/buffer restores
      // below would otherwise be sent straight into the discard window —
      // nondeterministically, which is the worst kind of sometimes.
      // process_count is zeroed by that same init block and then counts
      // every block, so two increasing reads prove the reset has run and
      // the engine is pumping.
      await this.#awaitEngineProcessing();
      await this.#finishInitialization();
    } catch (error) {
      this.#initializing = false;
      this.#initPromise = null;
      console.error("[Clockwork] Partial init failed:", error);
      this.#eventEmitter.emit('error', error);
      throw error;
    }
  }

  // ============================================================================
  // THE GUEST'S PUBLISH WINDOW
  // ============================================================================

  /**
   * Where the guest publishes, and how to reach it in either mode.
   *
   * Clockwork does not know what is in there — a node tree, a meter table, a
   * grid of anything. It reserves the region, copies it out beside the metrics
   * on change, and hands a consumer the bytes. Whoever knows the guest's
   * layout casts them.
   *
   * @returns {{buffer: ArrayBuffer|SharedArrayBuffer, offset: number, size: number}|null}
   */
  readWindow() {
    if (!this.#initialized) return null;
    const bc = this.#metricsReader.bufferConstants;
    if (!bc) return null;

    if (this.#config.mode === 'postMessage') {
      // The snapshot is METRICS followed by the window, copied as one unit,
      // so the window starts where the metrics end.
      const snapshot = this.#metricsReader.getSnapshotBuffer();
      if (!snapshot) return null;
      return { buffer: snapshot, offset: bc.METRICS_SIZE, size: bc.SHM_WINDOW_SIZE };
    }

    const sab = this.#metricsReader.sharedBuffer;
    if (!sab) return null;
    return {
      buffer: sab,
      offset: this.#metricsReader.ringBufferBase + bc.SHM_WINDOW_START,
      size:   bc.SHM_WINDOW_SIZE,
    };
  }

  // ============================================================================
  // SCOPE API
  // ============================================================================

  /** @returns {object} Cached TypedArray views for a scope stream slot (lazily created) */
  #getScopeSlotViews(scopeNum) {
    if (!this.#scopeViews) {
      this.#scopeViews = new Array(this.#metricsReader.bufferConstants.SHM_SCOPE_SLOT_COUNT);
    }
    let views = this.#scopeViews[scopeNum];
    if (views) return views;

    const bc = this.#metricsReader.bufferConstants;
    const sab = this.#metricsReader.sharedBuffer;
    const base = this.#metricsReader.ringBufferBase;
    // One index space: the guest's slots in the scope region, then the
    // engine's track taps in the clockwork block (docs/ARENA.md).
    const slotOffset = scopeNum < bc.SHM_SCOPE_MAX_SCOPES
      ? base + bc.SHM_SCOPE_START + bc.SHM_SCOPE_HEADER_SIZE + scopeNum * bc.SHM_SCOPE_SLOT_SIZE
      : base + bc.SHM_TRACK_TAPS_START + (scopeNum - bc.SHM_SCOPE_MAX_SCOPES) * bc.SHM_SCOPE_SLOT_SIZE;
    const ringFrames = bc.SHM_SCOPE_RING_FRAMES;

    // shm_scope_stream layout (shm_scope_stream.hpp): state u32, channels u32,
    // capacity u32, pad u32, write_position u64, base_engine_frames u64, then
    // an interleaved float ring of ringFrames * channels.
    views = {
      meta: new Uint32Array(sab, slotOffset, 4),
      cursor: new BigUint64Array(sab, slotOffset + 16, 2), // [write_position, base_engine_frames]
      data: new Float32Array(sab, slotOffset + bc.SHM_SCOPE_SLOT_HEADER_SIZE,
                             ringFrames * bc.SHM_SCOPE_CHANNELS),
      ringFrames,
    };
    this.#scopeViews[scopeNum] = views;
    return views;
  }

  /**
   * Get the newest `frames` frames of a scope stream.
   *
   * The stream is a lossless interleaved ring with a monotonic write cursor
   * (see docs/PORTS.md); this copies out the window ending at
   * the current cursor. SAB mode only for now.
   *
   * @param {number} scopeNum - Scope slot index (0 to maxScopes-1)
   * @param {number} [frames] - Window length; defaults to 1024
   * @returns {{ frames: number, channels: number, writePosition: bigint, interleaved: Float32Array }|null}
   */
  getScope(scopeNum, frames = 1024) {
    if (!this.#initialized) return null;

    const bc = this.#metricsReader.bufferConstants;
    if (!bc || bc.SHM_SCOPE_START == null || bc.SHM_SCOPE_SLOT_COUNT == null) return null;
    if (scopeNum < 0 || scopeNum >= bc.SHM_SCOPE_SLOT_COUNT) return null;

    // TODO: PM mode — read from latest heartbeat snapshot
    if (!this.#metricsReader.sharedBuffer) return null;

    const views = this.#getScopeSlotViews(scopeNum);
    if (Atomics.load(views.meta, 0) !== 1) return null;  // free/inactive

    // Untrusted runtime value: clamp so a corrupt slot can't index outside
    // the ring views.
    const channels = Math.min(Math.max(views.meta[1], 1), bc.SHM_SCOPE_CHANNELS);
    const cap = views.ringFrames;
    const writer = Atomics.load(views.cursor, 0);
    if (writer === 0n) return null;

    const want = Math.min(frames, cap);
    const end = writer;
    let start = end > BigInt(want) ? end - BigInt(want) : 0n;
    // Stay clear of the region the writer may currently be overwriting.
    // Same margin formula as the native copy_window: the writer can append
    // several blocks back-to-back per hardware callback, so stay
    // SHM_SCOPE_READ_MARGIN_FRAMES (2048, clamped to cap/4 for small rings)
    // behind the ring's oldest edge — keep in step with shm_scope_stream.hpp.
    const margin = BigInt(Math.min(cap >> 2, 2048));
    const oldest = end > BigInt(cap) ? end - BigInt(cap) + margin : 0n;
    if (start < oldest) start = oldest;

    const real = Number(end - start);
    const out = new Float32Array(want * channels); // zero-filled lead-in
    const fill = want - real;
    let at = Number(start % BigInt(cap));
    for (let i = 0; i < real; i++) {
      for (let c = 0; c < channels; c++) {
        out[(fill + i) * channels + c] = views.data[at * channels + c];
      }
      at = (at + 1) % cap;
    }

    return { frames: want, channels, writePosition: writer, interleaved: out };
  }

  /**
   * Get all active scope slots.
   * @returns {Array<{ index: number, channels: number }>}
   */
  getScopes() {
    if (!this.#initialized) return [];

    const bc = this.#metricsReader.bufferConstants;
    if (!bc || bc.SHM_SCOPE_START == null || bc.SHM_SCOPE_SLOT_COUNT == null) return [];
    if (!this.#metricsReader.sharedBuffer) return [];

    const scopes = [];
    for (let i = 0; i < bc.SHM_SCOPE_SLOT_COUNT; i++) {
      const views = this.#getScopeSlotViews(i);
      if (views.meta[0] !== 0) {
        scopes.push({ index: i, channels: views.meta[1] });
      }
    }
    return scopes;
  }

  /**
   * Get scope schema (capacity, ring frames per slot, channels).
   * @returns {{ maxScopes: number, ringFrames: number, channels: number }|null}
   */
  static getScopeSchema() {
    // Compile-time defaults — matches shared_memory.h SCOPE_ constants.
    // A proper implementation would read from the WASM buffer layout.
    return {
      maxScopes: 32,
      ringFrames: 16384,
      channels: 2,
    };
  }

  // ============================================================================
  // AUDIO CAPTURE API
  //
  // The audio taps are clockwork's and flow from boot: the OUT tap is what
  // left for the device each block, written by the tick at the device edge
  // (audio_processor.cpp), the IN tap what arrived. A capture is a reader's
  // notion — startCapture notes the writer's cursor, stopCapture returns
  // what was written since — and toggles nothing in the engine. See
  // js/lib/audio_capture.js and docs/ARENA.md.
  // ============================================================================

  startCapture() {
    this.#ensureInitialized("start capture");
    if (!this.#audioCapture.isAvailable()) {
      throw new Error(
        "Audio capture is only available in SAB mode (set mode: 'sab').");
    }
    this.#audioCapture.start();
  }

  stopCapture() {
    this.#ensureInitialized("stop capture");
    return this.#audioCapture.stop();
  }

  isCaptureEnabled() {
    return this.#audioCapture.isEnabled();
  }

  getCaptureFrames() {
    return this.#audioCapture.getFrameCount();
  }

  getMaxCaptureDuration() {
    return this.#audioCapture.getMaxDuration();
  }

  // ============================================================================
  // OSC MESSAGING API
  // ============================================================================

  send(address, ...args) {
    this.#ensureInitialized("send OSC messages");

    // Verbs the client refuses on the DSP's behalf.
    const blocked = this.#dsp.blockedVerbs;

    if (blocked[address]) {
      throw new Error(`${address} is not supported in Clockwork. ${blocked[address]}`);
    }

    // NOTHING IS INSPECTED HERE. What a product needs back after a reload is
    // the product's to remember, and it puts it back through
    // restoreClientState(). See reload().

    const normalizedArgs = args.map(arg => {
      if (arg instanceof ArrayBuffer) return new Uint8Array(arg);
      return arg;
    });

    const oscData = Clockwork.osc.encodeMessage(address, normalizedArgs);
    this.sendOSC(oscData);
  }

  sendOSC(oscData) {
    this.#ensureInitialized("send OSC data");

    const uint8Data = this.#toUint8Array(oscData);
    this.#sendPreparedOSC(uint8Data);
  }

  /**
   * Flush pending OSC from the WASM scheduler and the IN ring.
   *
   * Uses a postMessage flag (not the ring buffer) to avoid the race where stale
   * scheduled bundles would fire before a clearSched command could be read from
   * the ring buffer. Resolves when the worklet acks — or, if the worklet is
   * gone, when that ack times out.
   *
   * @returns {Promise<void>}
   */
  async purge() {
    this.#ensureInitialized("purge");

    await new Promise(resolve => {
      let settled = false;
      const finish = () => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        this.#workletNode.port.removeEventListener('message', handler);
        resolve();
      };
      const handler = (event) => {
        if (event.data.type === 'clearSchedAck') finish();
      };
      // The worklet acks synchronously from its message handler, so a live
      // worklet — even one whose AudioContext is suspended — replies almost
      // immediately. A missing ack therefore means the worklet is gone (its
      // global scope was reclaimed, e.g. a long-backgrounded mobile tab — the
      // exact case recover() exists to handle). Resolve best-effort on a
      // timeout so purge()/resume()/recover() can't wedge and recover() falls
      // through to reload() instead of hanging forever.
      const timer = setTimeout(() => {
        if (__DEV__) console.warn('[Dbg-Clockwork] purge() timed out waiting for clearSchedAck; worklet may be gone');
        finish();
      }, PURGE_ACK_TIMEOUT_MS);
      this.#workletNode.port.addEventListener('message', handler);
      this.#workletNode.port.postMessage({ type: 'clearSched', ack: true });
    });
  }

  /**
   * A transferable channel letting a Web Worker send OSC straight to the
   * worklet, bypassing the main thread. SAB-backed or MessagePort-backed
   * depending on transport.
   *
   * @returns {OscChannel}
   */
  createOscChannel(options = {}) {
    this.#ensureInitialized("create OSC channel");
    return this.#osc.createOscChannel(options);
  }

  /**
   * Get the next unique node ID.
   *
   * Globally unique with no coordination. IDs start at 1000: 0 is the root group
   * and 1-999 are left free for the client to assign by hand.
   *
   * SAB mode is a single atomic increment; PM mode allocates by range.
   * Also available on OscChannel, for Web Workers.
   *
   * @returns {number} A unique node ID (>= 1000)
   */
  nextNodeId() {
    this.#ensureInitialized("allocate node IDs");
    return this.#oscChannel.nextNodeId();
  }

  // ============================================================================
  // ASSET LOADING API
  // ============================================================================

  /**
   * Where the guest's memory is, and how far it may grow.
   *
   * Clockwork reserves this region and never interprets a byte of it. What
   * lives there — an allocator arena, sample frames, a wavetable, anything —
   * is decided entirely by the guest and its client code.
   *
   * @returns {{offset: number, size: number, maxSize: number}} byte addresses
   *          in the guest's own address space
   */
  guestMemory() {
    const mem = this.#config.memory;
    return {
      offset:  mem.guestMemoryOffset,
      size:    mem.guestMemorySize,
      maxSize: mem.guestMemorySize,   // fixed at boot: the inbox is what grows
    };
  }

  /**
   * The inbound staging region. THE CLIENT WRITES IT, THE GUEST ONLY READS IT.
   *
   * Bulk into the engine goes here — a sample, a wavetable, an impulse
   * response — followed by a short message saying where it landed. It is not
   * the guest's arena, and there is deliberately no way to write to that from
   * here: a region with two writers would need both sides to agree at runtime
   * about which bytes belong to whom, and nothing could enforce that.
   *
   * @returns {{offset: number, size: number}}
   */
  inbox() {
    const mem = this.#config.memory;
    // SIZE IS MEASURED, NOT REMEMBERED. growInbox moves the end of the
    // memory, so reading it back keeps a grown region from being reported at
    // its boot size — which would refuse every write past the initial commit,
    // since the range check is what a caller is held to. In postMessage mode
    // there is no heap here to measure, so growInbox maintains the number.
    const committed = this.#wasmMemory
      ? Math.max(0, this.#wasmMemory.buffer.byteLength - mem.inboxOffset)
      : mem.inboxSize;
    return {
      offset:  mem.inboxOffset,
      size:    committed,
      maxSize: this.#config.effectiveMaxInbox ?? mem.maxInboxSize,
    };
  }

  /**
   * The outbound staging region. THE GUEST WRITES IT, THE CLIENT ONLY READS IT.
   *
   * @returns {{offset: number, size: number}}
   */
  outbox() {
    const mem = this.#config.memory;
    return { offset: mem.outboxOffset, size: mem.outboxSize };
  }

  /**
   * Commit more inbox, growing the WASM heap by whole pages.
   *
   * The CEILING CANNOT MOVE. `maximum` is fixed when the memory is
   * constructed and reserves address space that growth then commits into, so
   * this can only ever reach `inbox().maxSize`. A client that needs more
   * than that must ask for it before boot, not here.
   *
   * @param {number} bytes  how much more is needed, rounded up to whole pages
   * @returns {Promise<boolean>} false if the ceiling refused it
   */
  async growInbox(bytes) {
    this.#ensureInitialized("grow inbox");
    const pages = Math.ceil(bytes / 65536);
    if (pages <= 0) return true;

    if (this.#config.mode === "sab") {
      // Growing a SHARED memory does not detach its buffer, which is what
      // lets an allocator keep references to regions it took earlier.
      return this.#wasmMemory.grow(pages) !== -1;
    }
    // PM mode: the worklet owns the only heap there is, so it does the growing.
    // There is nothing here to measure afterwards, so the committed size is
    // carried forward by hand — see inbox().
    const growId = ++this.#guestGrowSeq;
    return this.#workletReply(
      { type: "growMemory", growId, pages },
      "memoryGrown",
      (d) => d.growId === growId,
      "grow inbox",
    ).then((d) => {
      const ok = !!d.success;
      if (ok) this.#config.memory.inboxSize += pages * 65536;
      return ok;
    }).catch(() => false);
  }

  /**
   * Call a named wasm export on the audio thread and return its result.
   *
   * A guest (or a demo like rerezzed) sometimes has to invoke one of its own
   * exports from inside the audio context — spawning a program, staging a
   * pipeline — where it can touch the live engine directly. The worklet runs
   * the export and posts the result back.
   */
  async callExport(name, args = []) {
    const callId = ++this.#exportCallSeq;
    const reply = await this.#workletReply(
      { type: "callExport", callId, name, args },
      "exportCalled",
      (d) => d.callId === callId,
      `call ${name}`,
    );
    return reply.result;
  }

  /**
   * Put bytes into the INBOX at `offset`, for the guest to read.
   *
   * SAB mode writes straight into the shared heap; postMessage mode ships the
   * bytes to the worklet to write. Bulk never rides OSC ingress either way.
   *
   * OFFSET, NOT POINTER — measured from the base of the inbox. Native maps the
   * same region at a different address than the engine's, so an absolute
   * pointer names the wrong bytes there without faulting.
   *
   * @param {number} offset  a byte offset into `inbox()`
   * @param {ArrayBuffer|ArrayBufferView} bytes
   * @returns {Promise<{offset: number, bytes: number}>}
   */
  async writeInbox(offset, bytes) {
    this.#ensureInitialized("write inbox");
    const src = ArrayBuffer.isView(bytes)
      ? new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength)
      : new Uint8Array(bytes);

    const region = this.inbox();
    this.#checkGuestRange(offset, src.byteLength, region, "write inbox");
    const ptr = region.offset + offset;

    if (this.#config.mode === "sab") {
      new Uint8Array(this.#wasmMemory.buffer, ptr, src.byteLength).set(src);
      return { offset, bytes: src.byteLength };
    }

    // A detached transferable cannot be recovered, so copy before handing over.
    const copyId = ++this.#guestWriteSeq;
    const buf = src.slice().buffer;
    await this.#workletReply(
      { type: "copyBufferData", copyId, ptr, data: buf },
      "bufferCopied",
      (d) => d.copyId === copyId,
      "write guest memory",
      [buf],
    );
    return { offset, bytes: src.byteLength };
  }

  /*
   * Refuse a range that leaves the guest's region, before anyone acts on it.
   *
   * Written as a subtraction rather than as `offset + len > size` because the
   * latter passes on overflow — a huge length wraps and the check waves it
   * through, which in SAB mode is a write past the region and in native terms
   * is the read the C++ side's region_at() exists to prevent. Same shape on
   * both sides on purpose.
   */
  #checkGuestRange(offset, len, region, what) {
    const size = region.size;
    if (!Number.isInteger(offset) || !Number.isInteger(len)
        || offset < 0 || len < 0 || offset > size || len > size - offset) {
      throw new RangeError(
        `${what}: [${offset}, ${offset + len}) is outside the region `
        + `(0..${size})`);
    }
  }

  /**
   * Read bytes out of the OUTBOX, which only the guest writes.
   *
   * The mirror of writeInbox: a guest writes a blob into the outbox and sends a
   * short message saying where. `offset` is region-relative, as there.
   *
   * ASYNC ON BOTH TRANSPORTS, though SAB could answer synchronously — a
   * signature that changed shape with the transport would break any caller that
   * awaited only in postMessage mode.
   *
   * @param {number} offset  a byte offset into `outbox()`
   * @param {number} len
   * @returns {Promise<Uint8Array>} a COPY, so a later growth cannot invalidate it
   */
  async readOutbox(offset, len) {
    this.#ensureInitialized("read outbox");
    const region = this.outbox();
    this.#checkGuestRange(offset, len, region, "read outbox");
    const ptr = region.offset + offset;

    if (this.#config.mode === "sab") {
      return new Uint8Array(this.#wasmMemory.buffer, ptr, len).slice();
    }

    const readId = ++this.#guestReadSeq;
    const reply = await this.#workletReply(
      { type: "readBufferData", readId, ptr, len },
      "bufferRead",
      (d) => d.readId === readId,
      "read guest memory",
    );
    return new Uint8Array(reply.data);
  }

  /*
   * One request to the worklet, one matching reply, or a timeout.
   *
   * Both memory primitives need the same shape, and the message names live in one
   * place so they can be checked against the worklet — a name the worklet never
   * implemented can only ever time out.
   */
  #workletReply(message, replyType, matches, what, transfer = []) {
    const port = this.#workletNode.port;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        port.removeEventListener("message", handler);
        reject(new Error(`${what}: the worklet did not answer within 10s`));
      }, 10000);
      const handler = (e) => {
        if (!e.data || e.data.type !== replyType || !matches(e.data)) return;
        clearTimeout(timer);
        port.removeEventListener("message", handler);
        e.data.success === false
          ? reject(new Error(e.data.error || `${what}: the worklet refused`))
          : resolve(e.data);
      };
      port.addEventListener("message", handler);
      port.postMessage(message, transfer);
    });
  }

  async sync(syncId = Math.floor(Math.random() * 2147483647), timeoutMs = SYNC_TIMEOUT_MS) {
    this.#ensureInitialized("sync");

    const syncPromise = new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        this.#syncListeners?.delete(syncId);
        reject(new Error(`Timeout waiting for ${this.#dsp.syncedVerb} response`));
      }, timeoutMs);

      const messageHandler = () => {
        clearTimeout(timeout);
        this.#syncListeners.delete(syncId);
        resolve();
      };

      if (!this.#syncListeners) this.#syncListeners = new Map();
      this.#syncListeners.set(syncId, messageHandler);
    });

    if (!this.#dsp.syncVerb) {
      throw new Error("sync() needs a dsp profile: no DSP declares a sync verb.");
    }
    this.send(this.#dsp.syncVerb, syncId);
    await syncPromise;

    if (this.#config.mode === 'postMessage') {
      await new Promise(r => setTimeout(r, this.#config.snapshotIntervalMs * 2));
    }
  }

  // ============================================================================
  // INFO API
  // ============================================================================

  getInfo() {
    this.#ensureInitialized("get info");

    return {
      sampleRate: this.#audioContext.sampleRate,
      totalMemory: this.#config.memory.totalMemory,
      wasmHeapSize: this.#config.memory.wasmHeapSize,
      guestMemorySize: this.#config.memory.guestMemorySize,
      bootTimeMs: this.bootStats.initDuration,
      capabilities: { ...this.#capabilities },
      version: this.#version,
    };
  }

  // ============================================================================
  // LIFECYCLE API
  // ============================================================================

  async shutdown() {
    if (!this.#initialized && !this.#initializing) return;

    this.#eventEmitter.emit("shutdown");
    this.#clock?.stopDriftTimer();
    this.#audioHealthMonitor?.reset();
    this.#audioHealthMonitor = null;
    this.#syncListeners?.clear();
    this.#syncListeners = null;

    if (this.#osc) {
      this.#osc.dispose();
      this.#osc = null;
    }
    // Stop forwarding worklet debug batches — the transport is gone. A live
    // handler here would deref the null #osc when a late batch arrives.
    this.#debugRawHandler = null;
    // The front's devices close with the transport that fed them.
    this.#front?.dispose();
    this.#front = null;

    if (this.#workletNode) {
      this.#workletNode.disconnect();
      this.#workletNode = null;
    }

    if (this.#audioContext) {
      await this.#audioContext.close();
      this.#audioContext = null;
    }


    this.#oscChannel = null;
    this.#initialized = false;
    this.#scopeViews = null;
    this.#initPromise = null;
    this.#wasmMemory = null;
    this.#clock?.reset();
    this.bootStats = { initStartTime: null, initDuration: null };
  }

  async destroy() {
    this.#eventEmitter.emit("destroy");
    await this.shutdown();
    this.#cachedWasmBytes = null;
    this.#eventEmitter.removeAllListeners();
  }

  async reset() {
    await this.shutdown();
    await this.init();
  }

  // ============================================================================
  // PRIVATE: INITIALIZATION HELPERS
  // ============================================================================

  #setAndValidateCapabilities() {
    this.#capabilities = {
      audioWorklet: typeof AudioWorklet !== "undefined",
      sharedArrayBuffer: typeof SharedArrayBuffer !== "undefined",
      crossOriginIsolated: window.crossOriginIsolated === true,
      atomics: typeof Atomics !== "undefined",
      webWorker: typeof Worker !== "undefined",
      playbackStats: typeof AudioContext !== "undefined" && 'playbackStats' in AudioContext.prototype,
    };

    const mode = this.#config.mode;
    const required = ["audioWorklet", "webWorker"];

    if (mode === 'sab') {
      required.push("sharedArrayBuffer", "crossOriginIsolated", "atomics");
    }

    const missing = required.filter((f) => !this.#capabilities[f]);

    if (missing.length > 0) {
      const error = new Error(`Missing required features for ${mode} mode: ${missing.join(", ")}`);
      if (mode === 'sab' && !this.#capabilities.crossOriginIsolated) {
        error.message += "\n\nConsider using mode: 'postMessage' which doesn't require COOP/COEP headers.";
      }
      throw error;
    }

    if (mode !== 'sab' && mode !== 'postMessage') {
      throw new Error(`Invalid mode: '${mode}'. Use 'sab' or 'postMessage'.`);
    }
  }

  #initializeMemory() {
    const memConfig = this.#config.memory;
    const mode = this.#config.mode;

    if (mode === 'sab') {
      // Initial pages must be at least the compile-time minimum (WASM binary constraint).
      // User overrides that increase guestMemorySize will increase initial; decreases are clamped.
      const minPages = MemoryLayout.totalPages;
      const totalPages = Math.max(Math.ceil(memConfig.totalMemory / 65536), minPages);
      // For shared WASM memory, maximum must match the compile-time cap.
      const maxPages = Math.ceil(memConfig.maxTotalMemory / 65536);
      this.#wasmMemory = new WebAssembly.Memory({
        initial: totalPages,
        maximum: maxPages,
        shared: true,
      });
    } else {
      this.#wasmMemory = null;
    }

    // No allocator here. Clockwork reserves the guest's region and moves
    // opaque bytes into it; whoever knows what those bytes mean runs its own
    // allocator over `guestMemory()`. A pool here would be a SECOND allocator
    // over the same region as the guest's, handing out overlapping pointers.
  }

  #initializeAudioContext() {
    if (this.#config.audioContext) {
      this.#audioContext = this.#config.audioContext;
    } else {
      this.#audioContext = new AudioContext(this.#config.audioContextOptions);
    }

    this.#audioContext.addEventListener('statechange', () => {
      const state = this.#audioContext?.state;
      if (!state) return;

      const previousState = this.#previousAudioContextState;
      this.#previousAudioContextState = state;

      if (state === 'running' && (previousState === 'suspended' || previousState === 'interrupted')) {
        this.#clock?.resync();
      }

      this.#eventEmitter.emit('audiocontext:statechange', { state });
      if (state === 'suspended') {
        this.#eventEmitter.emit('audiocontext:suspended');
        this.#audioHealthMonitor?.reset();
      } else if (state === 'running') {
        this.#eventEmitter.emit('audiocontext:resumed');
        this.#audioHealthMonitor?.reset();
      } else if (state === 'interrupted') {
        this.#eventEmitter.emit('audiocontext:interrupted');
        this.#audioHealthMonitor?.reset();
      }
    });

    this.#audioHealthMonitor = new AudioHealthMonitor({ audioContext: this.#audioContext });
  }

      async #loadWasm() {
    if (this.#cachedWasmBytes) return this.#cachedWasmBytes;

    const wasmName = this.#config.wasmUrl.split('/').pop();

    if (this.#config.wasmBytes) {
      const wasmBytes = this.#config.wasmBytes;
      this.#eventEmitter.emit('loading:start', { type: 'wasm', name: wasmName, size: wasmBytes.byteLength });
      this.#eventEmitter.emit('loading:complete', { type: 'wasm', name: wasmName, size: wasmBytes.byteLength });
      this.#cachedWasmBytes = wasmBytes;
      return wasmBytes;
    }
    const wasmBytes = await this.#assetLoader.fetch(this.#config.wasmUrl, { type: 'wasm', name: wasmName });
    this.#cachedWasmBytes = wasmBytes;

    return wasmBytes;
  }

  async #initializeAudioWorklet(wasmBytes) {
    await addWorkletModule(this.#audioContext.audioWorklet, this.#config.workletUrl);

    const numOutputChannels = this.#config.audio.outputChannels;
    this.#workletNode = new AudioWorkletNode(this.#audioContext, "clockwork-processor", {
      numberOfInputs: 1,
      numberOfOutputs: 1,
      outputChannelCount: [numOutputChannels],
    });

    if (this.#config.autoConnect) {
      const dest = this.#audioContext.destination;
      if (numOutputChannels > 2) {
        dest.channelCount = Math.min(numOutputChannels, dest.maxChannelCount);
        dest.channelInterpretation = 'discrete';
      }
      this.#workletNode.connect(dest);
    }

    this.#node = this.#createNodeWrapper();
    this.#workletNode.port.start();
    this.#setupMessageHandlers();

    const mode = this.#config.mode;
    const sharedBuffer = mode === 'sab' ? this.#wasmMemory.buffer : null;

    this.#workletNode.port.postMessage({
      type: "init",
      mode: mode,
      sharedBuffer: sharedBuffer,
      snapshotIntervalMs: this.#config.snapshotIntervalMs,
    });

    const loadWasmMsg = {
      type: "loadWasm",
      wasmBytes: wasmBytes,
      // The guest's config block, already encoded by whoever knows its shape.
      guestConfigBytes: this.encodeGuestConfig({
        mode,
        guestMemoryOffset: this.#config.memory.guestMemoryOffset,
      }),
      // Where the guest's opaque region is.
      guestMemoryOffset: this.#config.memory.guestMemoryOffset,
      guestMemorySize: this.#config.memory.guestMemorySize,
      // The two one-way bulk lanes. Each has exactly one writer: the client
      // writes the inbox and the guest only reads it; the guest writes the
      // outbox and the client only reads it. Neither overlaps the arena.
      inboxOffset: this.#config.memory.inboxOffset,
      inboxSize: this.#config.memory.inboxSize,
      outboxOffset: this.#config.memory.outboxOffset,
      outboxSize: this.#config.memory.outboxSize,
      // And the span clockwork::mem allocates from, which sits below that region and
      // is where the guest's real-time pool comes from. A build-time constant
      // cannot size it: how big that pool is, is a runtime question.
      memArenaSize: this.#config.memory.memArenaSize ?? 0,
      inputChannels: this.#config.audio.inputChannels,
      outputChannels: this.#config.audio.outputChannels,
      sampleRate: this.#audioContext.sampleRate,
    };

    if (mode === 'sab') {
      loadWasmMsg.wasmMemory = this.#wasmMemory;
    } else {
      const minPages = MemoryLayout.totalPages;
      loadWasmMsg.memoryPages = Math.max(Math.ceil(this.#config.memory.totalMemory / 65536), minPages);
      loadWasmMsg.maxMemoryPages = Math.ceil(this.#config.memory.maxTotalMemory / 65536);
    }

    this.#workletNode.port.postMessage(loadWasmMsg);

    await this.#waitForWorkletInit();
  }

  #createNodeWrapper() {
    const worklet = this.#workletNode;
    return Object.freeze({
      connect: (...args) => worklet.connect(...args),
      disconnect: (...args) => worklet.disconnect(...args),
      get context() { return worklet.context; },
      get numberOfOutputs() { return worklet.numberOfOutputs; },
      get numberOfInputs() { return worklet.numberOfInputs; },
      get channelCount() { return worklet.channelCount; },
      get input() { return worklet; },
    });
  }

  async #initializeOSC() {
    const mode = this.#config.mode;
    const bc = this.#metricsReader.bufferConstants;
    const ringBufferBase = this.#metricsReader.ringBufferBase;
    const sharedBuffer = this.#metricsReader.sharedBuffer;

    const transportConfig = {
      workerBaseURL: this.#config.workerBaseURL,
      snapshotIntervalMs: this.#config.snapshotIntervalMs,
      bypassLookaheadS: this.#config.bypassLookaheadMs / 1000,
      getAudioContextTime: () => this.#audioContext?.currentTime ?? 0,
      getNTPStartTime: () => this.#clock?.getNTPStartTime() ?? 0,
    };

    if (mode === 'sab') {
      transportConfig.sharedBuffer = sharedBuffer;
      transportConfig.ringBufferBase = ringBufferBase;
      transportConfig.bufferConstants = bc;

      // Every SAB context reaches the rings by running the engine's own C over
      // this memory (js/lib/wasm_client.js), so it needs the module and the
      // memory rather than just the bytes behind them. Compiled once here and
      // shared: a WebAssembly.Module is cloneable, so workers are handed this
      // one instead of compiling their own copy.
      transportConfig.wasmMemory = this.#wasmMemory;
      transportConfig.wasmModule = await WebAssembly.compile(this.#cachedWasmBytes);

      // Initialize node ID counter in shared memory.
      // Starts at 1000: 0 is the root group and
      // 1–999 are left free for the client to assign by hand.
      if (bc?.NODE_ID_COUNTER_START !== undefined) {
        const counterBase = ringBufferBase + bc.NODE_ID_COUNTER_START;
        const counterView = new Int32Array(sharedBuffer, counterBase, 1);
        Atomics.store(counterView, 0, 1000);
      }
    } else {
      this.#nodeIdCounter = 1000;
      transportConfig.nodeIdSource = (rangeSize) => {
        const from = this.#nodeIdCounter;
        this.#nodeIdCounter += rangeSize;
        return { from, to: from + rangeSize };
      };

      // PM mode: provision the AudioWorklet with its own node ID range
      // for the C++ UUID rewriter (same protocol as OscChannel workers)
      const workletNodeIdRangeSize = 10000;
      const workletRange = transportConfig.nodeIdSource(workletNodeIdRangeSize);
      const nodeIdChannel = new MessageChannel();
      const nodeIdSource = transportConfig.nodeIdSource;
      nodeIdChannel.port1.onmessage = (e) => {
        if (e.data.type === 'requestNodeIdRange') {
          const r = nodeIdSource(workletNodeIdRangeSize);
          nodeIdChannel.port1.postMessage({ type: 'nodeIdRange', from: r.from, to: r.to });
        }
      };
      this.#workletNode.port.postMessage(
        { type: 'nodeIdRange', from: workletRange.from, to: workletRange.to },
        [nodeIdChannel.port2]
      );
    }

    this.#osc = createTransport(mode, transportConfig);

    this.#osc.onReply((oscData, sequence, timestamp) => {
      // A verb the worklet forwarded to the host is the front's to answer,
      // not the client's to hear: it never sent one of those inbound.
      if (this.#front && this.#front.take(oscData)) return;
      this.#deliverInbound(oscData, sequence, timestamp);
    });

    // Debug arrives as /clockwork/debug on the OSC-in path above.

    this.#osc.onError((error, workerName) => {
      console.error(`[Clockwork] ${workerName} error:`, error);
      this.#eventEmitter.emit('error', new Error(`${workerName}: ${error}`));
    });

    this.#osc.onOscLog((entries) => {
      for (const entry of entries) {
        const scheduledTime = oscFast.getBundleTimeTag(entry.oscData) || null;
        this.#eventEmitter.emit('out:osc', {
          oscData: entry.oscData,
          sourceId: entry.sourceId,
          sequence: entry.sequence,
          timestamp: entry.timestamp,
          scheduledTime,
        });

        const needsDecode = this.#eventEmitter.hasListeners('out') ||
          this.#eventEmitter.hasListeners('out:text') ||
          this.#eventEmitter.hasListeners('out:html') ||
          this.#config.debug || this.#config.debugOscOut;
        if (needsDecode) {
          try {
            const msg = oscFast.decodePacket(entry.oscData);
            this.#eventEmitter.emit('out', msg);

            if (this.#eventEmitter.hasListeners('out:text') || this.#config.debug || this.#config.debugOscOut) {
              const maxLen = this.#config.activityEvent.oscOutMaxLineLength ?? this.#config.activityEvent.maxLineLength;
              const outAddr = msg[0];
              const outArgs = msg.slice(1);
              const argsStr = outArgs.map(a => formatOscArg(a, maxLen)).join(', ');
              const text = `${outAddr}${argsStr ? ' ' + argsStr : ''}`;
              this.#eventEmitter.emit('out:text', { text, sequence: entry.sequence, timestamp: entry.timestamp });
            }

            if (this.#eventEmitter.hasListeners('out:html')) {
              const html = formatBundleHtml(msg, entry.sequence, entry.timestamp, this.initTime, entry.sourceId);
              this.#eventEmitter.emit('out:html', { html, sequence: entry.sequence, timestamp: entry.timestamp });
            }
          } catch (e) { /* skip decoded events on decode failure */ }
        }
      }
    });

    if (this.#config.debug || this.#config.debugOscIn) {
      this.on('in:text', ({ text }) => console.log(`[← OSC] ${text}`));
    }
    if (this.#config.debug || this.#config.debugOscOut) {
      this.on('out:text', ({ text }) => console.log(`[OSC →] ${text}`));
    }
    if (this.#config.debug || this.#config.debugEngine) {
      this.on('debug', (msg) => console.log(`[synth] ${msg.text}`));
    }

    if (mode === 'sab') {
      await this.#osc.initialize();
    } else {
      await this.#osc.initialize(this.#workletNode.port);

      // PM mode: pass bufferConstants to transport for decoder worker
      this.#osc.setBufferConstants(bc);

      if (this.#earlyDebugMessages?.length > 0) {
        for (const data of this.#earlyDebugMessages) {
          this.#osc.handleDebugRaw(data);
        }
      }
      // Optional-chain #osc: a debugRawBatch queued from the old worklet can
      // be dispatched during reload()/teardown after #osc has been nulled.
      this.#debugRawHandler = (data) => this.#osc?.handleDebugRaw(data);
      this.#earlyDebugMessages = [];
    }

    // Create main-thread OscChannel for sendOSC()
    // Main thread uses sourceId 0, workers get 1+
    this.#oscChannel = this.#osc.createOscChannel({ sourceId: 0 });
  }

  /*
   * The host's front (js/lib/host_front.js). Always present once the
   * transport is: the worklet forwards every /clockwork/ verb its audio
   * thread does not answer, so SOMETHING on this side has to be the far end
   * that answers or refuses. With `midi` / `gamepad` enabled, that is the
   * managers; without, every such verb is refused by name.
   */
  async #initializeFront() {
    this.#front?.dispose();
    this.#front = new HostFront({
      midi: this.#config.midi,
      gamepad: this.#config.gamepad,
      wasmBaseURL: this.#config.wasmBaseURL,
      deliver: (bytes) => this.#deliverInbound(bytes, -1, performance.now()),
      // An event goes INTO the engine, as a native subsystem's would: the
      // transport exists by now, and the client need not be 'initialized'
      // for a keyboard to be heard during boot.
      ingest: (bytes) => this.#sendPreparedOSC(this.#toUint8Array(bytes)),
      // A subsystem that cannot come up is not a failed boot: the engine
      // runs without it, the reason is on the console and the 'error'
      // event, and `midiError` / `gamepadError` keeps it.
      onUnavailable: (name, error) => {
        console.warn(`[Clockwork] ${name} unavailable, booting without it:`, error);
        this.#eventEmitter.emit('error', new Error(`${name} unavailable: ${error?.message ?? error}`));
      },
    });
    await this.#front.init();
  }

  /*
   * One inbound frame for the client: an engine reply or push off the
   * egress, or a reply or push the front made on the engine's behalf — the
   * same path, the same events, so a listener cannot tell which side
   * answered. `sequence` is the egress frame's, or -1 for the front's.
   */
  #deliverInbound(oscData, sequence, timestamp) {
    const scheduledTime = oscFast.getBundleTimeTag(oscData) || null;
    this.#eventEmitter.emit('in:osc', { oscData, sequence, timestamp, scheduledTime });

    try {
      const msg = oscFast.decodePacket(oscData);

      const address = msg[0];
      const args = msg.slice(1);
      if (address === clockworkSys("debug")) {
        // Debug log lines ride the egress as /clockwork/debug — surface them on
        // the 'debug' event, not the regular OSC-in stream.
        const eventMaxLen = this.#config.activityEvent.engineMaxLineLength ?? this.#config.activityEvent.maxLineLength;
        let text = args[0] ?? '';
        if (eventMaxLen > 0 && text.length > eventMaxLen) text = text.slice(0, eventMaxLen) + '...';
        this.#eventEmitter.emit('debug', { text, sequence, timestamp });
        return;
      } else if (address === this.#dsp.syncedVerb && args.length > 0) {
        const syncId = args[0];
        if (this.#syncListeners?.has(syncId)) {
          this.#syncListeners.get(syncId)(msg);
        }
      }

      this.#eventEmitter.emit('in', msg);

      if (this.#eventEmitter.hasListeners('in:text') || this.#config.debug || this.#config.debugOscIn) {
        const maxLen = this.#config.activityEvent.oscInMaxLineLength ?? this.#config.activityEvent.maxLineLength;
        const argsStr = args.map(a => formatOscArg(a, maxLen)).join(', ') || '';
        const text = `${address}${argsStr ? ' ' + argsStr : ''}`;
        this.#eventEmitter.emit('in:text', { text, sequence, timestamp });
      }

      if (this.#eventEmitter.hasListeners('in:html')) {
        const html = formatOscLineHtml(msg, sequence, timestamp, this.initTime);
        this.#eventEmitter.emit('in:html', { html, sequence, timestamp });
      }
    } catch (e) {
      console.error('[Clockwork] Failed to decode OSC message:', e);
    }
  }

  async #finishInitialization() {
    this.#initialized = true;
    this.#initializing = false;
    this.bootStats.initDuration = performance.now() - this.bootStats.initStartTime;

    await this.#eventEmitter.emitAsync('setup');
    this.#eventEmitter.emit('ready', { capabilities: this.#capabilities, bootStats: this.bootStats });

    // TODO(v1): Consider whether to keep this dev console helper.
    // It auto-registers instances to window.__clockwork__ for quick debugging (ss.metrics(), ss.tree(), etc.)
    // Unusual pattern - most libs expect devs to do `window.sonic = sonic` themselves.
    // Useful but the `instances` array for multiple engines is over-engineered.
    if (__DEV__ && typeof window !== 'undefined') {
      if (!window.__clockwork__) {
        const ss = window.__clockwork__ = { instances: [] };
        Object.defineProperties(ss, {
          primary: { get: () => ss.instances[0] },
          layout: { get: () => ss.primary?.bufferConstants },
        });
        ss.metrics = () => ss.primary?.getMetrics();
        ss.window = () => ss.primary?.readWindow();
        ss.snapshot = () => ss.primary?.getSnapshot();
      }
      window.__clockwork__.instances.push(this);
    }
  }

  #waitForWorkletInit() {
    return new Promise((resolve, reject) => {
      const timeout = setTimeout(() => {
        reject(new Error("AudioWorklet initialization timeout"));
      }, WORKLET_INIT_TIMEOUT_MS);

      const messageHandler = async (event) => {
        if (event.data.type === "error") {
          clearTimeout(timeout);
          this.#workletNode.port.removeEventListener("message", messageHandler);
          reject(new Error(event.data.error || "AudioWorklet error"));
          return;
        }

        if (event.data.type === "initialized") {
          clearTimeout(timeout);
          this.#workletNode.port.removeEventListener("message", messageHandler);

          if (event.data.success) {
            const ringBufferBase = event.data.ringBufferBase ?? 0;
            const bufferConstants = event.data.bufferConstants;
            const sharedBuffer = this.#config.mode === 'sab' ? this.#wasmMemory.buffer : null;

            this.#metricsReader.initSharedViews(sharedBuffer, ringBufferBase, bufferConstants);

            // How many mirror entries a guest will use is a number only the guest knows
            // the name of. Clockwork publishes the capacity; the guest decides whether it
            // is enough.

            // Initialize NTP timing
            this.#clock = new ClockworkClock({
              mode: this.#config.mode,
              audioContext: this.#audioContext,
              workletPort: this.#workletNode.port,
            });
            this.#clock.initSharedViews(sharedBuffer, ringBufferBase, bufferConstants);
            await this.#clock.initialize();
            this.#clock.startDriftTimer();

            // Initialize audio capture (SAB mode only)
            if (this.#config.mode === 'sab') {
              this.#audioCapture.update(sharedBuffer, ringBufferBase, bufferConstants);
            }

            // PostMessage mode: set initial snapshot
            if (this.#config.mode === 'postMessage' && event.data.initialSnapshot) {
              this.#metricsReader.updateSnapshot(event.data.initialSnapshot);
            }

            resolve();
          } else {
            reject(new Error(event.data.error || "AudioWorklet initialization failed"));
          }
        }
      };

      this.#workletNode.port.addEventListener("message", messageHandler);
      this.#workletNode.port.start();
    });
  }


  #setupMessageHandlers() {
    this.#workletNode.port.addEventListener('message', (event) => {
      const { data } = event;

      switch (data.type) {
        case "error":
          console.error("[Worklet] Error:", data.error);
          this.#eventEmitter.emit('error', new Error(data.error));
          break;

        case "version":
          this.#version = data.version;
          break;

        case "snapshot":
          if (data.buffer) {
            this.#metricsReader.updateSnapshot(data.buffer);
            this.#snapshotsSent = data.snapshotsSent;
          }
          break;

        case "debugRawBatch":
          if (this.#debugRawHandler) {
            this.#debugRawHandler(data);
          } else if (this.#earlyDebugMessages) {
            this.#earlyDebugMessages.push(data);
          }
          break;

        // Note: oscLog is handled directly by the PM transport's #handleWorkletMessage
        // and by the SAB transport's osc_out_log_sab_worker — no forwarding needed here.
      }
    });
  }

  // ============================================================================
  // PRIVATE: METRICS
  // ============================================================================

  /**
   * Extra metrics context from the product built on clockwork.
   *
   * A client holds state clockwork cannot see — how many samples are
   * loaded, how often its own pool has grown — and the metrics object is
   * where a user looks for it. Overriding this is the supported way to get it
   * there; the alternative is a parallel metrics path beside clockwork's,
   * reported separately and never lining up with it.
   *
   * Return whatever the reader understands. Anything it does not recognise is
   * ignored rather than surfaced, so an unknown key is inert, not an error.
   *
   * @returns {object} merged over clockwork's own context
   */
  clientMetrics() { return {}; }

  /**
   * Encode the guest's config block, or null when it has none.
   *
   * Clockwork reserves a region for it and copies the bytes in; it does not
   * know or care what they mean. Clockwork's own geometry travels as arguments
   * to clockwork_init rather than through this block, so a product's encoder is
   * free to lay it out however its guest wants.
   *
   * @param {{mode: string, guestMemoryOffset: number}} ctx  what only
   *        clockwork knows and the encoder may need
   * @returns {ArrayBuffer|ArrayBufferView|null}
   */
  encodeGuestConfig(ctx) { return null; }

  /**
   * Re-send whatever the product had in the engine, after a reload.
   *
   * A reload tears the engine down and builds it again, so everything the
   * engine held is gone, and clockwork has no idea what any of it was.
   * Override this to put it back — definitions, samples, anything.
   *
   * This is the ONLY restore path — there is no second mechanism for any one
   * category of state.
   *
   * Called before the barrier that waits on all of it, so a product does not
   * need its own sync.
   */
  async restoreClientState() {}

  /*
   * The declared guest metrics, merged into the schema clockwork reports so
   * a caller sees one list rather than clockwork's plus a product's.
   */
  getMetricsSchema() {
    return Clockwork.mergeGuestMetrics(this.#dsp);
  }

  /**
   * Clockwork's schema with one guest's declarations folded in.
   *
   * Static, because a product overriding the static `getMetricsSchema()` needs
   * the same merge before any instance exists — a caller inspecting what a
   * product reports should not have to boot one to find out.
   */
  static mergeGuestMetrics(dsp) {
    const declared = dsp?.metrics || {};
    const metrics = { ...METRICS_SCHEMA.metrics };
    for (const [name, def] of Object.entries(declared)) {
      metrics[name] = { ...def, offset: GUEST_METRICS_BASE + def.slot };
    }
    const layout = {
      ...METRICS_SCHEMA.layout,
      panels: [...(METRICS_SCHEMA.layout?.panels || []), ...(dsp?.metricsPanels || [])],
    };
    return { ...METRICS_SCHEMA, metrics, layout };
  }

  #metricsContext() {
    return {
      // Only what the guest declared gets through; the rest has no slot.
      guestMetrics: this.clientMetrics(),
      transportMetrics: this.#osc?.getMetrics(),
      driftOffsetMs: this.#clock?.getDriftOffset() ?? 0,
      ntpStartTime: this.#clock?.getNTPStartTime() ?? 0,
      clockOffsetMs: this.#clock?.getClockOffset() ?? 0,
      audioContextState: this.#audioContext?.state || "unknown",
      audioHealthPct: this.#audioHealthMonitor?.update() ?? 100,
      playbackStats: this.#capabilities.playbackStats ? this.#audioContext?.playbackStats : null,
    };
  }

  #gatherMetrics() {
    return this.#metricsReader.gatherMetrics(this.#metricsContext());
  }

  #updateMergedArray() {
    this.#metricsReader.updateMergedArray(this.#metricsContext());
  }

  // ============================================================================
  // PRIVATE: UTILITIES
  // ============================================================================

  #readProcessCount() {
    if (this.#config.mode === 'sab') {
      const view = this.#metricsReader.getMetricsView();
      return view ? view[0] : null;
    }
    const buffer = this.#metricsReader.getSnapshotBuffer();
    if (!buffer) return null;
    return new Uint32Array(buffer, 0, 1)[0];
  }

  #ensureInitialized(actionDescription = "perform this operation") {
    if (!this.#initialized) {
      throw new Error(`Clockwork not initialized. Call init() before attempting to ${actionDescription}.`);
    }
  }


  #toUint8Array(data) {
    if (data instanceof Uint8Array) return data;
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    throw new Error("oscData must be ArrayBuffer or Uint8Array");
  }

  #sendPreparedOSC(preparedData) {
    // A message larger than the IN ring can never be delivered — fail loudly
    // rather than dropping it silently in the ring writer.
    const bc = this.#metricsReader?.bufferConstants;
    const maxSize = bc?.IN_BUFFER_SIZE;
    if (maxSize && preparedData.length > maxSize - 16 /* Message header */) {
      throw new Error(
        `OSC message too large to send (${preparedData.length} > ${maxSize - 16} bytes)`
      );
    }

    // Dumb send: frame the bytes onto the IN ring (SAB) or postMessage them (PM).
    // The audio thread classifies + schedules (OscIngress + Scheduler).
    // A SAB write that loses the lock race / hits a full ring is dropped and
    // counted as ringBufferDirectWriteFails (no fallback) — not silent.
    this.#oscChannel.send(preparedData);
  }

      }

export const osc = Clockwork.osc;
