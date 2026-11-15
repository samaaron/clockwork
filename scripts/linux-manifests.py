#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
# Copyright (c) 2026 Sam Aaron
#
# Rewrite a copy of this tree's Cargo manifests for a Linux-only build.
#
# cargo resolves every dependency a workspace names, for every platform,
# before it builds for one. A build that has only a distribution's own crate
# packages (Debian's librust-*-dev) therefore fails on crates it would never
# compile: the Windows, macOS, Android and browser ones. A distribution
# removes those tables with patches of its own; this writes the same change
# from the manifests as they are, so it cannot fall behind them.
#
#   scripts/linux-manifests.py [--drop-member NAME]... [--list-crates] <tree>
#
# <tree> is a COPY of the repository (a source package being assembled),
# rewritten in place: every Cargo.toml under rust/ (the fuzz workspace apart)
# and under external/. From each it removes
#
#   - a [target.'cfg(...)'.*dependencies] table whose cfg cannot hold on
#     Linux. One it cannot decide (an architecture, a C library) stays;
#   - from [features], an entry naming a crate that went with such a table.
#     The feature itself stays, so a dependant asking for it still resolves;
#   - with --drop-member, that crate of ours from the workspace and from
#     the dependencies of the others, so that what it depends on need not
#     resolve: a tool the build does not need, or a subsystem a build leaves
#     out (clockwork-gamepad with -DCLOCKWORK_GAMEPAD=OFF).
#
# A manifest with nothing to remove is left byte for byte. Prints one line:
# how many manifests it read, how many it rewrote and how many tables went.
#
# --list-crates also prints, one a line, the name of every crate the build
# then takes from a registry: what the workspace's members, and the in-tree
# crates they reach by path, depend on directly. That is the list a package's
# build dependencies are written from (Debian: librust-<name>-dev). An
# optional dependency of a fork under external/ is listed only where a
# feature in use turns it on. The account line goes to stderr in that case.

import argparse
import pathlib
import re
import sys
import tomllib

KINDS = ("dependencies", "dev-dependencies", "build-dependencies")
HEADER = re.compile(r"^\s*\[\[?\s*(.+?)\s*\]\]?\s*(#.*)?$")
TARGET = re.compile(r"""^target\.(['"])cfg\((.*)\)\1\.([a-z-]+)(\..+)?$""")
TOKEN = re.compile(r'\s*(?:([A-Za-z_][A-Za-z0-9_]*)|"((?:[^"\\]|\\.)*)"|([(),=]))')


def on_linux(cfg):
    """True or False where a cfg expression is decided by 'this is Linux',
    None where it is not (the architecture, the C library, a feature)."""
    tokens = [m.group(1) or m.group(3) or ("\0" + m.group(2))
              for m in TOKEN.finditer(cfg)]
    pos = 0

    def peek():
        return tokens[pos] if pos < len(tokens) else None

    def take():
        nonlocal pos
        pos += 1
        return tokens[pos - 1]

    def args():
        values = []
        take()                                   # (
        while peek() != ")":
            values.append(expr())
            if peek() == ",":
                take()
        take()                                   # )
        return values

    def expr():
        name = take()
        if name == "not":
            (value,) = args()
            return None if value is None else not value
        if name == "all":
            values = args()
            if False in values:
                return False
            return None if None in values else True
        if name == "any":
            values = args()
            if True in values:
                return True
            return None if None in values else False
        if peek() == "=":
            take()
            return key_value(name, take()[1:])
        # A bare name: the two families cargo defines, else a custom cfg,
        # which is unset unless a build sets it in RUSTFLAGS.
        return {"unix": True, "windows": False}.get(name, False)

    def key_value(key, value):
        if key == "target_os":
            return value == "linux"
        if key == "target_family":
            return {"unix": True, "windows": False, "wasm": False}.get(value)
        if key == "target_vendor":
            return False if value == "apple" else None
        if key == "target_arch":
            return False if value in ("wasm32", "wasm64") else None
        return None

    return expr()


def sections(text):
    """(header name or None, lines) for the preamble and each table."""
    out = [(None, [])]
    for line in text.splitlines(keepends=True):
        m = HEADER.match(line)
        if m and not line.lstrip().startswith("#"):
            out.append((m.group(1), [line]))
        else:
            out[-1][1].append(line)
    return out


def dependency_names(toml):
    names = set()
    for kind in KINDS:
        names |= set(toml.get(kind, {}))
        for table in toml.get("target", {}).values():
            names |= set(table.get(kind, {}))
    return names


def feature_target(entry):
    """The crate or feature a [features] entry names."""
    if entry.startswith("dep:"):
        return entry[4:]
    return entry.split("/")[0].rstrip("?")


def for_linux(text, drop_members=()):
    """The manifest for a Linux-only build, and how many tables were dropped."""
    before = dependency_names(tomllib.loads(text))
    kept, dropped = [], 0
    for name, lines in sections(text):
        m = TARGET.match(name) if name else None
        if m and m.group(3) in KINDS and on_linux(m.group(2)) is False:
            dropped += 1
            continue
        kept.append((name, lines))

    members_changed = False
    if drop_members:
        for name, lines in kept:
            if name == "workspace":
                for i, line in enumerate(lines):
                    if any(re.fullmatch(r'\s*"%s",?\s*(#.*)?\n?' % re.escape(d), line)
                           for d in drop_members):
                        lines[i] = ""
                        members_changed = True

    # A dropped member's entry in another crate's dependency tables: the
    # line that names it, and the lines after it while its brackets are open.
    for name, lines in kept:
        m = TARGET.match(name) if name else None
        if not drop_members or not (name in KINDS or (m and m.group(3) in KINDS)):
            continue
        i = 1
        while i < len(lines):
            if any(re.match(r'\s*%s\s*=' % re.escape(d), lines[i]) for d in drop_members):
                depth = 0
                while True:
                    depth += sum(lines[i].count(c) for c in "[{") - sum(lines[i].count(c) for c in "]}")
                    lines[i] = ""
                    i += 1
                    if depth <= 0 or i >= len(lines):
                        break
                members_changed = True
            else:
                i += 1

    if not dropped and not members_changed:
        return text, 0

    result = "".join("".join(lines) for _, lines in kept)
    toml = tomllib.loads(result)
    gone = before - dependency_names(toml)
    features = toml.get("features", {})
    if gone and any(feature_target(e) in gone and feature_target(e) not in features
                    for entries in features.values() for e in entries):
        rewritten = ["[features]\n"]
        for feature, entries in features.items():
            entries = [e for e in entries
                       if feature_target(e) not in gone or feature_target(e) in features]
            rewritten.append("%s = [%s]\n" % (feature, ", ".join('"%s"' % e for e in entries)))
        rewritten.append("\n")
        kept = [(n, rewritten if n == "features" else lines) for n, lines in kept]
        result = "".join("".join(lines) for _, lines in kept)
        tomllib.loads(result)
    return result, dropped


def registry_crates(tree):
    """Names of the crates the workspace takes from a registry: the direct
    dependencies of its members and of every crate they reach by path.

    An optional dependency counts only where a feature turns it on. Any
    feature of a workspace member may be on (the build chooses them); a crate
    outside the workspace (a fork under external/) has its default features
    and the ones its dependants ask for."""
    root = tree / "rust" / "Cargo.toml"
    workspace = tomllib.loads(root.read_text())
    members = {(root.parent / m / "Cargo.toml").resolve() for m in workspace["workspace"]["members"]}
    patched = [(root.parent / spec["path"] / "Cargo.toml").resolve()
               for table in workspace.get("patch", {}).values()
               for spec in table.values() if isinstance(spec, dict) and "path" in spec]

    tomls = {}

    def read(manifest):
        if manifest not in tomls:
            tomls[manifest] = tomllib.loads(manifest.read_text())
        return tomls[manifest]

    def entries(toml):
        for table in [toml] + list(toml.get("target", {}).values()):
            for kind in KINDS:
                for key, spec in table.get(kind, {}).items():
                    yield key, spec if isinstance(spec, dict) else {}

    # Features on, per manifest. Grown until nothing changes: a feature turns
    # on others of its crate, and "dep/feature" one of a dependency's.
    on = {m: set(read(m).get("features", {})) for m in members}
    for m in patched:
        on.setdefault(m, {"default"})
    changed = True
    while changed:
        changed = False
        for manifest in list(on):
            toml = read(manifest)
            features = toml.get("features", {})
            paths = {key: (manifest.parent / spec["path"] / "Cargo.toml").resolve()
                     for key, spec in entries(toml) if "path" in spec}
            optional = {key for key, spec in entries(toml) if spec.get("optional")}
            wanted = {}                          # dependency manifest -> features asked of it
            active = set()                       # optional dependencies turned on
            for feature in list(on[manifest]):
                for entry in features.get(feature, []):
                    target = feature_target(entry)
                    if target in features and not entry.startswith("dep:") and "/" not in entry:
                        if target not in on[manifest]:
                            on[manifest].add(target)
                            changed = True
                        continue
                    if "?/" not in entry:
                        active.add(target)
                    if "/" in entry and target in paths:
                        wanted.setdefault(paths[target], set()).add(entry.split("/", 1)[1])
            for key, spec in entries(toml):
                if key not in paths or (key in optional and key not in active and manifest not in members):
                    continue
                asked = set(spec.get("features", []))
                if spec.get("default-features", True):
                    asked.add("default")
                wanted.setdefault(paths[key], set()).update(asked)
            for dep, asked in wanted.items():
                if not asked <= on.setdefault(dep, set()):
                    on[dep] |= asked
                    changed = True
                elif dep not in tomls:
                    changed = True
                read(dep)

    names = set()
    for manifest, features_on in on.items():
        toml = read(manifest)
        features = toml.get("features", {})
        active = {feature_target(e) for f in features_on for e in features.get(f, [])
                  if "?/" not in e}
        for key, spec in entries(toml):
            if "path" in spec:
                continue
            if spec.get("optional") and manifest not in members and key not in active:
                continue
            names.add(spec.get("package", key))
    return sorted(names)


def main():
    parser = argparse.ArgumentParser(description="Rewrite a tree's Cargo manifests for a Linux-only build.")
    parser.add_argument("--drop-member", action="append", default=[], metavar="NAME")
    parser.add_argument("--list-crates", action="store_true")
    parser.add_argument("tree", type=pathlib.Path)
    args = parser.parse_args()

    manifests = sorted(
        p for sub in ("rust", "external") for p in (args.tree / sub).rglob("Cargo.toml")
        if "target" not in p.parts and "fuzz" not in p.parts)
    if not manifests:
        sys.exit("linux-manifests: no Cargo.toml under %s/rust or %s/external" % (args.tree, args.tree))

    rewritten = tables = 0
    for path in manifests:
        text = path.read_text()
        result, dropped = for_linux(text, args.drop_member)
        if result != text:
            path.write_text(result)
            rewritten += 1
            tables += dropped
    for member in args.drop_member:
        workspace = tomllib.loads((args.tree / "rust" / "Cargo.toml").read_text())["workspace"]
        if member in workspace["members"]:
            sys.exit("linux-manifests: could not drop workspace member %s" % member)
    print("linux-manifests: %d manifests read, %d rewritten, %d tables dropped"
          % (len(manifests), rewritten, tables),
          file=sys.stderr if args.list_crates else sys.stdout)
    if args.list_crates:
        print("\n".join(registry_crates(args.tree)))


if __name__ == "__main__":
    main()
