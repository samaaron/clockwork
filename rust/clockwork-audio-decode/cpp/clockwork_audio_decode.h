/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2026 Sam Aaron
 *
 * clockwork_audio_decode.h — the decoders, as clockwork_audio_file.cpp calls
 * them. Not a client header: clients include clockwork_audio_file.h, which
 * adds the file reading, the versioned info struct and the writer around
 * these two calls.
 *
 * Both take a file's bytes and give back a ClockworkStatus. Decode hands its
 * frames into a buffer it grows through `grow`, which has realloc's shape:
 * called with null for the first block, returning the new address or null
 * when it cannot grow (the old block is then still the caller's). The buffer
 * is the caller's from the first byte, so what frees it is whatever pairs
 * with `grow`, and nothing the decoder allocated outlives the call.
 *
 * `max_bytes` is the most float a decode may hand over. A header that claims
 * more is refused as CLOCKWORK_E_TOO_BIG before a frame is decoded; a file
 * that yields more than it claimed is refused the moment it passes the
 * limit. `info->frames` is the header's claim from a probe and the frames
 * actually decoded from a decode.
 */
#ifndef CLOCKWORK_AUDIO_DECODE_H
#define CLOCKWORK_AUDIO_DECODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ClockworkAudioInfo without its size field, in the order the Rust side
 * lays it out. The values of format and encoding are ClockworkAudioFormat
 * and ClockworkAudioEncoding. */
typedef struct ClockworkAudioDecodeInfo {
    uint32_t format;
    uint32_t encoding;
    uint32_t channels;
    uint32_t sample_rate;
    uint32_t source_bits;
    uint32_t reserved;
    uint64_t frames;
} ClockworkAudioDecodeInfo;

typedef void* (*ClockworkAudioGrow)(void* block, size_t bytes);

int clockwork_audio_decode_probe(const uint8_t* data, size_t len,
                                 ClockworkAudioDecodeInfo* info);

int clockwork_audio_decode_run(const uint8_t* data, size_t len, uint64_t max_bytes,
                               ClockworkAudioGrow grow,
                               ClockworkAudioDecodeInfo* info, float** out);

#ifdef __cplusplus
}
#endif

#endif /* CLOCKWORK_AUDIO_DECODE_H */
