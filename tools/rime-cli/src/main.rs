// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Milestone-0 stub for the Rime command-line tool. Today it only proves the Rust
// toolchain and Cargo workspace build and run — the Rust-side mirror of the C++
// `rime_hello`. Real subcommands (cook a project, run headless, inspect assets) arrive
// from Milestone 6, and will reach the engine only across the stable FFI boundary
// (see tools/README.md and docs/adr/0001).

/// The tool's banner line, e.g. `"rime-cli 0.0.1"`. Kept as a small pure function so it
/// is trivially testable, and so the version lives in exactly one place: Cargo's
/// `CARGO_PKG_VERSION`, substituted at compile time from the crate's manifest.
fn banner() -> String {
    format!("rime-cli {}", env!("CARGO_PKG_VERSION"))
}

fn main() {
    println!("{}", banner());
    println!("Frost tooling online. (Milestone 0: the Rust workspace builds.)");
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirrors the C++ side's version test: pin the format (name prefix) and prove the
    // version is Cargo's, not a hand-typed literal — so the two can never drift.
    #[test]
    fn banner_has_name_and_version() {
        let b = banner();
        assert!(b.starts_with("rime-cli "));
        assert!(b.ends_with(env!("CARGO_PKG_VERSION")));
    }
}
