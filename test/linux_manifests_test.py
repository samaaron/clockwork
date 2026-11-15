# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# scripts/linux-manifests.py: what it drops from a Cargo.toml for a Linux
# build, what it keeps, and that the tree's own manifests still parse and
# name no other platform's crate afterwards.
#
#   python3 test/linux_manifests_test.py

import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest

# Loading the script must not leave a __pycache__ beside it.
sys.dont_write_bytecode = True

ROOT = pathlib.Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "linux-manifests.py"


def load():
    spec = importlib.util.spec_from_file_location("linux_manifests", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Cfg(unittest.TestCase):
    def setUp(self):
        self.on_linux = load().on_linux

    def test_other_platforms_are_false(self):
        for cfg in ('windows', 'target_os = "macos"', 'target_os = "android"',
                    'target_os = "ios"', 'target_arch = "wasm32"', 'loom',
                    'all(unix, target_os = "macos")',
                    'any(windows, target_arch = "wasm32")',
                    'not(unix)'):
            self.assertIs(self.on_linux(cfg), False, cfg)

    def test_linux_is_true(self):
        for cfg in ('unix', 'target_os = "linux"', 'not(windows)',
                    'not(target_arch = "wasm32")', 'any(windows, unix)',
                    'all(not(target_arch = "wasm32"), not(windows), not(target_os = "macos"))'):
            self.assertIs(self.on_linux(cfg), True, cfg)

    def test_what_it_cannot_decide_is_kept(self):
        # An architecture or an environment is the build machine's to say.
        for cfg in ('target_arch = "aarch64"', 'target_env = "musl"',
                    'all(unix, target_pointer_width = "64")'):
            self.assertIsNone(self.on_linux(cfg), cfg)


MANIFEST = '''\
[package]
name = "example"
version = "0.1.0"

[features]
default = []
# kept as it is: nothing in it goes
plain = ["log"]
winrt = ["windows/Foundation", "plain"]
mac = ["dep:coremidi"]

[dependencies]
log = { version = "0.4", optional = true }

[target.'cfg(unix)'.dependencies]
libc = "0.2"

[target.'cfg(windows)'.dependencies]
windows = { version = "0.62", optional = true }

[target.'cfg(target_os = "macos")'.dependencies]
coremidi = { version = "0.9", optional = true }

[target.'cfg(target_os = "macos")'.dependencies.core-foundation]
version = "0.10"

[target.'cfg(target_arch = "aarch64")'.dependencies]
bytemuck = "1"

[target.'cfg(loom)'.dev-dependencies]
loom = "0.7"
'''


class Rewrite(unittest.TestCase):
    def setUp(self):
        self.text, self.dropped = load().for_linux(MANIFEST)
        self.toml = tomllib.loads(self.text)

    def test_other_platforms_tables_go(self):
        targets = self.toml["target"]
        self.assertEqual(sorted(targets), ["cfg(target_arch = \"aarch64\")", "cfg(unix)"])
        self.assertEqual(self.dropped, 4)

    def test_features_lose_only_what_named_a_dropped_crate(self):
        features = self.toml["features"]
        self.assertEqual(features["plain"], ["log"])
        self.assertEqual(features["winrt"], ["plain"])   # still a feature a dependant may ask for
        self.assertEqual(features["mac"], [])
        self.assertEqual(features["default"], [])

    def test_a_dropped_member_goes_from_its_dependants_too(self):
        dependant = (
            '[package]\nname = "umbrella"\nversion = "0.1.0"\n\n'
            '[features]\npad = ["dep:pads"]\n\n'
            '[dependencies]\nlog = "0.4"\n'
            'pads = { path = "../pads", optional = true, features = [\n    "a",\n] }\n'
            'padsmore = "1"\n')
        text, _ = load().for_linux(dependant, drop_members=["pads"])
        toml = tomllib.loads(text)
        self.assertEqual(sorted(toml["dependencies"]), ["log", "padsmore"])
        self.assertEqual(toml["features"], {"pad": []})

    def test_an_untouched_manifest_is_returned_byte_for_byte(self):
        plain = '[package]\nname = "p"\nversion = "1.0.0"\n\n[dependencies]\nlog = "0.4"  # why\n'
        self.assertEqual(load().for_linux(plain), (plain, 0))


OTHER_PLATFORM_CRATES = {
    "windows", "windows-sys", "coremidi", "core-foundation", "objc2", "block2",
    "objc2-foundation", "objc2-core-foundation", "objc2-game-controller",
    "jni", "jni-min-helper", "ndk-context", "wasm-bindgen", "js-sys", "web-sys",
    "rusty-xinput",
}


def dependency_names(toml):
    names = set()
    for kind in ("dependencies", "dev-dependencies", "build-dependencies"):
        names |= set(toml.get(kind, {}))
        for table in toml.get("target", {}).values():
            names |= set(table.get(kind, {}))
    return names


class TheTree(unittest.TestCase):
    """The script over a copy of this tree's own manifests."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = pathlib.Path(tempfile.mkdtemp())
        for sub in ("rust", "external/midir", "external/symphonia-bundle-flac"):
            shutil.copytree(ROOT / sub, cls.tmp / sub,
                            ignore=shutil.ignore_patterns("target", "corpus"))
        cls.done = subprocess.run(
            [sys.executable, str(SCRIPT), "--drop-member", "clockwork-guards", str(cls.tmp)],
            capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp)

    def manifests(self):
        return [p for p in self.tmp.rglob("Cargo.toml") if "fuzz" not in p.parts]

    def test_it_says_what_it_did(self):
        self.assertEqual(self.done.returncode, 0, self.done.stderr)
        # One line of account: how many manifests it read and tables it dropped.
        self.assertRegex(self.done.stdout, r"linux-manifests: \d+ manifests read, \d+ rewritten, \d+ tables dropped")
        read = int(self.done.stdout.split(" manifests read")[0].split()[-1])
        self.assertEqual(read, len(self.manifests()))
        self.assertGreater(read, 20)

    def test_no_manifest_names_another_platforms_crate(self):
        # The canary is the tree before the script: it must name some of them.
        before = set()
        for sub in ("rust", "external/midir"):
            for p in (ROOT / sub).rglob("Cargo.toml"):
                if "target" in p.parts or "fuzz" in p.parts:
                    continue
                before |= dependency_names(tomllib.loads(p.read_text()))
        self.assertTrue(before & OTHER_PLATFORM_CRATES)

        for p in self.manifests():
            # midir's browser example is a crate of midir's own workspace,
            # which a build of ours never reads.
            if "examples" in p.parts:
                continue
            toml = tomllib.loads(p.read_text())
            self.assertFalse(dependency_names(toml) & OTHER_PLATFORM_CRATES, p)

    def test_linux_dependencies_stay(self):
        midi = tomllib.loads((self.tmp / "rust/clockwork-midi/Cargo.toml").read_text())
        self.assertIn("alsa", dependency_names(midi))
        gamepad = tomllib.loads((self.tmp / "rust/clockwork-gamepad/Cargo.toml").read_text())
        self.assertIn("gilrs", dependency_names(gamepad))

    def test_the_dropped_member_leaves_the_workspace(self):
        workspace = tomllib.loads((self.tmp / "rust/Cargo.toml").read_text())["workspace"]
        self.assertNotIn("clockwork-guards", workspace["members"])
        self.assertIn("clockwork-native", workspace["members"])

    def test_it_lists_the_crates_a_registry_must_supply(self):
        listed = subprocess.run(
            [sys.executable, str(SCRIPT), "--list-crates", str(self.tmp)],
            capture_output=True, text=True)
        self.assertEqual(listed.returncode, 0, listed.stderr)
        self.assertRegex(listed.stderr, r"linux-manifests: \d+ manifests read")
        crates = listed.stdout.split()
        self.assertEqual(crates, sorted(set(crates)))
        # Ours, by direct dependency; the fork's (midir's libc); and none of
        # the tree's own crates, another platform's, or a dropped tool's.
        for name in ("alsa", "gilrs", "socket2", "symphonia", "libc"):
            self.assertIn(name, crates)
        self.assertNotIn("loom", crates)            # cfg(loom): no build sets it
        self.assertFalse(set(crates) & OTHER_PLATFORM_CRATES)
        self.assertFalse([c for c in crates if c.startswith("clockwork-")])
        self.assertNotIn("midir", crates)
        # midir's JACK backend is a feature nothing here asks for.
        self.assertNotIn("jack-sys", crates)
        self.assertNotIn("object", crates)          # clockwork-guards's alone

    def test_cargo_still_reads_the_workspace(self):
        if shutil.which("cargo") is None:
            self.skipTest("no cargo")
        # Reading the manifests is all this asks: no lock file, no registry.
        out = subprocess.run(
            ["cargo", "read-manifest", "--manifest-path",
             str(self.tmp / "rust/clockwork-midi/Cargo.toml")],
            capture_output=True, text=True)
        self.assertEqual(out.returncode, 0, out.stderr)


if __name__ == "__main__":
    unittest.main()
