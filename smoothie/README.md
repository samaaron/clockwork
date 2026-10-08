<!-- SPDX-License-Identifier: ISC -->
<!-- Copyright (c) 2026 Sam Aaron -->

# Smoothie

The audio-device layer — soundcards, drivers, sample-rate and buffer
negotiation, the audio callback — on macOS, Windows and Linux. A vendored,
ISC-licensed fork of the four permissive modules of JUCE 7, which we maintain
and hack on directly; the name is JUCE, blended.

## The worklet

Smoothie is to Clockwork what a browser's AudioWorklet is to it on the web:
the platform owns the hardware and calls a processor with buffers.
`worklet/smoothie_worklet.h` is that whole interface — a `Processor` handed
plain facts about the device and plain buffers, and `startPlatform()` — and it
contains no JUCE. JUCE is used here and nowhere above: the engine, its hosts
and their tests reach the hardware through the worklet alone, and a JUCE type,
include or initialiser outside this tree is a bug to move into it.
`worklet/smoothie_juce_device_callback.h` is the worklet's JUCE side.

## Provenance

- Source: JUCE tag **7.0.12** — the last JUCE whose core/audio modules are ISC.
  JUCE 8 and later are AGPLv3/commercial, and we chose not to take an AGPL
  dependency; Rule 1 below is what that choice costs us in practice.
- Modules: `juce_core`, `juce_events`, `juce_audio_basics`, `juce_audio_devices`
  — all ISC. `juce_audio_formats` (GPL-dual) was deliberately NOT vendored; the
  recorder uses `src/clockwork_audio_file.h` instead.
- Pruned relative to upstream: Android glue (`native/java*`, oboe) — Smoothie
  targets macOS / Windows / Linux only; and `AudioSourcePlayer` and
  `AudioTransportSource` (`juce_audio_devices/sources`), which Clockwork never
  uses: it drives the device through its own callback.
- The `juce` namespace and per-file copyright headers are retained, as the ISC
  licence requires. Smoothie is the subproject/target name, not a rename.

## Rules

1. **Never sync, backport, or transcribe code from JUCE 8 or later.** Those
   trees are AGPL-side; a retyped fragment still carries AGPL. Fixes here are
   written fresh or derived from this tree itself.
2. New files added under this tree are ISC, copyright Sam Aaron.

## Build

`smoothie/CMakeLists.txt` builds one static library target `smoothie`
that compiles each module's unity source with the module-format defines and
the worklet, and exports `worklet/` and `modules/` as include roots. Code
above Smoothie still includes JUCE in places while it moves behind the
worklet; when none does, `modules/` stops being exported.
