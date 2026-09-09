// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * OSC IN Worker — drains the OUT ring buffer and forwards every reply to the
 * main thread. The main thread fans out to registered callbacks; this worker
 * is a dumb pump (Atomics.wait for instant wake, copy out, postMessage).
 *
 * The draining itself is clockwork_client_poll, reached through this worker's
 * own module instance (js/lib/wasm_client.js). What used to be here — walking
 * the frames, checking the magic, following the padding marker at the end of
 * the ring, publishing the new tail — is the engine's own code now, so there
 * is one implementation of it rather than two that can disagree.
 */

import * as MetricsOffsets from '../lib/metrics_offsets.js';
import { calculateOutControlIndices } from '../lib/control_offsets.js';
import { getCurrentNTPFromPerformance as getCurrentNTP } from '../lib/osc_classifier.js';
import { runSabWorker } from '../lib/sab_worker_loop.js';

// Sequence tracking for dropped message detection
let lastSequenceReceived = -1;

/**
 * Take whatever replies are waiting on the OUT ring.
 */
function readOscMessages(ctx) {
    const { client, metricsView } = ctx;
    if (!client) return [];

    const messages = [];

    client.poll((bytes, origin, route, sequence) => {
        // A gap in the sequence means the ring lapped this reader. The library
        // counts the frames that went by; this counter is the one the metrics
        // HUD reads, so it is kept here where the numbers it reports are.
        if (lastSequenceReceived >= 0) {
            const expectedSeq = (lastSequenceReceived + 1) & 0xFFFFFFFF;
            if (sequence !== expectedSeq) {
                const dropped = (sequence - expectedSeq + 0x100000000) & 0xFFFFFFFF;
                if (dropped < 1000) { // Sanity check
                    console.error('[OSCInWorker] Detected', dropped, 'dropped messages (expected seq', expectedSeq, 'got', sequence, ')');
                    if (metricsView) Atomics.add(metricsView, MetricsOffsets.OSC_IN_DROPPED_MESSAGES, dropped);
                }
            }
        }
        lastSequenceReceived = sequence;

        // `bytes` is a view into the ring, which the engine will write over.
        // These entries outlive this call, so they take a copy.
        messages.push({
            oscData: bytes.slice(),
            sequence,
            timestamp: getCurrentNTP(),
        });
    });

    return messages;
}

runSabWorker({
    name: 'OSCInWorker',
    calculateControlIndices: calculateOutControlIndices,
    headIndex: (idx) => idx.OUT_HEAD,
    tailIndex: (idx) => idx.OUT_TAIL,
    readMessages: readOscMessages,
    postResults: (messages) => self.postMessage({ type: 'messages', messages }),
});
