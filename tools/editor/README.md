# editor — the Rime editor (M9, Editor v1)

The editor is a **client of a live engine process** (ADR-0016). It launches
`rime-engine --editor-host`, connects over the s1.4 local socket, and edits the world through the
reflection-described **editor channel** (the [`rime-protocol`](../rime-protocol) crate). The engine
renders and owns the world; the editor is a thin, crash-isolated shell around it. Nothing here
reaches into engine internals — every capability is a message on the versioned wire, which is why
the whole workflow is provable headless (see the smoke below).

## What it does (Editor v1)

The interactive shell (feature `gui`) is an **egui docking layout**:

- **Viewport** — draws the engine's streamed frames as a live texture and forwards pointer input.
  Click to **pick** the entity under the cursor (an engine-side ID-buffer pass, m9.6); the selected
  object shows **transform gizmos** you drag to move/rotate/scale it (`W`/`E`/`R`, m9.6b).
- **Outliner** — the world's entities; spawn/despawn; selection is shared with the viewport.
- **Inspector** — a selected entity's components as **editable, reflection-typed fields** (m9.4):
  edit any scalar/struct field, add/remove components, all with **undo/redo**. No per-component UI
  code — the widgets are generated from the schema the engine sends.
- **Assets** — a browser over the engine's cook manifest (`--assets`, m9.5): search/filter cooked
  content and **place** a mesh into the world.
- **Play toolbar** — **Play ▶ / Pause / Step / Stop ■** run the simulation live *in the editor*
  (play-in-editor, m9.7): the sim ticks the very world you are editing, snapshotted first so **Stop
  restores it bit-exactly**. The viewport border is coloured by play state.

An edit made in Edit mode moves the object *immediately* — the engine composes the authored
`LocalTransform` into the rendered `WorldTransform` every frame, in Edit as well as during Play.

The windowing stack (eframe → wgpu/winit) is behind the `gui` feature so the headless smoke stays a
light build. Per ADR-0031 the on-screen result is **Mac-eyeballed** — CI compile-checks it but does
not run a window (a windowed UI is not provable on a headless box). Everything the UI *calls*,
however, is exercised headlessly:

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
   transform value). `Ctrl+Z` / `Ctrl+Shift+Z` undo/redo.
4. **Place** — in the Assets panel, search the browser and place a mesh; it appears in the Outliner.
5. **Play** — hit **▶**. The simulation runs live (a dynamic body falls). **Step** advances one
   tick; **Stop ■** restores the exact pre-play scene.

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
