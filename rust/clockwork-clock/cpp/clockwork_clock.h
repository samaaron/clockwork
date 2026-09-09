/* SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
 * Copyright (c) 2026 Sam Aaron
 *
 * clockwork_clock.h — the C ABI of rust/clockwork-clock.
 *
 * Clockwork's timelines: the beat/time snapshot every reader answers from, and
 * the registry of midi:<port> follower timelines behind
 * /clockwork/clock/midi:<port>/. The C++ contract is src/clock/Timeline.h and
 * src/clock/MidiTimelines.h; this is the boundary between them and the Rust that
 * implements them.
 *
 * Time is NTP seconds everywhere; nothing here reads a clock — every `now`
 * is the caller's. The registry handle is internally locked: any thread may
 * call any function on it — except the audio thread, which takes
 * clockwork_midi_timelines_timeline_rt, the one lock-free reader.
 */
#ifndef CLOCKWORK_CLOCK_H
#define CLOCKWORK_CLOCK_H

#include <stdint.h>

/* 8-byte alignment, stated rather than inherited. ClockworkTimeline is 4
 * doubles + 5 int32 = 52 bytes of members, and on a 64-bit ABI a double's
 * alignment rounds that to 56. On i386 a double aligns to 4, so the struct
 * would be 52 — which breaks BOTH of the things that depend on its size:
 * Timeline.h pins it at 56, and timeline_mirror.h carries it across a process
 * boundary "as whole 64-bit words", which 52 is not. This struct is carried
 * as 64-bit words, so it aligns like one, everywhere. */
#if defined(__cplusplus)
#  define CLOCKWORK_ALIGN8 alignas(8)
#elif defined(_MSC_VER)
#  define CLOCKWORK_ALIGN8 __declspec(align(8))
#else
#  define CLOCKWORK_ALIGN8 _Alignas(8)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* One beat grid, copied out of its source at the moment of asking. Mirrors
 * clockwork_clock::Timeline field for field (repr(C)); both sides pin the size. */
typedef struct CLOCKWORK_ALIGN8 ClockworkTimeline {
    double  bpm;              /* >= 1 */
    double  anchor_beat;      /* anchor_beat is at anchor_ntp; bpm/60 per second either side */
    double  anchor_ntp;
    double  transition_ntp;   /* when the transport last changed; 0 = never (a sentinel, not a time) */
    int32_t id;               /* 0 Link, 1..K midi slots, -1 a never-seen name */
    int32_t playing;
    int32_t anchored;         /* something has defined where beat 0 is */
    int32_t meter_num;        /* how quarter-note beats group into bars; 4/4 until set */
    int32_t meter_den;
} ClockworkTimeline;

ClockworkTimeline clockwork_timeline_placeholder(int32_t id);      /* 60 BPM from the epoch, stopped, unanchored */
double clockwork_timeline_beat_at(const ClockworkTimeline* t, double ntp);
double clockwork_timeline_time_at_beat(const ClockworkTimeline* t, double beat);
double clockwork_timeline_phase_at(const ClockworkTimeline* t, double ntp, double quantum);
/* Bars: quarter-note beats per bar is meter_num * 4 / meter_den, and bar 0
 * starts at beat 0. bar_at is whole-valued (negative before beat 0);
 * beat_in_bar_at is in 0..beats_per_bar. */
double clockwork_timeline_beats_per_bar(const ClockworkTimeline* t);
double clockwork_timeline_bar_at(const ClockworkTimeline* t, double ntp);
double clockwork_timeline_beat_in_bar_at(const ClockworkTimeline* t, double ntp);
/* 1 if num/den is a meter a bar can be built from: num >= 1, den in {1,2,4,8,16,32}. */
int32_t clockwork_timeline_meter_valid(int32_t num, int32_t den);
/* 1 if the segment names a timeline — "link", "midi", "midi:<port>" — as the
 * /clockwork/clock/<tl>/<verb> router must decide before it can resolve. */
int32_t clockwork_timeline_is_name(const char* name, uint32_t name_len);

/* MIDI clock out. A follower of a timeline emits pulse k at the grid's beat
 * k / 24; it remembers only the index of the next one it has not handed over.
 * clockwork_midi_clock_out_due answers which indices, from `next` (or UNSTARTED),
 * fall inside now + horizon — at most `max`, the rest owed next call — writing
 * the count to *count and returning the first; ask from first + count next
 * time. `floor` is the latest time already committed to the wire: a follower
 * that starts or re-syncs begins after it. A grid that moved more than a beat
 * from `next` re-syncs to the first pulse after now instead of catching up.
 * Rust: midi_clock_out.rs. */
#define CLOCKWORK_MIDI_CLOCK_OUT_UNSTARTED INT64_MIN
int64_t clockwork_midi_clock_out_due(const ClockworkTimeline* t, int64_t next, double floor, double now,
                               double horizon, uint32_t max, uint32_t* count);
double  clockwork_midi_clock_out_pulse_ntp(const ClockworkTimeline* t, int64_t index);

/* Opaque registry. */
typedef struct ClockworkMidiTimelines ClockworkMidiTimelines;

/* One listing row, borrowed for the duration of the callback: copy what you keep. */
typedef struct ClockworkTimelineInfo {
    int32_t     id;
    const char* name;  uint32_t name_len;   /* wire identity: "midi:<handle>" */
    const char* raw;   uint32_t raw_len;    /* OS device name, for display */
    double      bpm;
    int32_t     clocking;                   /* a clock is arriving (not stale) */
    int32_t     stale;
    int32_t     primary;
} ClockworkTimelineInfo;

typedef void (*ClockworkTimelineInfoFn)(void* ctx, const ClockworkTimelineInfo* info);

/* Every mutator returns 1 when the listing changed (added, removed, stale,
 * primary, or a tempo worth pushing) so the caller can broadcast
 * /clockwork/clock/timelines, else 0. Ids are 1..max_slots; -1 is "none". */
ClockworkMidiTimelines* clockwork_midi_timelines_new(uint32_t max_slots);
void    clockwork_midi_timelines_free(ClockworkMidiTimelines* h);
int32_t clockwork_midi_timelines_claim(ClockworkMidiTimelines* h,
                                 const char* normalized, uint32_t normalized_len,
                                 const char* raw, uint32_t raw_len,
                                 double now, int32_t* changed);
int32_t clockwork_midi_timelines_release(ClockworkMidiTimelines* h, int32_t id);
int32_t clockwork_midi_timelines_resolve(const ClockworkMidiTimelines* h, const char* name, uint32_t name_len);
int32_t clockwork_midi_timelines_resolve_or_claim(ClockworkMidiTimelines* h, const char* name, uint32_t name_len,
                                            double now, int32_t* changed);
int32_t clockwork_midi_timelines_pulse(ClockworkMidiTimelines* h, int32_t id, uint64_t ts_us, double now);
int32_t clockwork_midi_timelines_set_tempo(ClockworkMidiTimelines* h, int32_t id, double bpm, double now);
/* 1 if set; 0 for a meter clockwork_timeline_meter_valid refuses or an unheld slot.
 * The listing does not carry the meter, so this never asks for a push. */
int32_t clockwork_midi_timelines_set_meter(ClockworkMidiTimelines* h, int32_t id, int32_t num, int32_t den);
/* kind: 0 Start, 1 Continue, 2 Stop, 3 Song Position (at `beat`). */
int32_t clockwork_midi_timelines_transport(ClockworkMidiTimelines* h, int32_t id, int32_t kind, double beat, double now);
int32_t clockwork_midi_timelines_tick_stale(ClockworkMidiTimelines* h, double now);
int32_t clockwork_midi_timelines_primary(const ClockworkMidiTimelines* h);
/* 1 and *out filled if slot `id` is held; 0 and *out untouched otherwise. */
int32_t clockwork_midi_timelines_timeline(const ClockworkMidiTimelines* h, int32_t id, ClockworkTimeline* out);
/* The same, without the lock: reads the copy every mutator publishes behind a
 * seqlock, so the audio thread may call it. An unheld slot reads as the id -1
 * placeholder (1); 0 for an id out of range or a writer caught mid-way after
 * a bounded number of tries — keep the last snapshot then. */
int32_t clockwork_midi_timelines_timeline_rt(const ClockworkMidiTimelines* h, int32_t id, ClockworkTimeline* out);
/* Rows in slot order, under the registry's lock: cb must not call back in. */
void    clockwork_midi_timelines_each(const ClockworkMidiTimelines* h, void* ctx, ClockworkTimelineInfoFn cb);

#ifdef __cplusplus
}
#endif
#endif /* CLOCKWORK_CLOCK_H */
