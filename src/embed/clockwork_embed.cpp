// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
// clockwork_embed.h: the attach path and render, and what both paths share.
// The device boot is native/ClockworkEmbedDevice.cpp.
#include "clockwork_embed_impl.h"
#include "lanes/lanes.h"
#include "clockwork_config.h"   // clockwork_log
#include "memory_profile.h"     // CLOCKWORK_EMBED_ASSETS_BYTES
#include "shared_memory.h"      // GUEST_CONFIG_SIZE
#include "shm_segment.hpp"      // shm_lane_geometry
#include <chrono>
#include <cstring>
#include <new>

namespace {

ClockworkStatus fail(ClockworkStatus* out, ClockworkStatus s) { if (out) *out = s; return s; }

bool readable(const ClockworkEmbedConfig* c) {
    return c && c->struct_bytes >= sizeof(ClockworkEmbedConfig)
        && (!c->guest_config || std::strlen(c->guest_config) < GUEST_CONFIG_SIZE);
}

// The handle's bulk lanes and the guest's options, as `config` asks. The
// lanes are one allocation and stay untouched: address space until a client
// writes a sample in. False when they could not be had.
bool takeBulk(ClockworkEmbed* h, const ClockworkEmbedConfig* config) {
    const auto lanes = detail_shm_segment::shm_lane_geometry::of(
        config->assets_bytes ? config->assets_bytes : CLOCKWORK_EMBED_ASSETS_BYTES,
        config->returns_bytes ? config->returns_bytes : CLOCKWORK_EMBED_RETURNS_BYTES);
    const size_t bytes = lanes.assets_size + lanes.returns_size;
    if (bytes) {
        h->lanes.reset(new (std::nothrow) uint8_t[bytes]);   // NOLINT(cppcoreguidelines-owning-memory): nothrow new into the unique_ptr
        if (!h->lanes) return false;
    }
    h->assetsBytes  = static_cast<uint32_t>(lanes.assets_size);
    h->returnsBytes = static_cast<uint32_t>(lanes.returns_size);
    h->guestConfig  = config->guest_config ? config->guest_config : "";
    return true;
}

// The core booted, or brought back, at `config`'s geometry with the handle's
// lanes and options: what attach and reconfigure share.
void initCore(ClockworkEmbed* h, const ClockworkEmbedConfig* config) {
    const bool options = !h->guestConfig.empty();
    uint8_t* const lanes = h->lanes.get();
    clockwork_init(h->core, config->sample_rate, config->block_size,
                   config->input_channels, config->output_channels,
                   /*verbosity*/ 0, nullptr, 0,
                   options ? h->guestConfig.c_str() : nullptr,
                   options ? static_cast<uint32_t>(h->guestConfig.size() + 1) : 0,
                   /*arena_bytes*/ 0,
                   h->assetsBytes ? lanes : nullptr, h->assetsBytes,
                   h->returnsBytes ? lanes + h->assetsBytes : nullptr, h->returnsBytes);
}

double ntpNow() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count() + 2208988800.0;
}
double monoNow() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A FIFO of `ch` channel rings, `cap` frames each, channel-major in one span.
struct Ring {
    float* base; uint32_t ch, cap;
    float* channel(uint32_t c) { return base + static_cast<size_t>(c) * cap; }
};

// Size the handle to `config` and the engine it now runs at: the geometry,
// and the two FIFOs, empty. What attach and reconfigure share. False when the
// FIFOs could not be had.
bool fit(ClockworkEmbed* h, const ClockworkEmbedConfig* config) {
    h->sampleRate  = clockwork_sample_rate(h->core);
    h->block       = clockwork_block_size(h->core);
    h->inChannels  = config->input_channels;
    h->outChannels = config->output_channels;
    const uint32_t maxFrames = config->max_render_frames ? config->max_render_frames : 4096;
    h->capacity = h->block + maxFrames;
    try {
        h->outRing.assign(static_cast<size_t>(h->outChannels) * h->capacity, 0.0f);
        h->inRing.assign(static_cast<size_t>(h->inChannels ? h->inChannels : 1) * h->capacity, 0.0f);
    } catch (const std::bad_alloc&) {
        return false;
    }
    h->outHead = h->outCount = 0;
    h->inHead = h->inCount = 0;
    // The clock is the engine's. clockwork_audio_starts, which follows this,
    // makes the next block re-anchor it, never below the sample clock already
    // in the arena, so it carries on from the previous time rather than going
    // backwards.
    h->rendered = 0;
    return true;
}

} // namespace

extern "C" {

ClockworkEmbed* clockwork_embed_attach(const ClockworkEmbedConfig* config, ClockworkStatus* status) {
    if (!readable(config) || config->sample_rate <= 0 || config->output_channels == 0) {
        fail(status, CLOCKWORK_E_ARG);
        return nullptr;
    }
    // A core of its own: refused (E_PERM) only where a process holds one.
    ClockworkStatus made = CLOCKWORK_OK;
    ClockworkCore* core = clockwork_core_new(&made);
    if (!core) {
        fail(status, made);
        return nullptr;
    }
    if (config->profile && clockwork_set_profile(core, config->profile)) {
        clockwork_core_close(core);
        fail(status, CLOCKWORK_E_ARG);
        return nullptr;
    }
    auto* h = new (std::nothrow) ClockworkEmbed();
    if (!h) { clockwork_core_close(core); fail(status, CLOCKWORK_E_NOMEM); return nullptr; }
    h->kind        = ClockworkEmbed::Kind::Attached;
    h->core        = core;
    if (!takeBulk(h, config)) {
        clockwork_core_close(core);
        delete h;
        fail(status, CLOCKWORK_E_NOMEM);
        return nullptr;
    }
    initCore(h, config);
    if (!fit(h, config)) {
        clockwork_core_close(core);
        delete h;
        fail(status, CLOCKWORK_E_NOMEM);
        return nullptr;
    }

    // An engine with no guest is no engine for a plugin: the build's refusal
    // — one instance too many in this process, a guest that would not start
    // — is the handle's refusal, with the reason on the core's log.
    if (const char* why = clockwork_boot_error(core); why || !clockwork_lanes_base(core)) {
        clockwork_log(core, "embed attach refused: %s", why ? why : "the engine's memory could not be laid out");
        clockwork_core_close(core);
        delete h;
        fail(status, why ? CLOCKWORK_E_NO_INSTANCE : CLOCKWORK_E_NOMEM);
        return nullptr;
    }
    ClockworkStatus st = CLOCKWORK_OK;
    uint8_t* const base = static_cast<uint8_t*>(clockwork_lanes_base(core));
    const uint32_t arenaBytes = arenaHeader(base)->arena_bytes;
    h->client = clockwork_client_open_memory(base, arenaBytes, &st);
    if (!h->client) {
        clockwork_core_close(core);
        delete h;
        fail(status, st);
        return nullptr;
    }
    h->ownsClient = true;
    // This host's audio starts now: the next block re-anchors the clock.
    clockwork_audio_starts(h->core, ntpNow(), monoNow());
    fail(status, CLOCKWORK_OK);
    return h;
}

ClockworkClient* clockwork_embed_client(ClockworkEmbed* h) { return h ? h->client : nullptr; }

ClockworkStatus clockwork_embed_reconfigure(ClockworkEmbed* h, const ClockworkEmbedConfig* config) {
    if (!h || h->kind != ClockworkEmbed::Kind::Attached || !readable(config)
        || config->sample_rate <= 0 || config->output_channels == 0)
        return CLOCKWORK_E_ARG;
    // Audio coming back to the engine that is up (docs/RESTARTS.md): the
    // instance carries on, is told, or is built again. The lanes and the
    // guest's options are the ones it was attached with.
    initCore(h, config);
    if (!fit(h, config)) return CLOCKWORK_E_NOMEM;
    // This host's audio starts again now: the next block re-anchors the clock.
    clockwork_audio_starts(h->core, ntpNow(), monoNow());
    return CLOCKWORK_OK;
}
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
            float* busIn = clockwork_audio_in(h->core);
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
            // Pass this host's frame count and clocks to the tick, which keeps
            // the clock. Output latency is 0: the host's own buffering is not
            // the engine's concern.
            ClockworkBlockTime time {};
            time.struct_bytes   = sizeof time;
            time.audio_frames   = h->rendered;
            time.wall_ntp       = ntpNow();
            time.monotonic_secs = monoNow();
            const ClockworkTick tick = clockwork_tick(h->core, &time, h->outChannels, h->inChannels);
            h->rendered += block;

            // Silence for a block the core did not render (lanes.h).
            const float* bus = tick == CLOCKWORK_TICK_RENDERED ? clockwork_audio_out(h->core) : nullptr;
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
    clockwork_core_close(h->core);
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
