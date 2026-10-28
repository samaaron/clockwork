// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! A crate's fuzz corpus, replayed through its fuzz body as a test.
//!
//! Every crate that reads bytes from outside the process keeps a `corpus/`
//! beside its code: `seeds/`, what its fuzz target starts from, and
//! `regressions/`, every crasher the nightly fuzz run ever found and that
//! was then fixed. The crate's corpus test calls [`replay`] with its fuzz
//! body, so every push proves the whole corpus is still harmless; the nightly
//! run (`.github/workflows/security.yml`) starts libFuzzer from the same
//! files. A dev-dependency only: nothing here is in the staticlib.

use std::path::Path;

/// Every file under `<manifest_dir>/corpus/seeds` and `/corpus/regressions`
/// through `body`, dotfiles skipped; how many there were, so a test can
/// refuse an empty corpus rather than pass over nothing.
pub fn replay(manifest_dir: &str, body: fn(&[u8])) -> usize {
    let root = Path::new(manifest_dir).join("corpus");
    let mut n = 0;
    for dir in ["seeds", "regressions"] {
        let Ok(entries) = std::fs::read_dir(root.join(dir)) else { continue };
        for entry in entries {
            let path = entry.expect("a corpus entry").path();
            if path.file_name().is_some_and(|f| f.to_string_lossy().starts_with('.')) {
                continue;
            }
            body(&std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display())));
            n += 1;
        }
    }
    n
}
