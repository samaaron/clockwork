// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * clockwork_embed.h — an engine in your process, in one call.
 *
 * Two ways to bring one up, and the same handle either way:
 *
 *   clockwork_embed_boot    clockwork opens the audio device and ticks itself
 *                           from the device callback. For an application.
 *   clockwork_embed_attach  nothing is opened; YOU tick, from whatever hands
 *                           you buffers — a DAW's process callback, a
 *                           worklet, a test — through clockwork_embed_render.
 *
 * Above the boot line the two are identical: clockwork_embed_client hands
 * back a client (clockwork_client.h) over the engine's memory, and sending,
 * polling, taps, metrics and the scope is the client's. A product
 * that plays the same piece as an app and as a plugin writes its playing
 * once, against the client, and only the boot differs.
 *
 * ── Rendering, for a host that ticks ──────────────────────────────────────
 * The engine renders fixed blocks (clockwork_embed_block_size); a host's
 * buffers are any size. render() keeps two small FIFOs so a call for N
 * frames is answered with N frames whatever N is: host input is queued,
 * blocks are ticked as output is needed, and rendered audio is served from
 * the queue. Output is exact and immediate — frame 0 of the first call is
 * frame 0 of the first block. Input arrives at the engine up to one block
 * late (a block must be full to tick it; the first is padded with silence),
 * which is the price of any-size buffers and the same on every DAW plugin
 * ever written.
 *
 * Time is the engine's own: the moment of attach as NTP, plus the frames
 * rendered since. A DAW rendering offline at ten times real time gets a
 * clock that runs at the audio's pace, not the wall's.
 *
 * ── Engines side by side ──────────────────────────────────────────────────
 * Each handle has an engine of its own (a core, clockwork_core.h): its own
 * memory, client and stream, independent of every other in the process. A
 * wasm build and an ESP-IDF board hold one engine per process, and refuse a
 * second (CLOCKWORK_E_PERM).
 *
 * Pure C. Boot needs the device layer (CLOCKWORK_DEVICE); a build without
 * one answers CLOCKWORK_E_ABSENT and attach still works.
 */
#pragma once
#include <stdint.h>
#include "clockwork_client.h"
#include "clockwork_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ClockworkEmbed ClockworkEmbed;

typedef struct ClockworkEmbedConfig {
    uint32_t    struct_bytes;      /* sizeof(ClockworkEmbedConfig), for later fields */
    double      sample_rate;       /* boot: 0 takes the device's; attach: required */
    uint32_t    block_size;        /* frames per engine block; 0 = the platform default */
    uint32_t    input_channels;    /* ceilings. boot: 0 = what the device has */
    uint32_t    output_channels;   /* attach: what you will render, at most */
    uint32_t    max_render_frames; /* attach: the largest `frames` you will pass to render; 0 = 4096 */
    const char* app_name;          /* boot: the name the OS shows; NULL = the product's */
    const char* device;            /* boot: a device to open by name; NULL = the default */
    const ClockworkProfile* profile; /* the arena's sizes (clockwork_profile.h); NULL = the target's
                                    * own. One no arena can hold is refused with E_ARG: ask
                                    * clockwork_profile_check why. */
    uint32_t    flags;             /* CLOCKWORK_EMBED_* below, or 0 */
    const char* driver;            /* boot: the driver (device type) to open on — the names the
                                    * platform reports: "Windows Audio", "DirectSound", "ASIO",
                                    * "CoreAudio", "PipeWire", "JACK", "ALSA"; NULL = the platform's
                                    * default. Resolved as a host's --audio-driver is: the exact
                                    * name, else a unique case-insensitive match; a name that
                                    * resolves to nothing keeps the default with a warning rather
                                    * than refusing to boot. ASIO needs `device` as well. */
} ClockworkEmbedConfig;

/* What a booted engine is actually running on: the driver and device that
 * opened (which may not be what was asked for — see `driver` above), the
 * rate, the hardware callback in frames, the engine's block, channels and
 * latencies. Fixed-size strings so nothing here has a lifetime. */
typedef struct ClockworkEmbedDevice {
    uint32_t struct_bytes;         /* sizeof(ClockworkEmbedDevice), for later fields */
    char     driver[64];           /* "Windows Audio", "CoreAudio", ... */
    char     device[256];          /* the output device's name */
    double   sample_rate;
    uint32_t buffer_frames;        /* the hardware callback */
    uint32_t block_frames;         /* the engine's block, as clockwork_embed_block_size */
    uint32_t input_channels;
    uint32_t output_channels;
    uint32_t output_latency_frames;
    uint32_t input_latency_frames;
} ClockworkEmbedDevice;

/* Fill `out` for a booted handle. E_ARG for an attached one (it has no
 * device: the host renders) or a struct too small to fill; `out` is zeroed
 * first either way. */
ClockworkStatus clockwork_embed_device(const ClockworkEmbed* h, ClockworkEmbedDevice* out);

/* boot: open the device for output only. `input_channels` cannot say this —
 * its 0 means "what the device has" — and a host that never reads input
 * should not open it: on macOS that is a microphone permission prompt, and
 * on DirectSound a second stream on its own clock beside the output's. */
#define CLOCKWORK_EMBED_NO_INPUT 1u

/* Bring an engine up on the audio device and let it tick itself. NULL with
 * `status` on refusal: E_ABSENT when this build has no device layer, E_PERM
 * where a process holds one engine and has it, E_ARG for a config it cannot
 * read, E_NO_INSTANCE when the guest did not start (as for attach, below). */
ClockworkEmbed* clockwork_embed_boot(const ClockworkEmbedConfig* config, ClockworkStatus* status);

/* Bring an engine up with no device: you render. NULL with `status` on
 * refusal: E_PERM where a process holds one engine and has it; E_ARG for a
 * config or profile it cannot read; E_NO_INSTANCE when the guest did not
 * start — one instance more than the guest allows a process
 * (DspInfo::instances), or a guest that refused — with the reason on the
 * core's log; E_NOMEM when the engine's memory could not be laid out. */
ClockworkEmbed* clockwork_embed_attach(const ClockworkEmbedConfig* config, ClockworkStatus* status);

/* Change an attached engine's rate, block or channels and keep the engine:
 * what a plugin host does between deactivating an instance and activating
 * it again. Not while render runs. The engine does the least that gets it
 * back where it was (docs/RESTARTS.md): at the rate it had, the instance
 * carries on; at another it takes, it is told; otherwise it is rebuilt.
 * E_ARG for a booted handle (its device is the engine's own) or a config it
 * cannot read. Closing the handle instead ends the engine. */
ClockworkStatus clockwork_embed_reconfigure(ClockworkEmbed* h, const ClockworkEmbedConfig* config);

/* The client over this engine. Owned by the handle: do not close it. */
ClockworkClient* clockwork_embed_client(ClockworkEmbed* h);

/* The geometry the engine actually runs at. */
double   clockwork_embed_sample_rate(const ClockworkEmbed* h);
uint32_t clockwork_embed_block_size(const ClockworkEmbed* h);

/* Render `frames` frames: `out[c][0..frames)` for c < out_channels is
 * filled, `in[c][0..frames)` for c < in_channels is consumed (either may be
 * NULL with a count of 0). Channels past the configured ceilings are
 * silence out and ignored in. Audio-thread safe: no allocation, no lock.
 * Returns the frames rendered, which is `frames` unless the handle was
 * booted (a device host renders itself) — then 0. */
uint32_t clockwork_embed_render(ClockworkEmbed* h,
                                float* const* out, uint32_t out_channels,
                                const float* const* in, uint32_t in_channels,
                                uint32_t frames);

/* Release: a booted engine closes its device and shuts down; an attached
 * one detaches. The client is closed with it. */
void clockwork_embed_close(ClockworkEmbed* h);

#ifdef __cplusplus
}
#endif
