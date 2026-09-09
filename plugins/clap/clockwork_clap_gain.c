// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
/*
 * clockwork_clap_gain.c — the smallest CLAP plugin that can prove the host works.
 *
 * It exists so plugin hosting is testable in-tree, with no third-party binary
 * to install and nothing to go stale. Its behaviour is therefore a SPEC, not
 * an implementation detail, and it is deliberately shared with the VST3 test
 * plugin so a differential test can assert both formats produce identical
 * samples:
 *
 *     out[c][i] = in[c][i] * gain          for c < input channel count
 *     out[c][i] = 0                        for the outputs beyond that
 *
 * One parameter: id 0, "gain", 0.0 .. 2.0, default 0.5. Zero latency. State
 * is the gain as a little-endian IEEE-754 double, exactly 8 bytes — the
 * smallest blob that is still a real serialisation, and identical across
 * formats so a blob is comparable.
 *
 * The gain is applied as a float multiply against a float-narrowed gain (not
 * a double multiply narrowed afterwards) so that the two formats' arithmetic
 * is bit-identical rather than merely close. Do not "improve" that.
 *
 * NO GUI EXTENSION. Not an omission: plugin_host.h refuses plugin editors
 * outright, and a plugin with no clap.gui is what proves the host never asks
 * for one.
 */
#include <clap/clap.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCKWORK_GAIN_PARAM_ID   0
#define CLOCKWORK_GAIN_MIN        0.0
#define CLOCKWORK_GAIN_MAX        2.0
#define CLOCKWORK_GAIN_DEFAULT    0.5

typedef struct {
   clap_plugin_t        plugin;
   const clap_host_t   *host;
   double               gain;
   double               sample_rate;
} clockwork_gain_t;

static const char *const s_features[] = {
   CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
   CLAP_PLUGIN_FEATURE_UTILITY,
   CLAP_PLUGIN_FEATURE_STEREO,
   NULL,
};

static const clap_plugin_descriptor_t s_descriptor = {
   .clap_version = CLAP_VERSION_INIT,
   .id           = "clockwork.clap.gain",
   .name         = "Clockwork Test Gain",
   .vendor       = "Clockwork",
   .url          = "",
   .manual_url   = "",
   .support_url  = "",
   .version      = "1.0.0",
   .description  = "out = in * gain; clockwork's in-tree hosting fixture",
   .features     = s_features,
};

/* ── clap.audio-ports ─────────────────────────────────────────────────────── */

static uint32_t CLAP_ABI ap_count(const clap_plugin_t *plugin, bool is_input) {
   (void)plugin; (void)is_input;
   return 1;
}

static bool CLAP_ABI
ap_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info) {
   (void)plugin;
   if (index != 0)
      return false;
   info->id            = 0;
   snprintf(info->name, sizeof(info->name), "%s", is_input ? "in" : "out");
   info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
   info->channel_count = 2;
   info->port_type     = CLAP_PORT_STEREO;
   info->in_place_pair = CLAP_INVALID_ID;   /* no in-place: the host copies */
   return true;
}

static const clap_plugin_audio_ports_t s_audio_ports = {
   .count = ap_count,
   .get   = ap_get,
};

/* ── clap.params ──────────────────────────────────────────────────────────── */

static uint32_t CLAP_ABI params_count(const clap_plugin_t *plugin) {
   (void)plugin;
   return 1;
}

static bool CLAP_ABI
params_get_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info) {
   (void)plugin;
   if (index != 0)
      return false;
   memset(info, 0, sizeof(*info));
   info->id            = CLOCKWORK_GAIN_PARAM_ID;
   info->flags         = CLAP_PARAM_IS_AUTOMATABLE;
   info->cookie        = NULL;
   snprintf(info->name, sizeof(info->name), "gain");
   info->module[0]     = '\0';
   info->min_value     = CLOCKWORK_GAIN_MIN;
   info->max_value     = CLOCKWORK_GAIN_MAX;
   info->default_value = CLOCKWORK_GAIN_DEFAULT;
   return true;
}

static bool CLAP_ABI
params_get_value(const clap_plugin_t *plugin, clap_id id, double *out) {
   clockwork_gain_t *p = plugin->plugin_data;
   if (id != CLOCKWORK_GAIN_PARAM_ID)
      return false;
   *out = p->gain;
   return true;
}

static bool CLAP_ABI params_value_to_text(
   const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t cap) {
   (void)plugin;
   if (id != CLOCKWORK_GAIN_PARAM_ID)
      return false;
   snprintf(out, cap, "%.3f", value);
   return true;
}

static bool CLAP_ABI
params_text_to_value(const clap_plugin_t *plugin, clap_id id, const char *text, double *out) {
   (void)plugin;
   if (id != CLOCKWORK_GAIN_PARAM_ID)
      return false;
   *out = atof(text);
   return true;
}

/* Clamping is the plugin's job: a host may automate outside the range and the
   plugin is the only thing that knows what the range means. */
static void clockwork_set_gain(clockwork_gain_t *p, double v) {
   if (v < CLOCKWORK_GAIN_MIN) v = CLOCKWORK_GAIN_MIN;
   if (v > CLOCKWORK_GAIN_MAX) v = CLOCKWORK_GAIN_MAX;
   p->gain = v;
}

static void clockwork_handle_event(clockwork_gain_t *p, const clap_event_header_t *hdr) {
   if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
      return;
   if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
      const clap_event_param_value_t *ev = (const clap_event_param_value_t *)hdr;
      if (ev->param_id == CLOCKWORK_GAIN_PARAM_ID)
         clockwork_set_gain(p, ev->value);
   }
}

static void CLAP_ABI params_flush(const clap_plugin_t        *plugin,
                                  const clap_input_events_t  *in,
                                  const clap_output_events_t *out) {
   clockwork_gain_t    *p = plugin->plugin_data;
   const uint32_t n = in->size(in);
   (void)out;
   for (uint32_t i = 0; i < n; ++i)
      clockwork_handle_event(p, in->get(in, i));
}

static const clap_plugin_params_t s_params = {
   .count         = params_count,
   .get_info      = params_get_info,
   .get_value     = params_get_value,
   .value_to_text = params_value_to_text,
   .text_to_value = params_text_to_value,
   .flush         = params_flush,
};

/* ── clap.state ───────────────────────────────────────────────────────────── */
/* Eight bytes, little-endian IEEE-754 double. Written byte by byte rather
   than by memcpy of the host's native double so the blob is the same on a
   big-endian machine, and so it is the same blob the VST3 plugin writes. */

static bool CLAP_ABI state_save(const clap_plugin_t *plugin, const clap_ostream_t *os) {
   clockwork_gain_t *p = plugin->plugin_data;
   uint64_t    bits;
   uint8_t     bytes[8];
   double      g = p->gain;
   memcpy(&bits, &g, sizeof(bits));
   for (int i = 0; i < 8; ++i)
      bytes[i] = (uint8_t)((bits >> (8 * i)) & 0xFFu);

   uint32_t written = 0;
   while (written < 8) {
      int64_t n = os->write(os, bytes + written, 8 - written);
      if (n <= 0)
         return false;
      written += (uint32_t)n;
   }
   return true;
}

static bool CLAP_ABI state_load(const clap_plugin_t *plugin, const clap_istream_t *is) {
   clockwork_gain_t *p = plugin->plugin_data;
   uint8_t     bytes[8];
   uint32_t    got = 0;
   while (got < 8) {
      int64_t n = is->read(is, bytes + got, 8 - got);
      if (n <= 0)
         return false;               /* short blob: not ours, refuse it */
      got += (uint32_t)n;
   }
   uint64_t bits = 0;
   for (int i = 0; i < 8; ++i)
      bits |= ((uint64_t)bytes[i]) << (8 * i);
   double g;
   memcpy(&g, &bits, sizeof(g));
   if (!(g >= CLOCKWORK_GAIN_MIN && g <= CLOCKWORK_GAIN_MAX))
      return false;                  /* NaN, or out of range: not ours */
   p->gain = g;
   return true;
}

static const clap_plugin_state_t s_state = {
   .save = state_save,
   .load = state_load,
};

/* ── clap.latency ─────────────────────────────────────────────────────────── */

static uint32_t CLAP_ABI latency_get(const clap_plugin_t *plugin) {
   (void)plugin;
   return 0;
}

static const clap_plugin_latency_t s_latency = { .get = latency_get };

/* ── the plugin itself ────────────────────────────────────────────────────── */

static bool CLAP_ABI plug_init(const clap_plugin_t *plugin) {
   (void)plugin;
   return true;
}

static void CLAP_ABI plug_destroy(const clap_plugin_t *plugin) {
   free(plugin->plugin_data);
}

static bool CLAP_ABI
plug_activate(const clap_plugin_t *plugin, double sr, uint32_t min_frames, uint32_t max_frames) {
   clockwork_gain_t *p = plugin->plugin_data;
   (void)min_frames; (void)max_frames;
   p->sample_rate = sr;
   return true;
}

static void CLAP_ABI plug_deactivate(const clap_plugin_t *plugin) { (void)plugin; }
static bool CLAP_ABI plug_start_processing(const clap_plugin_t *plugin) { (void)plugin; return true; }
static void CLAP_ABI plug_stop_processing(const clap_plugin_t *plugin) { (void)plugin; }

static void CLAP_ABI plug_reset(const clap_plugin_t *plugin) { (void)plugin; }

/* The transport the host handed the last block, read back through the
   clockwork_clap_gain_last_transport boundary below: what the host SAYS the block is,
   which no output sample can show. Written and read on the test's thread. */
static struct {
   bool   any;        /* a block has been processed */
   bool   has;        /* ...and it carried a transport */
   double tempo, beat, bar_start;
   int    bar_number, num, den, playing;
} s_last_transport;

static clap_process_status CLAP_ABI
plug_process(const clap_plugin_t *plugin, const clap_process_t *proc) {
   clockwork_gain_t    *p       = plugin->plugin_data;
   const uint32_t nframes = proc->frames_count;

   s_last_transport.any = true;
   s_last_transport.has = proc->transport != NULL;
   if (proc->transport) {
      const clap_event_transport_t *t = proc->transport;
      s_last_transport.tempo      = t->tempo;
      s_last_transport.beat       = (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
      s_last_transport.bar_start  = (double)t->bar_start / (double)CLAP_BEATTIME_FACTOR;
      s_last_transport.bar_number = t->bar_number;
      s_last_transport.num        = t->tsig_num;
      s_last_transport.den        = t->tsig_denom;
      s_last_transport.playing    = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
   }
   const uint32_t nev     = proc->in_events ? proc->in_events->size(proc->in_events) : 0;

   const uint32_t in_ch  = proc->audio_inputs_count  > 0 ? proc->audio_inputs[0].channel_count  : 0;
   const uint32_t out_ch = proc->audio_outputs_count > 0 ? proc->audio_outputs[0].channel_count : 0;

   uint32_t ev_index = 0;
   uint32_t i        = 0;

   while (i < nframes) {
      /* Apply every event at or before frame i, then find where the next one
         lands and run plainly up to it. This is what makes automation
         sample-accurate: the gain changes between sample i-1 and sample i,
         not at a block boundary. Events at or before i are consumed rather
         than deferred, so an out-of-order queue cannot stall the loop. */
      uint32_t next_ev = nframes;
      while (ev_index < nev) {
         const clap_event_header_t *hdr = proc->in_events->get(proc->in_events, ev_index);
         if (!hdr) { ++ev_index; continue; }
         if (hdr->time > i) { next_ev = hdr->time; break; }
         clockwork_handle_event(p, hdr);
         ++ev_index;
      }
      if (next_ev > nframes)
         next_ev = nframes;

      const float g = (float)p->gain;
      for (; i < next_ev; ++i) {
         for (uint32_t c = 0; c < out_ch; ++c) {
            float *o = proc->audio_outputs[0].data32[c];
            o[i] = (c < in_ch) ? proc->audio_inputs[0].data32[c][i] * g : 0.0f;
         }
      }
   }

   return CLAP_PROCESS_CONTINUE;
}

/* Deliberately no "clap.gui": plugin_host.h hosts no editors, and this is
   how that is proved rather than assumed. */
static const void *CLAP_ABI plug_get_extension(const clap_plugin_t *plugin, const char *id) {
   (void)plugin;
   if (!strcmp(id, CLAP_EXT_PARAMS))       return &s_params;
   if (!strcmp(id, CLAP_EXT_STATE))        return &s_state;
   if (!strcmp(id, CLAP_EXT_LATENCY))      return &s_latency;
   if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))  return &s_audio_ports;
   return NULL;
}

static void CLAP_ABI plug_on_main_thread(const clap_plugin_t *plugin) { (void)plugin; }

/* ── factory ──────────────────────────────────────────────────────────────── */

static uint32_t CLAP_ABI factory_get_plugin_count(const clap_plugin_factory_t *f) {
   (void)f;
   return 1;
}

static const clap_plugin_descriptor_t *CLAP_ABI
factory_get_plugin_descriptor(const clap_plugin_factory_t *f, uint32_t index) {
   (void)f;
   return index == 0 ? &s_descriptor : NULL;
}

static const clap_plugin_t *CLAP_ABI factory_create_plugin(const clap_plugin_factory_t *f,
                                                           const clap_host_t           *host,
                                                           const char                  *plugin_id) {
   (void)f;
   if (!clap_version_is_compatible(host->clap_version))
      return NULL;
   if (strcmp(plugin_id, s_descriptor.id))
      return NULL;

   clockwork_gain_t *p = calloc(1, sizeof(*p));
   if (!p)
      return NULL;
   p->host = host;
   p->gain = CLOCKWORK_GAIN_DEFAULT;

   p->plugin.desc             = &s_descriptor;
   p->plugin.plugin_data      = p;
   p->plugin.init             = plug_init;
   p->plugin.destroy          = plug_destroy;
   p->plugin.activate         = plug_activate;
   p->plugin.deactivate       = plug_deactivate;
   p->plugin.start_processing = plug_start_processing;
   p->plugin.stop_processing  = plug_stop_processing;
   p->plugin.reset            = plug_reset;
   p->plugin.process          = plug_process;
   p->plugin.get_extension    = plug_get_extension;
   p->plugin.on_main_thread   = plug_on_main_thread;
   return &p->plugin;
}

static const clap_plugin_factory_t s_factory = {
   .get_plugin_count      = factory_get_plugin_count,
   .get_plugin_descriptor = factory_get_plugin_descriptor,
   .create_plugin         = factory_create_plugin,
};

/* ── entry ────────────────────────────────────────────────────────────────── */
/* init/deinit are counted: CLAP 1.2 makes multiple unmatched pairs a hard
   requirement rather than a courtesy, and clockwork's scanner really does
   init the same DSO twice when it scans a file it already has open. */

static int s_init_count = 0;

static bool CLAP_ABI entry_init(const char *plugin_path) {
   (void)plugin_path;
   ++s_init_count;
   return true;
}

static void CLAP_ABI entry_deinit(void) {
   if (s_init_count > 0)
      --s_init_count;
}

static const void *CLAP_ABI entry_get_factory(const char *factory_id) {
   if (s_init_count <= 0)
      return NULL;
   if (!strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID))
      return &s_factory;
   return NULL;
}

/* Not CLAP: a test boundary. The transport the host handed the last block, in
   plain doubles and ints so a test needs no CLAP header: returns false when
   no block has run or the last block came with no transport (free running). */
CLAP_EXPORT bool clockwork_clap_gain_last_transport(double *tempo, double *beat, double *bar_start,
                                              int *bar_number, int *num, int *den, int *playing) {
   if (!s_last_transport.any || !s_last_transport.has)
      return false;
   if (tempo)      *tempo      = s_last_transport.tempo;
   if (beat)       *beat       = s_last_transport.beat;
   if (bar_start)  *bar_start  = s_last_transport.bar_start;
   if (bar_number) *bar_number = s_last_transport.bar_number;
   if (num)        *num        = s_last_transport.num;
   if (den)        *den        = s_last_transport.den;
   if (playing)    *playing    = s_last_transport.playing;
   return true;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
   .clap_version = CLAP_VERSION_INIT,
   .init         = entry_init,
   .deinit       = entry_deinit,
   .get_factory  = entry_get_factory,
};
