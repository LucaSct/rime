# tools/ — the Rust tooling

Rime's **editor**, **asset pipeline**, and **command-line tools** live here, written in
**Rust** for memory safety and excellent tooling. It is a Cargo workspace; as of M6.2 the
`asset-pipeline` and `rime-cli` crates have landed (glTF mesh import → cook).

Why Rust for tools (and C++ for the runtime)? See
[../docs/adr/0001-cpp-core-rust-tooling.md](../docs/adr/0001-cpp-core-rust-tooling.md).

## Planned crates

| Crate | What it is |
| --- | --- |
| `editor` | the visual editor: scene/world editing, inspectors (driven by engine reflection), live preview |
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
