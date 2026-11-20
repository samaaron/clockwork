// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The audio thread's MIDI conversions (clockwork_midi_wire::exercise): a
//! port's bytes in and a client's send out, read with no allocation. Replayed
//! over the midi subsystem's corpus on every push; this is the same body under
//! libFuzzer.
#![no_main]
use libfuzzer_sys::fuzz_target;
fuzz_target!(|data: &[u8]| clockwork_midi_wire::exercise(data));
