// SPDX-License-Identifier: ISC
// Copyright (c) 2026 Sam Aaron
//
// A stand-in for the Bela SDK's Bela.h with the declarations
// modules/juce_audio_devices/native/juce_Bela_linux.cpp uses, so the driver
// compiles without the SDK (scripts/check-drivers.sh). Compile only: a Bela
// build uses the SDK's header.
#pragma once

#include <cstdint>

#define BELA_FLAG_INTERLEAVED (1u << 0)

struct BelaContext
{
    const float* audioIn;
    float* audioOut;
    const float* analogIn;
    float* analogOut;
    uint32_t audioFrames;
    uint32_t analogFrames;
    uint32_t audioInChannels;
    uint32_t audioOutChannels;
    uint32_t analogInChannels;
    uint32_t analogOutChannels;
    uint64_t audioFramesElapsed;
    uint32_t flags;
};

struct BelaInitSettings
{
    int periodSize;
    int useAnalog;
    unsigned int numAnalogInChannels;
    unsigned int numAnalogOutChannels;
    int uniformSampleRate;
    unsigned int numAudioInChannels;
    unsigned int numAudioOutChannels;
    int detectUnderruns;
    bool (*setup) (BelaContext*, void*);
    void (*render) (BelaContext*, void*);
    void (*cleanup) (BelaContext*, void*);
    int interleave;
};

void Bela_defaultSettings (BelaInitSettings* settings);
int Bela_initAudio (BelaInitSettings* settings, void* userData);
int Bela_startAudio();
void Bela_stopAudio();
void Bela_cleanupAudio();
