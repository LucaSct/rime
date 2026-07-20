// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The Rime editor (M9) — a **client of a live engine process** (ADR-0016). It launches
//! `rime-engine --editor-host`, connects over the s1.4 local socket, and works the world through the
//! reflection-described editor channel (the `rime-protocol` crate), drawing the engine's streamed
//! viewport in a panel.
//!
//! Two faces, one binary:
//!   * **`editor --smoke [--frames N]`** — the headless end-to-end check (always built): the
//!     CI-provable proof that the editor-as-client loop (channel + streamed frames) works.
//!   * **`editor`** — the interactive **egui docking shell** (built with `--features gui`): the
//!     windowed FrostEd. The windowing stack (eframe → wgpu/winit) is feature-gated so the headless
//!     smoke stays a light build; per ADR-0031 the live UI is Mac-eyeballed, not CI-run.

mod smoke;

// Gizmo math (m9.6b) is pure — no egui, no wire — and deliberately NOT feature-gated: its unit
// battery (the plan's "gizmo math edge cases" fence) runs under the plain `cargo test`, GUI
// feature or not. Only the interaction wiring in `gui` needs the windowing stack.
#[allow(dead_code)] // consumed by the `gui` feature; headless builds keep only the tests
mod gizmo;

#[cfg(feature = "gui")]
mod gui;

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
    launch(&args)
}

// The default face: the docking shell when built with the `gui` feature.
#[cfg(feature = "gui")]
fn launch(args: &[String]) -> ExitCode {
    gui::run(args)
}

// Built without the GUI: only the headless smoke exists. Point the user at it (or a GUI rebuild).
#[cfg(not(feature = "gui"))]
fn launch(_args: &[String]) -> ExitCode {
    eprintln!(
        "editor: built without the GUI — rebuild with `--features gui` for the docking shell."
    );
    eprintln!("        `editor --smoke` runs the headless editor<->engine check.");
    print_usage();
    ExitCode::from(2)
}

fn print_usage() {
    eprintln!("usage:");
    eprintln!(
        "  editor [--engine <rime-engine>] [--scene <file.rscene>] [--assets <manifest>]\n\
         \x20                              launch the docking shell (needs --features gui)"
    );
    eprintln!("  editor --smoke [--frames N] [--engine <rime-engine>] [--scene <file.rscene>]");
    eprintln!(
        "      --frames N   receive+decode N streamed viewport frames (else the editor channel)"
    );
    eprintln!("      --engine P   the rime-engine binary to launch (else $RIME_ENGINE_BIN)");
    eprintln!(
        "      --scene  P   a .rscene for the engine to load into the world (else a default scene)"
    );
}
