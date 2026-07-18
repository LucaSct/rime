// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The egui **docking shell** (feature `gui`) — the windowed FrostEd (M9.3/M9.4, ADR-0016/0031). It
//! launches `rime-engine --editor-host --viewport`, connects over the s1.4 local socket on a
//! background thread, and presents a docking layout:
//!
//!   * **Viewport** — the engine's streamed frames (LZ4 → RGBA → an egui texture), with pointer/key
//!     input forwarded back to the engine.
//!   * **Outliner** — the world's entities (from the snapshot), with spawn/despawn.
//!   * **Inspector** — the selected entity's components as **editable, reflection-typed fields**
//!     (m9.4): each field decoded from the schema, edited with a drag-number / checkbox, and pushed
//!     back as a `SetComponent`. Add/remove a component; undo/redo edits.
//!   * **Assets** — a placeholder (the manifest-driven browser is m9.5).
//!
//! Everything below the window is the same `rime-protocol` wire the headless `--smoke` exercises, so
//! the plumbing is CI-proven; only the on-screen result is Mac-eyeballed (a windowed UI is not
//! provable on a headless CI box — ADR-0031). The engine renders and owns the world; this shell is a
//! thin, crash-isolated client of it, and **every edit is a [`commands::Command`]** — the same typed
//! mutations a scripted test drives, which is what makes undo and the headless proof honest.

use std::collections::HashSet;
use std::process::ExitCode;
use std::sync::mpsc::{self, Sender};
use std::sync::{Arc, Mutex};

use eframe::egui;
use egui_dock::{DockArea, DockState, NodeIndex, Style, TabViewer};

use rime_protocol::{
    decode_value, encode_value, AssetEntry, AssetKind, EditorMessage, PickRequest, Schema,
    SnapshotEntity, Value,
};

mod commands;
mod session;

use commands::{Command, CommandStack, Edit, EntityKey};
use session::{EngineSession, Outbound, Shared, SharedState};

/// Launch the docking shell. `--engine <path>` / `$RIME_ENGINE_BIN` names the engine binary to host.
pub fn run(args: &[String]) -> ExitCode {
    let engine = arg_value(args, "--engine").or_else(|| std::env::var("RIME_ENGINE_BIN").ok());
    let Some(engine) = engine else {
        eprintln!("editor: no engine binary — pass --engine <rime-engine> or set RIME_ENGINE_BIN");
        return ExitCode::from(2);
    };

    // Optional cook manifest to forward to the engine child, so the asset browser has content
    // (`editor --engine <bin> --assets <manifest>`). Without it the browser is empty but everything
    // else works.
    let assets = arg_value(args, "--assets");

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("Rime Editor")
            .with_inner_size([1280.0, 800.0]),
        ..Default::default()
    };
    match eframe::run_native(
        "Rime Editor",
        options,
        Box::new(move |_cc| Ok(Box::new(EditorApp::new(engine, assets.clone())))),
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

/// A component edit in progress — one drag or one focused text entry. It captures the component's
/// bytes at the *start* of the gesture so that when the gesture ends we can push a single undo step
/// (old → new), not one per intermediate frame. `new_blob` tracks the latest value the live edits
/// have reached.
struct ActiveEdit {
    key: EntityKey,
    type_hash: u64,
    old_blob: Vec<u8>,
    new_blob: Vec<u8>,
}

struct EditorApp {
    dock: DockState<Tab>,
    shared: Shared,
    out_tx: Sender<Outbound>,
    _session: EngineSession,
    // The viewport texture, re-uploaded when a newer frame arrives.
    frame_tex: Option<egui::TextureHandle>,
    shown_seq: u64,
    selected: Option<usize>, // index into the snapshot's entities
    stack: CommandStack,
    active_edit: Option<ActiveEdit>,
    // Asset browser (m9.5) filter state.
    asset_search: String,
    asset_kind_filter: Option<AssetKind>,
}

impl EditorApp {
    fn new(engine: String, assets: Option<String>) -> Self {
        let shared: Shared = Arc::new(Mutex::new(SharedState::default()));
        let (out_tx, out_rx) = mpsc::channel();
        let session = EngineSession::spawn(engine, assets, Arc::clone(&shared), out_rx);
        Self {
            dock: default_layout(),
            shared,
            out_tx,
            _session: session,
            frame_tex: None,
            shown_seq: 0,
            selected: None,
            stack: CommandStack::default(),
            active_edit: None,
            asset_search: String::new(),
            asset_kind_filter: None,
        }
    }

    /// Apply the commands the UI produced this frame: put each on the wire, patch the mirror
    /// optimistically for value edits, and request a fresh snapshot after a structural change (so the
    /// mirror re-syncs with the engine's truth — e.g. an added component's real default values).
    fn dispatch(&mut self, actions: Vec<Command>) {
        for cmd in actions {
            let (msg, payload) = cmd.to_wire();
            let _ = self.out_tx.send(Outbound::Editor { msg, payload });
            match &cmd {
                Command::SetComponent {
                    key,
                    type_hash,
                    blob,
                } => {
                    self.shared
                        .lock()
                        .unwrap()
                        .apply_optimistic_set(*key, *type_hash, blob);
                }
                c if c.is_structural() => {
                    let (msg, payload) = Command::RequestSnapshot.to_wire();
                    let _ = self.out_tx.send(Outbound::Editor { msg, payload });
                }
                _ => {}
            }
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
        // streamed frame, not once per repaint. The schema + entities are cloned so the widgets can
        // borrow them freely without holding the lock across the whole render.
        let (connected, error, entities, schema, assets, frames, fps, frame_dims, new_image, pick) = {
            let mut s = self.shared.lock().unwrap();
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
            let pick = s.last_pick.take(); // consumed here: each answer moves selection once
            (
                s.connected,
                s.error.clone(),
                s.snapshot.entities.clone(),
                s.schema.clone(),
                s.assets.clone(),
                s.frames_received,
                s.fps,
                s.frame.as_ref().map(|f| (f.width, f.height)),
                new_image,
                pick,
            )
        };

        // Click-to-select (m9.6): the engine answered a viewport click with the entity under that
        // pixel; map its handle back to the outliner row. A miss (empty space) — or a handle the
        // snapshot no longer contains (despawned mid-flight) — clears the selection, exactly what
        // clicking nothing should do.
        if let Some(pick) = pick {
            self.selected = if pick.is_hit() {
                entities
                    .iter()
                    .position(|e| (e.index, e.generation) == (pick.index, pick.generation))
            } else {
                None
            };
        }

        if let Some((seq, image)) = new_image {
            self.frame_tex =
                Some(ctx.load_texture("viewport", image, egui::TextureOptions::LINEAR));
            self.shown_seq = seq;
        }

        // Every mutation the panels/menu produce this frame lands here, then is dispatched once.
        let mut actions: Vec<Command> = Vec::new();

        // Undo/redo — keyboard (Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z or Ctrl/Cmd+Y) and the Edit menu.
        let mut do_undo = false;
        let mut do_redo = false;
        ctx.input(|i| {
            if i.modifiers.command && i.key_pressed(egui::Key::Z) {
                if i.modifiers.shift {
                    do_redo = true;
                } else {
                    do_undo = true;
                }
            }
            if i.modifiers.command && i.key_pressed(egui::Key::Y) {
                do_redo = true;
            }
        });

        let can_undo = self.stack.can_undo();
        let can_redo = self.stack.can_redo();
        egui::TopBottomPanel::top("menu").show(ctx, |ui| {
            egui::menu::bar(ui, |ui| {
                ui.label(egui::RichText::new("Rime Editor").strong());
                ui.separator();
                ui.label("File");
                ui.menu_button("Edit", |ui| {
                    if ui
                        .add_enabled(can_undo, egui::Button::new("Undo\tCtrl+Z"))
                        .clicked()
                    {
                        do_undo = true;
                        ui.close_menu();
                    }
                    if ui
                        .add_enabled(can_redo, egui::Button::new("Redo\tCtrl+Y"))
                        .clicked()
                    {
                        do_redo = true;
                        ui.close_menu();
                    }
                });
                ui.label("View");
            });
        });
        if do_undo {
            if let Some(cmd) = self.stack.undo() {
                actions.push(cmd);
            }
        }
        if do_redo {
            if let Some(cmd) = self.stack.redo() {
                actions.push(cmd);
            }
        }

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

        // The component types an entity can gain (registered components only — never a bare Vec3).
        let addable: Vec<(u64, String)> = schema
            .types
            .iter()
            .filter(|t| t.is_component)
            .map(|t| (t.type_hash, t.name.clone()))
            .collect();

        // The type_hash the browser's "place a mesh" writes — the authoring MeshAsset reference. None
        // if the engine predates it (then meshes are browse-only), keeping the editor forward/back
        // compatible with the host it launched.
        let mesh_asset_hash = schema
            .types
            .iter()
            .find(|t| t.name == "rime::render::MeshAsset")
            .map(|t| t.type_hash);

        let mut viewer = EditorTabs {
            frame_tex: self.frame_tex.as_ref(),
            frame_dims,
            entities: &entities,
            schema: &schema,
            addable: &addable,
            assets: &assets,
            mesh_asset_hash,
            asset_search: &mut self.asset_search,
            asset_kind_filter: &mut self.asset_kind_filter,
            selected: &mut self.selected,
            out_tx: &self.out_tx,
            actions: &mut actions,
            active_edit: &mut self.active_edit,
            stack: &mut self.stack,
        };
        DockArea::new(&mut self.dock)
            .style(Style::from_egui(ctx.style().as_ref()))
            .show(ctx, &mut viewer);

        self.dispatch(actions);

        // Keep animating while a session is live so streamed frames flow smoothly.
        ctx.request_repaint();
    }
}

/// The per-frame view over app state handed to each dock panel.
struct EditorTabs<'a> {
    frame_tex: Option<&'a egui::TextureHandle>,
    frame_dims: Option<(u32, u32)>,
    entities: &'a [SnapshotEntity],
    schema: &'a Schema,
    addable: &'a [(u64, String)],
    assets: &'a [AssetEntry],
    mesh_asset_hash: Option<u64>,
    asset_search: &'a mut String,
    asset_kind_filter: &'a mut Option<AssetKind>,
    selected: &'a mut Option<usize>,
    out_tx: &'a Sender<Outbound>,
    actions: &'a mut Vec<Command>,
    active_edit: &'a mut Option<ActiveEdit>,
    stack: &'a mut CommandStack,
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
            Tab::Viewport => viewport_ui(ui, self.frame_tex, self.frame_dims, self.out_tx),
            Tab::Outliner => outliner_ui(ui, self.entities, self.selected, self.actions),
            Tab::Inspector => inspector_ui(
                ui,
                self.entities,
                self.schema,
                self.addable,
                *self.selected,
                self.actions,
                self.active_edit,
                self.stack,
            ),
            Tab::Assets => assets_ui(
                ui,
                self.assets,
                self.mesh_asset_hash,
                self.asset_search,
                self.asset_kind_filter,
                self.actions,
            ),
        }
    }
}

// The asset browser (m9.5): the engine's cook manifest, searchable + kind-filterable, with "place"
// spawning an entity that references the asset. v1 places meshes (as an authoring MeshAsset ref);
// other kinds are browse-only (a texture/material is referenced by a mesh/material, not placed as a
// standalone entity). Thumbnails and drag-out are deferred (kind icons only).
fn assets_ui(
    ui: &mut egui::Ui,
    assets: &[AssetEntry],
    mesh_asset_hash: Option<u64>,
    search: &mut String,
    kind_filter: &mut Option<AssetKind>,
    actions: &mut Vec<Command>,
) {
    ui.horizontal(|ui| {
        ui.label("🔍");
        ui.text_edit_singleline(search);
        egui::ComboBox::from_id_salt("asset-kind-filter")
            .selected_text(kind_filter.map_or("All kinds", AssetKind::label))
            .show_ui(ui, |ui| {
                ui.selectable_value(kind_filter, None, "All kinds");
                for k in [
                    AssetKind::Mesh,
                    AssetKind::Texture,
                    AssetKind::Material,
                    AssetKind::Skeleton,
                    AssetKind::AnimationClip,
                    AssetKind::Destructible,
                ] {
                    ui.selectable_value(kind_filter, Some(k), k.label());
                }
            });
    });
    ui.separator();

    if assets.is_empty() {
        ui.weak("(no cooked assets — launch the engine with --assets <manifest>)");
        return;
    }

    let needle = search.to_lowercase();
    let mut shown = 0usize;
    egui::ScrollArea::vertical().show(ui, |ui| {
        for a in assets {
            if kind_filter.is_some_and(|k| k != a.kind) {
                continue;
            }
            if !needle.is_empty()
                && !a.source_path.to_lowercase().contains(&needle)
                && !a.kind.label().to_lowercase().contains(&needle)
            {
                continue;
            }
            shown += 1;
            ui.horizontal(|ui| {
                ui.label(kind_glyph(a.kind));
                ui.monospace(&a.source_path);
                // Only meshes place (as a MeshAsset authoring reference); and only if the host's
                // schema has that component.
                if a.kind == AssetKind::Mesh {
                    if let Some(hash) = mesh_asset_hash {
                        if ui.small_button("place").clicked() {
                            // MeshAsset { asset: <content id> } — one u64 field, packed as-is.
                            let blob = encode_value(&Value::Struct(vec![(
                                "asset".to_string(),
                                Value::U64(a.id),
                            )]));
                            actions.push(Command::SpawnEntity {
                                components: vec![(hash, blob)],
                            });
                        }
                    }
                }
            });
        }
    });
    if shown == 0 {
        ui.weak("(no assets match the filter)");
    }
}

// A little glyph per asset kind — the "kind icon" v1 (real thumbnails are a backlog cook pass).
fn kind_glyph(kind: AssetKind) -> &'static str {
    match kind {
        AssetKind::Mesh => "◆",
        AssetKind::Texture => "▦",
        AssetKind::Material => "●",
        AssetKind::Skeleton => "🦴",
        AssetKind::AnimationClip => "▶",
        AssetKind::Destructible => "✸",
        AssetKind::Unknown(_) => "?",
    }
}

fn viewport_ui(
    ui: &mut egui::Ui,
    frame_tex: Option<&egui::TextureHandle>,
    frame_dims: Option<(u32, u32)>,
    out_tx: &Sender<Outbound>,
) {
    let avail = ui.available_size();
    let (rect, response) = ui.allocate_exact_size(avail, egui::Sense::click_and_drag());
    if let Some(tex) = frame_tex {
        ui.painter().image(
            tex.id(),
            rect,
            egui::Rect::from_min_max(egui::pos2(0.0, 0.0), egui::pos2(1.0, 1.0)),
            egui::Color32::WHITE,
        );
        forward_input(out_tx, &response, rect, frame_dims);
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

// Translate egui pointer input over the viewport into engine InputEvents (viewport-relative pixels).
// A minimal set for v1 — move / press / release; the engine maps them back to platform input.
fn forward_input(
    out_tx: &Sender<Outbound>,
    response: &egui::Response,
    rect: egui::Rect,
    frame_dims: Option<(u32, u32)>,
) {
    let Some((w, h)) = frame_dims else {
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
            let _ = out_tx.send(Outbound::Input(protocol_input::Input::PointerMove { x, y }));
        }
        if response.drag_started() || response.clicked() {
            let _ = out_tx.send(Outbound::Input(protocol_input::Input::PointerDown {
                x,
                y,
                button: 0,
            }));
        }
        if response.drag_stopped() {
            let _ = out_tx.send(Outbound::Input(protocol_input::Input::PointerUp {
                x,
                y,
                button: 0,
            }));
        }
        // A plain click — egui reports it only when the press/release pair never became a drag —
        // asks the engine what is under the cursor (m9.6). The engine runs its ID-buffer pick pass
        // at this pixel and answers with a PickResult a frame later; selection moves when it lands
        // (see the pick consumption in `update`). Drags keep their existing meaning (forwarded
        // input), so navigating never steals a selection.
        if response.clicked() {
            let _ = out_tx.send(Outbound::Editor {
                msg: EditorMessage::PickRequest,
                payload: PickRequest { x, y }.encode(),
            });
        }
    }
}

fn outliner_ui(
    ui: &mut egui::Ui,
    entities: &[SnapshotEntity],
    selected: &mut Option<usize>,
    actions: &mut Vec<Command>,
) {
    ui.horizontal(|ui| {
        if ui.button("+ spawn").clicked() {
            *selected = None;
            actions.push(Command::Spawn);
        }
    });
    ui.separator();
    if entities.is_empty() {
        ui.weak("(no entities yet)");
        return;
    }
    egui::ScrollArea::vertical().show(ui, |ui| {
        for (i, e) in entities.iter().enumerate() {
            let label = format!("entity {} · {} components", i, e.components.len());
            if ui.selectable_label(*selected == Some(i), label).clicked() {
                *selected = Some(i);
            }
        }
    });
}

#[allow(clippy::too_many_arguments)] // an inspector genuinely needs its world view + edit sinks
fn inspector_ui(
    ui: &mut egui::Ui,
    entities: &[SnapshotEntity],
    schema: &Schema,
    addable: &[(u64, String)],
    selected: Option<usize>,
    actions: &mut Vec<Command>,
    active_edit: &mut Option<ActiveEdit>,
    stack: &mut CommandStack,
) {
    let Some(idx) = selected else {
        ui.weak("select an entity in the Outliner");
        return;
    };
    let Some(entity) = entities.get(idx) else {
        ui.weak("(stale selection)");
        return;
    };
    let key: EntityKey = (entity.index, entity.generation);

    ui.horizontal(|ui| {
        ui.heading(format!("entity {idx}"));
        if ui.button("✖ despawn").clicked() {
            actions.push(Command::Despawn { key });
        }
    });
    ui.label(format!("handle {}:{}", entity.index, entity.generation));

    // Add-component menu: the registered component types this entity does not already have.
    let present: HashSet<u64> = entity.components.iter().map(|c| c.type_hash).collect();
    ui.menu_button("+ add component", |ui| {
        let mut any = false;
        for (hash, name) in addable {
            if present.contains(hash) {
                continue;
            }
            any = true;
            if ui.button(name).clicked() {
                actions.push(Command::AddComponent {
                    key,
                    type_hash: *hash,
                });
                ui.close_menu();
            }
        }
        if !any {
            ui.weak("(all registered components present)");
        }
    });
    ui.separator();

    egui::ScrollArea::vertical().show(ui, |ui| {
        for comp in &entity.components {
            let name = schema
                .type_by_hash(comp.type_hash)
                .map(|t| t.name.clone())
                .unwrap_or_else(|| format!("type 0x{:016x}", comp.type_hash));
            egui::CollapsingHeader::new(name)
                .default_open(true)
                .show(ui, |ui| {
                    if ui.button("remove").clicked() {
                        actions.push(Command::RemoveComponent {
                            key,
                            type_hash: comp.type_hash,
                        });
                    }
                    component_fields_ui(ui, comp, key, schema, actions, active_edit, stack);
                });
        }
    });
}

// Decode one component into typed fields, render an editor per field, and turn edits into a live
// `SetComponent` + a single undo step per gesture. A component the schema can't describe (or whose
// bytes don't fit) is shown honestly as read-only.
fn component_fields_ui(
    ui: &mut egui::Ui,
    comp: &rime_protocol::SnapshotComponent,
    key: EntityKey,
    schema: &Schema,
    actions: &mut Vec<Command>,
    active_edit: &mut Option<ActiveEdit>,
    stack: &mut CommandStack,
) {
    let mut value = match decode_value(schema, comp.type_hash, &comp.data) {
        Ok(v) => v,
        Err(_) => {
            ui.weak(format!(
                "{} bytes · hash 0x{:016x} — no schema/undecodable (read-only)",
                comp.data.len(),
                comp.type_hash
            ));
            return;
        }
    };

    let mut it = Interaction::default();
    if let Value::Struct(fields) = &mut value {
        for (fname, fval) in fields.iter_mut() {
            render_field(ui, fname, fval, &mut it);
        }
    }

    if it.changed {
        // Begin a gesture if none is active for this component (capture the pre-edit bytes so undo is
        // exact); then push a live edit and remember the newest bytes.
        let new_blob = encode_value(&value);
        if active_edit.as_ref().map(|a| (a.key, a.type_hash)) != Some((key, comp.type_hash)) {
            *active_edit = Some(ActiveEdit {
                key,
                type_hash: comp.type_hash,
                old_blob: comp.data.clone(),
                new_blob: new_blob.clone(),
            });
        }
        if let Some(a) = active_edit.as_mut() {
            a.new_blob = new_blob.clone();
        }
        actions.push(Command::SetComponent {
            key,
            type_hash: comp.type_hash,
            blob: new_blob,
        });
    }

    if it.committed {
        if let Some(a) = active_edit.take() {
            // Only record an undo step if this gesture actually changed the bytes (a click that
            // opened and closed a drag with no delta shouldn't litter the history).
            if (a.key, a.type_hash) == (key, comp.type_hash) && a.old_blob != a.new_blob {
                stack.push(Edit {
                    forward: Command::SetComponent {
                        key,
                        type_hash: comp.type_hash,
                        blob: a.new_blob,
                    },
                    inverse: Command::SetComponent {
                        key,
                        type_hash: comp.type_hash,
                        blob: a.old_blob,
                    },
                });
            } else {
                // Not ours / no change — put it back so the real owner can still commit it.
                if (a.key, a.type_hash) != (key, comp.type_hash) {
                    *active_edit = Some(a);
                }
            }
        }
    }
}

/// Whether the widgets for one component saw a change and/or the end of an edit gesture this frame.
#[derive(Default)]
struct Interaction {
    changed: bool,
    committed: bool,
}

fn render_field(ui: &mut egui::Ui, name: &str, value: &mut Value, it: &mut Interaction) {
    match value {
        Value::Struct(children) => {
            egui::CollapsingHeader::new(name)
                .default_open(true)
                .show(ui, |ui| {
                    for (cname, cval) in children.iter_mut() {
                        render_field(ui, cname, cval, it);
                    }
                });
        }
        scalar => {
            ui.horizontal(|ui| {
                ui.label(name);
                render_scalar(ui, scalar, it);
            });
        }
    }
}

fn render_scalar(ui: &mut egui::Ui, value: &mut Value, it: &mut Interaction) {
    // A checkbox is a discrete edit (no drag): its change both starts and ends the gesture at once.
    // A drag-number streams changes; the gesture ends on release / focus loss.
    let resp = match value {
        Value::Bool(b) => {
            let r = ui.checkbox(b, "");
            if r.changed() {
                it.committed = true;
            }
            r
        }
        Value::F32(x) => ui.add(egui::DragValue::new(x).speed(0.01)),
        Value::F64(x) => ui.add(egui::DragValue::new(x).speed(0.01)),
        Value::I32(x) => ui.add(egui::DragValue::new(x).speed(1.0)),
        Value::U32(x) => ui.add(egui::DragValue::new(x).speed(1.0)),
        Value::I64(x) => ui.add(egui::DragValue::new(x).speed(1.0)),
        Value::U64(x) => ui.add(egui::DragValue::new(x).speed(1.0)),
        Value::Struct(_) => return, // handled by render_field
    };
    it.changed |= resp.changed();
    if resp.drag_stopped() || resp.lost_focus() {
        it.committed = true;
    }
}

// A small, UI-side input vocabulary the session thread turns into rime_protocol::InputEvent. Kept
// separate so the UI does not depend on the wire's exact field layout.
pub mod protocol_input {
    // The Pointer* prefix is meaningful (all are pointer events; Key* join later), so the shared
    // prefix is intentional here.
    #[allow(clippy::enum_variant_names)]
    pub enum Input {
        PointerMove { x: i32, y: i32 },
        PointerDown { x: i32, y: i32, button: u32 },
        PointerUp { x: i32, y: i32, button: u32 },
    }
}
