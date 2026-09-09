// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Build the engine and say how it links.
//!
//! The shape is the ordinary one for a `-sys` crate: configure and build a
//! CMake tree, then `rustc-link-search` and `rustc-link-lib` for what it
//! produced. Those two directives are recorded in this crate's rlib and
//! reach the final link of whatever binary depends on it, which is what
//! makes the engine a dependency rather than a link line every host copies.
//!
//! WHAT IS LINKED, and what deliberately is not. The C++: `clockwork`, the
//! DSP the tree was configured with, `smoothie` when the device layer is on,
//! and `clockwork_comms` under the `comms` feature — plus the system
//! libraries each of those needs on this platform, transcribed from the same
//! `target_link_libraries` calls in CMakeLists.txt that a C++ host inherits.
//! NOT the Rust umbrella staticlib (`clockwork-native`, tau's
//! `tau-next-native`): that archive carries its own copy of std, and a Rust
//! binary already has one. A Rust host takes the subsystem crates as crate
//! dependencies instead, so there is one std and rustc owns the whole link.
//! `CLOCKWORK_RUST=OFF` tells the tree so, and it builds no umbrella.
//!
//! The one thing that crosses back over: the C++ calls into the Rust
//! subsystems — ports, sinks, the scope plane, the DSP entry points — some
//! hundred and fifty `#[no_mangle]` symbols defined in crates of the same
//! graph, the cycle a C++ host closes with `--start-group`. Here rustc owns
//! the line, the archives ride inside this crate's rlib, and the link
//! resolves as it stands: verified with link.exe, and with GNU ld on Ubuntu
//! 24.04 in both the dev and the release profile, LTO off. What a host DOES
//! want LTO for is the guest — its kernels are called across crate
//! boundaries on every sample — and that is a profile setting in the host's
//! own Cargo.toml (rerezzed's says why).
//!
//! WHERE THE TREE IS, three environment variables, each read once:
//!
//! * `CLOCKWORK_SYS_SOURCE` — the CMake source directory to configure. This
//!   tree by default; a project that embeds clockwork as a subdirectory (tau,
//!   clockwork-supersonic) names its own root, and its CMakeLists chooses
//!   the DSP and the options. An embedder sets it in `.cargo/config.toml`
//!   (`[env]`, `relative = true`) so the crate builds from a clean checkout;
//!   a value in the shell wins over that.
//! * `CLOCKWORK_SYS_BUILD_DIR` — where to build. `$OUT_DIR/build` by
//!   default, which is cargo's and is what `cargo clean` removes. Name an
//!   existing tree to share it with a C++ host or the tests; it is
//!   reconfigured with the options below on every run, which a configured
//!   tree takes in a second.
//! * `CLOCKWORK_SYS_CONFIG` — the CMake configuration, `Release` unless
//!   said otherwise. Release for a debug binary is deliberate: rustc links
//!   the release CRT on MSVC whatever the profile, and an engine compiled
//!   -O0 is not an engine anyone wants to hear.
//!
//! `CMAKE_GENERATOR` is honoured by cmake itself. `CLOCKWORK_SYS_CMAKE_ARGS`
//! passes extra `-D`s through, space-separated.

use std::collections::BTreeMap;
use std::env;
use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    for v in [
        "CLOCKWORK_SYS_SOURCE",
        "CLOCKWORK_SYS_BUILD_DIR",
        "CLOCKWORK_SYS_CONFIG",
        "CLOCKWORK_SYS_CMAKE_ARGS",
        "CMAKE",
        "CMAKE_GENERATOR",
    ] {
        println!("cargo:rerun-if-env-changed={v}");
    }
    // On the web the engine is not linked: it runs as its own module in the
    // AudioWorklet (scripts/build-web.sh), and a host reaches it over the
    // bridge. Nothing below applies.
    if env::var("CARGO_CFG_TARGET_ARCH").as_deref() == Ok("wasm32") {
        return;
    }
    let os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    let msvc = env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc");
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let out = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));

    let source = env::var_os("CLOCKWORK_SYS_SOURCE")
        .map(PathBuf::from)
        .unwrap_or_else(|| manifest.join("../.."));
    let build = env::var_os("CLOCKWORK_SYS_BUILD_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| out.join("build"));
    let config = env::var("CLOCKWORK_SYS_CONFIG").unwrap_or_else(|_| "Release".into());
    assert!(
        source.join("CMakeLists.txt").is_file(),
        "no CMakeLists.txt at {} — CLOCKWORK_SYS_SOURCE names the tree to configure",
        source.display()
    );

    configure(&source, &build, &config, msvc);
    let cache = Cache::read(&build.join("CMakeCache.txt"));

    // Where clockwork's own targets land, whether it is the tree's root or a
    // subdirectory of the embedding project's: CMake caches a project's
    // binary directory under its project() name — and its source directory,
    // which is what the engine is rebuilt from.
    let bin = cache.path("Clockwork_BINARY_DIR");
    let clockwork_src = cache.path("Clockwork_SOURCE_DIR");

    // A change to the C++ must reach a host's next `cargo build`, or a fix
    // in the engine is tested against the archive the last build left —
    // which is how a header's new promise once passed a Rust test that
    // linked the old library. Cargo re-runs this script when anything under
    // these paths changes: the tree's root CMakeLists.txt (an embedding
    // project's, or this one's), and clockwork's own sources, device layer,
    // in-tree DSPs and CMake modules. Not the whole source tree: that would
    // include a project's node_modules, its C++ build directories and
    // cargo's own target directory, and the last of those changes on every
    // build. A DSP outside the tree names itself through CLOCKWORK_DSP_DIR.
    let mut watched = vec![source.join("CMakeLists.txt")];
    for sub in ["CMakeLists.txt", "src", "dsp", "smoothie", "cmake"] {
        watched.push(clockwork_src.join(sub));
    }
    let dsp_dir = cache.get("CLOCKWORK_DSP_DIR").to_string();
    if !dsp_dir.is_empty() {
        watched.push(PathBuf::from(dsp_dir));
    }
    for w in watched {
        if w.exists() {
            println!("cargo:rerun-if-changed={}", w.display());
        }
    }
    let dsp = cache.get("CLOCKWORK_DSP").to_string();
    let device = cache.flag("CLOCKWORK_DEVICE");
    let midi = cache.flag("CLOCKWORK_MIDI");
    let gamepad = cache.flag("CLOCKWORK_GAMEPAD");
    let osc = cache.flag("CLOCKWORK_OSC");
    let scheduler = cache.flag("CLOCKWORK_SCHEDULER");
    let link = cache.flag("CLOCKWORK_LINK");
    let system_zlib = cache.flag("CLOCKWORK_SYSTEM_ZLIB");
    let comms = env::var_os("CARGO_FEATURE_COMMS").is_some();

    let mut targets = vec!["clockwork".to_string(), dsp.clone()];
    if comms {
        targets.push("clockwork_comms".into());
    }
    build_targets(&build, &config, &targets);

    // ── The archives ─────────────────────────────────────────────────────
    // Each named by directory and stem so rustc finds `libX.a` or `X.lib`
    // itself. A multi-configuration generator puts them one level down,
    // under the configuration's name.
    let mut libs = vec![
        (bin.clone(), "clockwork"),
        (bin.join(format!("dsp-{dsp}")), dsp.as_str()),
    ];
    if device {
        libs.push((bin.join("smoothie"), "smoothie"));
    }
    if comms {
        libs.push((bin.clone(), "clockwork_comms"));
    }
    let mut searched = Vec::new();
    for (dir, stem) in &libs {
        let dir = archive_dir(dir, stem, &config, msvc);
        if !searched.contains(&dir) {
            println!("cargo:rustc-link-search=native={}", dir.display());
            searched.push(dir);
        }
        println!("cargo:rustc-link-lib=static={stem}");
    }

    // ── The system libraries ─────────────────────────────────────────────
    // What CMakeLists.txt links to `clockwork` and `smoothie` on each
    // platform, in the same order. A Rust binary carries std's own needs
    // already (ntdll, userenv, dbghelp on Windows; pthread, dl, m elsewhere);
    // they are listed where CMake lists them and cost nothing twice.
    let mut sys = Libs::default();
    match os.as_str() {
        "windows" => {
            // CMakeLists.txt: the core (Ws2_32 winmm), Link's scheduler
            // (avrt), midir's WinMM and WinRT backends (ole32 oleaut32
            // runtimeobject), std (ntdll userenv dbghelp). smoothie's JUCE
            // modules ask for most of theirs through #pragma comment(lib);
            // juce_core's shell calls (ShellExecuteW, SHGetSpecialFolderPathW,
            // CommandLineToArgvW) are not among them, so shell32 is named
            // here — a binary with no windowing crate beside it has nothing
            // else pulling it in.
            sys.dylibs(["Ws2_32", "winmm"]);
            if link {
                sys.dylibs(["avrt"]);
            }
            if midi {
                sys.dylibs(["ole32", "oleaut32", "runtimeobject"]);
            }
            if device {
                sys.dylibs(["shell32"]);
            }
            sys.dylibs(["ntdll", "userenv", "dbghelp"]);
        }
        "macos" | "ios" => {
            // CMakeLists.txt: the device path's frameworks on clockwork,
            // CoreMIDI for MIDI, GameController + objc for the gamepad;
            // smoothie/CMakeLists.txt: the vendored modules' framework
            // lists. Then the C++ runtime, which a Rust link does not add.
            sys.frameworks(["CoreAudio", "AudioUnit", "Foundation", "CoreFoundation", "AVFoundation"]);
            if midi {
                sys.frameworks(["CoreMIDI"]);
            }
            if gamepad {
                sys.frameworks(["GameController"]);
                sys.dylibs(["objc"]);
            }
            if device {
                sys.frameworks([
                    "Cocoa",
                    "Foundation",
                    "IOKit",
                    "Security",
                    "Accelerate",
                    "CoreAudio",
                    "CoreMIDI",
                    "AudioToolbox",
                ]);
            }
            if system_zlib {
                sys.dylibs(["z"]);
            }
            sys.dylibs(["c++"]);
        }
        _ => {
            // CMakeLists.txt (UNIX): Threads rt dl on clockwork, asound for
            // MIDI, udev for the gamepad's evdev hotplug;
            // smoothie/CMakeLists.txt: Threads ALSA dl rt. JACK and PipeWire
            // are dlopen'd, no link dependency. Then the C++ runtime.
            if device {
                sys.dylibs(["asound"]);
            }
            sys.dylibs(["pthread", "rt", "dl"]);
            if midi {
                sys.dylibs(["asound"]);
            }
            if gamepad {
                sys.dylibs(["udev"]);
            }
            if system_zlib {
                sys.dylibs(["z"]);
            }
            let cxx = if matches!(os.as_str(), "freebsd" | "openbsd" | "netbsd" | "dragonfly") {
                "c++"
            } else {
                "stdc++"
            };
            sys.dylibs([cxx]);
        }
    }
    sys.emit();

    // ── For a dependent's build script ───────────────────────────────────
    // DEP_CLOCKWORK_INCLUDE: the directories clockwork's headers are found
    // under (CLOCKWORK_INCLUDE_DIRS, which CMake caches as part of its
    // contract with an embedder), joined the way std::env::split_paths
    // reads. DEP_CLOCKWORK_DSP: which DSP was linked. DEP_CLOCKWORK_FEATURES:
    // the cargo features CMake would build the umbrella with for this tree's
    // options — the ones to enable on the subsystem crates so the Rust half
    // matches the C++ half.
    let include = cache.get("CLOCKWORK_INCLUDE_DIRS").split(';').map(PathBuf::from);
    let include = env::join_paths(include).expect("join CLOCKWORK_INCLUDE_DIRS");
    println!("cargo:include={}", include.to_string_lossy());
    println!("cargo:dsp={dsp}");
    let mut features = Vec::new();
    if midi {
        features.push("midi");
        if os == "windows" {
            features.push("winrt");
        }
    }
    if gamepad {
        features.push("gamepad");
    }
    if osc {
        features.extend(["osc", "comms"]);
    }
    if scheduler {
        features.push("schedule");
    }
    println!("cargo:features={}", features.join(","));
    println!("cargo:build_dir={}", build.display());
}

/// Configure the tree — every time, so an existing build directory takes
/// these options too. cmake reads CMAKE_GENERATOR from the environment for a
/// fresh directory and refuses to change the generator of a configured one,
/// which is the behaviour wanted here.
fn configure(source: &Path, build: &Path, config: &str, msvc: bool) {
    let fresh = !build.join("CMakeCache.txt").is_file();
    let mut cmd = Command::new(cmake());
    cmd.arg("-S").arg(source).arg("-B").arg(build);
    if fresh && msvc && env::var_os("CMAKE_GENERATOR").is_none() {
        // The Visual Studio generator, cmake's default here, builds for the
        // host unless told the platform; name the one rustc is building for.
        let arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
        let platform = match arch.as_str() {
            "x86_64" => "x64",
            "x86" => "Win32",
            "aarch64" => "ARM64",
            other => panic!("no Visual Studio platform for target arch {other}"),
        };
        cmd.args(["-A", platform]);
    }
    cmd.arg(format!("-DCMAKE_BUILD_TYPE={config}"));
    // The engine as a library and nothing around it. CLOCKWORK_RUST=OFF is
    // the point (the Rust half is this binary's own crates); the rest are
    // the targets that would need the umbrella, which the tree refuses to
    // configure without it.
    cmd.args([
        "-DCLOCKWORK_RUST=OFF",
        "-DCLOCKWORK_STANDALONE=OFF",
        "-DBUILD_TESTS=OFF",
        "-DCLOCKWORK_PLUGINS=OFF",
        "-DCLOCKWORK_NIF=OFF",
    ]);
    if let Ok(extra) = env::var("CLOCKWORK_SYS_CMAKE_ARGS") {
        cmd.args(extra.split_whitespace());
    }
    run(cmd, "configuring the engine failed");
}

fn build_targets(build: &Path, config: &str, targets: &[String]) {
    let mut cmd = Command::new(cmake());
    cmd.arg("--build").arg(build).args(["--config", config]).arg("--target").args(targets);
    if let Ok(jobs) = env::var("NUM_JOBS") {
        cmd.args(["--parallel", &jobs]);
    }
    run(cmd, "building the engine failed");
}

fn cmake() -> String {
    env::var("CMAKE").unwrap_or_else(|_| "cmake".into())
}

fn run(mut cmd: Command, what: &str) {
    let status = cmd
        .status()
        .unwrap_or_else(|e| panic!("could not run {:?}: {e}", cmd.get_program()));
    assert!(status.success(), "{what}: {cmd:?}");
}

/// The directory holding `stem`'s archive: `dir` for a single-configuration
/// generator, `dir/<config>` for a multi-configuration one.
fn archive_dir(dir: &Path, stem: &str, config: &str, msvc: bool) -> PathBuf {
    let file = if msvc { format!("{stem}.lib") } else { format!("lib{stem}.a") };
    for candidate in [dir.to_path_buf(), dir.join(config)] {
        if candidate.join(&file).is_file() {
            return candidate;
        }
    }
    panic!("the engine build produced no {file} under {}", dir.display());
}

/// Link libraries in the order given, each once.
#[derive(Default)]
struct Libs(Vec<(&'static str, &'static str)>);

impl Libs {
    fn dylibs<const N: usize>(&mut self, names: [&'static str; N]) {
        self.add("dylib", names);
    }
    fn frameworks<const N: usize>(&mut self, names: [&'static str; N]) {
        self.add("framework", names);
    }
    fn add<const N: usize>(&mut self, kind: &'static str, names: [&'static str; N]) {
        for n in names {
            if !self.0.contains(&(kind, n)) {
                self.0.push((kind, n));
            }
        }
    }
    fn emit(&self) {
        for (kind, name) in &self.0 {
            println!("cargo:rustc-link-lib={kind}={name}");
        }
    }
}

/// CMakeCache.txt: `KEY:TYPE=VALUE` per line, comments and blanks between.
struct Cache(BTreeMap<String, String>);

impl Cache {
    fn read(path: &Path) -> Self {
        let text = std::fs::read_to_string(path)
            .unwrap_or_else(|e| panic!("could not read {}: {e}", path.display()));
        let mut map = BTreeMap::new();
        for line in text.lines() {
            if line.starts_with('#') || line.starts_with("//") {
                continue;
            }
            if let Some((key, value)) = line.split_once('=') {
                let key = key.split_once(':').map_or(key, |(k, _)| k);
                map.insert(key.to_string(), value.to_string());
            }
        }
        Self(map)
    }
    fn get(&self, key: &str) -> &str {
        self.0
            .get(key)
            .unwrap_or_else(|| panic!("CMakeCache.txt has no {key}: is this a clockwork tree?"))
    }
    fn path(&self, key: &str) -> PathBuf {
        PathBuf::from(self.get(key))
    }
    fn flag(&self, key: &str) -> bool {
        matches!(
            self.get(key).to_ascii_uppercase().as_str(),
            "ON" | "TRUE" | "YES" | "Y" | "1"
        )
    }
}
