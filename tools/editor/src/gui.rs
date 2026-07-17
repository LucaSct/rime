// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The egui **docking shell** (feature `gui`) — the windowed FrostEd (M9.3, ADR-0016/0031). It
//! launches `rime-engine --editor-host --viewport`, connects over the s1.4 local socket on a
//! background thread, and presents a docking layout:
//!
//!   * **Viewport** — the engine's streamed frames (LZ4 → RGBA → an egui texture), with pointer/key
//!     input forwarded back to the engine.
//!   * **Outliner** — the world's entities (from the snapshot).
//!   * **Inspector** — the selected entity's components, labelled from the reflection **schema**.
//!   * **Assets** — a placeholder (the manifest-driven browser is m9.5).
//!
//! Everything below the window is the same `rime-protocol` wire the headless `--smoke` exercises, so
//! the plumbing is CI-proven; only the on-screen result is Mac-eyeballed (a windowed UI is not
//! provable on a headless CI box — ADR-0031). The engine renders and owns the world; this shell is a
//! thin, crash-isolated client of it.

use std::process::ExitCode;
use std::sync::mpsc::{self, Sender};
use std::sync::{Arc, Mutex};

use eframe::egui;
use egui_dock::{DockArea, DockState, NodeIndex, Style, TabViewer};

mod session;

use session::{EngineSession, Shared, SharedState};

/// Launch the docking shell. `--engine <path>` / `$RIME_ENGINE_BIN` names the engine binary to host.
pub fn run(args: &[String]) -> ExitCode {
    let engine = arg_value(args, "--engine").or_else(|| std::env::var("RIME_ENGINE_BIN").ok());
    let Some(engine) = engine else {
        eprintln!("editor: no engine binary — pass --engine <rime-engine> or set RIME_ENGINE_BIN");
        return ExitCode::from(2);
    };

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Rime Editor")
            .with_inner_size([1280.0, 800.0]),
        ..Default::default()
    };
    match eframe::run_native(
        "Rime Editor",
        options,
        Box::new(move |_cc| Ok(Box::new(EditorApp::new(engine)))),
    ) {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("editor: could not start the window: {e}");
            ExitCode::from(1)
        }
    }
}

fn arg_value(args: &[String], flag: &str) -> Option<String> {
    args.iter()
        .position(|a| a == flag)
        .and_then(|i| args.get(i + 1))
        .cloned()
}

/// The dockable panels.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Tab {
    Viewport,
    Outliner,
    Inspector,
    Assets,
}

struct EditorApp {
    dock: DockState<Tab>,
    shared: Shared,
    input_tx: Sender<protocol_input::Input>,
    _session: EngineSession,
    // The viewport texture, re-uploaded when a newer frame arrives.
    frame_tex: Option<egui::TextureHandle>,
    shown_seq: u64,
    selected: Option<usize>, // index into the snapshot's entities
}

impl EditorApp {
    fn new(engine: String) -> Self {
        let shared: Shared = Arc::new(Mutex::new(SharedState::default()));
        let (input_tx, input_rx) = mpsc::channel();
        let session = EngineSession::spawn(engine, Arc::clone(&shared), input_rx);
        Self {
            dock: default_layout(),
            shared,
            input_tx,
            _session: session,
            frame_tex: None,
            shown_seq: 0,
            selected: None,
        }
    }
}

// The four panels in a docked layout: the viewport centre, the outliner left, the inspector right,
// the assets browser under the outliner.
fn default_layout() -> DockState<Tab> {
    let mut dock = DockState::new(vec![Tab::Viewport]);
    let surface = dock.main_surface_mut();
    let [_, outliner] = surface.split_left(NodeIndex::root(), 0.22, vec![Tab::Outliner]);
    let [_, _assets] = surface.split_below(outliner, 0.6, vec![Tab::Assets]);
    let [_, _inspector] = surface.split_right(NodeIndex::root(), 0.78, vec![Tab::Inspector]);
    dock
}

impl eframe::App for EditorApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        // Pull the cheap state for this UI frame (hold the lock only briefly), plus the newest frame
        // as an egui image ONLY when its sequence changed — so the ~2 MB RGBA is copied once per
        // streamed frame, not once per repaint.
        let (connected, error, entities, schema_names, frames, fps, frame_dims, new_image) = {
            let s = self.shared.lock().unwrap();
            let new_image = match &s.frame {
                Some(f) if f.seq != self.shown_seq => Some((
                    f.seq,
                    egui::ColorImage::from_rgba_unmultiplied(
                        [f.width as usize, f.height as usize],
                        &f.rgba,
                    ),
                )),
                _ => None,
            };
            (
                s.connected,
                s.error.clone(),
                s.snapshot.entities.clone(),
                s.schema_names(),
                s.frames_received,
                s.fps,
                s.frame.as_ref().map(|f| (f.width, f.height)),
                new_image,
            )
        };

        if let Some((seq, image)) = new_image {
            self.frame_tex =
                Some(ctx.load_texture("viewport", image, egui::TextureOptions::LINEAR));
            self.shown_seq = seq;
        }

        egui::TopBottomPanel::top("menu").show(ctx, |ui| {
            egui::menu::bar(ui, |ui| {
                ui.label(egui::RichText::new("Rime Editor").strong());
                ui.separator();
                ui.label("File");
                ui.label("Edit");
                ui.label("View");
            });
        });

        egui::TopBottomPanel::bottom("status").show(ctx, |ui| {
            ui.horizontal(|ui| {
                let (dot, text) = match (&error, connected) {
                    (Some(e), _) => (egui::Color32::from_rgb(220, 80, 80), format!("error: {e}")),
                    (None, true) => (
                        egui::Color32::from_rgb(90, 200, 120),
                        "connected".to_owned(),
                    ),
                    (None, false) => (
                        egui::Color32::from_rgb(220, 190, 90),
                        "connecting…".to_owned(),
                    ),
                };
                ui.colored_label(dot, "●");
                ui.label(text);
                ui.separator();
                ui.label(format!("{} entities", entities.len()));
                ui.separator();
                ui.label(format!("{frames} frames · {fps:.0} fps"));
            });
        });

        let mut viewer = EditorTabs {
            frame_tex: self.frame_tex.as_ref(),
            frame_dims,
            entities: &entities,
            schema_names: &schema_names,
            selected: &mut self.selected,
            input_tx: &self.input_tx,
        };
        DockArea::new(&mut self.dock)
            .style(Style::from_egui(ctx.style().as_ref()))
            .show(ctx, &mut viewer);

        // Keep animating while a session is live so streamed frames flow smoothly.
        ctx.request_repaint();
    }
}

/// The per-frame view over app state handed to each dock panel.
struct EditorTabs<'a> {
    frame_tex: Option<&'a egui::TextureHandle>,
    frame_dims: Option<(u32, u32)>,
    entities: &'a [rime_protocol::SnapshotEntity],
    schema_names: &'a std::collections::HashMap<u64, String>,
    selected: &'a mut Option<usize>,
    input_tx: &'a Sender<protocol_input::Input>,
}

impl TabViewer for EditorTabs<'_> {
    type Tab = Tab;

    fn title(&mut self, tab: &mut Self::Tab) -> egui::WidgetText {
        match tab {
            Tab::Viewport => "Viewport",
            Tab::Outliner => "Outliner",
            Tab::Inspector => "Inspector",
            Tab::Assets => "Assets",
        }
        .into()
    }

    fn ui(&mut self, ui: &mut egui::Ui, tab: &mut Self::Tab) {
        match tab {
            Tab::Viewport => self.viewport_ui(ui),
            Tab::Outliner => self.outliner_ui(ui),
            Tab::Inspector => self.inspector_ui(ui),
            Tab::Assets => {
                ui.vertical_centered(|ui| {
                    ui.add_space(24.0);
                    ui.weak("Asset browser");
                    ui.weak("(manifest-driven — m9.5)");
                });
            }
        }
    }
}

impl EditorTabs<'_> {
    fn viewport_ui(&mut self, ui: &mut egui::Ui) {
        let avail = ui.available_size();
        let (rect, response) = ui.allocate_exact_size(avail, egui::Sense::click_and_drag());
        if let Some(tex) = self.frame_tex {
            ui.painter().image(
                tex.id(),
                rect,
                egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
                egui::Color32::WHITE,
            );
            self.forward_input(&response, rect);
        } else {
            ui.painter()
                .rect_filled(rect, 0.0, egui::Color32::from_gray(20));
            ui.painter().text(
                rect.center(),
                egui::Align2::CENTER_CENTER,
                "waiting for the engine's viewport…",
                egui::FontId::proportional(16.0),
                egui::Color32::from_gray(140),
            );
        }
    }

    // Translate egui pointer input over the viewport into engine InputEvents (viewport-relative
    // pixels). A minimal set for v1 — move / press / release / scroll; the engine maps them back to
    // platform input.
    fn forward_input(&self, response: &egui::Response, rect: egui::Rect) {
        let Some((w, h)) = self.frame_dims else {
            return;
        };
        let to_pixels = |p: egui::Pos2| -> (i32, i32) {
            let u = ((p.x - rect.left()) / rect.width()).clamp(0.0, 1.0);
            let v = ((p.y - rect.top()) / rect.height()).clamp(0.0, 1.0);
            ((u * w as f32) as i32, (v * h as f32) as i32)
        };
        if let Some(pos) = response.hover_pos() {
            let (x, y) = to_pixels(pos);
            if response.dragged() || response.hovered() {
                let _ = self
                    .input_tx
                    .send(protocol_input::Input::PointerMove { x, y });
            }
            if response.drag_started() || response.clicked() {
                let _ = self
                    .input_tx
                    .send(protocol_input::Input::PointerDown { x, y, button: 0 });
            }
            if response.drag_stopped() {
                let _ = self
                    .input_tx
                    .send(protocol_input::Input::PointerUp { x, y, button: 0 });
            }
        }
    }

    fn outliner_ui(&mut self, ui: &mut egui::Ui) {
        if self.entities.is_empty() {
            ui.weak("(no entities yet)");
            return;
        }
        egui::ScrollArea::vertical().show(ui, |ui| {
            for (i, e) in self.entities.iter().enumerate() {
                let label = format!("entity {} · {} components", i, e.components.len());
                if ui
                    .selectable_label(*self.selected == Some(i), label)
                    .clicked()
                {
                    *self.selected = Some(i);
                }
            }
        });
    }

    fn inspector_ui(&mut self, ui: &mut egui::Ui) {
        let Some(idx) = *self.selected else {
            ui.weak("select an entity in the Outliner");
            return;
        };
        let Some(entity) = self.entities.get(idx) else {
            ui.weak("(stale selection)");
            return;
        };
        ui.heading(format!("entity {idx}"));
        ui.label(format!("handle {}:{}", entity.index, entity.generation));
        ui.separator();
        for comp in &entity.components {
            let name = self
                .schema_names
                .get(&comp.type_hash)
                .cloned()
                .unwrap_or_else(|| format!("type 0x{:016x}", comp.type_hash));
            egui::CollapsingHeader::new(name)
                .default_open(true)
                .show(ui, |ui| {
                    // v1 shows the component's identity + size; a reflection-schema-typed field editor
                    // (the value view/edit that sends SetComponent) is the m9.4 inspector brick.
                    ui.weak(format!(
                        "{} bytes · hash 0x{:016x}",
                        comp.data.len(),
                        comp.type_hash
                    ));
                });
        }
    }
}

// A small, UI-side input vocabulary the session thread turns into rime_protocol::InputEvent. Kept
// separate so the UI does not depend on the wire's exact field layout.
mod protocol_input {
    // The Pointer* prefix is meaningful (all are pointer events; Key* join later), so the shared
    // prefix is intentional here.
    #[allow(clippy::enum_variant_names)]
    pub enum Input {
        PointerMove { x: i32, y: i32 },
        PointerDown { x: i32, y: i32, button: u32 },
        PointerUp { x: i32, y: i32, button: u32 },
    }
}
