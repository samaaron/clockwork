// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//
// clockwork_audio_file.cpp also fronts the Rust decoder for its probe and
// decode calls. The encoder target never reaches them, and linking the Rust
// workspace into this build would make it the main tree's. So they are
// answered here with "absent", which is what a build without a decoder says.
#include "clockwork_audio_decode.h"
#include "clockwork_client.h"

extern "C" int clockwork_audio_decode_probe(const uint8_t*, size_t, ClockworkAudioDecodeInfo*) {
    return CLOCKWORK_E_ABSENT;
}
extern "C" int clockwork_audio_decode_run(const uint8_t*, size_t, uint64_t, ClockworkAudioGrow,
                                          ClockworkAudioDecodeInfo*, float**) {
    return CLOCKWORK_E_ABSENT;
}
