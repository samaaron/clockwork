// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025 Sam Aaron

/**
 * The engine's sample clock, read where the audio thread publishes it into the arena
 * (ClockworkClock::publishSampleClock; the region's layout is src/shared_memory.h SAMPLE_CLOCK_*): which frame the
 * engine had reached at the start of the last block it rendered, and that frame's NTP time at the DAC.
 *
 * It is the one reading of the engine's time a thread without the audio context can take: a worker cannot see an
 * AudioContext, and the time a page passes one goes stale. This is the audio thread's own, once a block: it stands
 * still while the audio does, and after a reload it is the new engine's from its first block.
 *
 * Seqlocked, as the C++ reader (read_sample_clock) is: an odd seq is a write under way; a read is kept only if the
 * seq has not moved across it.
 */

// Byte offsets within the region (src/shared_memory.h)
export const SAMPLE_CLOCK_SEQ = 0;            // u32 seqlock (odd = mid-update; 0 = never published)
export const SAMPLE_CLOCK_SAMPLE_RATE = 4;    // u32
export const SAMPLE_CLOCK_ENGINE_FRAMES = 8;  // u64 engine frames at block start
export const SAMPLE_CLOCK_DAC_NTP = 16;       // f64 (as u64 bits) NTP seconds when that frame reaches the DAC
export const SAMPLE_CLOCK_OUT_LATENCY = 24;   // u32 device output latency, frames

/**
 * A reader over the region at `byteOffset` in `buffer` (the arena's SharedArrayBuffer, or a wasm memory's buffer).
 * Returns read(into = {}): `into`, filled with `{ frames, sampleRate, outputLatencyFrames, dacNtp, renderNtp }`, or
 * null before the engine has published a block. renderNtp is the block's time on the engine's clock, the one bundles
 * are stamped on. A caller that reads often (the audio thread, a worker's scheduler) passes the same object each time,
 * and nothing is allocated.
 *
 * `buffer` may be a function returning the buffer, for a wasm memory that can grow (its buffer is replaced when it
 * does): the views are made again when it changes.
 */
export function sampleClockReader(buffer, byteOffset) {
  const source = typeof buffer === "function" ? buffer : () => buffer;
  let at = null, u32 = null;
  // the f64 travels as its bits: read as two 32-bit halves (the seqlock makes the pair one reading) and put back here.
  // No 64-bit loads: those give BigInts, and a BigInt is an allocation
  const halves = new Uint32Array(2), f64 = new Float64Array(halves.buffer);
  return function read(into = {}) {
    const b = source();
    if (!b) return null;
    if (b !== at) {
      at = b;
      u32 = new Uint32Array(b, byteOffset, 8);
    }
    for (let tries = 0; tries < 8; tries++) {
      const s0 = Atomics.load(u32, SAMPLE_CLOCK_SEQ >> 2);
      if (s0 === 0) return null;
      if (s0 & 1) continue;
      const sampleRate = Atomics.load(u32, SAMPLE_CLOCK_SAMPLE_RATE >> 2);
      const framesLo = Atomics.load(u32, SAMPLE_CLOCK_ENGINE_FRAMES >> 2), framesHi = Atomics.load(u32, (SAMPLE_CLOCK_ENGINE_FRAMES >> 2) + 1);
      halves[0] = Atomics.load(u32, SAMPLE_CLOCK_DAC_NTP >> 2);   // little-endian, as the engine's memory is
      halves[1] = Atomics.load(u32, (SAMPLE_CLOCK_DAC_NTP >> 2) + 1);
      const outputLatencyFrames = Atomics.load(u32, SAMPLE_CLOCK_OUT_LATENCY >> 2);
      if (Atomics.load(u32, SAMPLE_CLOCK_SEQ >> 2) !== s0) continue;
      if (!sampleRate) return null;
      into.frames = framesHi * 4294967296 + framesLo;
      into.sampleRate = sampleRate;
      into.outputLatencyFrames = outputLatencyFrames;
      into.dacNtp = f64[0];
      into.renderNtp = into.dacNtp - outputLatencyFrames / sampleRate;
      return into;
    }
    return null;
  };
}
