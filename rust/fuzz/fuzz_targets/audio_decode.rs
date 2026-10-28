// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Every decoder clockwork carries, through the one entry point a client
//! hands a file's bytes to: the widest door clockwork has to untrusted input.
//! The body is the crate's (clockwork_audio_decode::exercise), replayed over
//! its corpus on every push; this is the same body under libFuzzer.
#![no_main]
use libfuzzer_sys::fuzz_target;
fuzz_target!(|data: &[u8]| clockwork_audio_decode::exercise(data));
