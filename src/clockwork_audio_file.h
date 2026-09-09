/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2026 Sam Aaron
 *
 * clockwork_audio_file.h — audio files, in and out, for clients of clockwork.
 *
 * clockwork_client.h moves opaque bytes between a client and an engine. This
 * moves audio between a file and a client, and it sits beside that header for
 * the same reason: every consumer needs it, each was writing its own, and the
 * copies disagreed. A GUI loading a sample, a recorder writing a session, a
 * browser tab that has fetched an ArrayBuffer and a command-line tool all want
 * the same two verbs, so they are declared once.
 *
 * The engine below is unaware of any of this. Clockwork's contract is binary
 * blobs; a SAMPLE is a guest's idea, and a guest that wants one asks its
 * client to decode a file and send the frames down. That direction is
 * deliberate — it keeps codec code out of the audio thread and out of the
 * substrate, and it is why this is a client header.
 *
 *
 * ── EVERY PLATFORM, THE SAME FILES ──────────────────────────────────────────
 *
 * The decoder is compiled into clockwork rather than found on the host, so a
 * file that loads on one platform loads on all of them, including WebAssembly
 * where there is no system codec to find and where a browser's own decoder
 * refuses AIFF. What a build reads is a property of the source, checkable
 * here, and identical everywhere:
 *
 *   WAV    PCM 8/16/24/32, IEEE float 32/64, A-law, mu-law, ADPCM
 *   AIFF   PCM 8/16/24/32 and float, big-endian
 *   W64    Sony Wave64, for takes past 4 GB
 *   RF64   the broadcast 64-bit RIFF
 *   FLAC   any bit depth up to 32, any of the standard rates
 *   OGG    Ogg Vorbis
 *   MP3    MPEG-1/2 Audio Layer III, any of the common rates
 *
 * Decoding hands back interleaved 32-bit float, whatever the file held. That
 * is the one representation every caller here wants, and converting once
 * inside the library beats each caller converting differently.
 *
 *
 * ── WHAT IS WRITTEN ─────────────────────────────────────────────────────────
 *
 * WAV and FLAC: the two lossless formats, so a session recorded by clockwork
 * is a session clockwork loads, sample for sample. The lossy formats are read
 * and not written — a recorder that re-encoded a take would be choosing to
 * lose part of it, and that is a decision for whatever tool a user reaches for
 * afterwards rather than one to make silently during a performance.
 *
 * Writing is a stream rather than a buffer: a recorder hands over frames as
 * they arrive and the header is patched with the true length at close, which
 * is what lets a take of unknown duration be written to disk as it happens.
 *
 *
 * ── LICENCE ─────────────────────────────────────────────────────────────────
 *
 * The whole path is permissive. Decoding is dr_wav, dr_flac and dr_mp3
 * (Unlicense or MIT-0, at the user's choice, under src/vendor/dr_libs) and
 * stb_vorbis (public domain or MIT, under src/vendor/stb). Encoding is this
 * repository's own: dr_wav writes the WAV, src/flac_encoder.h writes the FLAC.
 * Nothing here is copyleft, nothing here is fetched at build time, and nothing
 * here is a system dependency — a commercial embedder links clockwork and
 * ships.
 *
 *
 * ── THREADS ─────────────────────────────────────────────────────────────────
 *
 * Client threads only, as with the rest of the client ABI: decoding allocates
 * and writing touches a file, and neither belongs on an audio thread. One
 * writer handle is for one thread. Separate handles share nothing, so two
 * threads may decode at once.
 */
#ifndef CLOCKWORK_AUDIO_FILE_H
#define CLOCKWORK_AUDIO_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "clockwork_client.h"   /* ClockworkStatus, and its text */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Formats ─────────────────────────────────────────────────────────────────
 *
 * The container. Values are stable across versions; new containers append.
 */
typedef enum ClockworkAudioFormat {
    CLOCKWORK_AUDIO_FORMAT_UNKNOWN = 0,
    CLOCKWORK_AUDIO_FORMAT_WAV     = 1,
    CLOCKWORK_AUDIO_FORMAT_AIFF    = 2,
    CLOCKWORK_AUDIO_FORMAT_FLAC    = 3,
    CLOCKWORK_AUDIO_FORMAT_W64     = 4,
    CLOCKWORK_AUDIO_FORMAT_RF64    = 5,
    CLOCKWORK_AUDIO_FORMAT_OGG     = 6,   /* Ogg Vorbis; decoded, not written */
    CLOCKWORK_AUDIO_FORMAT_MP3     = 7    /* decoded, not written */
} ClockworkAudioFormat;

/* How samples sit in the file. Decoding converts away from these; encoding
 * chooses one. FLAC carries integer samples only, which is a fact about the
 * format rather than a limit of this implementation. A lossy format reports
 * COMPRESSED: the question "how many bits per sample" has no answer there. */
typedef enum ClockworkAudioEncoding {
    CLOCKWORK_AUDIO_ENCODING_UNKNOWN = 0,
    CLOCKWORK_AUDIO_ENCODING_PCM_S16 = 1,
    CLOCKWORK_AUDIO_ENCODING_PCM_S24 = 2,
    CLOCKWORK_AUDIO_ENCODING_PCM_S32 = 3,
    CLOCKWORK_AUDIO_ENCODING_FLOAT32 = 4,
    CLOCKWORK_AUDIO_ENCODING_COMPRESSED = 5
} ClockworkAudioEncoding;

/* ── What a file turned out to be ────────────────────────────────────────────
 *
 * Filled by probe and by decode. `struct_bytes` works as it does everywhere in
 * the client ABI: the caller sets it to sizeof(its own version) and the
 * library fills that much.
 */
typedef struct ClockworkAudioInfo {
    uint32_t struct_bytes;
    uint32_t format;         /* ClockworkAudioFormat */
    uint32_t encoding;       /* ClockworkAudioEncoding, as stored in the file */
    uint32_t channels;
    uint32_t sample_rate;    /* Hz, as the file declares it */
    uint32_t source_bits;    /* bits per sample in the file: 8, 16, 24, 32 */
    uint64_t frames;         /* frames, not samples: one frame is `channels` */
} ClockworkAudioInfo;

/* Seconds of audio, from an info a probe or decode filled. Zero when the file
 * declares no rate, so a caller dividing by it does not have to guard. */
double clockwork_audio_duration(const ClockworkAudioInfo* info);

/* ── Reading ─────────────────────────────────────────────────────────────────
 *
 * Probe reads headers alone: enough to size a buffer, choose a target rate, or
 * show a user what a file is, without paying for the samples.
 *
 * Decode hands back one allocation of `frames * channels` interleaved floats,
 * released with clockwork_audio_free. The out-pointer is set only on success,
 * so a caller that checks the status alone still holds a null on failure.
 *
 * The memory forms take the file's bytes as they already are — which is what a
 * browser has after a fetch, what a plugin has after a host hands it a chunk,
 * and what a test has without touching a disk. The library reads the buffer
 * during the call and keeps no reference to it afterwards.
 */
ClockworkStatus clockwork_audio_probe_file(const char* path,
                                           ClockworkAudioInfo* info);
ClockworkStatus clockwork_audio_probe_memory(const void* data, size_t bytes,
                                             ClockworkAudioInfo* info);

ClockworkStatus clockwork_audio_decode_file(const char* path,
                                            ClockworkAudioInfo* info,
                                            float** out_interleaved);
ClockworkStatus clockwork_audio_decode_memory(const void* data, size_t bytes,
                                              ClockworkAudioInfo* info,
                                              float** out_interleaved);

/* Release what decode allocated. A null pointer is accepted and ignored, so
 * an error path need not branch. */
void clockwork_audio_free(void* interleaved);

/* ── Writing ─────────────────────────────────────────────────────────────────
 *
 * A writer is opened, fed interleaved frames, and closed. Close is what
 * finishes the stream: a WAV's sizes and a FLAC's stream info are patched with
 * the totals only once they are known, so THE OUTPUT IS COMPLETE AFTER CLOSE
 * RETURNS and not before.
 *
 * A writer's destination is a path or a buffer, and the two are fed
 * identically. A buffer is what a WebAssembly build has — there is no file
 * system in a browser tab — and what a caller wants when the bytes are bound
 * for a socket or a blob rather than a disk.
 *
 * A combination the format cannot carry is refused at open, on the calling
 * thread, rather than accepted and discovered at close — a recorder finds out
 * before a take rather than after one.
 */
typedef struct ClockworkAudioWriterConfig {
    uint32_t struct_bytes;
    uint32_t format;       /* ClockworkAudioFormat: WAV or FLAC */
    uint32_t encoding;     /* ClockworkAudioEncoding */
    uint32_t channels;
    uint32_t sample_rate;
} ClockworkAudioWriterConfig;

typedef struct ClockworkAudioWriter ClockworkAudioWriter;

/* Null on failure, with the reason in *status when status is non-null.
 *
 * A build without a file system reports CLOCKWORK_E_ABSENT from the path form
 * and serves the memory form as usual. */
ClockworkAudioWriter* clockwork_audio_writer_open(
    const char* path, const ClockworkAudioWriterConfig* config,
    ClockworkStatus* status);

ClockworkAudioWriter* clockwork_audio_writer_open_memory(
    const ClockworkAudioWriterConfig* config, ClockworkStatus* status);

/* Append `frames` interleaved frames of `config.channels` floats each.
 * Samples outside [-1, 1) are clamped when the target is integer PCM, which is
 * the behaviour a recorder wants from a bus that clipped. */
ClockworkStatus clockwork_audio_writer_write(ClockworkAudioWriter* writer,
                                             const float* interleaved,
                                             uint64_t frames);

/* Frames accepted so far. */
uint64_t clockwork_audio_writer_frames(const ClockworkAudioWriter* writer);

/* Finish the stream and free the handle. The status reports the last write or
 * the finalisation, so a caller that ignored every write can still learn the
 * take is short. The handle is freed either way; a null handle is accepted.
 *
 * For a memory writer, out_bytes and out_bytes_len receive the finished
 * stream, which the caller releases with clockwork_audio_free. Passing null
 * for them discards it — which is what a path writer always does, since its
 * bytes went to the file. */
ClockworkStatus clockwork_audio_writer_close(ClockworkAudioWriter* writer,
                                             void** out_bytes,
                                             size_t* out_bytes_len);

/* Whether this build can write a given pairing, without creating a file.
 * A tool offering a format menu asks this rather than opening a temporary. */
int clockwork_audio_can_write(uint32_t format, uint32_t encoding);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_AUDIO_FILE_H */
