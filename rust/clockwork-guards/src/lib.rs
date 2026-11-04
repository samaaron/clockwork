// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! Guards over what Clockwork builds. They read the compiled objects rather
//! than the sources, so they see exactly what the compiler kept.
//!
//! [`scan`] finds every mutable variable with static storage in a set of
//! objects and archives: globals, file statics, function-local statics and
//! thread-locals. [`check`] holds what it found to a [`List`] that says what
//! each one is. docs/ENGINES.md.3. [`ffi`] holds every `extern "C"`
//! function to the panic fence, from the sources.

pub mod ffi;

use std::collections::{BTreeMap, BTreeSet};
use std::fmt;
use std::io::Write as _;
use std::path::Path;
use std::process::{Command, Stdio};

use object::read::archive::ArchiveFile;
use object::{BinaryFormat, Object, ObjectSection, ObjectSymbol, SectionKind, SymbolKind, SymbolSection};

/// Where a variable lives.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Storage {
    /// Initialised data.
    Data,
    /// Zero-initialised data, including common symbols.
    Bss,
    /// Thread-local.
    ThreadLocal,
}

impl fmt::Display for Storage {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Storage::Data => "data",
            Storage::Bss => "bss",
            Storage::ThreadLocal => "thread-local",
        })
    }
}

/// One variable, named the way the list names it: a C or C++ variable with
/// internal linkage is `file.cpp: name`; anything else is its demangled
/// name alone (a Rust static's name starts with its crate).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Var {
    pub key: String,
    pub storage: Storage,
}

/// What a scan read and found.
#[derive(Debug, Default)]
pub struct Scan {
    pub archives: usize,
    pub objects: usize,
    /// The platform the objects were built for: `macos`, `linux`, `windows`.
    pub platform: Option<&'static str>,
    pub vars: BTreeMap<String, Storage>,
    /// Objects built with global merging, which hides variables' names.
    pub merged: BTreeSet<String>,
}

/// A symbol before demangling.
struct Raw {
    mangled: String,
    stem: Option<String>,
    rust: bool,
    storage: Storage,
}

/// Read every object in `paths` (archives or single objects).
pub fn scan(paths: &[impl AsRef<Path>]) -> Result<Scan, String> {
    let mut scan = Scan::default();
    let mut raws = Vec::new();
    for path in paths {
        let path = path.as_ref();
        let data = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        if let Ok(archive) = ArchiveFile::parse(&*data) {
            scan.archives += 1;
            for member in archive.members() {
                let member = member.map_err(|e| format!("{}: {e}", path.display()))?;
                let name = String::from_utf8_lossy(member.name()).into_owned();
                // Archive symbol tables and string tables are members too.
                if !(name.ends_with(".o") || name.ends_with(".obj")) {
                    continue;
                }
                let bytes = member.data(&*data).map_err(|e| format!("{}({name}): {e}", path.display()))?;
                read_object(bytes, &name, &mut scan, &mut raws).map_err(|e| format!("{}({name}): {e}", path.display()))?;
            }
        } else {
            let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            read_object(&data, &name, &mut scan, &mut raws).map_err(|e| format!("{}: {e}", path.display()))?;
        }
    }
    let names = demangle(raws.iter().map(|r| r.mangled.as_str()))?;
    for (raw, name) in raws.iter().zip(names) {
        let name = normalise(strip_rust_hash(&name), raw.rust);
        // A Clockwork crate's object also holds what it instantiated from
        // std and its dependencies.
        if raw.rust && !name.starts_with("clockwork_") && !name.starts_with("<clockwork_") {
            continue;
        }
        let key = match &raw.stem {
            Some(stem) => format!("{stem}: {name}"),
            None => name.to_string(),
        };
        scan.vars.insert(key, raw.storage);
    }
    Ok(scan)
}

fn read_object(bytes: &[u8], member: &str, scan: &mut Scan, raws: &mut Vec<Raw>) -> Result<(), String> {
    let file = object::File::parse(bytes).map_err(|e| e.to_string())?;
    scan.objects += 1;
    let format = file.format();
    scan.platform.get_or_insert(match format {
        BinaryFormat::MachO => "macos",
        BinaryFormat::Coff | BinaryFormat::Pe => "windows",
        _ => "linux",
    });
    let rust = member.ends_with(".rcgu.o");
    // The staticlib carries std and every dependency, each object named for
    // its crate; the list is about Clockwork's own.
    if rust && !member.starts_with("clockwork_") {
        return Ok(());
    }
    let stem = member.trim_end_matches(".obj").trim_end_matches(".o").to_string();
    for sym in file.symbols() {
        let Some(storage) = storage_of(&file, &sym) else { continue };
        let Ok(name) = sym.name() else { continue };
        let name = match format {
            // Mach-O prefixes every C-level name with an underscore; a name
            // without one is an assembler label.
            BinaryFormat::MachO => match name.strip_prefix('_') {
                Some(n) => n,
                None => continue,
            },
            _ => name,
        };
        if name.contains("MergedGlobals") {
            scan.merged.insert(member.to_string());
            continue;
        }
        if skipped(name) {
            continue;
        }
        raws.push(Raw {
            mangled: name.to_string(),
            stem: (!rust && sym.is_local()).then(|| stem.clone()),
            rust,
            storage,
        });
    }
    Ok(())
}

/// Whether `sym` is a variable with static storage the program may write.
fn storage_of(file: &object::File, sym: &object::Symbol) -> Option<Storage> {
    if matches!(sym.kind(), SymbolKind::Section | SymbolKind::File | SymbolKind::Text) {
        return None;
    }
    let index = match sym.section() {
        SymbolSection::Common => return Some(Storage::Bss),
        SymbolSection::Section(index) => index,
        _ => return None,
    };
    let section = file.section_by_index(index).ok()?;
    let name = section.name().unwrap_or("");
    // Data the language runtime owns: Objective-C metadata, initialiser
    // tables, Core Foundation constant strings, indirect pointers.
    if name.starts_with("__objc_")
        || matches!(
            name,
            "__mod_init_func" | "__mod_term_func" | "__cfstring" | "__got" | "__la_symbol_ptr" | "__nl_symbol_ptr"
        )
        || name.starts_with(".init_array")
        || name.starts_with(".fini_array")
        // Read-only once the loader has relocated it: GCC's home for vtables,
        // typeinfo and constant tables that hold addresses.
        || name.starts_with(".data.rel.ro")
    {
        return None;
    }
    match section.kind() {
        SectionKind::Data => Some(Storage::Data),
        SectionKind::UninitializedData | SectionKind::Common => Some(Storage::Bss),
        SectionKind::Tls | SectionKind::UninitializedTls | SectionKind::TlsVariables => Some(Storage::ThreadLocal),
        _ => None,
    }
}

/// Names that are not a variable of the program's: reserved names,
/// Objective-C metadata, C++ initialisation guards, a thread-local's initial
/// image (Mach-O names the variable itself separately), local labels.
fn skipped(name: &str) -> bool {
    // C and C++ reserve names that start with two underscores for the
    // implementation: compiler-rt's, a thread-local's init guard.
    name.starts_with("__")
        || name.starts_with("_OBJC_")
        || name.starts_with("_ZGV")
        || name.starts_with("DW.ref.")   // GCC's references to the unwinder's personality and typeinfo
        || name.ends_with("$tlv$init")
        || name.starts_with(".L")
        || name.starts_with("ltmp")
        || name.starts_with("OBJC_")
        || name.is_empty()
}

/// Demangle C++ and Rust names through `c++filt` (or `$CXXFILT`), one
/// process for the lot.
fn demangle<'a>(names: impl Iterator<Item = &'a str>) -> Result<Vec<String>, String> {
    let names: Vec<&str> = names.collect();
    if names.is_empty() {
        return Ok(Vec::new());
    }
    let tool = std::env::var("CXXFILT").unwrap_or_else(|_| "c++filt".into());
    let mut child = Command::new(&tool)
        // -n: the names have no leading underscore to strip (scan removed
        // Mach-O's already).
        .arg("-n")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .map_err(|e| format!("{tool}: {e}"))?;
    let mut input = names.join("\n");
    input.push('\n');
    let mut stdin = child.stdin.take().ok_or("c++filt: no stdin")?;
    let writer = std::thread::spawn(move || stdin.write_all(input.as_bytes()));
    let out = child.wait_with_output().map_err(|e| format!("{tool}: {e}"))?;
    writer.join().map_err(|_| "c++filt: writer panicked")?.map_err(|e| format!("{tool}: {e}"))?;
    let text = String::from_utf8_lossy(&out.stdout);
    let lines: Vec<String> = text.lines().map(str::to_string).collect();
    if lines.len() != names.len() {
        return Err(format!("{tool}: {} names in, {} out", names.len(), lines.len()));
    }
    Ok(lines)
}

/// A demangled name as the list writes it, the same from every compiler and
/// toolchain: a lambda is `{lambda}` (clang numbers them `$_9`, GCC writes
/// `{lambda(double)#1}`, and either count moves when a lambda is added
/// above); a Rust name loses v0's crate disambiguators and thread_local!'s
/// expansion, so a thread-local is its path.
fn normalise(name: &str, rust: bool) -> String {
    let mut out = String::with_capacity(name.len());
    let mut rest = name;
    while !rest.is_empty() {
        // A function-local static: clang names the function with its
        // parentheses (`f()::x`), GCC without (`f::x`). One spelling, and
        // a lambda after the parentheses is still a lambda.
        if let Some(tail) = rest.strip_prefix("()::") {
            out.push_str("::");
            rest = tail;
            if let Some(after) = rest.strip_prefix("$_") {
                let digits = after.bytes().take_while(u8::is_ascii_digit).count();
                if digits > 0 {
                    out.push_str("{lambda}");
                    rest = &after[digits..];
                }
            }
            continue;
        }
        if let Some(tail) = rest.strip_prefix("::$_") {
            let digits = tail.bytes().take_while(u8::is_ascii_digit).count();
            if digits > 0 {
                out.push_str("::{lambda}");
                rest = &tail[digits..];
                continue;
            }
        }
        if let Some(tail) = rest.strip_prefix("{lambda(") {
            if let Some(end) = lambda_end(tail) {
                out.push_str("{lambda}");
                rest = &tail[end..];
                continue;
            }
        }
        if rust && rest.starts_with('[') {
            if let Some(close) = rest.find(']') {
                if close > 1 && rest[1..close].bytes().all(|b| b.is_ascii_hexdigit()) {
                    rest = &rest[close + 1..];
                    continue;
                }
            }
        }
        if rust && rest.starts_with("::{") {
            break;
        }
        let c = rest.chars().next().unwrap_or_default();
        out.push(c);
        rest = &rest[c.len_utf8()..];
    }
    out
}

/// Given what follows `{lambda(` in a GCC name, the length up to and
/// including the closing `)#N}`.
fn lambda_end(tail: &str) -> Option<usize> {
    let mut depth = 1usize;
    for (i, c) in tail.char_indices() {
        match c {
            '(' => depth += 1,
            ')' => {
                depth -= 1;
                if depth == 0 {
                    let after = &tail[i + 1..];
                    let count = after.strip_prefix('#')?;
                    let digits = count.bytes().take_while(u8::is_ascii_digit).count();
                    return (digits > 0 && count[digits..].starts_with('}')).then_some(i + 2 + digits + 1);
                }
            }
            _ => {}
        }
    }
    None
}

/// `path::NAME::h0123456789abcdef` → `path::NAME`: the hash changes with
/// every build.
fn strip_rust_hash(name: &str) -> &str {
    if let Some((head, tail)) = name.rsplit_once("::h") {
        if tail.len() == 16 && tail.bytes().all(|b| b.is_ascii_hexdigit()) {
            return head;
        }
    }
    name
}

/// What a listed variable is.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Class {
    /// Belongs to one engine: must move onto it (docs/ENGINES.md).
    Engine,
    /// One per plugin-bridge process, and a bridge serves one engine.
    Bridge,
    /// One per process for a reason the entry gives.
    Shared,
    /// Built once at start-up, before anything reads it, and unable to be a
    /// compile-time constant. One that can be is labelled `constexpr` or
    /// `const` instead, and the scan never sees it.
    Constant,
    /// Only tests write it.
    Test,
}

impl Class {
    fn parse(s: &str) -> Option<Class> {
        Some(match s {
            "engine" => Class::Engine,
            "bridge" => Class::Bridge,
            "shared" => Class::Shared,
            "constant" => Class::Constant,
            "test" => Class::Test,
            _ => return None,
        })
    }
}

impl fmt::Display for Class {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Class::Engine => "engine",
            Class::Bridge => "bridge",
            Class::Shared => "shared",
            Class::Constant => "constant",
            Class::Test => "test",
        })
    }
}

/// One line of the list.
#[derive(Clone, Debug)]
pub struct Entry {
    pub class: Class,
    /// Only built on these platforms or with these options; empty for
    /// every build.
    pub only: Vec<String>,
    pub reason: String,
    pub line: usize,
}

/// The list: `class [only=a,b] | key | reason`, `#` comments.
#[derive(Debug, Default)]
pub struct List {
    pub entries: BTreeMap<String, Entry>,
}

impl List {
    pub fn parse(text: &str) -> Result<List, String> {
        let mut list = List::default();
        for (n, line) in text.lines().enumerate() {
            let n = n + 1;
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let fields: Vec<&str> = line.splitn(3, " | ").map(str::trim).collect();
            let [head, key, reason] = fields[..] else {
                return Err(format!("line {n}: want `class | key | reason`"));
            };
            let mut words = head.split_whitespace();
            let class = words.next().and_then(Class::parse).ok_or_else(|| format!("line {n}: unknown class `{head}`"))?;
            let mut only = Vec::new();
            for word in words {
                let tags = word.strip_prefix("only=").ok_or_else(|| format!("line {n}: unknown `{word}`"))?;
                only.extend(tags.split(',').map(str::to_string));
            }
            if reason.is_empty() {
                return Err(format!("line {n}: `{key}` gives no reason"));
            }
            let entry = Entry { class, only, reason: reason.to_string(), line: n };
            if let Some(old) = list.entries.insert(key.to_string(), entry) {
                return Err(format!("line {n}: `{key}` is listed already, at line {}", old.line));
            }
        }
        Ok(list)
    }
}

/// What [`check`] found wrong, and what it counted.
#[derive(Debug, Default)]
pub struct Report {
    /// Found, not listed.
    pub unlisted: Vec<(String, Storage)>,
    /// Listed for this build, not found.
    pub missing: Vec<(String, usize)>,
    pub counts: BTreeMap<Class, usize>,
}

impl Report {
    pub fn clean(&self) -> bool {
        self.unlisted.is_empty() && self.missing.is_empty()
    }
}

/// Hold `scan` to `list`. `with` names the options this build has, beside
/// its platform, for entries marked `only=`.
pub fn check(scan: &Scan, list: &List, with: &[String]) -> Report {
    let mut report = Report::default();
    for (key, storage) in &scan.vars {
        match list.entries.get(key) {
            Some(entry) => *report.counts.entry(entry.class).or_default() += 1,
            None => report.unlisted.push((key.clone(), *storage)),
        }
    }
    let applies = |entry: &Entry| {
        entry.only.is_empty() || entry.only.iter().any(|t| Some(t.as_str()) == scan.platform || with.contains(t))
    };
    for (key, entry) in &list.entries {
        if applies(entry) && !scan.vars.contains_key(key) {
            report.missing.push((key.clone(), entry.line));
        }
    }
    report
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scan_of(vars: &[&str]) -> Scan {
        Scan {
            platform: Some("linux"),
            vars: vars.iter().map(|k| (k.to_string(), Storage::Bss)).collect(),
            ..Scan::default()
        }
    }

    #[test]
    fn a_listed_variable_is_counted_by_its_class() {
        let list = List::parse("engine | a.cpp: g_x | moves onto the core\nshared | g_y | one per process\n").unwrap();
        let report = check(&scan_of(&["a.cpp: g_x", "g_y"]), &list, &[]);
        assert!(report.clean());
        assert_eq!(report.counts[&Class::Engine], 1);
        assert_eq!(report.counts[&Class::Shared], 1);
    }

    #[test]
    fn an_unlisted_variable_fails() {
        let list = List::parse("shared | g_y | one per process\n").unwrap();
        let report = check(&scan_of(&["g_y", "b.cpp: g_new"]), &list, &[]);
        assert_eq!(report.unlisted, vec![("b.cpp: g_new".to_string(), Storage::Bss)]);
    }

    #[test]
    fn a_listed_variable_the_build_lacks_fails_unless_it_is_for_another_build() {
        let list = List::parse(
            "shared | g_gone | was here\nshared only=windows | g_win | Windows only\nshared only=plugins | g_plug | with plugins\n",
        )
        .unwrap();
        let report = check(&scan_of(&[]), &list, &[]);
        assert_eq!(report.missing, vec![("g_gone".to_string(), 1)]);
        let report = check(&scan_of(&[]), &list, &["plugins".to_string()]);
        assert_eq!(report.missing.len(), 2);
    }

    #[test]
    fn the_list_refuses_a_duplicate_a_missing_reason_and_an_unknown_class() {
        assert!(List::parse("shared | g | a\nshared | g | b\n").is_err());
        assert!(List::parse("shared | g | \n").is_err());
        assert!(List::parse("global | g | a\n").is_err());
        assert!(List::parse("shared g a\n").is_err());
    }

    #[test]
    fn a_lambda_is_named_the_same_by_every_compiler() {
        // clang numbers a function's lambdas $_0, $_1…; GCC writes the
        // signature and a count. Neither survives a lambda added above.
        assert_eq!(
            normalise("E::init(E::Config const&)::$_9::operator()(double) const::lastLogSec", false),
            "E::init(E::Config const&)::{lambda}::operator()(double) const::lastLogSec"
        );
        assert_eq!(
            normalise("E::init(E::Config const&)::{lambda(double)#1}::operator()(double) const::lastLogSec", false),
            "E::init(E::Config const&)::{lambda}::operator()(double) const::lastLogSec"
        );
        assert_eq!(normalise("f()::{lambda(std::pair<int, int>)#12}::x", false), "f::{lambda}::x");
        // clang and GCC name a function-local static's function differently.
        assert_eq!(normalise("guest_info()::info", false), "guest_info::info");
        assert_eq!(normalise("guest_info::info", false), "guest_info::info");
        assert_eq!(normalise("f()::$_4::operator()(int) const::x", false), "f::{lambda}::operator()(int) const::x");
        assert_eq!(normalise("price$_value", false), "price$_value");
    }

    #[test]
    fn a_rust_name_is_its_path_on_every_toolchain() {
        // v0 mangling's crate disambiguators, and thread_local!'s expansion.
        assert_eq!(normalise("clockwork_heap[1a2b3c4d5e6f7a8b]::router::REGION", true), "clockwork_heap::router::REGION");
        assert_eq!(
            normalise("clockwork_heap::router::SCOPE::{K#0}::{closure#1}::__RUST_STD_INTERNAL_VAL", true),
            "clockwork_heap::router::SCOPE"
        );
        assert_eq!(normalise("clockwork_heap::router::SCOPE::__getit::__KEY", true), "clockwork_heap::router::SCOPE::__getit::__KEY");
    }

    #[test]
    fn a_rust_hash_is_not_part_of_the_name() {
        assert_eq!(strip_rust_hash("clockwork_heap::router::REGION::h0123456789abcdef"), "clockwork_heap::router::REGION");
        assert_eq!(strip_rust_hash("clockwork_heap::router::REGION"), "clockwork_heap::router::REGION");
        assert_eq!(strip_rust_hash("a::hello"), "a::hello");
    }
}
