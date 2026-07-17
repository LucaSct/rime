# editor — the Rime editor (M9)

The editor is a **client of a live engine process** (ADR-0016). It launches
`rime-engine --editor-host`, connects over the s1.4 local socket, and edits the world through the
reflection-described **editor channel** (the [`rime-protocol`](../rime-protocol) crate). The engine
renders and owns the world; the editor is a thin, crash-isolated shell around it.

## Status

- **`editor --smoke`** — a headless end-to-end check (this brick): spawn `rime-engine --editor-host`
  over a fresh local socket, handshake, pull the component **schema** + a full-world **snapshot**,
  push an **edit** back, and assert the engine exits cleanly. No window, no GPU — the CI-provable
  proof that the editor-as-client loop works. Run it via [`scripts/editor-smoke.sh`](../../scripts/editor-smoke.sh).
- **The interactive shell** — an egui docking layout (outliner, inspectors, asset browser) with the
  engine's **streamed viewport** in a panel — is the next brick. Per ADR-0031 the windowed UI is
  *Mac-eyeballed* (not provable on a headless CI box), so it lands separately from this verifiable
  spine.

## Try it

```bash
# One command (builds rime-engine + editor, runs the smoke):
scripts/editor-smoke.sh

# Or by hand, against any .rscene (or omit --scene for the engine's default world):
cargo build -p editor
./target/debug/editor --smoke \
    --engine ../build/dev/bin/rime-engine \
    --scene ../samples/07-first-light/first_light.rscene
```

`--engine` (or `$RIME_ENGINE_BIN`) points at the built `rime-engine`. The smoke is Unix-only for now
(it drives the `AF_UNIX` wire through `std::os::unix`); the cross-platform path arrives with the
shell.
