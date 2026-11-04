// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Every mutable variable with static storage in the objects given must be
//! on the list, and everything the list names for this build must be found.
//!
//!   check-globals --list scripts/globals.list --canary canary.o [--with opt]... lib.a...
//!   check-globals --guest --canary canary.o lib.a...
//!   check-globals --print lib.a...          every variable found, and its class
//!
//! A guest that says any number of it can run in a process
//! (DSP_INSTANCES_ANY) keeps nothing process-wide (docs/BOUNDARY.md): with
//! --guest there is no list, and every variable found fails.
//!
//! The canary is an object holding one variable, `clockwork_globals_canary`,
//! that the list does not name: a scan that does not report it is not
//! reading what it should, and the check fails. scripts/check-globals.sh
//! builds Clockwork's inputs and runs this; scripts/check-guest-globals.sh
//! runs it over a guest's.

use std::process::ExitCode;

use clockwork_guards::{check, scan, List};

const CANARY: &str = "clockwork_globals_canary";

fn main() -> ExitCode {
    match run() {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("check-globals: {e}");
            ExitCode::from(2)
        }
    }
}

fn run() -> Result<bool, String> {
    let mut list_path = None;
    let mut canary = None;
    let mut with = Vec::new();
    let mut print = false;
    let mut guest = false;
    let mut inputs = Vec::new();
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--list" => list_path = Some(args.next().ok_or("--list wants a file")?),
            "--canary" => canary = Some(args.next().ok_or("--canary wants an object")?),
            "--with" => with.push(args.next().ok_or("--with wants an option name")?),
            "--print" => print = true,
            "--guest" => guest = true,
            _ => inputs.push(arg),
        }
    }
    if inputs.is_empty() {
        return Err("no objects or archives given".into());
    }
    let found = scan(&inputs)?;
    if !found.merged.is_empty() {
        let objects: Vec<_> = found.merged.iter().cloned().collect();
        return Err(format!(
            "built with global merging, which hides variables' names: {}. Build unoptimised (scripts/check-globals.sh).",
            objects.join(", ")
        ));
    }
    let list = match &list_path {
        Some(path) => List::parse(&std::fs::read_to_string(path).map_err(|e| format!("{path}: {e}"))?)
            .map_err(|e| format!("{path}: {e}"))?,
        None => List::default(),
    };

    if print {
        for (key, storage) in &found.vars {
            let class = list.entries.get(key).map_or("?".to_string(), |e| e.class.to_string());
            println!("{class} | {key} | {storage}");
        }
        return Ok(true);
    }
    let list_path = match (guest, list_path) {
        (true, None) => None,
        (true, Some(_)) => return Err("--guest takes no list: a guest that says any keeps no variable".into()),
        (false, Some(path)) => Some(path),
        (false, None) => return Err("--list is required".into()),
    };
    let canary = canary.ok_or("--canary is required")?;

    // The canary first: the same reading, over an object whose one variable
    // the list must not name.
    let tested = scan(&[&canary])?;
    let seen = check(&tested, &list, &with);
    let caught = seen.unlisted.len() == 1 && seen.unlisted[0].0.ends_with(CANARY);
    if !caught {
        println!("check-globals: the canary in {canary} was not caught (found {:?}): the scan is not reading what it should", seen.unlisted);
        return Ok(false);
    }

    let report = check(&found, &list, &with);
    let counts: Vec<String> = report.counts.iter().map(|(class, n)| format!("{n} {class}")).collect();
    let classes = if counts.is_empty() { String::new() } else { format!(": {}", counts.join(", ")) };
    println!(
        "check-globals{}: read {} objects in {} archives ({}); {} variables{classes}; canary caught",
        if guest { " (guest)" } else { "" },
        found.objects,
        found.archives,
        found.platform.unwrap_or("?"),
        found.vars.len(),
    );
    for (key, storage) in &report.unlisted {
        match &list_path {
            Some(path) => println!("  not on {path}: {key} ({storage})"),
            None => println!(
                "  {key} ({storage}): a guest that says DSP_INSTANCES_ANY keeps nothing process-wide (docs/BOUNDARY.md)"
            ),
        }
    }
    if let Some(path) = &list_path {
        for (key, line) in &report.missing {
            println!("  {path}:{line} names {key}, which this build does not have");
        }
    }
    Ok(report.clean())
}
