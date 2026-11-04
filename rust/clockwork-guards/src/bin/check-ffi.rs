// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Every `extern "C"` function in the Rust sources given is the panic fence
//! alone (clockwork_guards::ffi).
//!
//!   check-ffi --canary canary.rs root...
//!
//! The canary holds an `extern "C"` function with no fence: a check that does
//! not report it is not reading what it should, and fails. scripts/check-ffi.sh
//! runs this over every crate's sources.

use std::process::ExitCode;

use clockwork_guards::ffi::{check_ffi, check_source, FfiReport};

fn main() -> ExitCode {
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("check-ffi: {e}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<bool, String> {
    let mut canary = None;
    let mut roots = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--canary" => canary = Some(args.next().ok_or("--canary wants a file")?),
            _ => roots.push(arg),
        }
    }
    let canary = canary.ok_or("no --canary")?;
    if roots.is_empty() {
        return Err("no sources to read".into());
    }

    let text = std::fs::read_to_string(&canary).map_err(|e| format!("{canary}: {e}"))?;
    let mut caught = FfiReport::default();
    check_source(std::path::Path::new(&canary), &text, &mut caught)?;
    if caught.unfenced.is_empty() {
        println!("check-ffi: the canary was not caught: the check is looking at nothing");
        return Ok(false);
    }

    let report = check_ffi(&roots)?;
    for u in &report.unfenced {
        println!("{}:{}: extern \"C\" fn {} is not the fence alone", u.file.display(), u.line, u.name);
    }
    if !report.unfenced.is_empty() {
        println!(
            "check-ffi: {} of {} extern \"C\" functions can let a panic into the host: make each body \
             no_unwind(fallback, || ...) (clockwork-fence), the fallback what the function answers when it fails",
            report.unfenced.len(),
            report.functions
        );
        return Ok(false);
    }
    println!(
        "check-ffi: read {} files; {} extern \"C\" functions, every one the fence alone; canary caught",
        report.files, report.functions
    );
    Ok(true)
}
