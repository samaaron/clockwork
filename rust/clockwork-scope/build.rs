// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Hostless defaults for the two services this crate calls out to —
//! `clockwork_shm_base` and `clockwork_scope_geometry` (src/lib.rs) — for the
//! binaries that have no host to provide them.
//!
//! WHY THIS IS NEEDED, and only on Windows: a `cargo test` binary built from
//! this crate, or from anything that depends on it, references both symbols
//! and defines neither. Unix gets away with it because nothing in a test
//! reaches those calls and `--gc-sections` drops the code before anything has
//! to be resolved. link.exe resolves symbols FIRST and discards with /OPT:REF
//! afterwards, so the same build is LNK2019, twice — and `cargo test
//! --manifest-path rust/Cargo.toml`, which is what CI runs, cannot link on
//! Windows at all.
//!
//! They ship as an ARCHIVE rather than an object, and that is the whole
//! design. A linker pulls a member out of an archive only to resolve a symbol
//! that is still undefined, so these definitions are linked when nothing else
//! defines them and are silently passed over when a real host does — the
//! behaviour `__attribute__((weak))` buys on Unix, available on both
//! toolchains, and without MSVC's objection to a second strong definition
//! (LNK2005). The Unix build keeps the weak attribute as well; it costs
//! nothing and states the intent locally.
//!
//! An archive is also what REACHES far enough. `cargo:rustc-link-arg` applies
//! to its own package's binaries and stops there, which fixes this crate's
//! tests and leaves every dependent's tests broken; `rustc-link-lib` and
//! `rustc-link-search` accumulate through the dependency graph and land on
//! the final link. `:-bundle` then keeps the archive OUT of any staticlib
//! built from this crate, so the C++ hosts — which link `clockwork_native`
//! and define these symbols themselves — never see it.

use std::path::PathBuf;
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    let target = std::env::var("TARGET").unwrap_or_default();
    if target.contains("wasm") || target.contains("emscripten") {
        return; // the worklet build always has its host
    }
    // A Rust host that links the engine (the `host` feature, Cargo.toml):
    // the real definitions are on the line and these must not be, because
    // they would come first.
    if std::env::var_os("CARGO_FEATURE_HOST").is_some() {
        return;
    }
    let msvc = target.contains("msvc");
    let out = PathBuf::from(std::env::var("OUT_DIR").expect("OUT_DIR"));

    // Eight out-parameters, matching the declaration in src/lib.rs. C linkage
    // checks none of it, but a signature that drifts from the one the host
    // implements is a trap for whoever reads this next.
    let decl = if msvc { "" } else { "__attribute__((weak)) " };
    let src = format!(
        "#include <stddef.h>\n\
         {decl}void* clockwork_shm_base(void) {{ return 0; }}\n\
         {decl}int clockwork_scope_geometry(size_t* start, size_t* header_size,\n\
         \x20                              size_t* slot_size, size_t* max_scopes,\n\
         \x20                              unsigned* ring_frames, unsigned* channels,\n\
         \x20                              size_t* track_start, size_t* track_slots) {{\n\
         \x20   (void)start; (void)header_size; (void)slot_size; (void)max_scopes;\n\
         \x20   (void)ring_frames; (void)channels; (void)track_start; (void)track_slots;\n\
         \x20   return 0;\n\
         }}\n"
    );

    let c = out.join("scope_hostless.c");
    std::fs::write(&c, src).expect("write the hostless scope defaults");
    let obj = out.join(if msvc { "scope_hostless.obj" } else { "scope_hostless.o" });
    let cc = std::env::var("CC")
        .unwrap_or_else(|_| if msvc { "cl.exe".into() } else { "cc".into() });
    let mut cc_cmd = Command::new(&cc);
    if msvc {
        // /MD, not cl's default /MT: rustc links the dynamic CRT, and an
        // object built against the static one drags LIBCMT in beside MSVCRT
        // (LNK4098) with two copies of its state behind it.
        cc_cmd
            .args(["/nologo", "/c", "/O2", "/MD"])
            .arg(format!("/Fo{}", obj.display()))
            .arg(&c);
    } else {
        cc_cmd.args(["-c", "-fPIC", "-O2", "-o"]).arg(&obj).arg(&c);
    }
    run(cc_cmd, "could not compile the hostless scope defaults");

    // `-l static=NAME` looks for NAME.lib on MSVC and libNAME.a elsewhere.
    const NAME: &str = "clockwork_scope_hostless";
    let lib = out.join(if msvc { format!("{NAME}.lib") } else { format!("lib{NAME}.a") });
    let _ = std::fs::remove_file(&lib); // ar appends to an existing archive
    let ar = std::env::var("AR")
        .unwrap_or_else(|_| if msvc { "lib.exe".into() } else { "ar".into() });
    let mut ar_cmd = Command::new(&ar);
    if msvc {
        ar_cmd.arg("/nologo").arg(format!("/OUT:{}", lib.display())).arg(&obj);
    } else {
        ar_cmd.arg("crs").arg(&lib).arg(&obj);
    }
    run(ar_cmd, "could not archive the hostless scope defaults");
    assert!(lib.is_file(), "no archive at {}", lib.display());

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static:-bundle={NAME}");
}

fn run(mut cmd: Command, what: &str) {
    let ok = cmd.status().map(|s| s.success()).unwrap_or(false);
    assert!(ok, "{what}");
}
