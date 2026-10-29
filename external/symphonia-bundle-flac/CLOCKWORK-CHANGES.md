# symphonia-bundle-flac 0.6.1, as clockwork carries it

The crates.io package of `symphonia-bundle-flac` 0.6.1 (upstream
<https://github.com/pdeljanov/Symphonia>, tag `v0.6.1`, commit
`ee35874b571a35a9a6e15d3bc9a3aaf8f11fbeee`), byte for byte, with one change.
It replaces the crates.io crate through `[patch.crates-io]` in
`rust/Cargo.toml` and `rust/fuzz/Cargo.toml`. Delete this directory and the
two patch tables once upstream releases the fix.

## The change

`src/decoder.rs`, `lpc_predict`: the predicted value is added to the
residual with `wrapping_add` rather than `+=`, in both loops.

`fixed_predict`, a few lines above, already adds with wrapping arithmetic.
The linear predictor did not, so a frame whose coefficients take a sample
past 32 bits overflowed a plain add: a panic where overflow checks are on (a
debug build, the fuzzer), a silent wrap where they are off. Wrapping in both
predictors makes the arithmetic defined and the same everywhere. A file that
reaches it decodes to noise, which is what a malformed stream is.

Found by clockwork's `audio_decode` fuzz target on 2026-10-28 (the fifth
hand-started run). The input is
`rust/clockwork-audio-decode/corpus/regressions/crash-7d0d3efe3ad9587732444d61b6492583fb4156e5`.
It reaches the predictor only in a build with `--cfg fuzzing`, under which
symphonia skips the frame header checksum; the checksum is an integrity check,
not a guard, so a file with a correct one reaches it the same way.

## Who gets the change

A build of the `rust/` workspace (the CMake build and every CI job) and of
the fuzz workspace. Cargo applies `[patch]` only from the top-level workspace,
so a Rust program that takes clockwork's crates as dependencies of its own
(`CLOCKWORK_RUST=OFF`, `rust/clockwork-sys`) needs the same patch table in its
own `Cargo.toml`.
