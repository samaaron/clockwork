// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron

/**
 * ring.js — clockwork's message rings, from JavaScript.
 *
 * A thread outside the engine does four things to the rings: it writes
 * ingress, drains the two egress rings, and watches ingress. This is the code
 * for those four, so that such a thread runs nothing compiled and needs no
 * stack in the engine's memory (docs/THREADS.md).
 *
 *
 * ── A SECOND IMPLEMENTATION, HELD TO THE FIRST ──────────────────────────────
 *
 * The format is src/ring/ring.h's, and what follows restates, step for step,
 * RingBufferWriter.h (writing), lanes/ring_drain.h (reading), and from
 * clockwork_client.cpp the poll (reading egress) and the tap (watching).
 * A mistake in ring code does not fail
 * loudly — it tears one message, once, under load — so this copy is held to
 * the C twice over:
 *
 *   test/ring_wire.test.mjs   replays the corpus the C++ replays
 *                             (test/fixtures/ring_wire.txt), byte for byte
 *   test/ring_cross.test.mjs  boots the engine and runs this and the
 *                             compiled C on the same rings
 *
 * A change here that is not made there, or the reverse, fails one of them.
 *
 *
 * ── A RING, AS THESE FUNCTIONS TAKE ONE ─────────────────────────────────────
 *
 *   bytes     a Uint8Array over exactly the ring's bytes
 *   view      a DataView over the same bytes, for the header words
 *   words     the Int32Array the cursors below are indices into
 *   head      where the next frame goes             (writers move it)
 *   tail      where free space ends                 (the consumer moves it)
 *   read      where the walk has got to             (egress rings only)
 *   sequence  the next frame's number               (writers take from it)
 *   lock      the writers' turn-taking word         (rings with several)
 *
 * ringAt() builds one; engineRings() builds the engine's three from its
 * arena table.
 *
 * Writing and walking allocate nothing per message. A frame is reported as an
 * offset and a length into `bytes`, which the writer will reuse, so a caller
 * that keeps a frame copies it.
 */

import {
    IN_HEAD, IN_TAIL, IN_SEQUENCE, IN_WRITE_LOCK,
    OUT_HEAD, OUT_TAIL, OUT_SEQUENCE, OUT_READ,
    NRT_OUT_HEAD, NRT_OUT_TAIL, NRT_OUT_SEQUENCE, NRT_OUT_READ,
} from './ring_cursor_offsets.js';

// src/ring/ring.h
export const MESSAGE_MAGIC = 0xDEADBEEF;
export const PADDING_MAGIC = 0xBADDCAFE;   // end-of-ring marker; the next frame is at 0
export const HEADER_BYTES = 16;            // magic, length, sequence, source id

/**
 * A ring over `size` bytes of `buffer` at `offset`, with its cursors at the
 * given indices of `words`.
 */
export function ringAt(buffer, offset, size, words, { head, tail, read, sequence, lock }) {
    return {
        bytes: new Uint8Array(buffer, offset, size),
        view: new DataView(buffer, offset, size),
        words, head, tail, read, sequence, lock,
    };
}

/**
 * The engine's three rings, where its arena table says they are.
 *
 * The table also states the frame format, and a format this file does not
 * write is refused here, rather than written over.
 */
export function engineRings(buffer, arenaBase, constants) {
    if (constants.MESSAGE_MAGIC !== MESSAGE_MAGIC
        || constants.PADDING_MAGIC !== PADDING_MAGIC
        || constants.MESSAGE_HEADER_SIZE !== HEADER_BYTES) {
        throw new Error('the engine\'s ring format is not the one js/lib/ring.js reads and writes');
    }
    const words = new Int32Array(buffer);
    const control = arenaBase + constants.RING_CURSORS_START;
    const at = (byteOffset) => (control + byteOffset) / 4;
    const ring = (start, size, cursors) => ringAt(buffer, arenaBase + start, size, words, cursors);
    return {
        ingress: ring(constants.IN_RING_START, constants.IN_RING_SIZE, {
            head: at(IN_HEAD), tail: at(IN_TAIL), sequence: at(IN_SEQUENCE), lock: at(IN_WRITE_LOCK),
        }),
        egress: ring(constants.RT_OUT_RING_START, constants.RT_OUT_RING_SIZE, {
            head: at(OUT_HEAD), tail: at(OUT_TAIL), read: at(OUT_READ), sequence: at(OUT_SEQUENCE),
        }),
        nrtEgress: ring(constants.NRT_OUT_RING_START, constants.NRT_OUT_RING_SIZE, {
            head: at(NRT_OUT_HEAD), tail: at(NRT_OUT_TAIL), read: at(NRT_OUT_READ), sequence: at(NRT_OUT_SEQUENCE),
        }),
    };
}

// ── Writing ─────────────────────────────────────────────────────────────────

/**
 * Put one frame on a ring. RingBufferWriter::write.
 *
 * Frames never wrap: one that does not fit before the end of the ring leaves
 * a padding marker there and starts again at 0, which needs that much room
 * before the tail. So total free space is not enough on its own; the room has
 * to be in one piece.
 *
 * On a ring with a `lock`, writers take turns on it. A writer stopped while
 * holding it stops the ring for every writer, as it does in the C
 * (clockwork_client.h, ONE CLIENT).
 *
 * @param {object}     ring
 * @param {Uint8Array} payload   the frame's bytes; may be empty
 * @param {number}     sourceId  carried in the header (the origin)
 * @returns {boolean} false when it does not fit; the ring is then unchanged
 */
export function ringWrite(ring, payload, sourceId = 0) {
    const { bytes, view, words } = ring;
    const size = bytes.length;
    const total = HEADER_BYTES + payload.length;
    const aligned = (total + 3) & ~3;   // what the frame occupies; its header says `total`

    const locked = ring.lock !== undefined;
    if (locked) {
        // A moment at most: the holder is copying one frame.
        while (Atomics.compareExchange(words, ring.lock, 0, 1) !== 0) { /* its turn */ }
    }
    try {
        let h = Atomics.load(words, ring.head) >>> 0;
        const t = Atomics.load(words, ring.tail) >>> 0;

        // Free space, less one byte so that head == tail can only mean empty.
        const used = ((h - t + size) >>> 0) % size;
        const avail = size - used - 1;
        if (aligned > avail) return false;

        const toEnd = (size - h) >>> 0;
        if (aligned > toEnd) {
            const front = t > 0 ? t - 1 : 0;
            if (aligned > front) return false;
            // The padding marker, then zeros to the end. Written before the
            // head moves, so a reader never meets it half made.
            view.setUint32(h, PADDING_MAGIC, true);
            if (toEnd > 4) bytes.fill(0, h + 4, size);
            h = 0;
        }

        bytes.set(payload, h + HEADER_BYTES);

        // Taken now, not when the room was found, as the C takes it at commit.
        const sequence = Atomics.add(words, ring.sequence, 1) >>> 0;
        view.setUint32(h, MESSAGE_MAGIC, true);
        view.setUint32(h + 4, total, true);
        view.setUint32(h + 8, sequence, true);
        view.setUint32(h + 12, sourceId >>> 0, true);
        if (aligned > total) bytes.fill(0, h + total, h + aligned);   // no stale bytes inside a frame

        // The frame is published by the head moving, and only then: a reader
        // that sees the new head sees every byte written above.
        Atomics.store(words, ring.head, (h + aligned) % size);
        // Wake a reader parked on the head (sab_worker_loop.js). The C does
        // the same, for the same reason: only the writer knows it moved.
        Atomics.notify(words, ring.head);
        return true;
    } finally {
        if (locked) Atomics.store(words, ring.lock, 0);
    }
}

// ── Reading ─────────────────────────────────────────────────────────────────

/**
 * Walk a ring from a cursor, delivering each frame. clockwork_drain_ring.
 *
 * `from` is the cursor the walk advances, as { words, index }: the ring's own
 * tail for its consumer, its read cursor for an egress ring, or a tap's
 * private word. One consumer per cursor.
 *
 * The cursors and headers are shared memory and are not trusted. A cursor out
 * of range, a bad magic word, or a length that runs past what is published
 * or past the end of the ring drops everything pending (the cursor jumps to
 * the head) and is counted, rather than read byte by byte or waited on.
 *
 * A frame whose payload is empty is consumed without being delivered.
 *
 * @param {object}   ring
 * @param {{words: Int32Array, index: number}} from
 * @param {{lastSeq: number, stop?: string}} state  the consumer's; `stop`
 *   says why the walk ended: 'empty', 'max', 'bad-cursor', 'bad-magic' or
 *   'bad-length'
 * @param {number}   max      frames to consume at most; 0 for no limit
 * @param {(origin: number, offset: number, length: number, sequence: number) => void} onFrame
 *   `offset` is into ring.bytes, valid until the cursor passes it
 * @param {object}   [metrics]  { view, received, bytes, corrupted, seqGaps }:
 *   a Uint32Array and the indices in it to count into; any left out is not
 *   counted. Gaps in the sequence are tracked only when seqGaps is given.
 * @returns {number} frames consumed
 */
export function ringDrain(ring, from, state, max, onFrame, metrics = null) {
    const { bytes, view, words } = ring;
    const size = bytes.length;
    const cw = from.words, ci = from.index;
    const m = metrics ?? NO_METRICS;
    const count = (index, n) => { if (index !== undefined) Atomics.add(m.view, index, n); };

    let stop = 'empty';
    let consumed = 0;
    while (max === 0 || consumed < max) {
        const head = Atomics.load(words, ring.head);
        const tail = Atomics.load(cw, ci);
        if (head === tail) break;

        if ((head >>> 0) >= size) {   // the writer's, so not ours to repair
            count(m.corrupted, 1);
            stop = 'bad-cursor';
            break;
        }
        if ((tail >>> 0) >= size) {
            count(m.corrupted, 1);
            Atomics.store(cw, ci, head);
            stop = 'bad-cursor';
            break;
        }

        const ut = tail >>> 0;
        const avail = ((head >>> 0) - ut + size) % size;
        const toEnd = size - ut;

        if (toEnd < 4 || avail < 4) {   // not even a magic word: cursor damage
            count(m.corrupted, 1);
            Atomics.store(cw, ci, head);
            stop = 'bad-magic';
            break;
        }

        const magic = view.getUint32(ut, true);
        if (magic === PADDING_MAGIC) {
            // The writer went back to 0, where the next frame already is. A
            // marker AT 0 cannot be written, and one the writer did not wrap
            // for (head still past it) would send the walk round the frames
            // before it again, every pass: both are damage, not a loop.
            if (ut === 0 || (head >>> 0) > ut) {
                count(m.corrupted, 1);
                Atomics.store(cw, ci, head);
                stop = 'bad-magic';
                break;
            }
            Atomics.store(cw, ci, 0);
            continue;
        }
        if (magic !== MESSAGE_MAGIC) {
            count(m.corrupted, 1);
            Atomics.store(cw, ci, head);
            stop = 'bad-magic';
            break;
        }
        if (toEnd < HEADER_BYTES) {
            count(m.corrupted, 1);
            Atomics.store(cw, ci, head);
            stop = 'bad-length';
            break;
        }

        const length = view.getUint32(ut + 4, true);
        const sequence = view.getUint32(ut + 8, true);
        const origin = view.getUint32(ut + 12, true);
        // The length is held to the ring BEFORE it is rounded: rounding a
        // length within four of 2^32 wraps (int32 arithmetic) to a footprint
        // of nothing, which every fit check below would pass while the
        // payload claimed 4 GB.
        if (length < HEADER_BYTES || length > size) {
            count(m.corrupted, 1);
            Atomics.store(cw, ci, head);
            stop = 'bad-length';
            break;
        }
        const footprint = ((length + 3) & ~3) >>> 0;
        // A frame claiming more than is published is damage too: the writer
        // publishes whole frames, so waiting for the rest would wait for ever.
        if (footprint > size || footprint > toEnd || footprint > avail) {
            count(m.corrupted, 1);
            Atomics.store(cw, ci, head);
            stop = 'bad-length';
            break;
        }

        const payload = length - HEADER_BYTES;
        // Counted before delivery, so whoever is handed the frame sees it counted.
        count(m.received, 1);
        count(m.bytes, payload);
        if (payload > 0) onFrame(origin, ut + HEADER_BYTES, payload, sequence);

        Atomics.store(cw, ci, (ut + footprint) % size);

        if (m.seqGaps !== undefined) {
            const seq = sequence | 0;
            if (state.lastSeq >= 0) {
                const expected = (state.lastSeq + 1) & 0x7FFFFFFF;
                if (seq !== expected) {
                    const gap = (seq - expected + 0x80000000) & 0x7FFFFFFF;
                    if (gap > 0 && gap < 1000) count(m.seqGaps, gap);   // larger is a reset, not loss
                }
            }
            state.lastSeq = seq;
        }

        consumed++;
    }
    if (stop === 'empty' && max !== 0 && consumed >= max) stop = 'max';
    state.stop = stop;
    return consumed;
}

const NO_METRICS = Object.freeze({});

/**
 * Give an egress ring's writer back the space read so far:
 * clockwork_ring_give_back.
 *
 * Egress is read against `read` and freed against `tail`. A consumer that
 * hands frames out where they lie moves the tail up only when the caller is
 * done with them, which is the next time it reads.
 */
export function ringGiveBack(ring) {
    Atomics.store(ring.words, ring.tail, Atomics.load(ring.words, ring.read));
}

// An egress frame is [route:u32][osc].
export const ROUTE_BYTES = 4;

/**
 * The engine's replies, from both egress rings. clockwork_client_poll.
 *
 * The audio thread answers on one ring and the NRT side (subsystems, state
 * changes, broadcasts) on the other. A reader does not care which: it takes
 * from both, RT first, and each ring keeps its own sequence numbers.
 *
 * Each ring's read cursor is shared by everything that reads it, so a ring
 * has one reader at a time. On the web that is the reply pump
 * (osc_in_pump.js), or the worklet itself when there is no shared memory.
 */
export class EgressReader {
    #rings;
    #from;
    #states;
    #metrics;
    #taken = 0;

    /**
     * @param {{egress: object, nrtEgress: object}} rings  as engineRings() gives them
     * @param {object} [metrics]  where ringDrain counts (see there); the
     *   engine's own counters, so a reader outside the engine reports what
     *   the engine's client would
     */
    constructor(rings, metrics = null) {
        this.#rings = [rings.egress, rings.nrtEgress];
        this.#from = this.#rings.map((r) => ({ words: r.words, index: r.read }));
        this.#states = this.#rings.map(() => ({ lastSeq: -1, stop: 'empty' }));
        this.#metrics = metrics;
    }

    /**
     * Deliver what the engine has said since the last poll.
     *
     * `bytes` is a view into the ring, the OSC with the route word off. It
     * stays the reader's until the next poll, which is when its space goes
     * back to the writer: a caller keeping it longer copies it.
     *
     * @param {number} max  frames at most; 0 for no limit
     * @param {(bytes: Uint8Array, origin: number, route: number, sequence: number) => void} onMessage
     * @returns {number} messages delivered
     */
    poll(max, onMessage) {
        // The last poll's space goes back first, this poll's not yet.
        for (const ring of this.#rings) ringGiveBack(ring);

        this.#taken = 0;
        for (let i = 0; i < this.#rings.length; i++) {
            const ring = this.#rings[i];
            if (ring.bytes.length === 0) continue;
            if (i > 0 && max !== 0 && this.#taken >= max) break;
            const room = max === 0 ? 0 : max - this.#taken;
            ringDrain(ring, this.#from[i], this.#states[i], room, (origin, offset, length, sequence) => {
                if (length < ROUTE_BYTES) return;   // nothing after the route: consumed, not delivered
                this.#taken++;
                onMessage(ring.bytes.subarray(offset + ROUTE_BYTES, offset + length),
                          origin, ring.view.getUint32(offset, true), sequence);
            }, this.#metrics);
        }
        return this.#taken;
    }
}

// ── Watching ────────────────────────────────────────────────────────────────

/**
 * A reader that takes nothing. The tap in clockwork_client.cpp.
 *
 * It walks the same frames with the same code, from a cursor of its own, and
 * starts wherever the writer is when it opens. Writers measure free space
 * against the ring's real consumer and know nothing of a tap, so a tap that
 * falls behind is written over. That is expected: the walk's recovery jumps
 * it to the newest frames, and the sequence gap says how many went by.
 */
export class RingTap {
    #ring;
    #from;
    #state = { lastSeq: -1, stop: 'empty' };
    #gaps = new Uint32Array(1);
    #metrics;
    #missed = 0;

    constructor(ring) {
        this.#ring = ring;
        this.#from = { words: new Int32Array(1), index: 0 };
        this.#from.words[0] = Atomics.load(ring.words, ring.head);
        // Only gaps. The consumer's counts are the consumer's; a watcher
        // adding to them would report traffic twice.
        this.#metrics = { view: this.#gaps, seqGaps: 0 };
    }

    /**
     * Deliver what has been written since this tap last looked.
     * @returns {number} frames delivered
     */
    poll(max, onFrame) {
        let delivered = 0;
        this.#gaps[0] = 0;
        ringDrain(this.#ring, this.#from, this.#state, max, (origin, offset, length, sequence) => {
            delivered++;
            onFrame(origin, offset, length, sequence);
        }, this.#metrics);
        this.#missed += this.#gaps[0];
        return delivered;
    }

    /** Frames written past this tap before it read them, since it opened. */
    get missed() { return this.#missed; }
}
