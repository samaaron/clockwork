// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/**
 * AudioCapture — a client's reader of an audio tap (shm_audio_buffer.hpp).
 *
 * The taps are clockwork's and flow from boot: the OUT tap is what left for
 * the device each block, the IN tap what arrived. A capture is therefore a
 * READER'S notion: start() notes where the writer's cursor is, read()
 * returns the frames written since, and nothing is toggled in the engine.
 * The ring holds SHM_AUDIO_FRAMES (a second in production); a capture that
 * outlives it gets the newest ring's worth and says how much was lost.
 *
 * SAB mode only: the ring is read in place.
 */
export class AudioCapture {
  #sharedBuffer;
  #bufferConstants;
  #ringBufferBase;
  #slot;
  /** The writer's cursor when the capture started, or null when none is running. */
  #startPosition = null;

  /**
   * @param {object} [options]
   * @param {number} [options.slot] 0 for the OUT tap (default), 1 for the IN tap
   */
  constructor(options = {}) {
    this.#sharedBuffer = options.sharedBuffer || null;
    this.#bufferConstants = options.bufferConstants || null;
    this.#ringBufferBase = options.ringBufferBase || 0;
    this.#slot = options.slot ?? 0;
  }

  update(sharedBuffer, ringBufferBase, bufferConstants) {
    this.#sharedBuffer = sharedBuffer;
    this.#ringBufferBase = ringBufferBase;
    this.#bufferConstants = bufferConstants;
  }

  isAvailable() {
    return !!(this.#sharedBuffer && this.#bufferConstants);
  }

  // Header layout (Uint32 indices from the slot start), shm_audio_buffer.
  static #IDX_ENABLED     = 0;
  static #IDX_SAMPLE_RATE = 1;
  static #IDX_CHANNELS    = 2;
  static #IDX_CAPACITY    = 3;
  static #IDX_WPOS_LOW    = 4;
  static #IDX_WPOS_HIGH   = 5;
  static #HEADER_U32      = 8;

  #slotOffset() {
    const bc = this.#bufferConstants;
    return this.#ringBufferBase + bc.SHM_AUDIO_START + this.#slot * bc.SHM_AUDIO_SLOT_SIZE;
  }

  #header() {
    return new Uint32Array(this.#sharedBuffer, this.#slotOffset(), AudioCapture.#HEADER_U32);
  }

  #writerPosition(h) {
    const high = Atomics.load(h, AudioCapture.#IDX_WPOS_HIGH);
    const low = Atomics.load(h, AudioCapture.#IDX_WPOS_LOW);
    return high * 0x100000000 + low;
  }

  /** Whether the tap is live: the device has this direction. */
  isLive() {
    if (!this.isAvailable()) return false;
    return Atomics.load(this.#header(), AudioCapture.#IDX_ENABLED) === 1;
  }

  /** Begin: everything the tap writes from now on is the capture. */
  start() {
    if (!this.isAvailable()) throw new Error('AudioCapture not initialized');
    this.#startPosition = this.#writerPosition(this.#header());
  }

  /** End the capture and return it. */
  stop() {
    const out = this.read();
    this.#startPosition = null;
    return out;
  }

  /**
   * The frames written since start() — or, with no capture running, the
   * newest ring's worth. Deinterleaved into per-channel arrays; `lost` is
   * how many frames the ring overwrote before they were read.
   */
  read() {
    if (!this.isAvailable()) throw new Error('AudioCapture not initialized');
    const bc = this.#bufferConstants;
    const h = this.#header();
    const sampleRate = h[AudioCapture.#IDX_SAMPLE_RATE];
    const channels = h[AudioCapture.#IDX_CHANNELS];
    const capacity = h[AudioCapture.#IDX_CAPACITY];
    const writer = this.#writerPosition(h);
    const from = this.#startPosition ?? Math.max(0, writer - capacity);
    let lost = 0;
    let start = from;
    if (writer - start > capacity) { lost = writer - start - capacity; start = writer - capacity; }
    const frames = Math.max(0, writer - start);
    const data = new Float32Array(this.#sharedBuffer, this.#slotOffset() + bc.SHM_AUDIO_HEADER_SIZE,
                                  capacity * channels);
    const out = Array.from({ length: channels }, () => new Float32Array(frames));
    let at = start % capacity;
    for (let f = 0; f < frames; f++) {
      for (let c = 0; c < channels; c++) out[c][f] = data[at * channels + c];
      at = (at + 1) % capacity;
    }
    return {
      sampleRate, channels, frames, lost,
      channelData: out,
      // The names a stereo reader has always used.
      left: out[0] ?? new Float32Array(0),
      right: out[1] ?? null,
    };
  }

  /** Whether a capture is running. */
  isEnabled() {
    return this.#startPosition !== null;
  }

  /** Frames captured so far, or written in all when no capture is running. */
  getFrameCount() {
    if (!this.isAvailable()) return 0;
    const writer = this.#writerPosition(this.#header());
    return this.#startPosition === null ? writer : writer - this.#startPosition;
  }

  /** The longest capture the ring holds, in seconds. */
  getMaxDuration() {
    if (!this.#bufferConstants) return 0;
    const bc = this.#bufferConstants;
    return bc.SHM_AUDIO_FRAMES / (bc.SHM_AUDIO_SAMPLE_RATE || 48000);
  }
}
