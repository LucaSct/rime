# editor — the Rime editor (M9)

The editor is a **client of a live engine process** (ADR-0016). It launches
`rime-engine --editor-host`, connects over the s1.4 local socket, and edits the world through the
reflection-described **editor channel** (the [`rime-protocol`](../rime-protocol) crate). The engine
renders and owns the world; the editor is a thin, crash-isolated shell around it.

## Status

- **`editor --smoke`** — a headless end-to-end check, in two modes, proving both halves of
  editor-as-client without a window:
  - **channel**: spawn `rime-engine --editor-host`, handshake, pull the component **schema** + a
    full-world **snapshot**, push an **edit** back, assert a clean exit (GPU-free).
  - **`--frames N`**: the engine renders a scene and streams it; the smoke receives + LZ4-decodes N
    **viewport frames** to RGBA — the render → capture → encode → wire → decode path. Needs a
    Vulkan device (lavapipe) on the engine side.

  Run both via [`scripts/editor-smoke.sh`](../../scripts/editor-smoke.sh).
- **The interactive shell** (feature `gui`) — an **egui docking layout**: a **Viewport** panel drawing
  the engine's streamed frames (with pointer input forwarded back), an **Outliner** of the world's
  entities, an **Inspector** labelling a selected entity's components from the reflection schema, and
  an **Assets** placeholder (the manifest browser is m9.5), over a connection status bar. The
  windowing stack (eframe → wgpu/winit) is behind the `gui` feature so the headless smoke stays a
  light build. Per ADR-0031 the on-screen result is **Mac-eyeballed** — CI compile-checks it but does
  not run it (a windowed UI is not provable on a headless box).

## Try it

```bash
# One command (builds rime-engine + editor, runs the channel + viewport smokes):
scripts/editor-smoke.sh

# By hand — the editor channel against any .rscene (or omit --scene for the default world):
cargo build -p editor
./target/debug/editor --smoke \
    --engine ../build/dev/bin/rime-engine \
    --scene ../samples/07-first-light/first_light.rscene

# The streamed viewport (renders on the engine; needs any Vulkan ICD / lavapipe):
./target/debug/editor --smoke --frames 30 --engine ../build/dev/bin/rime-engine

# The interactive docking shell (needs a display + a Vulkan ICD on the engine side):
cargo run -p editor --features gui -- --engine ../build/dev/bin/rime-engine
```

`--engine` (or `$RIME_ENGINE_BIN`) points at the built `rime-engine`. The smoke is Unix-only for now
(it drives the `AF_UNIX` wire through `std::os::unix`); the cross-platform path arrives with the
shell.
