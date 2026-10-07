# editor — the Rime editor (M9, Editor v1)

The editor is a **client of a live engine process** (ADR-0016). It launches
`rime-engine --editor-host`, connects over the s1.4 local socket, and edits the world through the
reflection-described **editor channel** (the [`rime-protocol`](../rime-protocol) crate). The engine
renders and owns the world; the editor is a thin, crash-isolated shell around it. Nothing here
reaches into engine internals — every capability is a message on the versioned wire, which is why
the whole workflow is provable headless (see the smoke below).

## What it does (Editor v1)

The interactive shell (feature `gui`) is an **egui docking layout**:

- **Menu bar** — **File**: New, Open (a path typed into the box), Save, Save As. **Edit**: undo and
  redo. **View**: one checkmark per panel; clicking a closed panel reopens it in its default place.
  New and Open are unavailable while the simulation is playing.
- **Viewport** — draws the engine's streamed frames as a live texture and forwards pointer input.
  Click to **pick** the entity under the cursor (an engine-side ID-buffer pass, m9.6); the selected
  object shows **transform gizmos** you drag to move/rotate/scale it (`W`/`E`/`R`, m9.6b).
  Hold the **right button** over the viewport to **fly**: drag to look, and hold it with `W`/`A`/`S`/`D`
  to move, `Q`/`E` to go down/up, Shift to sprint. While the right button is up, `W`/`E`/`Q` set the
  gizmo instead, so one key does one job at a time.
- **Outliner** — the world's entities; spawn/despawn; selection is shared with the viewport.
- **Inspector** — a selected entity's components as **editable, reflection-typed fields** (m9.4):
  edit any scalar/struct field, add/remove components. No per-component UI code — the widgets are
  generated from the schema the engine sends. Integers are exact: a 64-bit field such as an asset id
  is a text box parsed as an integer, applied when focus leaves; text that is not a number is
  refused and the field goes back to the stored value.
- **Undo/redo covers every world edit** — field edits, gizmo drags, spawn, place, despawn (including
  the references other entities held to the despawned one), add and remove component (with its
  exact values). The history names entities by **EditorId** (ADR-0075), the engine host's stable,
  never-reused name for an entity, so it survives despawns, respawns and Play→Stop. A structural
  step is recorded only when the engine accepts the edit; a refusal is shown on the status line.
- **Assets** — a browser over the engine's cook manifest (`--assets`, m9.5): search/filter cooked
  content and **place** a mesh into the world.
- **Play toolbar** — **Play ▶ / Pause / Step / Stop ■** run the simulation live *in the editor*
  (play-in-editor, m9.7): the sim ticks the very world you are editing. The viewport border is
  coloured by play state. **What Stop guarantees** (ADR-0075):
  - every entity that existed before Play exists after Stop, with **exactly** its pre-play component
    set and reflected component values, and **the same EditorId** — so selection and undo history
    carry straight through;
  - an entity that survived play keeps its engine handle; one that play destroyed comes back under
    a new handle; one that play created is removed;
  - every entity reference (a `Parent`, any reflected `Entity` field) points at the same entity it
    did before Play, so saving after Stop writes the same file as saving before Play, byte for byte;
  - derived state (`WorldTransform`, physics bodies) is rebuilt from those components.
  It does **not** cover destruction side-tables (none are authored in the editor yet — ADR-0031 §4),
  or anything outside the ECS world. **The undo history is frozen while playing**: undo/redo are
  refused and play-time edits are not recorded, since Stop discards them. Stop also restores the
  selection you had when you pressed Play.

An edit made in Edit mode moves the object *immediately* — the engine composes the authored
`LocalTransform` into the rendered `WorldTransform` every frame, in Edit as well as during Play.

The windowing stack (eframe → wgpu/winit) is behind the `gui` feature so the headless smoke stays a
light build. ADR-0031 left the on-screen result **Mac-eyeballed** — "a windowed UI is not provable
on a headless box". Two harnesses have since narrowed that to *how it looks*:

- **Click tests** (`src/gui/click_tests.rs`, `cargo test -p editor --features gui`) run the shipped
  `EditorApp::update` headlessly through `egui_kittest` and drive it by widget role and label — the
  accessibility tree — against an in-process fake engine. One test per user-facing function. The
  tests named `view_is_a_dead_label` and the like pin **known defects**; read them as the editor's
  to-do list. A fixed defect's pin is rewritten into a positive test of the fixed behaviour — the
  `*_is_not_undoable` pins became exactly that in m19/E1.
- **[`scripts/editor-click-smoke.sh`](../../scripts/editor-click-smoke.sh)** runs the real binary
  and the real engine on a private Xvfb display and drives them with `xdotool`: place → select →
  edit → undo → redo → Play → Stop → undo → redo → Save As → quit, asserting the saved scene on
  disk — byte-identical to the one saved before Play. Linux-only
  (X11 + Mesa); it is not in CI.

Everything the UI *calls* is also exercised headlessly, without any window:

- **`editor --smoke`** — a headless end-to-end check proving editor-as-client without a window:
  - **channel**: spawn `rime-engine --editor-host`, handshake, pull the component **schema** + a
    full-world **snapshot**, push a typed **edit**, browse the **asset list**, assert a clean exit
    (GPU-free).
  - **`--frames N`**: the engine renders a scene and streams it; the smoke receives + LZ4-decodes N
    **viewport frames** to RGBA and **picks** a live pixel — the render → capture → encode → wire →
    decode → pick path. Needs a Vulkan device (lavapipe) on the engine side.

  Run both via [`scripts/editor-smoke.sh`](../../scripts/editor-smoke.sh).

## Your first five minutes

```bash
# 1. Build the engine and cook a little content (a manifest the browser can show).
scripts/build.sh --cpp-only               # builds build/dev/bin/rime-engine
#   (any cook manifest works; the gltf-zoo sample writes one you can point --assets at)

# 2. Launch the editor against the engine, loading a saved scene + a cook manifest.
cargo run -p editor --features gui -- \
    --engine build/dev/bin/rime-engine \
    --scene  samples/07-first-light/first_light.rscene \
    --assets <path/to/cook-manifest>       # a real path — NOT the literal <...>; omit to skip
```

Then, in the window:

1. **Select** — click an object in the Viewport (or a row in the Outliner). A gizmo appears.
2. **Move it** — with the gizmo mode buttons (`W` move · `E` rotate · `R` scale), drag a handle.
   The object moves live in the viewport as you drag.
3. **Tweak** — in the Inspector, edit a component field (a light's colour, a material index, a
   transform value). `Ctrl+Z` / `Ctrl+Shift+Z` undo/redo — any edit, spawn, place or despawn. While
   a text box has focus, `Ctrl+Z` is that box's undo, not the world's.
4. **Place** — in the Assets panel, search the browser and place a mesh; it appears in the Outliner.
5. **Open another scene** — **File ▸ Open**, type a path, press **Open**. A path that is not an
   existing readable file is refused at once, with the reason, and the current scene stays.
   Otherwise the editor starts a second engine on that file and switches to it once that engine has
   sent its first snapshot; an empty scene opens as an empty scene. **File ▸ New** starts an empty,
   unnamed scene; Save then waits for Save As.
   *Limit:* a malformed but existing file opens as whatever the engine managed to load. The engine
   keeps running on a bad `--scene` (`engine/app/editor_host_app.cpp`, `load_viewport_scene`), so the
   result may be empty or partial. An in-band load report from the engine is the follow-up that
   would refuse such a file.
6. **Play** — hit **▶**. The simulation runs live (a dynamic body falls). **Step** advances one
   tick; **Stop ■** restores the exact pre-play scene (see *What Stop guarantees* above).

> Heads-up on shells: `--assets <cook-manifest>` in this README uses `<…>` as *placeholder*
> notation. Type a **real path** there. Writing the angle brackets literally makes `zsh`/`bash`
> try to redirect I/O (`parse error near '\n'`).

`--engine` (or `$RIME_ENGINE_BIN`) points at the built `rime-engine`; `--scene` and `--assets` are
both optional (omit `--scene` for the built-in demo scene). The headless `--smoke` path is Unix-only
for now (it drives the `AF_UNIX` wire through `std::os::unix`); the interactive shell runs anywhere
eframe does.

## Layout

- `src/main.rs` — CLI entry: `--smoke` (headless) vs the `gui` shell.
- `src/gui.rs` — the docking shell, panels, and per-frame wiring.
- `src/gui/{session,commands}.rs` — the engine child process; the command layer (edits + undo/redo).
- `src/gizmo.rs` — pure, unit-tested gizmo math (pixel→ray, constrained drags; no linalg crate).
- `src/smoke.rs` — the headless end-to-end checks.

The wire itself (schema, snapshot, edits, frames, picks, gizmo/camera state, play control) lives in
[`rime-protocol`](../rime-protocol), with cross-language conformance against C++-emitted fixtures.
