// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
//
// A stand-in for the Bela SDK's Bela.h with the declarations
// modules/juce_audio_devices/native/juce_Bela_linux.cpp uses, so the driver
// compiles without the SDK (scripts/check-drivers.sh). Compile only: a Bela
// build uses the SDK's header.
//
// Each declaration here is the SDK's own, as include/Bela.h has it at Bela
// 1.13 (BelaPlatform/Bela, the header last changed 2023-03-22): the same
// names, the same types, the context's members const as they are there, and
// C linkage. So what compiles against this compiles against the SDK's for
// the same reason, and a write into the context or a wrong type fails here
// as it would on a Bela. The members the driver does not use are left out.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BELA_FLAG_INTERLEAVED (1 << 0)

typedef struct
{
    const float* const audioIn;
    float* const audioOut;
    const float* const analogIn;
    float* const analogOut;
    const uint32_t audioFrames;
    const uint32_t audioInChannels;
    const uint32_t audioOutChannels;
    const uint32_t analogFrames;
    const uint32_t analogInChannels;
    const uint32_t analogOutChannels;
    const uint64_t audioFramesElapsed;
    const uint32_t flags;
} BelaContext;

typedef struct
{
    int periodSize;
    int useAnalog;
    int numAudioInChannels;
    int numAudioOutChannels;
    int numAnalogInChannels;
    int numAnalogOutChannels;
    int detectUnderruns;
    int interleave;
    int uniformSampleRate;
    bool (*setup) (BelaContext*, void*);
    void (*render) (BelaContext*, void*);
    void (*cleanup) (BelaContext*, void*);
} BelaInitSettings;

void Bela_defaultSettings (BelaInitSettings* settings);
int Bela_initAudio (BelaInitSettings* settings, void* userData);
int Bela_startAudio();
void Bela_stopAudio();
void Bela_cleanupAudio();

#ifdef __cplusplus
}
#endif
