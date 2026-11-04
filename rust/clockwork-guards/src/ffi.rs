// SPDX-License-Identifier: AGPL-3.0-or-later OR LicenseRef-Clockwork-Commercial
// Copyright (c) 2026 Sam Aaron
//! No panic crosses into a host (docs/ENGINES.md): every `extern "C"`
//! function in Rust source, the ones C calls by name and the callbacks it
//! is handed, is one call to the fence, `no_unwind(fallback, || ...)`, with
//! nothing of its own outside it. Past an `extern "C"` boundary a panic
//! aborts the process, which in a plugin is the host's.
//!
//! Read from the sources, parsed: what is checked is the shape of each
//! function's body, which the compiled objects no longer show. Test code
//! (`#[cfg(test)]`) is not shipped and is not held to it.

use std::path::{Path, PathBuf};

use syn::visit::Visit;

/// One `extern "C"` function whose body is not the fence alone.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Unfenced {
    pub file: PathBuf,
    pub line: usize,
    pub name: String,
}

/// What a check read and found.
#[derive(Debug, Default)]
pub struct FfiReport {
    pub files: usize,
    pub functions: usize,
    pub unfenced: Vec<Unfenced>,
}

/// Every `.rs` file under `roots`, read and held to the rule.
pub fn check_ffi(roots: &[impl AsRef<Path>]) -> Result<FfiReport, String> {
    let mut report = FfiReport::default();
    let mut files = Vec::new();
    for root in roots {
        collect(root.as_ref(), &mut files)?;
    }
    files.sort();
    for file in files {
        let text = std::fs::read_to_string(&file).map_err(|e| format!("{}: {e}", file.display()))?;
        check_source(&file, &text, &mut report)?;
    }
    Ok(report)
}

/// One file's source, held to the rule.
pub fn check_source(file: &Path, text: &str, report: &mut FfiReport) -> Result<(), String> {
    let parsed = syn::parse_file(text).map_err(|e| format!("{}: {e}", file.display()))?;
    report.files += 1;
    let mut v = Visitor { file, report };
    v.visit_file(&parsed);
    Ok(())
}

fn collect(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), String> {
    if dir.is_file() {
        out.push(dir.to_path_buf());
        return Ok(());
    }
    let entries = std::fs::read_dir(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    for entry in entries {
        let path = entry.map_err(|e| format!("{}: {e}", dir.display()))?.path();
        if path.is_dir() {
            collect(&path, out)?;
        } else if path.extension().is_some_and(|x| x == "rs") {
            out.push(path);
        }
    }
    Ok(())
}

struct Visitor<'a> {
    file: &'a Path,
    report: &'a mut FfiReport,
}

fn is_test(attrs: &[syn::Attribute]) -> bool {
    attrs.iter().any(|a| {
        a.path().is_ident("cfg")
            && a.parse_args::<syn::Meta>().is_ok_and(|m| m.path().is_ident("test"))
    })
}

fn is_extern_c(sig: &syn::Signature) -> bool {
    sig.abi.as_ref().is_some_and(|abi| abi.name.as_ref().is_none_or(|n| n.value() == "C"))
}

/// The body is one expression: a call to `no_unwind` with a fallback and a
/// closure.
fn is_fenced(block: &syn::Block) -> bool {
    let [syn::Stmt::Expr(syn::Expr::Call(call), None)] = block.stmts.as_slice() else {
        return false;
    };
    let syn::Expr::Path(func) = call.func.as_ref() else {
        return false;
    };
    func.path.segments.last().is_some_and(|s| s.ident == "no_unwind")
        && call.args.len() == 2
        && matches!(call.args.last(), Some(syn::Expr::Closure(_)))
}

impl Visitor<'_> {
    fn function(&mut self, sig: &syn::Signature, block: &syn::Block) {
        if !is_extern_c(sig) {
            return;
        }
        self.report.functions += 1;
        if !is_fenced(block) {
            self.report.unfenced.push(Unfenced {
                file: self.file.to_path_buf(),
                line: sig.ident.span().start().line,
                name: sig.ident.to_string(),
            });
        }
    }
}

impl<'ast> Visit<'ast> for Visitor<'_> {
    fn visit_item_mod(&mut self, m: &'ast syn::ItemMod) {
        if !is_test(&m.attrs) {
            syn::visit::visit_item_mod(self, m);
        }
    }
    fn visit_item_fn(&mut self, f: &'ast syn::ItemFn) {
        if is_test(&f.attrs) {
            return;
        }
        self.function(&f.sig, &f.block);
        syn::visit::visit_item_fn(self, f);
    }
    fn visit_impl_item_fn(&mut self, f: &'ast syn::ImplItemFn) {
        if is_test(&f.attrs) {
            return;
        }
        self.function(&f.sig, &f.block);
        syn::visit::visit_impl_item_fn(self, f);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn check(text: &str) -> FfiReport {
        let mut r = FfiReport::default();
        check_source(Path::new("t.rs"), text, &mut r).unwrap();
        r
    }

    #[test]
    fn the_fence_alone_passes_and_anything_beside_it_does_not() {
        let r = check(
            r#"
            #[no_mangle]
            pub extern "C" fn fenced(x: u32) -> u32 { no_unwind(0, || x + 1) }
            pub extern "C" fn callback(p: *mut u8) { clockwork_fence::no_unwind((), || { let _ = p; }) }
            #[no_mangle]
            pub unsafe extern "C" fn bare(x: u32) -> u32 { x + 1 }
            pub extern "C" fn prelude(x: u32) -> u32 { let y = x; no_unwind(0, || y) }
            extern fn default_abi() {}
            pub fn not_extern() {}
            impl T { extern "C" fn method(&self) -> i32 { 0 } }
            "#,
        );
        assert_eq!(r.functions, 6);
        let names: Vec<_> = r.unfenced.iter().map(|u| u.name.as_str()).collect();
        assert_eq!(names, ["bare", "prelude", "default_abi", "method"]);
        assert_eq!(r.unfenced[0].line, 6);
    }

    #[test]
    fn test_code_is_not_held_to_it() {
        let r = check(
            r#"
            #[cfg(test)]
            mod tests { extern "C" fn capture() {} }
            #[cfg(test)]
            extern "C" fn helper() {}
            "#,
        );
        assert_eq!((r.functions, r.unfenced.len()), (0, 0));
    }
}
