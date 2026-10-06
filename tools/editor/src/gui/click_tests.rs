// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! **Click tests** of the docking shell: the shipped `EditorApp::update`, driven the way a person
//! drives it, with no window, no GPU and no engine process.
//!
//! # The technique
//!
//! egui is immediate-mode: a "frame" is one call of `update` with a `RawInput`, and what comes out
//! is draw data *plus an AccessKit tree* — the accessibility description a screen reader would be
//! given (roles, labels, enabled/toggled state, and the actions each node accepts). `egui_kittest`
//! runs those frames headlessly and lets a test query that tree and answer it: "the Button labelled
//! `▶`, click it". So the tests address widgets by what they SAY, never by where they are, and a
//! layout change cannot break them. That is the whole reason ADR-0031's "a windowed UI is not
//! provable headless" no longer holds for the widget layer.
//!
//! The one place this cannot reach is the viewport panel: it is a bare `allocate_exact_size` with
//! no widget info, so it has **no accessibility node at all** (itself a finding). Those tests send
//! raw pointer events instead — but at positions computed from the dock's own layout rect and from
//! the engine lens, not from hard-coded pixels.
//!
//! # What is faked
//!
//! Exactly one thing: the engine. [`FakeHost`] owns the far end of the two things the app knows the
//! engine by — the `Shared` mirror it reads and the `Outbound` channel it writes (the
//! `EditorApp::with_session` seam). It decodes every message with the real `rime-protocol` codecs
//! and answers with a small in-memory world. It is NOT `rime-engine`: its schema is hand-written to
//! the engine's type names, its "simulation" is one line, and its "save" is a `HashMap`. So these
//! tests prove what the UI *sends* and how it *reacts*; what the real engine does with it is
//! `scripts/editor-click-smoke.sh`'s job, against the real binary.
//!
//! # Tests that document a defect
//!
//! Several tests here pin behaviour that is wrong or missing, and say so in their name and first
//! comment (`..._is_not_undoable`, `view_is_a_dead_label`, …). They pass today. They are a ledger,
//! not an endorsement: when the defect is fixed the test fails, which is the prompt to rewrite it
//! as the positive claim.

use std::collections::HashMap;
use std::sync::mpsc::Receiver;

use egui::accesskit::{Role, Toggled};
use egui_kittest::kittest::Queryable;
use egui_kittest::Harness;
use rime_protocol::{
    ComponentRef, FieldDesc, FieldKind, PickResult, PlayState, SaveResult, SaveScene, SchemaEntry,
    SetComponent, Snapshot, SnapshotComponent, SpawnEntity,
};

use super::protocol_input::Input;
use super::session::ViewportFrame;
use super::*;

// ── The fake engine ─────────────────────────────────────────────────────────────────────────

// Type hashes. Arbitrary — the editor never interprets one, it only matches them against the
// schema — but the NAMES below are the engine's, because the editor does look types up by name
// (`rime::ecs::LocalTransform`, `rime::render::MeshAsset`, `rime::render::Camera`).
const H_VEC3: u64 = 0x01;
const H_QUAT: u64 = 0x02;
const H_LOCAL: u64 = 0x10;
const H_WORLD: u64 = 0x11;
const H_CAMERA: u64 = 0x12;
const H_MESH_ASSET: u64 = 0x13;
const H_LIGHT: u64 = 0x14;
/// A component the schema does not describe (a game's own type opened through the engine's host).
const H_UNKNOWN: u64 = 0xDEAD;

const CAMERA: EntityKey = (0, 0);
const LIGHT: EntityKey = (1, 0);
const CRATE: EntityKey = (2, 0);

const MESH_CRATE_ID: u64 = 0x1001;
const MESH_BARREL_ID: u64 = 0x1002;

fn field(name: &str, kind: FieldKind, nested_hash: u64) -> FieldDesc {
    FieldDesc {
        name: name.to_string(),
        kind,
        nested_hash,
    }
}

fn floats(names: &[&str]) -> Vec<FieldDesc> {
    names.iter().map(|n| field(n, FieldKind::F32, 0)).collect()
}

fn trs_fields() -> Vec<FieldDesc> {
    vec![
        field("translation", FieldKind::Struct, H_VEC3),
        field("rotation", FieldKind::Struct, H_QUAT),
        field("scale", FieldKind::Struct, H_VEC3),
    ]
}

fn fake_schema() -> Schema {
    let ty = |type_hash, name: &str, is_component, fields| SchemaEntry {
        type_hash,
        name: name.to_string(),
        is_component,
        fields,
    };
    Schema {
        types: vec![
            ty(H_VEC3, "rime::core::Vec3", false, floats(&["x", "y", "z"])),
            ty(
                H_QUAT,
                "rime::core::Quat",
                false,
                floats(&["x", "y", "z", "w"]),
            ),
            ty(H_LOCAL, "rime::ecs::LocalTransform", true, trs_fields()),
            ty(H_WORLD, "rime::ecs::WorldTransform", true, trs_fields()),
            ty(
                H_CAMERA,
                "rime::render::Camera",
                true,
                vec![
                    field("fov_y", FieldKind::F32, 0),
                    field("active", FieldKind::Bool, 0),
                ],
            ),
            ty(
                H_MESH_ASSET,
                "rime::render::MeshAsset",
                true,
                vec![field("asset", FieldKind::U64, 0)],
            ),
            ty(
                H_LIGHT,
                "rime::render::PointLight",
                true,
                vec![
                    field("intensity", FieldKind::F32, 0),
                    field("casts_shadow", FieldKind::Bool, 0),
                ],
            ),
        ],
    }
}

fn trs_at(x: f32, y: f32, z: f32) -> Vec<u8> {
    gizmo::encode_trs_blob(&gizmo::Trs {
        translation: gizmo::Vec3::new(x, y, z),
        rotation: gizmo::Quat {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            w: 1.0,
        },
        scale: gizmo::Vec3::new(1.0, 1.0, 1.0),
    })
}

fn comp(type_hash: u64, data: Vec<u8>) -> SnapshotComponent {
    SnapshotComponent { type_hash, data }
}

/// What the engine default-constructs for `AddComponent`.
fn default_blob(type_hash: u64) -> Vec<u8> {
    match type_hash {
        H_LOCAL | H_WORLD => trs_at(0.0, 0.0, 0.0),
        H_CAMERA => encode_value(&Value::Struct(vec![
            ("fov_y".into(), Value::F32(1.0)),
            ("active".into(), Value::Bool(true)),
        ])),
        H_MESH_ASSET => 0u64.to_le_bytes().to_vec(),
        H_LIGHT => light_blob(1.0, false),
        _ => Vec::new(),
    }
}

fn light_blob(intensity: f32, casts_shadow: bool) -> Vec<u8> {
    encode_value(&Value::Struct(vec![
        ("intensity".into(), Value::F32(intensity)),
        ("casts_shadow".into(), Value::Bool(casts_shadow)),
    ]))
}

/// The three-entity world every test starts from: a camera, a light at the origin, a placed mesh.
fn starting_world() -> Vec<SnapshotEntity> {
    let entity = |key: EntityKey, components| SnapshotEntity {
        index: key.0,
        generation: key.1,
        components,
    };
    vec![
        entity(
            CAMERA,
            vec![
                comp(H_LOCAL, trs_at(0.0, 0.0, 10.0)),
                comp(H_CAMERA, default_blob(H_CAMERA)),
            ],
        ),
        entity(
            LIGHT,
            vec![
                comp(H_LOCAL, trs_at(0.0, 0.0, 0.0)),
                comp(H_LIGHT, light_blob(2.0, false)),
            ],
        ),
        entity(
            CRATE,
            vec![
                comp(H_LOCAL, trs_at(3.0, 0.0, 0.0)),
                comp(H_MESH_ASSET, 0xAAu64.to_le_bytes().to_vec()),
            ],
        ),
    ]
}

fn fake_assets() -> Vec<AssetEntry> {
    let asset = |kind, id, source: &str| AssetEntry {
        kind,
        id,
        source_path: source.to_string(),
        cooked_file: format!("{id:016x}.bin"),
    };
    vec![
        asset(AssetKind::Mesh, MESH_CRATE_ID, "meshes/crate.gltf"),
        asset(AssetKind::Mesh, MESH_BARREL_ID, "meshes/barrel.gltf"),
        asset(AssetKind::Texture, 0x2001, "textures/brick.png"),
        asset(AssetKind::Material, 0x3001, "materials/brick.mat"),
    ]
}

/// The engine, as far as `EditorApp` can tell. See the module docs for what this does and does
/// not stand for.
struct FakeHost {
    shared: Shared,
    out_rx: Receiver<Outbound>,
    world: Vec<SnapshotEntity>,
    next_index: u32,
    play: PlayState,
    pre_play: Option<Vec<SnapshotEntity>>,
    /// The scene the session "opened" (what an empty-path save writes back to).
    opened: Option<String>,
    /// "Disk": path → the world that was saved there.
    files: HashMap<String, Vec<SnapshotEntity>>,
    /// What the next `PickRequest` hits.
    pick_answer: PickResult,
    /// Every editor-channel message received, in order.
    log: Vec<(EditorMessage, Vec<u8>)>,
    /// Every forwarded viewport input event, as `"down 10,20 b0"` / `"move 10,20"` / `"up …"`.
    inputs: Vec<String>,
}

impl FakeHost {
    fn entity_mut(&mut self, key: EntityKey) -> Option<&mut SnapshotEntity> {
        self.world
            .iter_mut()
            .find(|e| (e.index, e.generation) == key)
    }

    fn component(&self, key: EntityKey, type_hash: u64) -> Option<&[u8]> {
        self.world
            .iter()
            .find(|e| (e.index, e.generation) == key)?
            .components
            .iter()
            .find(|c| c.type_hash == type_hash)
            .map(|c| c.data.as_slice())
    }

    fn translation(&self, key: EntityKey) -> [f32; 3] {
        let t = gizmo::decode_trs_blob(self.component(key, H_LOCAL).expect("LocalTransform"))
            .expect("trs")
            .translation;
        [t.x, t.y, t.z]
    }

    fn publish_snapshot(&self) {
        self.shared.lock().unwrap().snapshot = Snapshot {
            entities: self.world.clone(),
        };
    }

    fn spawn(&mut self, mut components: Vec<SnapshotComponent>) {
        // m15.3: the host gives every spawned entity a LocalTransform it does not already carry.
        if !components.iter().any(|c| c.type_hash == H_LOCAL) {
            components.insert(0, comp(H_LOCAL, default_blob(H_LOCAL)));
        }
        self.world.push(SnapshotEntity {
            index: self.next_index,
            generation: 0,
            components,
        });
        self.next_index += 1;
    }

    /// One fixed tick of the "simulation": the crate falls a metre. Enough to make the played world
    /// differ from the edited one, which is all Stop's restore needs to be visible.
    fn tick(&mut self) {
        self.play.tick_count += 1;
        let [x, y, z] = self.translation(CRATE);
        if let Some(e) = self.entity_mut(CRATE) {
            e.components[0].data = trs_at(x, y - 1.0, z);
        }
    }

    fn begin_play_if_editing(&mut self) {
        if self.play.phase == PlayPhase::Edit {
            self.pre_play = Some(self.world.clone());
        }
    }

    /// Drain everything the UI sent since the last pump, apply it, and answer.
    fn pump(&mut self) {
        while let Ok(out) = self.out_rx.try_recv() {
            match out {
                Outbound::Input(input) => self.inputs.push(match input {
                    Input::PointerMove { x, y } => format!("move {x},{y}"),
                    Input::PointerDown { x, y, button } => format!("down {x},{y} b{button}"),
                    Input::PointerUp { x, y, button } => format!("up {x},{y} b{button}"),
                }),
                Outbound::Editor { msg, payload } => {
                    self.apply(msg, &payload);
                    self.log.push((msg, payload));
                }
            }
        }
        self.shared.lock().unwrap().play_state = self.play;
    }

    fn apply(&mut self, msg: EditorMessage, payload: &[u8]) {
        match msg {
            EditorMessage::SetComponent => {
                let set = SetComponent::decode(payload).expect("SetComponent");
                if let Some(e) = self.entity_mut((set.index, set.generation)) {
                    match e
                        .components
                        .iter_mut()
                        .find(|c| c.type_hash == set.type_hash)
                    {
                        Some(c) => c.data = set.blob,
                        None => e.components.push(comp(set.type_hash, set.blob)),
                    }
                }
            }
            EditorMessage::AddComponent => {
                let r = ComponentRef::decode(payload).expect("ComponentRef");
                if let Some(e) = self.entity_mut((r.index, r.generation)) {
                    e.components
                        .push(comp(r.type_hash, default_blob(r.type_hash)));
                }
            }
            EditorMessage::RemoveComponent => {
                let r = ComponentRef::decode(payload).expect("ComponentRef");
                if let Some(e) = self.entity_mut((r.index, r.generation)) {
                    e.components.retain(|c| c.type_hash != r.type_hash);
                }
            }
            EditorMessage::Spawn => self.spawn(Vec::new()),
            EditorMessage::SpawnEntity => {
                let spawn = SpawnEntity::decode(payload).expect("SpawnEntity");
                self.spawn(
                    spawn
                        .components
                        .into_iter()
                        .map(|(h, d)| comp(h, d))
                        .collect(),
                );
            }
            EditorMessage::Despawn => {
                // `[index:u32][generation:u32]` — the same bytes `encode_despawn` writes.
                let index = u32::from_le_bytes(payload[0..4].try_into().unwrap());
                let generation = u32::from_le_bytes(payload[4..8].try_into().unwrap());
                self.world
                    .retain(|e| (e.index, e.generation) != (index, generation));
            }
            EditorMessage::RequestSnapshot => self.publish_snapshot(),
            EditorMessage::PickRequest => {
                self.shared.lock().unwrap().last_pick = Some(self.pick_answer);
            }
            EditorMessage::Play => {
                self.begin_play_if_editing();
                self.play.phase = PlayPhase::Playing;
            }
            EditorMessage::Pause => self.play.phase = PlayPhase::Paused,
            EditorMessage::Step => {
                self.begin_play_if_editing();
                self.play.phase = PlayPhase::Paused;
                self.tick();
            }
            EditorMessage::Stop => {
                if let Some(world) = self.pre_play.take() {
                    self.world = world;
                }
                self.play.phase = PlayPhase::Edit;
            }
            EditorMessage::SaveScene => {
                let path = SaveScene::decode(payload).expect("SaveScene").path;
                let target = if path.is_empty() {
                    self.opened.clone()
                } else {
                    Some(path)
                };
                let result = match target {
                    None => SaveResult {
                        error: "no scene path to save to".to_string(),
                        ..SaveResult::default()
                    },
                    // Stands in for every way the engine can decline (unwritable path, or the
                    // m14.3 veto on a world whose load dropped unregistered components).
                    Some(p) if p.contains("refused") => SaveResult {
                        path: p,
                        error: "refused: the load skipped 213 unregistered components".to_string(),
                        ..SaveResult::default()
                    },
                    Some(p) => {
                        self.files.insert(p.clone(), self.world.clone());
                        SaveResult {
                            ok: true,
                            entities: self.world.len() as u64,
                            bytes: 0,
                            path: p,
                            error: String::new(),
                        }
                    }
                };
                self.shared.lock().unwrap().last_save = Some(result);
            }
            // GizmoState is engine render state; logged, nothing to simulate.
            _ => {}
        }
    }

    fn count(&self, msg: EditorMessage) -> usize {
        self.log.iter().filter(|(m, _)| *m == msg).count()
    }

    fn last(&self, msg: EditorMessage) -> Option<&[u8]> {
        self.log
            .iter()
            .rev()
            .find(|(m, _)| *m == msg)
            .map(|(_, p)| p.as_slice())
    }

    fn messages(&self) -> Vec<EditorMessage> {
        self.log.iter().map(|(m, _)| *m).collect()
    }

    /// The world edits and commands only — without the `GizmoState` render hints and the
    /// `RequestSnapshot` resyncs — for tests that assert "this and nothing else was sent".
    fn commands(&self) -> Vec<EditorMessage> {
        self.messages()
            .into_iter()
            .filter(|m| !matches!(m, EditorMessage::GizmoState))
            .collect()
    }
}

// ── The lens ────────────────────────────────────────────────────────────────────────────────

const EXTENT: (f32, f32) = (960.0, 540.0);
const EYE_Z: f32 = 10.0;
/// cot(fov_y/2). With the camera looking straight down -z from `EYE_Z`, a point on the z=0 plane
/// lands at ndc_x = (F / aspect) · x / EYE_Z, so one render pixel of cursor travel along the X
/// handle is exactly `WORLD_PER_PX` world units — which is what lets the gizmo-drag test state its
/// expected distance from arithmetic instead of from the code under test.
const F: f32 = 2.0;
const WORLD_PER_PX: f32 = (2.0 / EXTENT.0) * EYE_Z * (EXTENT.0 / EXTENT.1) / F;

/// A perspective lens at (0, 0, EYE_Z) looking down -z, with its exact inverse written out by hand
/// (the engine ships both; the editor never inverts a matrix). Column-major, Vulkan clip space
/// (y down) — the same conventions as the C++-emitted `viewport_camera.bin` fixture.
fn lens() -> ViewportCamera {
    let a = F / (EXTENT.0 / EXTENT.1);
    let b = -F;
    let (near, far) = (0.1f32, 100.0f32);
    let c = far / (near - far);
    let d = near * far / (near - far);
    // P = [a 0 0 0; 0 b 0 0; 0 0 c d; 0 0 -1 0], view = translate(0, 0, -EYE_Z).
    #[rustfmt::skip]
    let view_proj = [
        a,   0.0, 0.0, 0.0,
        0.0, b,   0.0, 0.0,
        0.0, 0.0, c,   -1.0,
        0.0, 0.0, d - c * EYE_Z, EYE_Z,
    ];
    // P⁻¹ = [1/a 0 0 0; 0 1/b 0 0; 0 0 0 -1; 0 0 1/d c/d], then translate(0, 0, +EYE_Z) in front.
    #[rustfmt::skip]
    let inv_view_proj = [
        1.0 / a, 0.0,     0.0,                 0.0,
        0.0,     1.0 / b, 0.0,                 0.0,
        0.0,     0.0,     EYE_Z / d,           1.0 / d,
        0.0,     0.0,     -1.0 + EYE_Z * c / d, c / d,
    ];
    ViewportCamera {
        view_proj,
        inv_view_proj,
        eye: [0.0, 0.0, EYE_Z],
        width: EXTENT.0 as u32,
        height: EXTENT.1 as u32,
    }
}

// ── The rig ─────────────────────────────────────────────────────────────────────────────────

struct Rig {
    harness: Harness<'static, EditorApp>,
    host: FakeHost,
}

impl Rig {
    /// A connected editor over the starting world, with no scene opened.
    fn new() -> Self {
        Self::build(None, starting_world())
    }

    /// As [`Rig::new`], but launched with `--scene <path>`.
    fn with_scene(path: &str) -> Self {
        Self::build(Some(path), starting_world())
    }

    fn build(scene: Option<&str>, world: Vec<SnapshotEntity>) -> Self {
        let shared: Shared = Arc::new(Mutex::new(SharedState::default()));
        let (out_tx, out_rx) = mpsc::channel();
        {
            let mut s = shared.lock().unwrap();
            s.connected = true;
            s.schema = fake_schema();
            s.assets = fake_assets();
        }
        let next_index = world.iter().map(|e| e.index + 1).max().unwrap_or(0);
        let host = FakeHost {
            shared: Arc::clone(&shared),
            out_rx,
            world,
            next_index,
            play: PlayState::default(),
            pre_play: None,
            opened: scene.map(str::to_string),
            files: HashMap::new(),
            pick_answer: PickResult::none(),
            log: Vec::new(),
            inputs: Vec::new(),
        };
        host.publish_snapshot();
        let scene = scene.map(str::to_string);
        let harness = Harness::builder()
            .with_size(egui::vec2(1280.0, 800.0))
            // A real display's frame time. kittest's default step is a quarter of a second, at
            // which a press held across one `settle` outlasts egui's 0.8 s click window and every
            // viewport click would be read as a long-press drag.
            .with_step_dt(1.0 / 60.0)
            .build_eframe(move |_cc| {
                EditorApp::with_session(shared, out_tx, EngineSession::detached(), scene)
            });
        let mut rig = Rig { harness, host };
        rig.settle();
        // The first frame announces "no gizmo"; start every test's log after that hello.
        rig.host.log.clear();
        rig
    }

    /// Run frames until the UI and the fake engine have finished talking: each round is one UI
    /// frame (per queued event) followed by the engine consuming what it sent. The app repaints
    /// continuously by design, so `Harness::run` (which waits for it to go idle) never returns —
    /// a fixed, generous number of rounds is the deterministic equivalent.
    fn settle(&mut self) {
        for _ in 0..4 {
            self.harness.step();
            self.host.pump();
        }
        self.harness.step();
    }

    fn app(&self) -> &EditorApp {
        self.harness.state()
    }

    /// Click the button with exactly this label, then settle. "Button" is the role of everything
    /// clickable-by-name here — push buttons, menu titles and items, selectable rows — and asking
    /// for it keeps `Edit` the menu apart from `Edit` the play-phase readout.
    fn click(&mut self, label: &str) {
        self.harness
            .get_by_role_and_label(Role::Button, label)
            .click();
        self.settle();
    }

    /// Whether anything on screen — a button, a label, a header — reads exactly this.
    fn has(&self, label: &str) -> bool {
        self.harness.query_all_by_label(label).next().is_some()
    }

    /// Whether a piece of static text (not a button) reads exactly this.
    fn has_text(&self, text: &str) -> bool {
        self.harness
            .query_all_by_role_and_label(Role::Label, text)
            .next()
            .is_some()
    }

    fn enabled(&self, label: &str) -> bool {
        !self
            .harness
            .get_by_role_and_label(Role::Button, label)
            .is_disabled()
    }

    /// The pressed state of a toggle: a selectable row/mode button, or a labelled checkbox.
    fn toggled(&self, label: &str) -> bool {
        let node = self
            .harness
            .query_by_role_and_label(Role::CheckBox, label)
            .unwrap_or_else(|| self.harness.get_by_role_and_label(Role::Button, label));
        node.toggled() == Some(Toggled::True)
    }

    fn click_checkbox(&mut self, label: &str) {
        self.harness
            .get_by_role_and_label(Role::CheckBox, label)
            .click();
        self.settle();
    }

    /// The labels of every button on screen, in tree order.
    fn button_labels(&self) -> Vec<String> {
        self.harness
            .get_all_by_role(Role::Button)
            .filter_map(|n| n.label())
            .collect()
    }

    /// Open File and type into its Save As path box. The asset browser's search box is a text
    /// input too, so the path box is identified as the one that was not there before the menu
    /// opened.
    fn open_file_menu_and_type_path(&mut self, path: &str) {
        let before: Vec<_> = self
            .harness
            .get_all_by_role(Role::TextInput)
            .map(|n| n.id())
            .collect();
        self.click("File");
        self.harness
            .get_all_by_role(Role::TextInput)
            .find(|n| !before.contains(&n.id()))
            .expect("the Save As path box")
            .type_text(path);
        self.settle();
    }

    /// Hold a key down (no release) — what the fly camera reads, as opposed to a tap.
    fn hold_key(&mut self, key: egui::Key) {
        self.harness.input_mut().events.push(egui::Event::Key {
            key,
            physical_key: None,
            pressed: true,
            repeat: false,
            modifiers: egui::Modifiers::NONE,
        });
        self.settle();
    }

    /// Whether a menu's item is enabled — opens the menu to look, as a user must.
    fn menu_item_enabled(&mut self, menu: &str, item: &str) -> bool {
        self.click(menu);
        let enabled = self.enabled(item);
        self.key(egui::Key::Escape);
        enabled
    }

    fn key(&mut self, key: egui::Key) {
        self.harness.press_key(key);
        self.settle();
    }

    fn chord(&mut self, modifiers: egui::Modifiers, key: egui::Key) {
        self.harness.press_key_modifiers(modifiers, key);
        self.settle();
    }

    /// The inspector's numeric fields, top to bottom.
    fn click_number_field(&mut self, nth: usize) {
        let fields: Vec<_> = self.harness.get_all_by_role(Role::SpinButton).collect();
        fields[nth].click();
        self.settle();
    }

    /// Edit a numeric inspector field the way a person types into it: click it (it becomes a text
    /// box with its contents selected), type the replacement, press Enter.
    fn type_into_number_field(&mut self, nth: usize, text: &str) {
        self.click_number_field(nth);
        self.harness
            .input_mut()
            .events
            .push(egui::Event::Text(text.to_string()));
        self.settle();
        self.key(egui::Key::Enter);
    }

    // ── Viewport: raw pointer input (it has no accessibility node to address) ──────────────

    /// Give the viewport something to show: one streamed frame and its lens.
    fn present_frame(&mut self) {
        {
            let mut s = self.host.shared.lock().unwrap();
            s.frames_received += 1;
            let seq = s.frames_received;
            s.frame = Some(ViewportFrame {
                width: 64,
                height: 36,
                rgba: vec![40; 64 * 36 * 4],
                seq,
            });
            s.camera = Some(lens());
        }
        self.settle();
    }

    /// The viewport panel's body rect, read from the dock's own layout.
    fn viewport_rect(&self) -> egui::Rect {
        self.app()
            .dock
            .main_surface()
            .iter()
            .find_map(|node| match node {
                egui_dock::Node::Leaf { tabs, viewport, .. } if tabs.contains(&Tab::Viewport) => {
                    Some(*viewport)
                }
                _ => None,
            })
            .expect("the layout has a Viewport panel")
            // The panel's `ui` is the tab body minus the dock style's inner margin — the same
            // style `update` builds. (`a_viewport_click_picks…` asserts this mapping end to end:
            // a click aimed at render pixel (480, 270) must arrive as exactly that.)
            - Style::from_egui(self.harness.ctx.style().as_ref())
                .tab
                .tab_body
                .inner_margin
    }

    /// Where a render-extent pixel falls on screen.
    fn screen(&self, px: (f32, f32)) -> egui::Pos2 {
        let r = self.viewport_rect();
        egui::pos2(
            r.left() + px.0 / EXTENT.0 * r.width(),
            r.top() + px.1 / EXTENT.1 * r.height(),
        )
    }

    fn pointer_move(&mut self, px: (f32, f32)) {
        let pos = self.screen(px);
        self.harness
            .input_mut()
            .events
            .push(egui::Event::PointerMoved(pos));
        self.settle();
    }

    fn pointer_button(&mut self, px: (f32, f32), button: egui::PointerButton, pressed: bool) {
        let pos = self.screen(px);
        self.harness
            .input_mut()
            .events
            .push(egui::Event::PointerButton {
                pos,
                button,
                pressed,
                modifiers: egui::Modifiers::NONE,
            });
        self.settle();
    }

    fn viewport_click(&mut self, px: (f32, f32)) {
        self.pointer_move(px);
        self.pointer_button(px, egui::PointerButton::Primary, true);
        self.pointer_button(px, egui::PointerButton::Primary, false);
    }

    fn drag(&mut self, button: egui::PointerButton, path: &[(f32, f32)]) {
        self.pointer_move(path[0]);
        self.pointer_button(path[0], button, true);
        for &p in &path[1..] {
            self.pointer_move(p);
        }
        self.pointer_button(*path.last().unwrap(), button, false);
    }

    /// The pixel a fraction of the way out along the selected entity's X handle.
    fn x_handle_px(&self, center: gizmo::Vec3, fraction: f32) -> (f32, f32) {
        let cam = lens();
        let eye = gizmo::Vec3::new(cam.eye[0], cam.eye[1], cam.eye[2]);
        let size = gizmo::gizmo_world_size(&cam.view_proj, eye, center);
        let on_handle = center.add(gizmo::Axis::X.dir().scale(size * fraction));
        gizmo::project_point(&cam.view_proj, EXTENT, on_handle).expect("in front of the lens")
    }
}

const CTRL: egui::Modifiers = egui::Modifiers::COMMAND;
const CTRL_SHIFT: egui::Modifiers = egui::Modifiers {
    alt: false,
    ctrl: false,
    shift: true,
    mac_cmd: false,
    command: true,
};

const SAVE: &str = "Save\tCtrl+S";
const UNDO: &str = "Undo\tCtrl+Z";
const REDO: &str = "Redo\tCtrl+Y";
const ROW_CAMERA: &str = "0  Camera";
const ROW_LIGHT: &str = "1  PointLight";
const ROW_CRATE: &str = "2  MeshAsset";

fn set_of(payload: &[u8]) -> SetComponent {
    SetComponent::decode(payload).expect("SetComponent")
}

// ── Status bar ──────────────────────────────────────────────────────────────────────────────

#[test]
fn status_bar_reports_connection_and_entity_count() {
    let rig = Rig::new();
    assert!(rig.has("connected"));
    assert!(rig.has("3 entities"));
    assert!(rig.has("0 frames · 0 fps"));
}

#[test]
fn status_bar_says_connecting_until_the_engine_answers() {
    let mut rig = Rig::new();
    rig.host.shared.lock().unwrap().connected = false;
    rig.settle();
    assert!(rig.has("connecting…"));
    assert!(!rig.has("connected"));
}

#[test]
fn status_bar_shows_a_session_error_verbatim() {
    // What a missing/unspawnable engine binary looks like to the user.
    let mut rig = Rig::new();
    rig.host.shared.lock().unwrap().error = Some("spawn 'nope': No such file".to_string());
    rig.settle();
    assert!(rig.has("error: spawn 'nope': No such file"));
}

// ── File menu: Save / Save As ───────────────────────────────────────────────────────────────

#[test]
fn file_menu_save_is_disabled_when_no_scene_was_opened() {
    let mut rig = Rig::new();
    rig.click("File");
    assert!(!rig.enabled(SAVE));
}

#[test]
fn file_menu_save_writes_back_to_the_opened_scene() {
    let mut rig = Rig::with_scene("/proj/level.rscene");
    rig.click("File");
    assert!(rig.enabled(SAVE));
    rig.click(SAVE);

    // An EMPTY path on the wire: "where you opened it" is the engine's decision.
    assert_eq!(rig.host.commands(), [EditorMessage::SaveScene]);
    let sent = SaveScene::decode(rig.host.last(EditorMessage::SaveScene).unwrap()).unwrap();
    assert_eq!(sent.path, "");
    assert!(rig.host.files.contains_key("/proj/level.rscene"));
    assert!(rig.has("saved 3 entities to /proj/level.rscene"));
    // Saving is not an edit: nothing to undo.
    assert!(!rig.app().stack.can_undo());
}

#[test]
fn ctrl_s_saves_the_opened_scene() {
    let mut rig = Rig::with_scene("/proj/level.rscene");
    rig.chord(CTRL, egui::Key::S);
    assert_eq!(rig.host.commands(), [EditorMessage::SaveScene]);
    assert!(rig.has("saved 3 entities to /proj/level.rscene"));
}

#[test]
fn ctrl_s_over_the_viewport_also_flies_the_camera_backwards() {
    // DOCUMENTS A DEFECT: the fly camera reads S as "back" whenever the pointer is over the
    // viewport and never looks at the modifiers. So the save chord, pressed where the pointer
    // usually is, also nudges the camera. The save itself goes out first and records the scene as
    // it was — which means the world is already different from the file the instant "saved"
    // appears, and the NEXT save writes a camera the user never moved. (The Xvfb smoke parks the
    // pointer on the status bar before every key for exactly this reason.)
    let mut rig = Rig::with_scene("/proj/level.rscene");
    rig.present_frame();
    rig.pointer_move((480.5, 270.5));
    rig.chord(CTRL, egui::Key::S);
    assert_eq!(rig.host.count(EditorMessage::SaveScene), 1);
    let saved_camera = &rig.host.files["/proj/level.rscene"][0].components[0].data;
    assert_eq!(*saved_camera, trs_at(0.0, 0.0, EYE_Z), "saved as it was");
    assert!(rig.host.translation(CAMERA)[2] > EYE_Z, "then it moved");
}

#[test]
fn ctrl_s_without_a_scene_does_nothing_and_says_nothing() {
    // DOCUMENTS A DEFECT (minor): the reflex key is silently swallowed when the session was started
    // without --scene. No message is sent and no hint appears; the user has to discover Save As.
    let mut rig = Rig::new();
    rig.chord(CTRL, egui::Key::S);
    assert!(rig.host.commands().is_empty());
    assert!(rig.app().save_status.is_none());
}

#[test]
fn save_as_write_is_disabled_until_a_path_is_typed() {
    let mut rig = Rig::new();
    rig.click("File");
    assert!(!rig.enabled("Write"));
    rig.key(egui::Key::Escape);
    rig.open_file_menu_and_type_path("/out/a.rscene");
    assert!(rig.enabled("Write"));
}

#[test]
fn save_as_writes_the_typed_path_and_a_reload_shows_the_edit() {
    let mut rig = Rig::new();
    // An edit worth saving: the light's x becomes 5.
    rig.click(ROW_LIGHT);
    rig.type_into_number_field(0, "5");
    assert_eq!(rig.host.translation(LIGHT), [5.0, 0.0, 0.0]);

    rig.open_file_menu_and_type_path("/out/edited.rscene");
    rig.click("Write");

    let sent = SaveScene::decode(rig.host.last(EditorMessage::SaveScene).unwrap()).unwrap();
    assert_eq!(sent.path, "/out/edited.rscene");
    assert!(rig.has("saved 3 entities to /out/edited.rscene"));
    // Save As adopts the path: plain Save is now available and Ctrl+S goes there.
    assert_eq!(rig.app().scene_path.as_deref(), Some("/out/edited.rscene"));

    // "Reload": a fresh editor over the saved world (the fake's disk) shows the edited value. This
    // round-trips the FAKE's storage — the real file format is the Xvfb smoke's assertion.
    let saved = rig.host.files["/out/edited.rscene"].clone();
    let mut reopened = Rig::build(Some("/out/edited.rscene"), saved);
    reopened.click(ROW_LIGHT);
    assert_eq!(reopened.host.translation(LIGHT), [5.0, 0.0, 0.0]);
    let x = reopened
        .harness
        .get_all_by_role(Role::SpinButton)
        .next()
        .unwrap();
    assert_eq!(x.numeric_value(), Some(5.0));
}

#[test]
fn a_refused_save_is_shown_with_the_engines_reason() {
    let mut rig = Rig::new();
    rig.open_file_menu_and_type_path("/refused/x.rscene");
    rig.click("Write");
    assert!(rig.has("refused: the load skipped 213 unregistered components"));
    assert_eq!(rig.app().save_status.as_ref().map(|s| s.1), Some(false));
    assert!(rig.host.files.is_empty());
}

#[test]
fn a_refused_save_as_still_becomes_the_sessions_scene() {
    // DOCUMENTS A DEFECT (minor): Save As adopts its path BEFORE the engine answers, so after a
    // refusal the editor believes a scene is open at a path nothing was written to. `Save` turns
    // on; what it sends is an empty path, which the engine resolves against what IT opened — here
    // nothing — so the user gets a second, differently worded failure.
    let mut rig = Rig::new();
    rig.open_file_menu_and_type_path("/refused/x.rscene");
    rig.click("Write");
    assert_eq!(rig.app().scene_path.as_deref(), Some("/refused/x.rscene"));
    rig.click("File");
    assert!(rig.enabled(SAVE));
    rig.click(SAVE);
    assert!(rig.has("no scene path to save to"));
}

#[test]
fn file_menu_has_no_new_or_open() {
    // DOCUMENTS A GAP: the File menu is Save + Save As and nothing else. A scene can only be
    // chosen on the command line (`--scene`); there is no New, no Open, no Recent, no Quit, and
    // closing the window never asks about unsaved edits (there is no dirty flag to ask with).
    let mut rig = Rig::new();
    rig.click("File");
    let buttons = rig.button_labels();
    for absent in ["New", "Open", "Quit", "Exit", "Revert"] {
        assert!(
            !buttons.iter().any(|b| b.contains(absent)),
            "found a {absent} entry: {buttons:?}"
        );
    }
    assert!(buttons.iter().any(|b| b == SAVE));
    assert!(buttons.iter().any(|b| b == "Write"));
}

// ── View ────────────────────────────────────────────────────────────────────────────────────

#[test]
fn view_menu_lists_every_panel_with_its_state() {
    // The View menu is a real menu with one checkmark per dock panel, all ticked at the default
    // layout.
    let mut rig = Rig::new();
    assert_eq!(rig.harness.get_by_label("View").role(), Role::Button);
    rig.click("View");
    for tab in Tab::ALL {
        assert!(
            rig.toggled(tab.label()),
            "{} starts open, so it is ticked",
            tab.label()
        );
    }
    rig.key(egui::Key::Escape);
}

/// Middle-click a panel's tab until the panel is gone — the way a person closes one. Tab titles are
/// not in the accessibility tree, so this finds the title the blunt way: along the tab bar.
fn close_panel(rig: &mut Rig, tab: Tab) {
    let has_tab = |rig: &Rig| {
        rig.app()
            .dock
            .main_surface()
            .iter()
            .any(|n| matches!(n, egui_dock::Node::Leaf { tabs, .. } if tabs.contains(&tab)))
    };
    let bar = rig
        .app()
        .dock
        .main_surface()
        .iter()
        .find_map(|node| match node {
            egui_dock::Node::Leaf {
                tabs,
                rect,
                viewport,
                ..
            } if tabs.contains(&tab) => Some(egui::Rect::from_min_max(
                rect.min,
                egui::pos2(rect.right(), viewport.top()),
            )),
            _ => None,
        })
        .expect("the layout has the panel");
    assert!(has_tab(rig));
    let mut x = bar.left() + 2.0;
    while has_tab(rig) && x < bar.right() {
        let pos = egui::pos2(x, bar.center().y);
        for event in [
            egui::Event::PointerMoved(pos),
            egui::Event::PointerButton {
                pos,
                button: egui::PointerButton::Middle,
                pressed: true,
                modifiers: egui::Modifiers::NONE,
            },
            egui::Event::PointerButton {
                pos,
                button: egui::PointerButton::Middle,
                pressed: false,
                modifiers: egui::Modifiers::NONE,
            },
        ] {
            rig.harness.input_mut().events.push(event);
            rig.harness.step();
        }
        x += 3.0;
    }
    assert!(!has_tab(rig), "a middle-click on the tab closed it");
    rig.settle();
}

/// The screen rect of the leaf holding an open panel, from the dock's own layout.
fn panel_rect(rig: &Rig, tab: Tab) -> egui::Rect {
    rig.app()
        .dock
        .main_surface()
        .iter()
        .find_map(|node| match node {
            egui_dock::Node::Leaf { tabs, viewport, .. } if tabs.contains(&tab) => Some(*viewport),
            _ => None,
        })
        .expect("the panel is open")
}

// ── Docking ─────────────────────────────────────────────────────────────────────────────────

#[test]
fn a_closed_panel_reopens_from_the_view_menu() {
    // A closed panel comes back from View, in the place the default layout gives it: the assets
    // browser sits below the outliner, on the left. Its checkmark follows the panel.
    let mut rig = Rig::new();
    close_panel(&mut rig, Tab::Assets);
    rig.click("View");
    assert!(!rig.toggled("Assets"), "closed, so unticked");
    rig.harness
        .get_by_role_and_label(Role::CheckBox, "Assets")
        .click();
    rig.settle();

    let assets = panel_rect(&rig, Tab::Assets);
    let outliner = panel_rect(&rig, Tab::Outliner);
    assert_eq!(assets.left(), outliner.left(), "down the outliner's column");
    assert!(assets.top() > outliner.top(), "below the outliner");
    rig.click("View");
    assert!(rig.toggled("Assets"), "open again, so ticked");
    rig.key(egui::Key::Escape);
}

#[test]
fn an_open_panel_is_not_closed_by_its_menu_item() {
    // Clicking the checkmark of an open panel changes nothing: closing is a tab gesture.
    let mut rig = Rig::new();
    rig.click("View");
    rig.harness
        .get_by_role_and_label(Role::CheckBox, "Inspector")
        .click();
    rig.settle();
    assert!(rig.app().dock.find_tab(&Tab::Inspector).is_some());
}

// ── Outliner ────────────────────────────────────────────────────────────────────────────────

#[test]
fn outliner_lists_entities_by_their_distinctive_component() {
    let rig = Rig::new();
    for row in [ROW_CAMERA, ROW_LIGHT, ROW_CRATE] {
        assert!(rig.has(row), "missing outliner row {row:?}");
    }
}

#[test]
fn outliner_click_selects_and_the_inspector_follows() {
    let mut rig = Rig::new();
    assert!(rig.has("select an entity in the Outliner"));
    rig.click(ROW_LIGHT);
    assert_eq!(rig.app().selected, Some(1));
    assert!(rig.toggled(ROW_LIGHT));
    assert!(rig.has("entity 1"));
    assert!(rig.has("handle 1:0"));
    assert!(rig.has("rime::render::PointLight"));
    // Selecting is not an edit: nothing goes to the engine but the (hidden) gizmo state.
    assert!(rig.host.commands().is_empty());
}

#[test]
fn spawn_creates_an_entity_and_clears_the_selection() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.click("+ spawn");
    assert_eq!(
        rig.host.commands(),
        [EditorMessage::Spawn, EditorMessage::RequestSnapshot]
    );
    assert!(rig.has("3  entity"));
    assert!(rig.has("4 entities"));
    // The new entity is NOT selected — the user must find and click it to do anything with it.
    assert_eq!(rig.app().selected, None);
}

#[test]
fn spawn_is_not_undoable() {
    // DOCUMENTS A LIMITATION (commands.rs: "a considered v1 limitation"): creating an entity leaves
    // no history, so Ctrl+Z after a mis-click does nothing and Edit ▸ Undo stays greyed out.
    let mut rig = Rig::new();
    rig.click("+ spawn");
    assert!(!rig.menu_item_enabled("Edit", UNDO));
    rig.chord(CTRL, egui::Key::Z);
    assert_eq!(rig.host.world.len(), 4, "the spawned entity is still there");
}

// ── Inspector: entity ───────────────────────────────────────────────────────────────────────

#[test]
fn despawn_removes_the_selected_entity() {
    let mut rig = Rig::new();
    rig.click(ROW_CRATE);
    rig.click("✖ despawn");
    assert_eq!(
        rig.host.commands(),
        [EditorMessage::Despawn, EditorMessage::RequestSnapshot]
    );
    assert_eq!(
        rig.host.last(EditorMessage::Despawn).unwrap(),
        rime_protocol::encode_despawn(CRATE.0, CRATE.1)
    );
    assert!(!rig.has(ROW_CRATE));
    assert!(rig.has("2 entities"));
}

#[test]
fn despawn_is_not_undoable() {
    // DOCUMENTS A LIMITATION, and the one that costs work: a despawn is immediate, unconfirmed and
    // permanent. The entity and every component value on it are gone; Undo is greyed out.
    let mut rig = Rig::new();
    rig.click(ROW_CRATE);
    rig.click("✖ despawn");
    assert!(!rig.menu_item_enabled("Edit", UNDO));
    rig.chord(CTRL, egui::Key::Z);
    assert_eq!(rig.host.world.len(), 2, "nothing brought it back");
}

#[test]
fn despawn_slides_the_selection_onto_the_next_entity() {
    // DOCUMENTS A DEFECT: the selection is a ROW INDEX, not an entity. Despawning row 1 leaves
    // `selected == Some(1)`, and row 1 is now the entity that used to be row 2 — so the inspector
    // silently shows a different object under the same "✖ despawn" button. A second click (a
    // double-click, or "did that work?") deletes an entity the user never selected, with no undo.
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.click("✖ despawn");
    assert_eq!(rig.app().selected, Some(1));
    assert!(rig.has("handle 2:0"), "the inspector now shows the crate");

    rig.click("✖ despawn");
    assert_eq!(rig.host.world.len(), 1, "the crate went too");
    assert_eq!(
        (rig.host.world[0].index, rig.host.world[0].generation),
        CAMERA
    );
    // …and with nothing left at row 1 the inspector finally admits it.
    assert!(rig.has("(stale selection)"));
}

// ── Inspector: field edits + undo/redo ──────────────────────────────────────────────────────

#[test]
fn typing_into_a_number_field_edits_the_component() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.type_into_number_field(1, "2.5"); // translation.y
    assert_eq!(rig.host.translation(LIGHT), [0.0, 2.5, 0.0]);
    let last = set_of(rig.host.last(EditorMessage::SetComponent).unwrap());
    assert_eq!(
        (last.index, last.generation, last.type_hash),
        (1, 0, H_LOCAL)
    );
    assert_eq!(last.blob, trs_at(0.0, 2.5, 0.0));
}

#[test]
fn a_typed_edit_is_one_undo_step_however_many_keystrokes_it_took() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.type_into_number_field(0, "12"); // two keystrokes' worth of live SetComponents…
    assert_eq!(rig.host.translation(LIGHT), [12.0, 0.0, 0.0]);
    rig.chord(CTRL, egui::Key::Z); // …one undo
    assert_eq!(rig.host.translation(LIGHT), [0.0, 0.0, 0.0]);
    assert!(!rig.app().stack.can_undo());
}

#[test]
fn toggling_a_checkbox_field_edits_and_is_undoable() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    let before = light_blob(2.0, false);
    assert_eq!(rig.host.component(LIGHT, H_LIGHT), Some(before.as_slice()));
    // The inspector's bool fields are unlabelled checkboxes; "Snap" is the toolbar's.
    rig.harness
        .get_all_by_role(Role::CheckBox)
        .find(|n| n.label().as_deref() != Some("Snap"))
        .expect("the casts_shadow checkbox")
        .click();
    rig.settle();
    assert_eq!(
        rig.host.component(LIGHT, H_LIGHT),
        Some(light_blob(2.0, true).as_slice())
    );
    rig.chord(CTRL, egui::Key::Z);
    assert_eq!(rig.host.component(LIGHT, H_LIGHT), Some(before.as_slice()));
}

#[test]
fn edit_menu_undo_and_redo_restore_exact_bytes() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    let before = rig.host.component(LIGHT, H_LOCAL).unwrap().to_vec();
    rig.type_into_number_field(0, "5");
    let after = rig.host.component(LIGHT, H_LOCAL).unwrap().to_vec();
    assert_ne!(before, after);

    rig.click("Edit");
    rig.click(UNDO);
    assert_eq!(rig.host.component(LIGHT, H_LOCAL).unwrap(), before);

    rig.click("Edit");
    rig.click(REDO);
    assert_eq!(rig.host.component(LIGHT, H_LOCAL).unwrap(), after);
}

#[test]
fn edit_menu_items_are_enabled_only_when_there_is_history() {
    let mut rig = Rig::new();
    assert!(!rig.menu_item_enabled("Edit", UNDO));
    assert!(!rig.menu_item_enabled("Edit", REDO));
    rig.click(ROW_LIGHT);
    rig.type_into_number_field(0, "5");
    assert!(rig.menu_item_enabled("Edit", UNDO));
    assert!(!rig.menu_item_enabled("Edit", REDO));
    rig.chord(CTRL, egui::Key::Z);
    assert!(!rig.menu_item_enabled("Edit", UNDO));
    assert!(rig.menu_item_enabled("Edit", REDO));
}

#[test]
fn keyboard_undo_and_both_redo_chords_work() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.type_into_number_field(0, "5");
    rig.chord(CTRL, egui::Key::Z);
    assert_eq!(rig.host.translation(LIGHT)[0], 0.0);
    rig.chord(CTRL_SHIFT, egui::Key::Z);
    assert_eq!(rig.host.translation(LIGHT)[0], 5.0);
    rig.chord(CTRL, egui::Key::Z);
    assert_eq!(rig.host.translation(LIGHT)[0], 0.0);
    rig.chord(CTRL, egui::Key::Y);
    assert_eq!(rig.host.translation(LIGHT)[0], 5.0);
}

#[test]
fn a_new_edit_after_undo_drops_the_redo_branch() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.type_into_number_field(0, "5");
    rig.chord(CTRL, egui::Key::Z);
    rig.type_into_number_field(0, "7");
    assert!(!rig.app().stack.can_redo());
    rig.chord(CTRL, egui::Key::Y);
    assert_eq!(rig.host.translation(LIGHT)[0], 7.0);
}

#[test]
fn a_u64_field_cannot_hold_an_exact_asset_id() {
    // DOCUMENTS A DEFECT: every numeric field is an egui `DragValue`, which holds its value as an
    // f64 — 53 bits of integer. A `MeshAsset.asset` is a 64-bit content hash, so the inspector
    // both DISPLAYS a different number than the scene holds and, on any edit, WRITES the rounded
    // one: typing an id in exactly yields a neighbouring id that names no asset.
    const ID: u64 = 0xdaba_e4d5_f45c_860b; // the cooked cube's real content id
    let rounded = (ID as f64) as u64;
    assert_ne!(rounded, ID);

    let mut world = starting_world();
    world[2].components[1].data = 0xAAu64.to_le_bytes().to_vec();
    let mut rig = Rig::build(None, world);
    rig.click(ROW_CRATE);
    rig.type_into_number_field(10, &ID.to_string()); // after the transform's ten floats
    assert_eq!(
        rig.host.component(CRATE, H_MESH_ASSET),
        Some(rounded.to_le_bytes().as_slice()),
        "the id that was typed is not the id that was stored"
    );
}

#[test]
fn ctrl_z_while_typing_in_a_text_box_undoes_a_world_edit() {
    // DOCUMENTS A DEFECT: the undo chord is read from global input before any widget sees it, with
    // no `wants_keyboard_input` gate (the gizmo hotkeys have one). So Ctrl+Z pressed to fix a typo
    // in the asset search box reverts the last change to the WORLD instead.
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.type_into_number_field(0, "5");
    rig.harness.get_by_role(Role::TextInput).type_text("crat");
    rig.settle();
    assert_eq!(rig.app().asset_search, "crat");
    rig.chord(CTRL, egui::Key::Z);
    assert_eq!(
        rig.host.translation(LIGHT)[0],
        0.0,
        "the world edit was undone"
    );
}

#[test]
fn undo_after_a_despawn_edits_an_entity_that_no_longer_exists() {
    // DOCUMENTS A DEFECT (minor): history entries name an entity by handle and are not pruned when
    // it is despawned. Undo is offered, "succeeds", consumes the step — and changes nothing.
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.type_into_number_field(0, "5");
    rig.click("✖ despawn");
    rig.host.log.clear();
    assert!(rig.menu_item_enabled("Edit", UNDO));
    rig.chord(CTRL, egui::Key::Z);
    let sent = set_of(rig.host.last(EditorMessage::SetComponent).unwrap());
    assert_eq!((sent.index, sent.generation), LIGHT);
    assert!(rig.host.component(LIGHT, H_LOCAL).is_none(), "it is gone");
    assert!(!rig.app().stack.can_undo());
}

// ── Inspector: components ───────────────────────────────────────────────────────────────────

#[test]
fn add_component_offers_only_what_the_entity_lacks_and_adds_it() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    // The menu's entries are whatever buttons opening it adds (the inspector's own component
    // headers are buttons named after types too). Present types are not offered; value types
    // (Vec3/Quat) never are.
    let before = rig.button_labels();
    rig.click("+ add component");
    let offered: Vec<String> = rig
        .button_labels()
        .into_iter()
        .filter(|l| !before.contains(l))
        .collect();
    assert_eq!(
        offered,
        [
            "rime::ecs::WorldTransform",
            "rime::render::Camera",
            "rime::render::MeshAsset"
        ]
    );
    rig.click("rime::render::MeshAsset");
    assert_eq!(
        rig.host.commands(),
        [EditorMessage::AddComponent, EditorMessage::RequestSnapshot]
    );
    let added = ComponentRef::decode(rig.host.last(EditorMessage::AddComponent).unwrap()).unwrap();
    assert_eq!((added.index, added.type_hash), (1, H_MESH_ASSET));
    // The engine's default arrived through the resync and is now editable in the inspector.
    assert!(rig.host.component(LIGHT, H_MESH_ASSET).is_some());
    assert!(rig.has("asset"));
}

#[test]
fn add_component_is_not_undoable() {
    // DOCUMENTS A DEFECT: gui/commands.rs states "adding a component undoes to removing it" — an
    // exact inverse exists and the stack can hold it — but the inspector pushes the command
    // without recording an `Edit`. Undo stays greyed out.
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    rig.click("+ add component");
    rig.click("rime::render::MeshAsset");
    assert!(!rig.menu_item_enabled("Edit", UNDO));
}

#[test]
fn remove_component_removes_it() {
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    // One "remove" per component, in component order: [LocalTransform, PointLight].
    let removes: Vec<_> = rig.harness.get_all_by_label("remove").collect();
    assert_eq!(removes.len(), 2);
    removes[1].click();
    rig.settle();
    assert_eq!(
        rig.host.commands(),
        [
            EditorMessage::RemoveComponent,
            EditorMessage::RequestSnapshot
        ]
    );
    assert!(rig.host.component(LIGHT, H_LIGHT).is_none());
    assert!(!rig.has("rime::render::PointLight"));
}

#[test]
fn remove_component_is_not_undoable_and_loses_its_values() {
    // DOCUMENTS A DEFECT (same cause as add): commands.rs promises "removing undoes to setting its
    // old bytes back", the inspector never records it. One click on "remove" discards an authored
    // light's values for good.
    let mut rig = Rig::new();
    rig.click(ROW_LIGHT);
    let removes: Vec<_> = rig.harness.get_all_by_label("remove").collect();
    removes[1].click();
    rig.settle();
    assert!(!rig.menu_item_enabled("Edit", UNDO));
    rig.chord(CTRL, egui::Key::Z);
    assert!(rig.host.component(LIGHT, H_LIGHT).is_none());
}

#[test]
fn a_component_the_schema_cannot_describe_is_shown_read_only() {
    let mut world = starting_world();
    world[1].components.push(comp(H_UNKNOWN, vec![1, 2, 3]));
    let mut rig = Rig::build(None, world);
    rig.click(ROW_LIGHT);
    assert!(rig.has("type 0x000000000000dead"));
    assert!(rig.has("3 bytes · hash 0x000000000000dead — no schema/undecodable (read-only)"));
}

// ── Asset browser ───────────────────────────────────────────────────────────────────────────

#[test]
fn asset_browser_lists_the_cook_manifest() {
    let rig = Rig::new();
    for path in [
        "meshes/crate.gltf",
        "meshes/barrel.gltf",
        "textures/brick.png",
        "materials/brick.mat",
    ] {
        assert!(rig.has(path), "missing asset row {path:?}");
    }
}

#[test]
fn asset_browser_says_so_when_there_is_no_manifest() {
    let mut rig = Rig::new();
    rig.host.shared.lock().unwrap().assets.clear();
    rig.settle();
    assert!(rig.has("(no cooked assets — launch the engine with --assets <manifest>)"));
}

#[test]
fn asset_search_filters_by_path() {
    let mut rig = Rig::new();
    rig.harness.get_by_role(Role::TextInput).type_text("barrel");
    rig.settle();
    assert!(rig.has("meshes/barrel.gltf"));
    assert!(!rig.has("meshes/crate.gltf"));
    assert!(!rig.has("textures/brick.png"));

    rig.harness.get_by_role(Role::TextInput).type_text("-nope");
    rig.settle();
    assert!(rig.has("(no assets match the filter)"));
}

#[test]
fn asset_kind_filter_narrows_the_list() {
    let mut rig = Rig::new();
    rig.harness.get_by_role(Role::ComboBox).click();
    rig.settle();
    rig.click("Texture");
    assert_eq!(rig.app().asset_kind_filter, Some(AssetKind::Texture));
    assert!(rig.has("textures/brick.png"));
    assert!(!rig.has("meshes/crate.gltf"));
    assert!(!rig.has("materials/brick.mat"));
}

#[test]
fn place_spawns_an_entity_referencing_the_mesh() {
    let mut rig = Rig::new();
    let places: Vec<_> = rig.harness.get_all_by_label("place").collect();
    places[1].click(); // the barrel
    rig.settle();

    assert_eq!(
        rig.host.commands(),
        [EditorMessage::SpawnEntity, EditorMessage::RequestSnapshot]
    );
    let spawn = SpawnEntity::decode(rig.host.last(EditorMessage::SpawnEntity).unwrap()).unwrap();
    assert_eq!(
        spawn.components,
        [(H_MESH_ASSET, MESH_BARREL_ID.to_le_bytes().to_vec())]
    );
    assert!(rig.has("3  MeshAsset"));
    assert!(rig.has("4 entities"));
    // Placed at the engine's default transform, and NOT selected: the user has to find the new row
    // (or click the mesh in the viewport) before they can move it.
    assert_eq!(rig.app().selected, None);
}

#[test]
fn place_is_on_screen_in_the_default_panel_for_a_real_source_path() {
    // A real cooked path ("samples/08-gltf-zoo/assets/cube.gltf") is far wider than the Assets
    // panel, which opens 22% of the window wide. The button used to sit AFTER the path, so the
    // path pushed "place" past the panel's right edge where a pointer could not reach it. It now
    // comes first, so the button's whole box is inside the panel at the default layout, with no
    // splitter dragging.
    let mut rig = Rig::new();
    rig.host.shared.lock().unwrap().assets = vec![AssetEntry {
        kind: AssetKind::Mesh,
        id: MESH_CRATE_ID,
        source_path: "samples/08-gltf-zoo/assets/cube.gltf".to_string(),
        cooked_file: "cube.rmesh".to_string(),
    }];
    rig.settle();
    let panel = panel_rect(&rig, Tab::Assets);
    let place = rig
        .harness
        .get_by_label("place")
        .raw_bounds()
        .expect("bounds");
    assert!(
        place.x0 as f32 >= panel.left() && place.x1 as f32 <= panel.right(),
        "place spans x = {}..{}, the panel is {}..{}",
        place.x0,
        place.x1,
        panel.left(),
        panel.right()
    );
    // And it is the button a person would press: a click at its centre places the mesh.
    rig.harness
        .get_by_role_and_label(Role::Button, "place")
        .click();
    rig.settle();
    assert!(rig.has("3  MeshAsset"));
}

#[test]
fn only_meshes_can_be_placed() {
    // DOCUMENTS A LIMITATION: four assets listed, two "place" buttons — the meshes. A texture or a
    // material is browse-only; there is no way to assign one to anything from the browser.
    let rig = Rig::new();
    assert_eq!(rig.harness.get_all_by_label("place").count(), 2);
}

#[test]
fn place_is_not_undoable() {
    // DOCUMENTS A LIMITATION: placement is a spawn, and spawns leave no history.
    let mut rig = Rig::new();
    let places: Vec<_> = rig.harness.get_all_by_label("place").collect();
    places[0].click();
    rig.settle();
    assert!(!rig.menu_item_enabled("Edit", UNDO));
}

// ── Gizmo toolbar ───────────────────────────────────────────────────────────────────────────

#[test]
fn gizmo_mode_buttons_switch_mode_and_tell_the_engine() {
    let mut rig = Rig::new();
    assert!(rig.toggled("Off (Q)"));
    rig.click(ROW_LIGHT);

    for (label, mode) in [
        ("Move (W)", GizmoMode::Translate),
        ("Rotate (E)", GizmoMode::Rotate),
        ("Scale (R)", GizmoMode::Scale),
    ] {
        rig.click(label);
        assert!(rig.toggled(label));
        assert!(!rig.toggled("Off (Q)"));
        let state = GizmoState::decode(rig.host.last(EditorMessage::GizmoState).unwrap()).unwrap();
        assert_eq!((state.index, state.generation, state.mode), (1, 0, mode));
    }
    rig.click("Off (Q)");
    let state = GizmoState::decode(rig.host.last(EditorMessage::GizmoState).unwrap()).unwrap();
    assert_eq!(state, GizmoState::none());
}

#[test]
fn gizmo_hotkeys_switch_mode() {
    let mut rig = Rig::new();
    rig.key(egui::Key::W);
    assert!(rig.toggled("Move (W)"));
    rig.key(egui::Key::E);
    assert!(rig.toggled("Rotate (E)"));
    rig.key(egui::Key::R);
    assert!(rig.toggled("Scale (R)"));
    rig.key(egui::Key::Q);
    assert!(rig.toggled("Off (Q)"));
    rig.key(egui::Key::W);
    rig.key(egui::Key::Escape);
    assert!(rig.toggled("Off (Q)"));
}

#[test]
fn gizmo_hotkeys_are_ignored_while_typing() {
    let mut rig = Rig::new();
    rig.harness.get_by_role(Role::TextInput).type_text("w");
    rig.settle();
    rig.key(egui::Key::W);
    assert!(rig.toggled("Off (Q)"));
}

#[test]
fn snap_checkbox_toggles() {
    let mut rig = Rig::new();
    assert!(!rig.toggled("Snap"));
    rig.click_checkbox("Snap");
    assert!(rig.toggled("Snap"));
    assert!(rig.app().gizmo_snap);
    rig.click_checkbox("Snap");
    assert!(!rig.app().gizmo_snap);
}

// ── Play toolbar ────────────────────────────────────────────────────────────────────────────

#[test]
fn play_toolbar_buttons_are_enabled_by_phase() {
    let mut rig = Rig::new();
    // Edit: Play and Step; nothing to pause or stop.
    assert!(rig.has_text("Edit"));
    assert_eq!(
        ["▶", "⏸", "⏭", "⏹"].map(|b| rig.enabled(b)),
        [true, false, true, false]
    );
    rig.click("▶");
    assert_eq!(
        ["▶", "⏸", "⏭", "⏹"].map(|b| rig.enabled(b)),
        [false, true, true, true]
    );
    rig.click("⏸");
    assert_eq!(
        ["▶", "⏸", "⏭", "⏹"].map(|b| rig.enabled(b)),
        [true, false, true, true]
    );
    rig.click("⏹");
    assert_eq!(
        ["▶", "⏸", "⏭", "⏹"].map(|b| rig.enabled(b)),
        [true, false, true, false]
    );
}

#[test]
fn play_pause_step_stop_send_their_commands_and_show_the_phase() {
    let mut rig = Rig::new();
    rig.click("▶");
    rig.host.tick();
    rig.host.tick();
    rig.settle();
    assert!(rig.has("▶ Playing · tick 2"));

    rig.click("⏸");
    assert!(rig.has("⏸ Paused · tick 2"));

    rig.click("⏭");
    assert!(rig.has("⏸ Paused · tick 3"));

    rig.click("▶"); // resume
    assert!(rig.has("▶ Playing · tick 3"));

    rig.click("⏹");
    assert!(rig.has_text("Edit"));
    assert_eq!(
        rig.host.commands(),
        [
            EditorMessage::Play,
            EditorMessage::Pause,
            EditorMessage::Step,
            EditorMessage::Play,
            EditorMessage::Stop,
            // Stop restores the world wholesale, so the mirror must be re-fetched.
            EditorMessage::RequestSnapshot,
        ]
    );
}

#[test]
fn step_from_edit_enters_paused() {
    let mut rig = Rig::new();
    rig.click("⏭");
    assert_eq!(rig.host.commands(), [EditorMessage::Step]);
    assert!(rig.has("⏸ Paused · tick 1"));
    assert!(rig.enabled("⏹"));
}

#[test]
fn stop_resyncs_the_mirror_to_the_restored_world() {
    let mut rig = Rig::new();
    let authored = rig.host.translation(CRATE);
    rig.click("▶");
    rig.host.tick();
    assert_ne!(rig.host.translation(CRATE), authored, "the sim moved it");
    rig.click("⏹");
    assert_eq!(rig.host.translation(CRATE), authored);
    let mirror = rig.host.shared.lock().unwrap().snapshot.entities.clone();
    assert_eq!(mirror, rig.host.world);
}

#[test]
fn the_inspector_is_not_live_during_play() {
    // DOCUMENTS A LIMITATION (commands.rs `is_structural`): nothing re-fetches the world while the
    // simulation runs, so the inspector keeps showing the pre-play numbers of an object that is
    // visibly falling in the viewport. The coloured viewport border is the only signal.
    let mut rig = Rig::new();
    rig.click(ROW_CRATE);
    rig.click("▶");
    rig.host.tick();
    rig.host.tick();
    rig.settle();
    assert_eq!(rig.host.translation(CRATE), [3.0, -2.0, 0.0]);
    let shown = rig
        .harness
        .get_all_by_role(Role::SpinButton)
        .nth(1)
        .unwrap();
    assert_eq!(shown.numeric_value(), Some(0.0), "still the authored y");
}

// ── Viewport (raw pointer input — the panel has no accessibility node) ───────────────────────

#[test]
fn the_viewport_is_invisible_to_accessibility() {
    // DOCUMENTS A GAP, and the reason the tests below use pointer positions: the viewport is
    // painted straight onto an `allocate_exact_size` rect. No role, no label, no actions — a
    // screen reader (or a label-driven test) cannot find the editor's main panel. Even the
    // "waiting for the engine's viewport…" notice is painter text, not a label.
    let rig = Rig::new();
    assert!(!rig.has("waiting for the engine's viewport…"));
    assert!(rig.harness.query_by_label_contains("iewport").is_none());
}

#[test]
fn a_streamed_frame_reaches_the_viewport_texture() {
    let mut rig = Rig::new();
    assert!(rig.app().frame_tex.is_none());
    rig.present_frame();
    assert!(rig.app().frame_tex.is_some());
    assert_eq!(rig.app().shown_seq, 1);
    assert!(rig.has("1 frames · 0 fps"));
}

#[test]
fn a_viewport_click_picks_and_the_answer_selects() {
    let mut rig = Rig::new();
    rig.present_frame();
    rig.host.pick_answer = PickResult {
        index: CRATE.0,
        generation: CRATE.1,
    };
    rig.viewport_click((480.5, 270.5));

    assert_eq!(rig.host.count(EditorMessage::PickRequest), 1);
    let pick = PickRequest::decode(rig.host.last(EditorMessage::PickRequest).unwrap()).unwrap();
    // In the ENGINE's pixel space (the lens extent), not the panel's.
    assert_eq!((pick.x, pick.y), (480, 270));
    assert_eq!(rig.app().selected, Some(2));
    assert!(rig.toggled(ROW_CRATE));
}

#[test]
fn a_viewport_click_on_nothing_clears_the_selection() {
    let mut rig = Rig::new();
    rig.present_frame();
    rig.click(ROW_LIGHT);
    rig.host.pick_answer = PickResult::none();
    rig.viewport_click((100.0, 100.0));
    assert_eq!(rig.app().selected, None);
}

#[test]
fn hovering_a_gizmo_handle_highlights_its_axis() {
    let mut rig = Rig::new();
    rig.present_frame();
    rig.click(ROW_LIGHT);
    rig.click("Move (W)");
    let origin = gizmo::Vec3::new(0.0, 0.0, 0.0);
    rig.pointer_move(rig.x_handle_px(origin, 0.5));
    assert_eq!(rig.app().gizmo_hover, Some(gizmo::Axis::X));
    let state = GizmoState::decode(rig.host.last(EditorMessage::GizmoState).unwrap()).unwrap();
    assert_eq!(state.axis, GizmoAxis::X);
}

/// Drag the light's X handle from 40% to 40% + `travel_px` render pixels; returns the rig.
fn drag_light_along_x(snap: bool, travel_px: f32) -> Rig {
    let mut rig = Rig::new();
    rig.present_frame();
    rig.click(ROW_LIGHT);
    rig.click("Move (W)");
    if snap {
        rig.click_checkbox("Snap");
    }
    rig.host.log.clear();
    let origin = gizmo::Vec3::new(0.0, 0.0, 0.0);
    let press = rig.x_handle_px(origin, 0.3);
    // egui only calls a press a drag once the pointer has travelled 6 pt; the gizmo anchors where
    // the drag is RECOGNISED, not where the button went down. `grab` is that point — still on the
    // handle — and the measured travel starts there.
    let grab = rig.x_handle_px(origin, 0.6);
    let end = (grab.0 + travel_px, grab.1);
    rig.drag(egui::PointerButton::Primary, &[press, grab, end]);
    rig
}

#[test]
fn dragging_a_gizmo_handle_moves_the_entity_along_that_axis() {
    let travel_px = 90.0;
    let rig = drag_light_along_x(false, travel_px);
    let [x, y, z] = rig.host.translation(LIGHT);
    // Expected from the lens geometry (see WORLD_PER_PX), not from the gizmo code.
    let expected = travel_px * WORLD_PER_PX;
    assert!(
        (x - expected).abs() < 1.0e-3,
        "moved {x}, expected {expected}"
    );
    assert_eq!((y, z), (0.0, 0.0), "constrained to X");
    // Live while dragging (several sets), no pick, and nothing forwarded as camera input.
    assert!(rig.host.count(EditorMessage::SetComponent) >= 2);
    assert_eq!(rig.host.count(EditorMessage::PickRequest), 0);
    assert!(rig.host.inputs.is_empty());
}

#[test]
fn a_gizmo_drag_is_one_undo_step() {
    let mut rig = drag_light_along_x(false, 90.0);
    assert!(rig.app().stack.can_undo());
    rig.chord(CTRL, egui::Key::Z);
    assert_eq!(rig.host.translation(LIGHT), [0.0, 0.0, 0.0]);
    assert!(!rig.app().stack.can_undo());
    rig.chord(CTRL, egui::Key::Y);
    assert!((rig.host.translation(LIGHT)[0] - 90.0 * WORLD_PER_PX).abs() < 1.0e-3);
}

#[test]
fn snap_quantizes_a_gizmo_drag_to_the_quarter_unit_grid() {
    let rig = drag_light_along_x(true, 90.0);
    let x = rig.host.translation(LIGHT)[0];
    // 90 px is 1.667 world units unsnapped; the quarter-unit grid makes that 1.75.
    assert_eq!(x, 1.75);
}

#[test]
fn holding_ctrl_does_not_snap_a_gizmo_drag() {
    // DOCUMENTS A STUB: the `gizmo_snap` field's own comment promises "Ctrl also snaps per-drag",
    // but only the toolbar checkbox is ever read. Ctrl-dragging lands off the grid.
    let mut rig = Rig::new();
    rig.present_frame();
    rig.click(ROW_LIGHT);
    rig.click("Move (W)");
    rig.harness.input_mut().modifiers = egui::Modifiers::CTRL;
    let origin = gizmo::Vec3::new(0.0, 0.0, 0.0);
    let grab = rig.x_handle_px(origin, 0.6);
    rig.drag(
        egui::PointerButton::Primary,
        &[rig.x_handle_px(origin, 0.3), grab, (grab.0 + 90.0, grab.1)],
    );
    let x = rig.host.translation(LIGHT)[0];
    assert!((x - 90.0 * WORLD_PER_PX).abs() < 1.0e-3, "unsnapped: {x}");
    assert_ne!(x, 1.75);
}

#[test]
fn a_left_drag_on_empty_viewport_is_forwarded_as_pointer_input() {
    let mut rig = Rig::new();
    rig.present_frame();
    rig.drag(
        egui::PointerButton::Primary,
        // Pixel centres: the editor truncates the cursor to whole render pixels.
        &[(100.5, 100.5), (140.5, 100.5), (180.5, 100.5)],
    );
    assert_eq!(
        rig.host.inputs.first().map(String::as_str),
        Some("down 140,100 b0")
    );
    assert_eq!(
        rig.host.inputs.last().map(String::as_str),
        Some("up 180,100 b0")
    );
    assert!(rig.host.inputs.iter().any(|i| i == "move 180,100"));
    assert_eq!(rig.host.count(EditorMessage::PickRequest), 0);
}

#[test]
fn right_drag_looks_around_by_editing_the_camera_entity() {
    let mut rig = Rig::new();
    rig.present_frame();
    let before = gizmo::decode_trs_blob(rig.host.component(CAMERA, H_LOCAL).unwrap()).unwrap();
    rig.drag(
        egui::PointerButton::Secondary,
        &[(400.0, 270.0), (440.0, 270.0), (500.0, 270.0)],
    );
    let after = gizmo::decode_trs_blob(rig.host.component(CAMERA, H_LOCAL).unwrap()).unwrap();
    assert_ne!(before.rotation, after.rotation, "the view turned");
    assert_eq!(before.translation, after.translation, "without moving");
    // Looking is not picking (the m17.1 fix: turning the view must not change the selection).
    assert_eq!(rig.host.count(EditorMessage::PickRequest), 0);
    // Navigation is not an edit to undo.
    assert!(!rig.app().stack.can_undo());
}

#[test]
fn a_right_drag_leaks_an_unpaired_pointer_up_to_the_engine() {
    // DOCUMENTS A DEFECT (minor): m17.1 gated the forwarded pointer-DOWN and pointer-MOVE on the
    // primary button, but not the pointer-UP. So every look-around ends by telling the engine
    // "left button released" for a press it was never told about. Harmless to today's host, which
    // ignores it; a game that acts on button-up during Play would see phantom releases.
    let mut rig = Rig::new();
    rig.present_frame();
    rig.drag(
        egui::PointerButton::Secondary,
        &[(400.5, 270.5), (440.5, 270.5), (500.5, 270.5)],
    );
    assert_eq!(rig.host.inputs, ["up 500,270 b0"]);
}

#[test]
fn wasd_over_the_viewport_flies_the_camera() {
    let mut rig = Rig::new();
    rig.present_frame();
    rig.pointer_move((480.0, 270.0));
    rig.hold_key(egui::Key::S);
    let [x, y, z] = rig.host.translation(CAMERA);
    assert!(z > EYE_Z, "S backs away along +z, got z = {z}");
    assert_eq!((x, y), (0.0, 0.0));
}

#[test]
fn w_e_and_q_both_fly_the_camera_and_switch_the_gizmo() {
    // DOCUMENTS A DEFECT: W/E/Q are bound twice. With the pointer over the viewport — the only
    // place either binding is useful — pressing W to fly forward also flips the gizmo to Move, E
    // (fly up) flips it to Rotate, and Q (fly down) hides it. Navigating therefore keeps changing
    // the tool in hand; the README documents both bindings and not the collision.
    let mut rig = Rig::new();
    rig.present_frame();
    rig.pointer_move((480.0, 270.0));
    rig.click("Scale (R)");
    rig.hold_key(egui::Key::W);
    assert!(rig.host.translation(CAMERA)[2] < EYE_Z, "flew forward");
    assert!(rig.toggled("Move (W)"), "and the gizmo changed under it");
}

#[test]
fn keys_are_never_forwarded_to_the_engine() {
    // DOCUMENTS A STUB: the editor→engine input vocabulary is pointer-only (`protocol_input::Input`
    // has no Key variant), so a game that reads the keyboard cannot be driven during Play from
    // this window. Only the editor's own camera sees keys.
    let mut rig = Rig::new();
    rig.present_frame();
    rig.click("▶");
    rig.pointer_move((480.0, 270.0));
    rig.host.log.clear();
    for key in [egui::Key::Space, egui::Key::ArrowUp, egui::Key::F] {
        rig.key(key);
    }
    assert!(rig.host.inputs.is_empty());
    assert!(rig.host.commands().is_empty());
}
