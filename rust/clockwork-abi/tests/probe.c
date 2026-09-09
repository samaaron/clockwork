/* The layout probe: every struct's size and every field's offset, as the C
 * compiler sees the real headers. tests/layout.rs compiles and runs this
 * and compares each line with what the Rust declarations say. */
#include <stddef.h>
#include <stdio.h>
#include "dsp_api.h"
#include "clockwork_client.h"
#include "clockwork_event_sink.h"
#include "lanes/lanes.h"
#include "clockwork_arena.h"
#include "clockwork_embed.h"

#define S(T)     printf("size %s %zu\n", #T, sizeof(T))
#define F(T, f)  printf("field %s %s %zu\n", #T, #f, offsetof(T, f))

int main(void) {
    S(DspConfig);
    F(DspConfig, sample_rate); F(DspConfig, block_size); F(DspConfig, max_input_channels);
    F(DspConfig, max_output_channels); F(DspConfig, clock); F(DspConfig, channel_map);
    F(DspConfig, shm_window); F(DspConfig, shm_window_bytes); F(DspConfig, persistent);
    F(DspConfig, persistent_bytes); F(DspConfig, arena); F(DspConfig, arena_bytes);
    F(DspConfig, arena_bulk); F(DspConfig, arena_bulk_bytes); F(DspConfig, inbox);
    F(DspConfig, inbox_bytes); F(DspConfig, outbox); F(DspConfig, outbox_bytes);
    F(DspConfig, guest_config); F(DspConfig, guest_config_bytes);
    F(DspConfig, deterministic_seed); F(DspConfig, fp_env);

    S(DspHost);
    F(DspHost, ctx); F(DspHost, emit_osc); F(DspHost, log); F(DspHost, open_sink);
    F(DspHost, send_sink); F(DspHost, free_bytes); F(DspHost, asset_release);

    S(DspInfo);
    F(DspInfo, name); F(DspInfo, version); F(DspInfo, holds_schedule);
    F(DspInfo, arena_bytes_wanted); F(DspInfo, arena_bulk_bytes_wanted); F(DspInfo, wants_events);

    S(ClockworkAsset);
    F(ClockworkAsset, struct_bytes); F(ClockworkAsset, id); F(ClockworkAsset, kind);
    F(ClockworkAsset, origin); F(ClockworkAsset, bytes); F(ClockworkAsset, byte_count);
    F(ClockworkAsset, channels); F(ClockworkAsset, frames); F(ClockworkAsset, sample_rate);

    S(ClockworkClientInfo);
    F(ClockworkClientInfo, struct_bytes); F(ClockworkClientInfo, abi_version);
    F(ClockworkClientInfo, engine_version); F(ClockworkClientInfo, features);
    F(ClockworkClientInfo, sample_rate); F(ClockworkClientInfo, block_frames);
    F(ClockworkClientInfo, input_channels); F(ClockworkClientInfo, output_channels);

    S(ClockworkClientMessage);
    F(ClockworkClientMessage, bytes); F(ClockworkClientMessage, length);
    F(ClockworkClientMessage, origin); F(ClockworkClientMessage, route);
    F(ClockworkClientMessage, sequence);

    S(ClockworkRegion);
    F(ClockworkRegion, base); F(ClockworkRegion, bytes); F(ClockworkRegion, writable);

    S(ClockworkClientClock);
    F(ClockworkClientClock, struct_bytes); F(ClockworkClientClock, bpm);
    F(ClockworkClientClock, beat_origin_ntp); F(ClockworkClientClock, is_playing_at_ntp);
    F(ClockworkClientClock, is_playing); F(ClockworkClientClock, flags);
    F(ClockworkClientClock, meter_num); F(ClockworkClientClock, meter_den);

    S(ClockworkScopeReader);
    F(ClockworkScopeReader, struct_bytes); F(ClockworkScopeReader, slot);
    F(ClockworkScopeReader, _internal); F(ClockworkScopeReader, _ring_frames);

    S(ClockworkSinkStats);
    F(ClockworkSinkStats, sent); F(ClockworkSinkStats, dropped); F(ClockworkSinkStats, late);
    F(ClockworkSinkStats, scheduled); F(ClockworkSinkStats, cancelled);

    S(ClockworkStatus); S(ClockworkRegionId); S(ClockworkSinkKind); S(ClockworkSink);

    /* The enumerators, so a renumbering is caught as well as a reshaping. */
    printf("const CLOCKWORK_E_NOMEM %d\n", (int)CLOCKWORK_E_NOMEM);
    printf("const CLOCKWORK_REGION_EGRESS %d\n", (int)CLOCKWORK_REGION_EGRESS);
    printf("const CLOCKWORK_REGION_NATIVE_STATS %d\n", (int)CLOCKWORK_REGION_NATIVE_STATS);
    printf("const kClockworkSinkOsc %d\n", (int)kClockworkSinkOsc);
    printf("const CLOCKWORK_FEATURE_SCHEDULER %d\n", (int)CLOCKWORK_FEATURE_SCHEDULER);
    printf("const CLOCKWORK_ROUTE_NOTIFY %d\n", (int)CLOCKWORK_ROUTE_NOTIFY);
    printf("const CLOCKWORK_ASSET_AUDIO_F32 %d\n", (int)CLOCKWORK_ASSET_AUDIO_F32);
    printf("const DSP_FP_ENV_FLUSH_TO_ZERO %d\n", (int)DSP_FP_ENV_FLUSH_TO_ZERO);
    printf("const CLOCKWORK_CLIENT_ABI_VERSION %d\n", (int)CLOCKWORK_CLIENT_ABI_VERSION);
    printf("const CLOCKWORK_ATTACH_REFUSED %d\n", (int)CLOCKWORK_ATTACH_REFUSED);
    printf("const CLOCKWORK_ATTACH_BOOTED %d\n", (int)CLOCKWORK_ATTACH_BOOTED);
    printf("const CLOCKWORK_ATTACH_JOINED %d\n", (int)CLOCKWORK_ATTACH_JOINED);
    printf("const CLOCKWORK_RESERVED_LANES_MAX %d\n", (int)CLOCKWORK_RESERVED_LANES_MAX);
    S(ClockworkArenaEntry);
    F(ClockworkArenaEntry, id); F(ClockworkArenaEntry, offset); F(ClockworkArenaEntry, bytes);
    F(ClockworkArenaEntry, owner); F(ClockworkArenaEntry, geom);
    S(ClockworkEmbedConfig);
    F(ClockworkEmbedConfig, struct_bytes); F(ClockworkEmbedConfig, sample_rate); F(ClockworkEmbedConfig, block_size);
    F(ClockworkEmbedConfig, input_channels); F(ClockworkEmbedConfig, output_channels);
    F(ClockworkEmbedConfig, max_render_frames); F(ClockworkEmbedConfig, app_name); F(ClockworkEmbedConfig, device);
    F(ClockworkEmbedConfig, instance_id); F(ClockworkEmbedConfig, flags); F(ClockworkEmbedConfig, driver);
    S(ClockworkEmbedDevice);
    F(ClockworkEmbedDevice, struct_bytes); F(ClockworkEmbedDevice, driver); F(ClockworkEmbedDevice, device);
    F(ClockworkEmbedDevice, sample_rate); F(ClockworkEmbedDevice, buffer_frames); F(ClockworkEmbedDevice, block_frames);
    F(ClockworkEmbedDevice, input_channels); F(ClockworkEmbedDevice, output_channels);
    F(ClockworkEmbedDevice, output_latency_frames); F(ClockworkEmbedDevice, input_latency_frames);
    S(ClockworkArenaHeader);
    F(ClockworkArenaHeader, magic); F(ClockworkArenaHeader, version); F(ClockworkArenaHeader, header_bytes);
    F(ClockworkArenaHeader, instance_id); F(ClockworkArenaHeader, arena_bytes); F(ClockworkArenaHeader, block_bytes);
    F(ClockworkArenaHeader, guest_offset); F(ClockworkArenaHeader, guest_bytes); F(ClockworkArenaHeader, entry_count);
    F(ClockworkArenaHeader, entry_bytes); F(ClockworkArenaHeader, state); F(ClockworkArenaHeader, reserved);
    F(ClockworkArenaHeader, entries);
    printf("const CLOCKWORK_ARENA_MAGIC %u\n", (unsigned)CLOCKWORK_ARENA_MAGIC);
    printf("const CLOCKWORK_ARENA_VERSION %d\n", (int)CLOCKWORK_ARENA_VERSION);
    printf("const CLOCKWORK_ARENA_HEADER_BYTES %d\n", (int)CLOCKWORK_ARENA_HEADER_BYTES);
    printf("const CLOCKWORK_ARENA_MAX_ENTRIES %d\n", (int)CLOCKWORK_ARENA_MAX_ENTRIES);
    printf("const CLOCKWORK_ARENA_SCOPE %d\n", (int)CLOCKWORK_ARENA_SCOPE);
    printf("const CLOCKWORK_OWNER_HOST %d\n", (int)CLOCKWORK_OWNER_HOST);
    printf("const CLOCKWORK_GEOM_SLOTS_STACK_BYTES %d\n", (int)CLOCKWORK_GEOM_SLOTS_STACK_BYTES);
    printf("const CLOCKWORK_ARENA_TRACK_TAPS %d\n", (int)CLOCKWORK_ARENA_TRACK_TAPS);
    printf("const CLOCKWORK_GEOM_TRACK_FIRST_INDEX %d\n", (int)CLOCKWORK_GEOM_TRACK_FIRST_INDEX);
    printf("const CLOCKWORK_TAP_IN %d\n", (int)CLOCKWORK_TAP_IN);
    return 0;
}
