// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The Rime editor (M9) — a **client of a live engine process** (ADR-0016). It launches
//! `rime-engine --editor-host`, connects over the s1.4 local socket, and edits the world through the
//! reflection-described editor channel (the `rime-protocol` crate).
//!
//! v1 ships the headless **`--smoke`** end-to-end check (spawn the engine, handshake, pull the
//! schema + world snapshot, push an edit, shut down cleanly) — the CI-provable proof that the
//! editor-as-client loop works. The interactive egui docking shell + streamed viewport are the next
//! brick (Mac-eyeballed per ADR-0031, since a windowed UI is not provable on a headless box).

mod smoke;

use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();

    if args.iter().any(|a| a == "--smoke") {
        return smoke::run(&args);
    }
    if args.iter().any(|a| a == "-h" || a == "--help") {
        print_usage();
        return ExitCode::SUCCESS;
    }

    eprintln!(
        "editor: the interactive shell (egui docking + streamed viewport) is the next brick."
    );
    eprintln!("        for now, `editor --smoke` runs the headless editor<->engine check.");
    print_usage();
    ExitCode::from(2)
}

fn print_usage() {
    eprintln!("usage: editor --smoke [--engine <rime-engine>] [--scene <file.rscene>]");
    eprintln!("  --engine <path>   the rime-engine binary to launch (else $RIME_ENGINE_BIN)");
    eprintln!(
        "  --scene  <path>   an optional .rscene for the engine to load (else a default world)"
    );
}
