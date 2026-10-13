// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * OSC OUT Log Worker (SAB Mode) — shows what was SENT to the engine.
 *
 * It watches the IN ring rather than draining it. The engine is that ring's
 * consumer, and a second reader taking from it would be eating the engine's
 * input, so this opens a TAP (lib/ring.js, the tap of src/clockwork_client.h
 * restated): a cursor of its own over the same bytes, which takes nothing.
 *
 * Being overtaken is normal and expected. Writers reserve space against the
 * ENGINE's position and know nothing about a watcher, so a logger that falls
 * behind has its unread bytes overwritten. The tap resynchronises to the
 * newest traffic and counts what went past; that recovery used to be hand-
 * written here, peeking at frame magic to guess whether it had been lapped.
 */

import { calculateInControlIndices } from '../lib/control_offsets.js';
import { getCurrentNTPFromPerformance as getCurrentNTP } from '../lib/osc_classifier.js';
import { runSabWorker } from '../lib/sab_worker_loop.js';
import { RingTap } from '../lib/ring.js';

let tap = null;
let reportedMissed = 0;

/**
 * Read whatever has been sent since this worker last looked.
 */
function readLogMessages(ctx) {
    if (!tap) return [];

    const entries = [];
    const bytes = ctx.rings.ingress.bytes;

    tap.poll(0, (origin, offset, length, sequence) => {
        // Posted to the main thread, so they outlive the ring's copy.
        entries.push({
            sourceId: origin,
            oscData: bytes.slice(offset, offset + length),
            sequence,
            timestamp: getCurrentNTP(),
        });
    });

    // Say so once per burst rather than per frame: a logger that cannot keep
    // up would otherwise spend its time reporting that it cannot keep up.
    const missed = tap.missed;
    if (missed > reportedMissed) {
        if (__DEV__) {
            console.warn(`[OSCOutLogWorker] ${missed - reportedMissed} messages`
                + ` were overwritten before they could be logged`
                + ` (${missed} since start)`);
        }
        reportedMissed = missed;
    }

    return entries;
}

runSabWorker({
    name: 'OSCOutLogWorker',
    calculateControlIndices: calculateInControlIndices,
    headIndex: (idx) => idx.IN_HEAD,
    // A watcher keeps its cursor inside the tap, so there is no shared tail to
    // idle against; the loop waits on the head standing still instead.
    tailIndex: null,
    readMessages: readLogMessages,
    postResults: (entries) => self.postMessage({ type: 'oscLog', entries }),
    initMetrics: false,
    onInit: (ctx) => {
        // Watching starts now: a tap opens at the ring's present head, so the
        // log shows what happens from here rather than replaying the past. A
        // new tap after a reload counts what it misses from zero again.
        tap = new RingTap(ctx.rings.ingress);
        reportedMissed = 0;
        if (__DEV__) console.log('[OSCOutLogWorker] watching the ingress ring');
    },
});
