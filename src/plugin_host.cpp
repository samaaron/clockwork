// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * plugin_host.cpp — the one definition of plugin_host.h's C symbols.
 *
 * WHY THIS FILE EXISTS. plugin_host.h declares plain C functions, and a build
 * that hosts both CLAP and VST3 cannot have plugin_clap.cpp and
 * plugin_vst3.cpp each define plugin_scan — that is a duplicate symbol at
 * link. So each adapter exports namespaced entry points (plugin_vst3.h,
 * plugin_clap.h) and this file is the only translation unit that defines the
 * public API, dispatching to whichever formats were compiled in.
 *
 * HOW A FORMAT IS CHOSEN. By asking, not by guessing at the file name. Both
 * adapters' scan() open the file and return 0 for anything they cannot load,
 * which plugin_host.h makes a refusal rather than an error — so scanning is
 * simply each format in turn, and opening is the first format whose scan
 * claims the path. A ".vst3" suffix means nothing on Linux, where a bundle is
 * a directory and a bare .so is common, so the suffix is not consulted.
 *
 * HostedPlugin is a two-word wrapper carrying the format tag and the adapter's
 * own instance pointer. The alternative — a shared tag as the first member of
 * every adapter's struct — saves one indirection per block and costs a layout
 * convention that nothing enforces.
 */

#include "plugin_host.h"

#if CLOCKWORK_PLUGIN_VST3
#  include "plugin_vst3.h"
#endif
#if CLOCKWORK_PLUGIN_CLAP
#  include "plugin_clap.h"
#endif

#include <new>
#include <cstdio>
#include <cstring>
#include <chrono>

struct HostedPlugin {
    PluginFormat format = kPluginFormatUnknown;
    void*        impl   = nullptr;

    // Health, kept per instance. Written only on the audio thread inside
    // plugin_process and read from a control thread; plain integers rather
    // than atomics because a torn read of a diagnostic counter is worth less
    // than an atomic on the audio path, and the numbers are advisory.
    char        name[96] = {};
    uint64_t    calls = 0, max_ns = 0, total_ns = 0, budget_overruns = 0;
    uint64_t    budget_ns = 0;          // set per call from frames / rate
    double      sample_rate = 0.0;
};

namespace {
// The name a user will see in a submitted log. The plugin's own if a scan
// reports one, otherwise the file it came from — never empty, because an
// unattributed complaint is the thing this exists to prevent.
void recordName(HostedPlugin* h, const char* path, uint32_t index) {
    PluginDesc d{};
    if (plugin_scan(path, &d, 1) > index && d.name && d.name[0]) {
        std::snprintf(h->name, sizeof h->name, "%s", d.name);
        return;
    }
    const char* base = path ? std::strrchr(path, '/') : nullptr;
    std::snprintf(h->name, sizeof h->name, "%s", base ? base + 1 : (path ? path : "?"));
}
} // namespace

extern "C" {

uint32_t plugin_scan(const char* path, PluginDesc* out, uint32_t cap) {
#if CLOCKWORK_PLUGIN_CLAP
    if (const uint32_t n = clockwork_plugin_clap::scan(path, out, cap)) return n;
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (const uint32_t n = clockwork_plugin_vst3::scan(path, out, cap)) return n;
#endif
    (void)path; (void)out; (void)cap;
    return 0;
}

struct HostedPlugin* plugin_open(const char* path, uint32_t index,
                                 double sample_rate, uint32_t max_block,
                                 const char** err) {
    if (err) *err = nullptr;
#if CLOCKWORK_PLUGIN_CLAP
    if (clockwork_plugin_clap::scan(path, nullptr, 0) > index) {
        if (auto* i = clockwork_plugin_clap::open(path, index, sample_rate, max_block, err)) {
            auto* h = new (std::nothrow) HostedPlugin();
            if (h) { h->format = kPluginFormatClap; h->impl = i;
                     h->sample_rate = sample_rate; recordName(h, path, index); return h; }
            clockwork_plugin_clap::close(i);
        }
        return nullptr;
    }
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (auto* i = clockwork_plugin_vst3::open(path, index, sample_rate, max_block, err)) {
        auto* h = new (std::nothrow) HostedPlugin();
        if (h) { h->format = kPluginFormatVst3; h->impl = i;
                 h->sample_rate = sample_rate; recordName(h, path, index); }
        if (h) return h;
        clockwork_plugin_vst3::close(i);
    }
    return nullptr;
#else
    // Reached when the path is not a CLAP this build recognises and there is
    // no VST3 adapter to try next. The message says which it is: a build
    // with no formats at all is a different problem from a build that has
    // one and was handed the other.
    (void)index; (void)sample_rate; (void)max_block;
    if (err) {
#  if CLOCKWORK_PLUGIN_CLAP
        *err = "not a CLAP plugin, and this build hosts no other format";
#  else
        *err = "no plugin formats compiled in";
#  endif
    }
    (void)path;
    return nullptr;
#endif
}

void plugin_close(struct HostedPlugin* p) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        clockwork_plugin_clap::close(static_cast<clockwork_plugin_clap::Instance*>(p->impl));
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        clockwork_plugin_vst3::close(static_cast<clockwork_plugin_vst3::Instance*>(p->impl));
#endif
    delete p;
}

PluginFormat plugin_format(const struct HostedPlugin* p) {
    return p ? p->format : kPluginFormatUnknown;
}

// Monotonic nanoseconds. steady_clock is CLOCK_MONOTONIC on Linux and macOS
// — a vDSO read, no syscall, no lock — and QueryPerformanceCounter on
// Windows, which is the same kind of read there. That is why timing every
// call is affordable at all, and why this is not clock_gettime, which
// Windows does not have.
static inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void plugin_process(struct HostedPlugin* p,
                    const float* const* in, uint32_t n_in,
                    float* const* out, uint32_t n_out,
                    uint32_t frames, int64_t block_time) {
    if (!p) return;
    const uint64_t t0 = now_ns();
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        clockwork_plugin_clap::process(static_cast<clockwork_plugin_clap::Instance*>(p->impl),
                                 in, n_in, out, n_out, frames, block_time);
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        clockwork_plugin_vst3::process(static_cast<clockwork_plugin_vst3::Instance*>(p->impl),
                                 in, n_in, out, n_out, frames, block_time);
#endif
    (void)in; (void)n_in; (void)out; (void)n_out; (void)block_time;

    // The budget is this block's worth of real time. A call that takes longer
    // than the audio it produced cannot be sustained, whatever the cause —
    // allocation, a lock, a page fault, or simply being slow.
    const uint64_t dt = now_ns() - t0;
    p->calls++;
    p->total_ns += dt;
    if (dt > p->max_ns) p->max_ns = dt;
    if (p->sample_rate > 0.0) {
        p->budget_ns = static_cast<uint64_t>(
            (static_cast<double>(frames) / p->sample_rate) * 1e9);
        if (dt > p->budget_ns) p->budget_overruns++;
    }
}

int plugin_health(const struct HostedPlugin* p, PluginHealth* out) {
    if (!p || !out) return 0;
    out->name            = p->name[0] ? p->name : "(unnamed)";
    out->calls           = p->calls;
    out->max_ns          = p->max_ns;
    out->total_ns        = p->total_ns;
    out->budget_ns       = p->budget_ns;
    out->budget_overruns = p->budget_overruns;
    out->allocations     = 0;   // instrumented builds fill this in
    return 1;
}

uint32_t plugin_health_line(const struct HostedPlugin* p, char* out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    PluginHealth h{};
    if (!plugin_health(p, &h)) { out[0] = 0; return 0; }
    const double avg_us = h.calls ? (double)h.total_ns / (double)h.calls / 1000.0 : 0.0;
    const char* fmt = h.budget_overruns
        ? "plugin %s: %llu calls, avg %.1fus, max %.1fus, budget %.1fus, OVER BUDGET %llu times"
        : "plugin %s: %llu calls, avg %.1fus, max %.1fus, budget %.1fus, within budget";
    const int n = std::snprintf(out, cap, fmt, h.name,
                                (unsigned long long)h.calls, avg_us,
                                h.max_ns / 1000.0, h.budget_ns / 1000.0,
                                (unsigned long long)h.budget_overruns);
    return n < 0 ? 0u : (uint32_t)((uint32_t)n < cap ? n : cap - 1);
}

uint32_t plugin_latency(const struct HostedPlugin* p) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        return clockwork_plugin_clap::latency(static_cast<const clockwork_plugin_clap::Instance*>(p->impl));
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::latency(static_cast<const clockwork_plugin_vst3::Instance*>(p->impl));
#endif
    return 0;
}

uint32_t plugin_param_count(const struct HostedPlugin* p) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        return clockwork_plugin_clap::param_count(static_cast<const clockwork_plugin_clap::Instance*>(p->impl));
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::param_count(static_cast<const clockwork_plugin_vst3::Instance*>(p->impl));
#endif
    return 0;
}

int plugin_param_info(const struct HostedPlugin* p, uint32_t index, PluginParam* out) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        return clockwork_plugin_clap::param_info(
            static_cast<const clockwork_plugin_clap::Instance*>(p->impl), index, out);
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::param_info(
            static_cast<const clockwork_plugin_vst3::Instance*>(p->impl), index, out);
#endif
    (void)index; (void)out;
    return 0;
}

void plugin_set_param_listener(struct HostedPlugin* p, PluginParamEditFn fn, void* ctx) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        clockwork_plugin_vst3::set_param_listener(
            static_cast<clockwork_plugin_vst3::Instance*>(p->impl), fn, ctx);
#endif
    (void)fn; (void)ctx;
}

/* ── Editor ─────────────────────────────────────────────────────────────── */
// VST3 only. CLAP's editor is an extension the adapter does not implement, so
// a CLAP plugin answers "no editor" rather than pretending.

int plugin_editor_has(struct HostedPlugin* p) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::editor_has(
            static_cast<clockwork_plugin_vst3::Instance*>(p->impl)) ? 1 : 0;
#endif
    return 0;
}

int plugin_editor_open(struct HostedPlugin* p, void* parent_native_view) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::editor_open(
            static_cast<clockwork_plugin_vst3::Instance*>(p->impl), parent_native_view) ? 1 : 0;
#endif
    (void)parent_native_view;
    return 0;
}

void plugin_editor_close(struct HostedPlugin* p) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        clockwork_plugin_vst3::editor_close(static_cast<clockwork_plugin_vst3::Instance*>(p->impl));
#endif
}

int plugin_editor_size(struct HostedPlugin* p, uint32_t* w, uint32_t* h) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::editor_size(
            static_cast<clockwork_plugin_vst3::Instance*>(p->impl), w, h) ? 1 : 0;
#endif
    (void)w; (void)h;
    return 0;
}

int plugin_editor_set_size(struct HostedPlugin* p, uint32_t w, uint32_t h) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::editor_set_size(
            static_cast<clockwork_plugin_vst3::Instance*>(p->impl), w, h) ? 1 : 0;
#endif
    (void)w; (void)h;
    return 0;
}

void plugin_set_clock(struct HostedPlugin* p, const struct ClockworkClockState* clock) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        clockwork_plugin_vst3::set_clock(static_cast<clockwork_plugin_vst3::Instance*>(p->impl), clock);
#endif
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        clockwork_plugin_clap::set_clock(static_cast<clockwork_plugin_clap::Instance*>(p->impl), clock);
#endif
    (void)clock;
}

void plugin_set_timeline(struct HostedPlugin* p, const struct ClockworkTimeline* timeline) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        clockwork_plugin_vst3::set_timeline(static_cast<clockwork_plugin_vst3::Instance*>(p->impl), timeline);
#endif
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        clockwork_plugin_clap::set_timeline(static_cast<clockwork_plugin_clap::Instance*>(p->impl), timeline);
#endif
    (void)timeline;
}

static void note_dispatch(struct HostedPlugin* p, bool on, int16_t channel,
                          int16_t pitch, float velocity, uint32_t frame_offset) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3) {
        clockwork_plugin_vst3::note(static_cast<clockwork_plugin_vst3::Instance*>(p->impl),
                              on, channel, pitch, velocity, frame_offset);
        return;
    }
#endif
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap) {
        clockwork_plugin_clap::note(static_cast<clockwork_plugin_clap::Instance*>(p->impl),
                              on, channel, pitch, velocity, frame_offset);
        return;
    }
#endif
    (void)on; (void)channel; (void)pitch; (void)velocity; (void)frame_offset;
}

void plugin_cc(struct HostedPlugin* p, int16_t channel, uint8_t number,
               float value, uint32_t frame_offset) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3) {
        clockwork_plugin_vst3::cc(static_cast<clockwork_plugin_vst3::Instance*>(p->impl),
                            channel, number, value, frame_offset);
        return;
    }
#endif
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap) {
        clockwork_plugin_clap::cc(static_cast<clockwork_plugin_clap::Instance*>(p->impl),
                            channel, number, value, frame_offset);
        return;
    }
#endif
    (void)channel; (void)number; (void)value; (void)frame_offset;
}

void plugin_pitch_bend(struct HostedPlugin* p, int16_t channel, float bend,
                       uint32_t frame_offset) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3) {
        clockwork_plugin_vst3::pitch_bend(static_cast<clockwork_plugin_vst3::Instance*>(p->impl),
                                    channel, bend, frame_offset);
        return;
    }
#endif
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap) {
        clockwork_plugin_clap::pitch_bend(static_cast<clockwork_plugin_clap::Instance*>(p->impl),
                                    channel, bend, frame_offset);
        return;
    }
#endif
    (void)channel; (void)bend; (void)frame_offset;
}

void plugin_all_notes_off(struct HostedPlugin* p, uint32_t frame_offset) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3) {
        clockwork_plugin_vst3::all_notes_off(static_cast<clockwork_plugin_vst3::Instance*>(p->impl),
                                       frame_offset);
        return;
    }
#endif
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap) {
        clockwork_plugin_clap::all_notes_off(static_cast<clockwork_plugin_clap::Instance*>(p->impl),
                                       frame_offset);
        return;
    }
#endif
    (void)frame_offset;
}

void plugin_note_on(struct HostedPlugin* p, int16_t channel, int16_t pitch,
                    float velocity, uint32_t frame_offset) {
    note_dispatch(p, true, channel, pitch, velocity, frame_offset);
}

void plugin_note_off(struct HostedPlugin* p, int16_t channel, int16_t pitch,
                     float velocity, uint32_t frame_offset) {
    note_dispatch(p, false, channel, pitch, velocity, frame_offset);
}

void plugin_param_set(struct HostedPlugin* p, uint32_t id, double value,
                      uint32_t frame_offset) {
    if (!p) return;
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap) {
        clockwork_plugin_clap::param_set(static_cast<clockwork_plugin_clap::Instance*>(p->impl),
                                   id, value, frame_offset);
        return;
    }
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        clockwork_plugin_vst3::param_set(static_cast<clockwork_plugin_vst3::Instance*>(p->impl),
                                   id, value, frame_offset);
#endif
    (void)id; (void)value; (void)frame_offset;
}

uint32_t plugin_state_save(struct HostedPlugin* p, uint8_t* out, uint32_t cap) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        return clockwork_plugin_clap::state_save(
            static_cast<clockwork_plugin_clap::Instance*>(p->impl), out, cap);
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::state_save(
            static_cast<clockwork_plugin_vst3::Instance*>(p->impl), out, cap);
#endif
    (void)out; (void)cap;
    return 0;
}

int plugin_state_load(struct HostedPlugin* p, const uint8_t* bytes, uint32_t len) {
    if (!p) return 0;
#if CLOCKWORK_PLUGIN_CLAP
    if (p->format == kPluginFormatClap)
        return clockwork_plugin_clap::state_load(
            static_cast<clockwork_plugin_clap::Instance*>(p->impl), bytes, len);
#endif
#if CLOCKWORK_PLUGIN_VST3
    if (p->format == kPluginFormatVst3)
        return clockwork_plugin_vst3::state_load(
            static_cast<clockwork_plugin_vst3::Instance*>(p->impl), bytes, len);
#endif
    (void)bytes; (void)len;
    return 0;
}

}  // extern "C"
