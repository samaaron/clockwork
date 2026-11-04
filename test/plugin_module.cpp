// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
/*
 * plugin_module.cpp — a plugin binary built on Clockwork as small as one can
 * be (clockwork_plugin_module, src/clockwork_plugin.h): one entry point, and
 * Clockwork inside, hidden. test_plugin_module.cpp loads two copies of it
 * into one process.
 */
#include "clockwork_plugin.h"
#include "clockwork_core.h"
#include "clockwork_embed.h"

#include <cstring>
#include <vector>

namespace {
// /dummy/ping, as OSC: the address padded to four, then an empty type tag.
const char kPing[] = "/dummy/ping\0,\0\0\0";
}

// The id of the first core this module's Clockwork makes, which is 1 in a
// module whose Clockwork is its own; then an engine attached, rendered and
// asked for a pong. Negative when any of it fails.
CLOCKWORK_PLUGIN_ENTRY int clockwork_test_plugin_entry(void) {
    ClockworkStatus st = CLOCKWORK_OK;
    ClockworkCore* core = clockwork_core_new(&st);
    if (!core) return -1;
    const int id = static_cast<int>(clockwork_core_id(core));
    clockwork_core_close(core);

    ClockworkEmbedConfig cfg {};
    cfg.struct_bytes    = sizeof cfg;
    cfg.sample_rate     = 48000;
    cfg.output_channels = 2;
    ClockworkEmbed* e = clockwork_embed_attach(&cfg, &st);
    if (!e) return -2;
    ClockworkClient* client = clockwork_embed_client(e);
    if (clockwork_client_send(client, reinterpret_cast<const uint8_t*>(kPing), sizeof kPing - 1, 7) != CLOCKWORK_OK) {
        clockwork_embed_close(e);
        return -3;
    }
    const uint32_t block = clockwork_embed_block_size(e);
    std::vector<float> l(block), r(block);
    float* out[] = { l.data(), r.data() };
    bool pong = false;
    for (int b = 0; b < 8 && !pong; ++b) {
        clockwork_embed_render(e, out, 2, nullptr, 0, block);
        ClockworkClientMessage m[8];
        const uint32_t n = clockwork_client_poll(client, m, 8);
        for (uint32_t i = 0; i < n; ++i)
            pong = pong || (m[i].length > 11 && std::memcmp(m[i].bytes, "/dummy/pong", 11) == 0);
    }
    clockwork_embed_close(e);
    return pong ? id : -4;
}
