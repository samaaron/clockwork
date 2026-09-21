// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/**
 * The egress reader, as something a host can run wherever it likes.
 *
 * Draining the OUT ring is a dedicated worker's job by default (workers/osc_in_worker.js), which pumps every
 * frame to the main thread for the client to fan out. That is the right shape when the main thread is where
 * the frames are wanted. It is the wrong shape when they are wanted somewhere else: an event that entered the
 * engine from the main thread then comes back to it only to be forwarded on again, and the hop is pure cost.
 *
 * So the pump is a function. A host that consumes the egress in a worker of its own — a language runtime, say,
 * that wants inbound MIDI where it can act on it — runs this there and forwards to the main thread whatever
 * the client still needs. The ring has ONE reader (a single OUT_TAIL, and a sequence gap means it lapped), so
 * whoever runs it owns the whole stream and must pass on the rest.
 *
 * The draining itself is clockwork_client_poll through this module's own client instance (lib/wasm_client.js),
 * so there is one implementation of walking the ring rather than two that can disagree.
 */
import * as MetricsOffsets from './metrics_offsets.js';
import { calculateOutControlIndices } from './control_offsets.js';
import { getCurrentNTPFromPerformance as getCurrentNTP } from './osc_classifier.js';
import { runSabWorker } from './sab_worker_loop.js';

/**
 * Run the egress reader.
 *
 * @param {object} options
 * @param {(messages: {oscData: Uint8Array, sequence: number, timestamp: number}[]) => void} options.onFrames
 *   what to do with each batch drained off the ring.
 * @param {Worker|MessagePort|DedicatedWorkerGlobalScope} [options.endpoint]
 *   where the init/start/stop protocol is spoken; the worker's own global by default.
 * @param {string} [options.name] what it calls itself in a log line.
 */
export function runOscInPump({ onFrames, endpoint, name = 'OSCInPump' } = {}) {
  // A gap in the sequence means the ring lapped this reader. The library counts the frames that went by; this
  // counter is the one the metrics HUD reads, so it is kept here where the numbers it reports are.
  let lastSequenceReceived = -1;

  function readOscMessages(ctx) {
    const { client, metricsView } = ctx;
    if (!client) return [];
    const messages = [];
    client.poll((bytes, origin, route, sequence) => {
      if (lastSequenceReceived >= 0) {
        const expectedSeq = (lastSequenceReceived + 1) & 0xFFFFFFFF;
        if (sequence !== expectedSeq) {
          const dropped = (sequence - expectedSeq + 0x100000000) & 0xFFFFFFFF;
          if (dropped < 1000) {   // Sanity check
            console.error(`[${name}] Detected`, dropped, 'dropped messages (expected seq', expectedSeq, 'got', sequence, ')');
            if (metricsView) Atomics.add(metricsView, MetricsOffsets.OSC_IN_DROPPED_MESSAGES, dropped);
          }
        }
      }
      lastSequenceReceived = sequence;

      // `bytes` is a view into the ring, which the engine will write over.
      // These entries outlive this call, so they take a copy.
      messages.push({ oscData: bytes.slice(), sequence, timestamp: getCurrentNTP() });
    });
    return messages;
  }

  runSabWorker({
    name,
    calculateControlIndices: calculateOutControlIndices,
    headIndex: (idx) => idx.OUT_HEAD,
    tailIndex: (idx) => idx.OUT_TAIL,
    readMessages: readOscMessages,
    postResults: onFrames,
    endpoint,
  });
}
