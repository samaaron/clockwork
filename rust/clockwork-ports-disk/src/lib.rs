// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2025-2026 Sam Aaron
//! Disk endpoints for the port substrate.
//!
//! Two of them, in opposite directions: a worker that streams a WAV file into
//! a source port, and one that drains a sink port to a WAV file. Between them
//! they are the eighth plumbing job in `docs/PORTS.md` — "streaming a sample
//! off disk", which was wanted and never built — and the recording path, which
//! was built three separate times.
//!
//! # Why a separate crate
//!
//! `clockwork-ports` must mention no endpoint at all, and the cheapest way to
//! guarantee that is a dependency edge that only runs one way. This crate
//! depends on the substrate; the substrate cannot depend on this one without
//! a cycle cargo would refuse. The rule is therefore checked by the build
//! rather than by review, which is the only kind of rule that survives.
//!
//! The same shape is what a Link endpoint would take: a crate that owns a
//! `LinkAudioBridge` handle, produces peer audio into a source port and
//! consumes aux output from a sink port, and touches nothing in the substrate.

pub mod ffi;
pub mod sink;
pub mod source;
pub mod wav;
