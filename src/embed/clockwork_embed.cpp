// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// clockwork_embed.h: the attach path and render, and what both paths share.
// The device boot is native/ClockworkEmbedDevice.cpp.
#include "clockwork_embed_impl.h"
#include "lanes/lanes.h"
#include "audio_config.h"
#include "shared_memory.h"
#include <chrono>
#include <cstring>
#include <new>

std::atomic<ClockworkEmbed*> g_clockwork_embed { nullptr };

namespace {

ClockworkStatus fail(ClockworkStatus* out, ClockworkStatus s) { if (out) *out = s; return s; }

bool readable(const ClockworkEmbedConfig* c) {
    return c && c->struct_bytes >= sizeof(ClockworkEmbedConfig);
}

double ntpNow() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count() + 2208988800.0;
}

// A FIFO of `ch` channel rings, `cap` frames each, channel-major in one span.
struct Ring {
    float* base; uint32_t ch, cap;
    float* channel(uint32_t c) { return base + static_cast<size_t>(c) * cap; }
};

} // namespace

extern "C" {

ClockworkEmbed* clockwork_embed_attach(const ClockworkEmbedConfig* config, ClockworkStatus* status) {
    if (!readable(config) || config->sample_rate <= 0 || config->output_channels == 0) {
        fail(status, CLOCKWORK_E_ARG);
        return nullptr;
    }
    if (g_clockwork_embed.load(std::memory_order_acquire)) {
        fail(status, CLOCKWORK_E_PERM);
        return nullptr;
    }
    const int rc = clockwork_attach(config->sample_rate, config->block_size,
                                    config->input_channels, config->output_channels,
                                    /*verbosity*/ 0, nullptr, 0, nullptr, 0, /*arena_bytes*/ 0,
                                    nullptr, 0, nullptr, 0);
    if (rc == CLOCKWORK_ATTACH_REFUSED) {
        fail(status, CLOCKWORK_E_PERM);
        return nullptr;
    }
    auto* h = new (std::nothrow) ClockworkEmbed();
    if (!h) { clockwork_detach(); fail(status, CLOCKWORK_E_NOMEM); return nullptr; }
    h->kind        = ClockworkEmbed::Kind::Attached;
    h->attached    = true;
    // What we actually got: a joiner's arguments were not applied.
    h->sampleRate  = clockwork_sample_rate();
    h->block       = clockwork_block_size();
    h->inChannels  = config->input_channels;
    h->outChannels = config->output_channels;

    const uint32_t maxFrames = config->max_render_frames ? config->max_render_frames : 4096;
    h->capacity = h->block + maxFrames;
    h->outRing.assign(static_cast<size_t>(h->outChannels) * h->capacity, 0.0f);
    h->inRing.assign(static_cast<size_t>(h->inChannels ? h->inChannels : 1) * h->capacity, 0.0f);
    h->ntpBase  = ntpNow();
    h->rendered = 0;

    ClockworkStatus st = CLOCKWORK_OK;
    h->client = clockwork_client_open_memory(clockwork_lanes_base(),
                                             arenaHeader(clockwork_lanes_base())->arena_bytes, &st);
    if (!h->client) {
        clockwork_detach();
        delete h;
        fail(status, st);
        return nullptr;
    }
    h->ownsClient = true;
    g_clockwork_embed.store(h, std::memory_order_release);
    fail(status, CLOCKWORK_OK);
    return h;
}

ClockworkClient* clockwork_embed_client(ClockworkEmbed* h) { return h ? h->client : nullptr; }
ClockworkStatus clockwork_embed_device(const ClockworkEmbed* h, ClockworkEmbedDevice* out) {
    if (!out) return CLOCKWORK_E_ARG;
    const uint32_t bytes = out->struct_bytes;
    std::memset(out, 0, sizeof *out);
    out->struct_bytes = bytes;
    if (bytes < sizeof(ClockworkEmbedDevice)) return CLOCKWORK_E_ARG;
    if (!h || h->kind != ClockworkEmbed::Kind::Booted || !h->deviceInfo) return CLOCKWORK_E_ARG;
    return h->deviceInfo(h->engine, out);
}
double   clockwork_embed_sample_rate(const ClockworkEmbed* h) { return h ? h->sampleRate : 0.0; }
uint32_t clockwork_embed_block_size(const ClockworkEmbed* h) { return h ? h->block : 0u; }

uint32_t clockwork_embed_render(ClockworkEmbed* h,
                                float* const* out, uint32_t out_channels,
                                const float* const* in, uint32_t in_channels,
                                uint32_t frames) {
    if (!h || h->kind != ClockworkEmbed::Kind::Attached || frames == 0) return 0;
    const uint32_t block = h->block, cap = h->capacity;
    Ring outRing { h->outRing.data(), h->outChannels, cap };
    Ring inRing  { h->inRing.data(),  h->inChannels ? h->inChannels : 1, cap };

    uint32_t done = 0;
    while (done < frames) {
        // A chunk the FIFOs can hold: at most what fits beside a full block.
        const uint32_t chunk = (frames - done) < (cap - block) ? (frames - done) : (cap - block);

        // Host input in, up to the ceiling; missing channels are silence.
        if (h->inChannels) {
            for (uint32_t c = 0; c < h->inChannels; ++c) {
                const float* src = (in && c < in_channels) ? in[c] : nullptr;
                float* ring = inRing.channel(c);
                for (uint32_t f = 0; f < chunk; ++f) {
                    const uint32_t at = (h->inHead + h->inCount + f) % cap;
                    ring[at] = src ? src[done + f] : 0.0f;
                }
            }
            h->inCount += chunk;
        }

        // Tick until the output FIFO can answer the chunk. A tick takes a
        // block of input (padded with silence when a block is not yet in)
        // and leaves a block of output.
        while (h->outCount < chunk) {
            float* busIn = clockwork_audio_in();
            if (h->inChannels && busIn) {
                const uint32_t have = h->inCount < block ? h->inCount : block;
                for (uint32_t c = 0; c < h->inChannels; ++c) {
                    const float* ring = inRing.channel(c);
                    float* dst = busIn + static_cast<size_t>(c) * block;
                    for (uint32_t f = 0; f < block; ++f)
                        dst[f] = f < have ? ring[(h->inHead + f) % cap] : 0.0f;
                }
                h->inHead  = (h->inHead + have) % cap;
                h->inCount -= have;
            }
            const double ntp = h->ntpBase + static_cast<double>(h->rendered) / h->sampleRate;
            clockwork_tick(ntp, h->outChannels, h->inChannels);
            h->rendered += block;

            const float* bus = clockwork_audio_out();
            for (uint32_t c = 0; c < h->outChannels; ++c) {
                float* ring = outRing.channel(c);
                const float* src = bus ? bus + static_cast<size_t>(c) * block : nullptr;
                for (uint32_t f = 0; f < block; ++f)
                    ring[(h->outHead + h->outCount + f) % cap] = src ? src[f] : 0.0f;
            }
            h->outCount += block;
        }

        // Output out, up to the ceiling; channels past it are silence.
        for (uint32_t c = 0; c < out_channels; ++c) {
            float* dst = out ? out[c] : nullptr;
            if (!dst) continue;
            if (c < h->outChannels) {
                const float* ring = outRing.channel(c);
                for (uint32_t f = 0; f < chunk; ++f) dst[done + f] = ring[(h->outHead + f) % cap];
            } else {
                std::memset(dst + done, 0, chunk * sizeof(float));
            }
        }
        h->outHead   = (h->outHead + chunk) % cap;
        h->outCount -= chunk;
        done += chunk;
    }
    return done;
}

void clockwork_embed_close(ClockworkEmbed* h) {
    if (!h) return;
    if (h->ownsClient && h->client) clockwork_client_close(h->client);
    if (h->closeEngine) h->closeEngine(h->engine);
    if (h->attached) clockwork_detach();
    ClockworkEmbed* expected = h;
    g_clockwork_embed.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    delete h;
}

#if !CLOCKWORK_DEVICE
ClockworkEmbed* clockwork_embed_boot(const ClockworkEmbedConfig* config, ClockworkStatus* status) {
    (void)config;
    // No device layer in this build: nothing to open. Attach, and render.
    fail(status, CLOCKWORK_E_ABSENT);
    return nullptr;
}
#endif

} // extern "C"
