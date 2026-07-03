# CLAUDE.md — Working in the Rime repository

This file orients Claude Code (and any AI assistant or new contributor) when working
in this repository. Read it before making changes. It is intentionally practical.

For the *why* behind the project, read [VISION.md](VISION.md) first — it outranks this
file on questions of intent.

---

## What Rime is

Rime is a free, open-source, AAA-grade game engine fusing **Frostbite-class
destruction**, **Unreal-class lighting/rendering**, and **O3DE-class modularity**.
Target fidelity: a game like Battlefield 6. The code is written to be *read and learned
from*. See [VISION.md](VISION.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

**Status:** pre-alpha. We are building foundations. Expect stubs; label them as such.

## Tech stack (settled)

- **Runtime / renderer / hot paths:** C++20 (moving to C++23 features where stable).
- **Editor / asset pipeline / tooling:** Rust.
- **Graphics:** Vulkan-first, behind a thin Render Hardware Interface (RHI). Never call
  Vulkan directly outside the `rhi` Vulkan backend — go through the RHI.
- **Build:** CMake for the C++ engine; Cargo for the Rust tools.
- **Platforms:** Windows, Linux, macOS (macOS Vulkan via MoltenVK). Power beats
  portability when they conflict (see VISION principle #2).
- **License:** Apache-2.0. Every new source file gets the standard header (below).

## Repository layout

```
rime/
├── VISION.md              # The north star — read first
├── CLAUDE.md              # This file
├── README.md              # Public-facing intro
├── docs/
│   ├── ARCHITECTURE.md    # How the engine is structured and why
│   ├── ROADMAP.md         # Milestones (added once the plan is approved)
│   ├── glossary.md        # Plain-language definitions of engine terms
│   ├── research/          # Survey of other engines + sources
│   └── adr/               # Architecture Decision Records (numbered, append-only)
├── engine/                # C++ engine. One subdirectory per module (see its README)
├── tools/                 # Rust tooling: editor, asset pipeline, CLIs
├── samples/               # Example projects / scenes that exercise the engine
├── third_party/           # Vendored dependencies (see its README for policy)
└── scripts/               # Cross-platform setup / build / lint helpers
```

## Build & run commands

> Real as of Milestone 0. The one-command path is **`scripts/build.sh`** (`scripts/build.ps1`
> on Windows) — it runs `conan install`, configures, builds, and tests. The raw CMake presets
> below also work, but only *after* a `conan install` has generated the toolchain in
> `build/<preset>/` (which the script does for you). Prefer the script; still, don't invent
> commands — check first.

```bash
# One command: build the C++ engine + Rust tools, run all tests (dev = Debug)
scripts/build.sh
scripts/build.sh --preset release --no-tests
scripts/build.sh --cpp-only --sanitizer address   # ASan+UBSan (GCC/Clang; see CI)

# First-time toolchain setup (Conan venv, Rust, Vulkan runtime discovery)
scripts/setup.sh

# Under the hood, after `conan install . -of build/dev -s build_type=Debug ...`:
cmake --preset dev            # configure (Ninja + the Conan toolchain)
cmake --build --preset dev    # build
ctest --preset dev            # run C++ tests

# Rust tooling (from tools/)
cargo build
cargo test
```

## Coding standards

### The teaching rule (non-negotiable)
This codebase is also a textbook. Comments explain **why**, not just **what**. When you
implement a non-obvious technique, write a short comment block naming the technique and
the idea behind it, so a reader learning engines can follow along. Prefer one clear
paragraph over a cryptic one-liner. Do not delete explanatory comments to "clean up."

### C++ (engine/)
- C++20. 4-space indent, 100-column soft limit. Formatting is enforced by
  [`.clang-format`](.clang-format) — run it; don't hand-format.
- Namespaces: everything under `rime::`, then the module, e.g. `rime::rhi`,
  `rime::render`, `rime::ecs`.
- No raw `new`/`delete` in engine code; use the engine's allocators / RAII / smart
  pointers. Memory ownership must be obvious from the type.
- No exceptions on hot paths; prefer explicit error types / status returns. (Engine
  init/tooling may use exceptions; runtime frame code must not.)
- Data-oriented by default: think in arrays of data and the transforms over them, not
  deep object hierarchies. Hot loops are cache-friendly.
- **Module boundaries are sacred.** A module depends on *interfaces* of others, never
  their internals. `core` depends on nothing above it. Nothing depends on a concrete
  RHI backend — only on the RHI interface.

### Rust (tools/)
- Standard `rustfmt` + `clippy` (deny warnings in CI). Edition 2021+.
- Tools talk to the engine through stable, documented boundaries (CLI, files, or a C
  ABI) — never by reaching into C++ internals.

### Everything
- Small, focused commits. Conventional-commit-style prefixes encouraged
  (`feat:`, `fix:`, `docs:`, `refactor:`, `perf:`, `test:`).
- Measure before optimizing; note the measurement in the commit/PR.
- If something is a stub or a known limitation, say so in a comment and/or the PR.

## Architectural guardrails (read before designing)

1. **RHI seam:** graphics code targets `rime::rhi` interfaces. The Vulkan backend is
   the only place that includes Vulkan headers. This is what lets D3D12/Metal/console
   land later without a rewrite.
2. **Modularity:** new features arrive as modules with clear interfaces. The engine
   must still build if a feature module is removed.
3. **Destruction & lighting are first-class.** Don't design systems that would make
   networked part-based destruction or a GI-friendly render graph impossible later.
   When in doubt, leave the seam.
4. **Threading:** assume a job-system / data-parallel world. Don't bake in
   single-threaded assumptions (global mutable singletons, hidden ordering deps).

## Decisions & docs

- A meaningful technical decision gets an **ADR** in [`docs/adr/`](docs/adr/) (copy the
  format of the existing ones). ADRs are append-only; supersede, don't rewrite.
- Changing intent/scope means editing [VISION.md](VISION.md) deliberately, with an ADR.
- Keep [docs/glossary.md](docs/glossary.md) growing — every acronym a newcomer might
  not know should be there.

## Standard source-file header

C++:
```cpp
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
```
Rust:
```rust
// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
```

## Working style here

- This is a long, ambitious project built in small bricks. Plan the brick, build it,
  verify it, document it, move on. Don't bite off a whole milestone at once.
- When asked to plan, plan the *next brick* concretely; keep the milestone roadmap in
  [docs/ROADMAP.md](docs/ROADMAP.md) as the map.
- Be honest in summaries: what's done, what's stubbed, what failed.
