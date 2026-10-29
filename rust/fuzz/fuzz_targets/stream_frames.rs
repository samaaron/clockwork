// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The length-prefixed framing every TCP and Unix stream client sends, cut
//! into reads anywhere. The body is the crate's
//! (clockwork_comms::stream::exercise), replayed over its corpus on every
//! push; this is the same body under libFuzzer.
#![no_main]
use libfuzzer_sys::fuzz_target;
fuzz_target!(|data: &[u8]| clockwork_comms::stream::exercise(data));
