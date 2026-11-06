// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! **Whole-pipeline click tests**: the shipped widgets driven the way a person drives them, against
//! the **real `rime-engine` process** — no fake host anywhere in the loop.
//!
//! # Why this sits between the two proofs that already existed
//!
//! `gui/click_tests.rs` drives the real `EditorApp::update` but substitutes its `FakeHost` for the
//! engine, so it proves what the UI *sends* and how it *reacts* — its own header says "what the
//! real engine does with it is `scripts/editor-click-smoke.sh`'s job". That script does close the
//! loop, and it is the only place a pointer event travels all the way to a `.rscene`, but it is
//! **structurally Linux-only**: it needs an Xvfb display, `xdotool` to synthesise input at pinned
//! screen coordinates, ImageMagick to read a pixel back, and it pins Vulkan to RADV. None of those
//! exist on Windows or macOS, so on two of the three platforms this engine ships to, *nothing*
//! proved that a click reaches the engine's world.
//!
//! This file is that missing middle. It keeps click_tests' technique — `egui_kittest` runs
//! `EditorApp::update` headlessly and addresses widgets through the AccessKit tree by what they
//! SAY, so no display server, no window and no screen coordinates are involved — and removes the
//! only thing click_tests fakes. `EditorApp::new` is the production constructor: it spawns
//! `rime-engine --editor-host <socket> --viewport`, connects over the s1.4 `AF_UNIX` socket, and
//! runs the wire on its own threads. Everything below therefore exercises the real schema, the real
//! reflection, the real world, the real codecs and the real process lifetime, on all three
//! platforms.
//!
//! It became possible on Windows only with the `uds_windows` transport port
//! (`crate::local_socket`): before that, this crate's client half did not compile here at all.
//!
//! # What it deliberately does NOT claim
//!
//! No pixels and no window. A streamed frame is checked for its geometry and arrival, never its
//! content, and the viewport panel has no accessibility node to address (click_tests documents that
//! finding). Whether the thing on screen *looks* right stays with `editor-click-smoke.sh` on Linux
//! and the Mac-eyeballing of ADR-0031. The claim here is narrower and mechanical: a click becomes a
//! command, the engine answers it, and the answer reaches the widgets.
//!
//! # Running it
//!
//! Needs a built `rime-engine`, named by `$RIME_ENGINE_BIN` (the same variable `--smoke` and the
//! shell smokes use). Without it the test SKIPS and says so — except when
//! `$RIME_REQUIRE_EDITOR_PIPELINE` is set, which turns a missing engine into a failure. That
//! pairing is deliberate and mirrors `RIME_REQUIRE_VULKAN` in the C++ suite: a proof that silently
//! skips reads exactly like a proof that passed, so CI sets the REQUIRE variable and a developer's
//! bare `cargo test` does not.
//!
//! ```text
//! RIME_ENGINE_BIN=../build/dev/bin/rime-engine \
//!   cargo test -p editor --features gui -- --ignored pipeline
//! ```
//!
//! It is `#[ignore]`d because it spawns a process and opens a GPU device: `cargo test` runs test
//! functions in parallel threads, and several engines racing for a Vulkan device in one process
//! tree is not a thing to do by accident.

use std::sync::Arc;
use std::time::{Duration, Instant};

use egui::accesskit::Role;
use egui_kittest::kittest::Queryable;
use egui_kittest::Harness;

use super::outliner::label as outliner_label;
use super::*;

/// How long any one engine answer may take before the test calls it a failure. Generous: a Debug
/// engine on a cold file cache opening a Vulkan device is not fast, and this bound exists to turn a
/// hang into a readable failure, not to measure anything.
const PATIENCE: Duration = Duration::from_secs(30);

/// The engine binary under test, or `None` to skip.
fn engine_binary() -> Option<String> {
    match std::env::var("RIME_ENGINE_BIN") {
        Ok(path) if !path.is_empty() => Some(path),
        _ => {
            assert!(
                std::env::var("RIME_REQUIRE_EDITOR_PIPELINE").is_err(),
                "RIME_REQUIRE_EDITOR_PIPELINE is set but $RIME_ENGINE_BIN is not: refusing to \
                 report a skip as a pass"
            );
            eprintln!(
                "pipeline_tests: SKIPPED — set $RIME_ENGINE_BIN to a built rime-engine to run it"
            );
            None
        }
    }
}

/// The editor over a live engine. Mirrors `click_tests::Rig`, minus the fake.
struct LiveRig {
    harness: Harness<'static, EditorApp>,
    /// The same mirror the UI reads, held separately so a wait can look at it while `harness` is
    /// borrowed mutably to step a frame.
    shared: Shared,
}

impl LiveRig {
    fn launch(engine: String) -> Self {
        let harness = Harness::builder()
            .with_size(egui::vec2(1280.0, 800.0))
            // click_tests' reasoning applies unchanged: kittest's default quarter-second step
            // outlasts egui's click window, so a press held across one settle reads as a drag.
            .with_step_dt(1.0 / 60.0)
            .build_eframe(move |_cc| EditorApp::new(engine, None, None));
        let shared = Arc::clone(&harness.state().shared);
        Self { harness, shared }
    }

    /// Step UI frames until the engine has made `what` true, or fail saying what was still missing.
    ///
    /// Unlike the fake-host rig there is nothing to `pump`: the session's receiver thread fills the
    /// mirror on its own schedule, so the test's job is only to keep repainting (which is what
    /// turns an arrived message into widget state) and to wait.
    fn until(&mut self, what: &str, pred: impl Fn(&SharedState) -> bool) {
        let start = Instant::now();
        loop {
            self.harness.step();
            if pred(&self.shared.lock().unwrap()) {
                return;
            }
            assert!(
                start.elapsed() <= PATIENCE,
                "timed out after {PATIENCE:?} waiting for {what}\n  {}",
                self.diagnosis()
            );
        }
    }

    /// Everything known about the session, for a failure message. A timeout here is almost always
    /// "the engine said something else, or nothing", and that is only actionable with the counters.
    fn diagnosis(&self) -> String {
        let s = self.shared.lock().unwrap();
        format!(
            "connected={} error={:?} schema_types={} snapshots={} entities={} frames={} phase={:?}",
            s.connected,
            s.error,
            s.schema.types.len(),
            s.snapshots_received,
            s.snapshot.entities.len(),
            s.frames_received,
            s.play_state.phase,
        )
    }

    /// A few frames, for the UI to settle after an input with no engine answer to wait for.
    fn settle(&mut self) {
        for _ in 0..5 {
            self.harness.step();
        }
    }

    fn app(&self) -> &EditorApp {
        self.harness.state()
    }

    fn click(&mut self, label: &str) {
        self.harness
            .get_by_role_and_label(Role::Button, label)
            .click();
        self.settle();
    }

    fn has(&self, label: &str) -> bool {
        self.harness.query_all_by_label(label).next().is_some()
    }

    /// The inspector's numeric fields, top to bottom.
    fn number_fields(&self) -> usize {
        self.harness.get_all_by_role(Role::SpinButton).count()
    }

    /// Edit a numeric inspector field the way a person types into it: click it (it becomes a text
    /// box with its contents selected), type the replacement, press Enter. Same sequence as
    /// `click_tests::Rig::type_into_number_field`.
    fn type_into_number_field(&mut self, nth: usize, text: &str) {
        {
            let fields: Vec<_> = self.harness.get_all_by_role(Role::SpinButton).collect();
            fields[nth].click();
        }
        self.settle();
        self.harness
            .input_mut()
            .events
            .push(egui::Event::Text(text.to_string()));
        self.settle();
        self.harness.press_key(egui::Key::Enter);
        self.settle();
    }

    /// Step frames until `pred` holds on the APP (not the mirror), bounded. Used for state the UI
    /// derives from an engine answer, like an undo step committing on an `ok` EditResult.
    fn until_app(&mut self, what: &str, pred: impl Fn(&EditorApp) -> bool) {
        let start = Instant::now();
        loop {
            self.harness.step();
            if pred(self.harness.state()) {
                return;
            }
            assert!(
                start.elapsed() <= PATIENCE,
                "timed out after {PATIENCE:?} waiting for {what}\n  {}",
                self.diagnosis()
            );
        }
    }
}

/// The ordered walk: launch, mirror, select, edit, play, stop, teardown — each step asserted from
/// what the ENGINE said, not from what the UI hoped.
///
/// One test rather than six because every step needs the one before it to have happened, and
/// because each `#[test]` would otherwise spawn its own engine and its own Vulkan device. The shell
/// smoke is one ordered walk for the same reason.
#[test]
#[ignore = "spawns the real rime-engine and opens a GPU device; needs $RIME_ENGINE_BIN"]
fn a_click_reaches_the_engines_world_and_its_answer_reaches_the_widgets() {
    let Some(engine) = engine_binary() else {
        return;
    };
    let mut rig = LiveRig::launch(engine);

    // ── 1. The two processes are talking ────────────────────────────────────────────────────
    // `connected` flips only after `Connection::handshake` returns, so this is the real s1.4
    // handshake over the real socket, not merely a spawned child.
    rig.until("the engine to connect", |s| {
        s.connected || s.error.is_some()
    });
    {
        let error = rig.shared.lock().unwrap().error.clone();
        assert!(
            error.is_none(),
            "the session reported an error instead of connecting: {error:?}"
        );
    }

    // ── 2. It described itself, and the description is the ENGINE'S, not a fixture's ────────
    rig.until("the schema and the first snapshot", |s| {
        !s.schema.types.is_empty() && s.snapshots_received > 0
    });
    let (type_count, entities, schema) = {
        let s = rig.shared.lock().unwrap();
        (
            s.schema.types.len(),
            s.snapshot.entities.clone(),
            s.schema.clone(),
        )
    };
    assert!(
        type_count > 1,
        "the engine sent a schema of {type_count} types, which is too few to be its real \
         reflected component set"
    );
    assert!(
        !entities.is_empty(),
        "the engine sent a snapshot with no entities; there is nothing to drive"
    );

    // ── 3. Every entity the engine sent is on screen ────────────────────────────────────────
    // The row label is computed by the shipped `outliner::label` from the real schema and the real
    // snapshot, so this asserts the UI rendered what arrived rather than re-stating a constant.
    let rows: Vec<String> = entities
        .iter()
        .enumerate()
        .map(|(i, e)| outliner_label(&schema, i, e))
        .collect();
    for row in &rows {
        assert!(
            rig.has(row),
            "the outliner is missing the row '{row}' for an entity the engine sent"
        );
    }

    // ── 4. A click selects a real entity ────────────────────────────────────────────────────
    rig.click(&rows[0]);
    assert_eq!(
        rig.app().selected,
        Some(entities[0].editor_id),
        "clicking the first outliner row did not select the entity the engine named"
    );

    // ── 5. An edit travels to the engine, and the ENGINE's acceptance is what commits it ─────
    // `CommandStack` commits an undo step only on an `ok` EditResult (see
    // `SharedState::edit_results` and `CommandStack::acknowledge`), so `can_undo()` flipping is
    // precisely the claim "the real engine applied this": the UI's optimistic patch of its own
    // mirror cannot produce it.
    assert!(
        rig.number_fields() > 0,
        "the inspector offered no numeric field for the selected entity, so there is no edit to \
         make (schema types: {type_count})"
    );
    assert!(
        !rig.app().stack.can_undo(),
        "nothing has been edited yet, so there should be no undo step"
    );
    rig.type_into_number_field(0, "4.25");
    rig.until_app("the engine to confirm the field edit", |app| {
        app.stack.can_undo()
    });

    // ── 6. Play and Stop are the engine's phases, round-tripped ─────────────────────────────
    // The toolbar reads `play_state`, which only ever arrives from the engine — so a phase change
    // here is the engine's report, not a local toggle.
    rig.click("▶");
    rig.until("the engine to report Playing", |s| {
        s.play_state.phase == rime_protocol::PlayPhase::Playing
    });
    rig.click("⏹");
    rig.until("the engine to report Edit again", |s| {
        s.play_state.phase == rime_protocol::PlayPhase::Edit
    });

    // ── 7. The streamed viewport, when there is a device to produce it ──────────────────────
    // `--viewport` degrades to channel-only on a GPU-less host, which is a legitimate outcome and
    // not this test's subject — so frames are REQUIRED only where the C++ suite would also require
    // a device. Geometry only; never content.
    if std::env::var("RIME_REQUIRE_VULKAN").is_ok() {
        rig.until("a streamed viewport frame", |s| s.frames_received > 0);
        let s = rig.shared.lock().unwrap();
        let frame = s.frame.as_ref().expect("a received frame is retained");
        assert!(
            frame.width > 0 && frame.height > 0,
            "a streamed frame arrived with degenerate geometry {}x{}",
            frame.width,
            frame.height
        );
        assert_eq!(
            frame.rgba.len(),
            frame.width as usize * frame.height as usize * 4,
            "the decoded frame's byte count does not match its own geometry"
        );
    } else if rig.shared.lock().unwrap().frames_received == 0 {
        eprintln!(
            "pipeline_tests: no viewport frames (no device, or channel-only) — set \
             RIME_REQUIRE_VULKAN to make that a failure"
        );
    }

    // ── 8. Teardown reaps the engine ────────────────────────────────────────────────────────
    // `EngineSession::drop` kills the child and joins the receiver thread. A leaked engine holding
    // a Vulkan device is exactly the failure that makes a later run flake, so the drop is part of
    // the claim rather than cleanup after it.
    drop(rig);
}
