// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! The engine, built and linked. There is nothing to call here: the C ABI is
//! declared in `clockwork-abi`, and the symbols it declares are resolved by
//! what this crate's build script links — see `build.rs` for what that is and
//! how to point it at a tree.
//!
//! A Rust host names this crate (`extern crate clockwork_sys;`) so it is
//! linked at all — a crate no path mentions is not — and takes the Rust
//! subsystems the engine calls back into as ordinary crate dependencies:
//! clockwork's own from this workspace, or a project's umbrella such as
//! tau's `tau-next`, which carries the guest as well.
