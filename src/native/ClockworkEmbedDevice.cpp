// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// clockwork_embed_boot: the device host. Clockwork opens the audio device
// and the hardware callback ticks the guest; the embedder gets the engine's
// own client and never touches the audio thread.
#include <cstring>
#include "embed/clockwork_embed_impl.h"
#include "native/ClockworkEngine.h"
#include "lanes/lanes.h"
#include <juce_events/juce_events.h>
#include <memory>
#include <new>

namespace {

struct DeviceEngine {
    // JUCE's message machinery, for the device layer, for as long as the
    // engine lives. One per process, like the engine.
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juce;
    std::unique_ptr<ClockworkEngine> engine;
};

void closeDevice(void* p) {
    auto* d = static_cast<DeviceEngine*>(p);
    if (!d) return;
    if (d->engine) d->engine->shutdown();
    d->engine.reset();
    d->juce.reset();
    delete d;
}

} // namespace

// What opened, from the engine's own account of its device. Strings are
// truncated to the struct's fields, never left unterminated.
static void copyField(char* dst, size_t cap, const std::string& s) {
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}
static ClockworkStatus deviceInfoOf(void* p, ClockworkEmbedDevice* out) {
    auto* d = static_cast<DeviceEngine*>(p);
    if (!d || !d->engine) return CLOCKWORK_E_ARG;
    const CurrentDeviceInfo info = d->engine->currentDevice();
    copyField(out->driver, sizeof out->driver, info.typeName);
    copyField(out->device, sizeof out->device, info.name);
    out->sample_rate           = info.activeSampleRate;
    out->buffer_frames         = info.activeBufferSize > 0 ? static_cast<uint32_t>(info.activeBufferSize) : 0u;
    out->block_frames          = clockwork_block_size();
    out->input_channels        = info.activeInputChannels  > 0 ? static_cast<uint32_t>(info.activeInputChannels)  : 0u;
    out->output_channels       = info.activeOutputChannels > 0 ? static_cast<uint32_t>(info.activeOutputChannels) : 0u;
    out->output_latency_frames = info.outputLatencySamples > 0 ? static_cast<uint32_t>(info.outputLatencySamples) : 0u;
    out->input_latency_frames  = info.inputLatencySamples  > 0 ? static_cast<uint32_t>(info.inputLatencySamples)  : 0u;
    return CLOCKWORK_OK;
}

extern "C" ClockworkEmbed* clockwork_embed_boot(const ClockworkEmbedConfig* config, ClockworkStatus* status) {
    const auto fail = [status](ClockworkStatus s) { if (status) *status = s; return static_cast<ClockworkEmbed*>(nullptr); };
    if (!config || config->struct_bytes < sizeof(ClockworkEmbedConfig)) return fail(CLOCKWORK_E_ARG);
    if (g_clockwork_embed.load(std::memory_order_acquire)) return fail(CLOCKWORK_E_PERM);

    auto* d = new (std::nothrow) DeviceEngine();
    if (!d) return fail(CLOCKWORK_E_NOMEM);
    d->juce   = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
    d->engine = std::make_unique<ClockworkEngine>();
    d->engine->onDebug = [](const std::string&) {};
    d->engine->onReply = [](const uint8_t*, uint32_t) {};

    ClockworkEngine::Config cfg;
    if (config->sample_rate > 0) cfg.sampleRate = static_cast<int>(config->sample_rate + 0.5);
    if (config->block_size)      cfg.bufferSize = static_cast<int>(config->block_size);
    if (config->input_channels)  cfg.numInputChannels  = static_cast<int>(config->input_channels);
    if (config->flags & CLOCKWORK_EMBED_NO_INPUT) cfg.numInputChannels = 0;   // the engine's own "none"
    if (config->output_channels) cfg.numOutputChannels = static_cast<int>(config->output_channels);
    if (config->app_name && *config->app_name) cfg.appName = config->app_name;
    if (config->device && *config->device)     cfg.hardwareDevice = config->device;
    if (config->driver && *config->driver)     cfg.audioDriver    = config->driver;   // resolveBootDriver, as --audio-driver
    cfg.udpPort          = 0;      // in-process: no command socket, no segment
    cfg.hostDrainsEgress = true;   // the embedder polls the client; the gateway must not also drain
    cfg.callbackWatchdog = true;
    d->engine->init(cfg);          // opens the device; the callback ticks from here

    auto* h = new (std::nothrow) ClockworkEmbed();
    if (!h) { closeDevice(d); return fail(CLOCKWORK_E_NOMEM); }
    h->kind        = ClockworkEmbed::Kind::Booted;
    h->engine      = d;
    h->closeEngine = closeDevice;
    h->deviceInfo  = deviceInfoOf;
    h->client      = d->engine->egressClient();   // the engine's; closed with it
    h->ownsClient  = false;
    h->sampleRate  = clockwork_sample_rate();
    h->block       = clockwork_block_size();
    h->inChannels  = (config->flags & CLOCKWORK_EMBED_NO_INPUT) ? 0 : config->input_channels;
    h->outChannels = config->output_channels;
    g_clockwork_embed.store(h, std::memory_order_release);
    if (status) *status = CLOCKWORK_OK;
    return h;
}
