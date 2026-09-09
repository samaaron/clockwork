// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_disk.h — the two disk endpoints, in the shape every endpoint takes.
 *
 * A source worker reads a WAV file and calls clockwork_port_produce; a sink worker
 * calls clockwork_port_consume and writes a WAV file. Neither appears anywhere in
 * clockwork_ports.h, and clockwork_ports.h appears nowhere in their reasoning beyond the
 * four calls they make — which is the property the substrate exists to have.
 * A network peer, a plugin, a capture buffer or Link's aux publish would each
 * get a header like this one and change nothing on the other side.
 *
 * WAV is parsed and written by hand, in Rust: 16-bit PCM and 32-bit float in,
 * 32-bit float out. That is narrower than clockwork_audio_file.h, which reads
 * every format the project supports, and it stays that way on purpose — this
 * is a streaming endpoint that touches the file as the audio thread drains it,
 * so it reads a frame at a time from disk rather than decoding a whole file
 * into memory the way a sample loader does. The two answer different
 * questions, and a test in test_ports.cpp reads what this writes with the
 * other one, so the formats cannot drift apart unnoticed.
 *
 * NO RATE CONVERSION. A 44.1 kHz file streamed into a 48 kHz session plays
 * fast. clockwork_disk_source_sample_rate() tells a caller the file's rate so it can
 * decide; deciding for it is docs/PORTS.md open question 2, which is not an
 * endpoint's to answer.
 *
 * A handle is an opaque pointer, not a slot: an endpoint is addressed only by
 * its owner, never enumerated, and never outlives it, so none of the reasons a
 * PORT has a slot apply. A second registry here would be the mistake ports
 * were built to stop.
 */
#ifndef CLOCKWORK_DISK_H
#define CLOCKWORK_DISK_H

#include <stdint.h>
#include "clockwork_ports.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkDiskSource ClockworkDiskSource;
typedef struct ClockworkDiskSink   ClockworkDiskSink;

/*
 * Stream `path` into `port`, which must be an open SOURCE. Starts a worker
 * thread that keeps the ring as full as the audio thread lets it. NULL if the
 * file will not open, is not a WAV this reader understands, or the port is
 * closed — the failure is at open time, on the calling thread, rather than a
 * silence to be diagnosed later.
 *
 * The file's channel count need not match the port's: missing channels are
 * silence, extra ones are discarded.
 */
ClockworkDiskSource* clockwork_disk_source_open(const char* path, ClockworkPort port);

/* Stop the worker and free the handle. The PORT is not closed: a port outlives
   its endpoint, which is what lets one be swapped under a running stream. */
void clockwork_disk_source_close(ClockworkDiskSource* source);

uint64_t clockwork_disk_source_frames(const ClockworkDiskSource* source);         /* in the file  */
uint32_t clockwork_disk_source_sample_rate(const ClockworkDiskSource* source);    /* the file's   */
uint64_t clockwork_disk_source_frames_produced(const ClockworkDiskSource* source);
/* Non-zero once the whole file has been handed over. The port then underruns
   to silence, which is how a stream ends. */
int      clockwork_disk_source_eof(const ClockworkDiskSource* source);

/*
 * Drain `port` (an open SINK) into a new 32-bit float WAV at `path`.
 * `channels` is what the FILE gets; the port's count is what arrives, and a
 * mismatch is handled the same way everywhere else here handles one.
 */
ClockworkDiskSink* clockwork_disk_sink_open(const char* path, ClockworkPort port,
                                uint32_t sample_rate, uint32_t channels);

/* Drain what is left, patch the header with the real length, join the worker,
   free the handle. The FILE IS ONLY COMPLETE AFTER THIS RETURNS. */
void clockwork_disk_sink_close(ClockworkDiskSink* sink);

uint64_t clockwork_disk_sink_frames(const ClockworkDiskSink* sink);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_DISK_H */
