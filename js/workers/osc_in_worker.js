// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron

/**
 * OSC IN Worker — a worker whose whole job is to drain the OUT ring buffer and
 * forward every reply to the main thread. The main thread fans out to the
 * registered callbacks; this worker is a dumb pump (Atomics.wait for an instant
 * wake, copy out, postMessage).
 *
 * The reading itself is lib/osc_in_pump.js — a function rather than a worker, so
 * a host that wants the frames somewhere else can run the same reader there. An
 * event that entered the engine from the main thread and is acted on in a worker
 * would otherwise come back to the main thread only to be forwarded on again.
 * This file is that reader in its default place, and stays what the transport
 * spawns unless a host names its own (transport/sab_transport.js).
 */

import { runOscInPump } from '../lib/osc_in_pump.js';

runOscInPump({
    name: 'OSCInWorker',
    onFrames: (messages) => self.postMessage({ type: 'messages', messages }),
});
