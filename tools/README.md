# tools/ — the Rust tooling

Rime's **editor**, **asset pipeline**, and **command-line tools** live here, written in
**Rust** for memory safety and excellent tooling. It is a Cargo workspace; as of M6 the
`asset-pipeline` + `rime-cli` crates (glTF/STL import → cook; `rime cook`/`inspect`) and the
`rime-ffi` crate (safe bindings to the engine's C ABI, M6.9) have landed. M9.3 adds `rime-protocol`
(the editor's Rust implementation of the engine wire protocol) and `editor` (the editor shell —
today a headless `--smoke` that drives a live `rime-engine --editor-host`; the egui docking UI +
streamed viewport follow).

Why Rust for tools (and C++ for the runtime)? See
[../docs/adr/0001-cpp-core-rust-tooling.md](../docs/adr/0001-cpp-core-rust-tooling.md).

## Planned crates

| Crate | What it is |
| --- | --- |
| `editor` | the visual editor: a client of a live engine (`rime-engine --editor-host`) — inspectors driven by engine reflection, live preview. v1 = a headless `--smoke`; egui shell follows |
| `rime-protocol` | the editor's Rust implementation of the engine streaming/editor wire protocol (mirrors `engine/stream`; cross-language conformance-tested) |
| `asset-pipeline` | importers, bakers, and cookers that turn source art (meshes, textures, audio) into runtime-ready engine assets |
| `rime-cli` | command-line entry points: build/cook a project, run headless, inspect assets |
| `ffi` | the stable boundary crate that talks to the C++ engine (C ABI / protocol) |

## The boundary (important)

Tools communicate with the C++ engine **only across stable, explicit boundaries** — a C
ABI, shared file formats, or a command protocol. Tools must **not** reach into engine
internals, and the engine must not depend on the tools. The boundary is a real,
versioned interface; treat changes to it with care.

## Conventions

- `rustfmt` + `clippy`, warnings denied in CI.
- Every source file carries the SPDX header (see [../CLAUDE.md](../CLAUDE.md)).
- Offline correctness over raw latency — this is where Rust's safety earns its keep.

> Crates appear here as they're implemented (Milestone-driven). See
> [../docs/ROADMAP.md](../docs/ROADMAP.md).
