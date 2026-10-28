// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The "/clockwork/schedule" wire form. The body is the crate's
//! (clockwork_schedule::exercise), replayed over its corpus on every push;
//! this is the same body under libFuzzer.
#![no_main]
use libfuzzer_sys::fuzz_target;
fuzz_target!(|data: &[u8]| clockwork_schedule::exercise(data));
