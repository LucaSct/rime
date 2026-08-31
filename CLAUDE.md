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
5. **Any per-peer "what they have" fact may only strengthen on confirmed *holding*** — never on
   "we sent it", "it arrived", or a proxy that has a blind spot. This has been the same bug five
   times across m11.3–m11.5 (watermarks, baselines, per-item announced/relevant arrays). Read
   [docs/design/replication.md](docs/design/replication.md) before touching replication state, and
   give every skip/drop/defer path a counter — a proof that cannot see what it skipped still reads
   as passing.

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

### Verification rhythm (calibrated for cost)

Verify proportionately to the change — don't re-run the whole world for every edit:

- While iterating a brick, run the specific test(s) it touches. Run the full `ctest`/`cargo` suite
  and the sanitizers (ASan/UBSan; TSan for threading) **once** at the brick boundary, before
  commit/PR — not after every edit. A green targeted test is sufficient; trust it.
- Match depth to risk: docs/one-liners need little; threading, buffer/vertex layouts, and FFI earn a
  sanitizer pass. Still exercise every change end-to-end at least once.
- GPU proofs are **structural** — properties the physics guarantees, checked with margins on
  lavapipe — never golden images (the M5.6/M6.4 pattern in `tests/render/pbr_pipeline_test.cpp`).
- **Judge a build by its EXIT STATUS, never by grepping its output.** `cmake --build ... | grep -E
  "error:"` is the habit to break: glslang reports a failed shader compile as `ERROR:` and ninja as
  `FAILED:`, so a broken build passes the grep, ninja leaves the previous binaries in place, and the
  tests that follow run green **against a stale executable**. In m15.6 that hid a one-line shader
  error (`'alpha' : redefinition`) through an add, a run, and two falsification attempts — the
  "proof" and both "it still passes when broken" results were all measured on an hour-old binary.
  Run the build as its own command and check `$?`, or grep `-iE "error|FAILED"` at minimum. Doctest
  makes the same failure quiet: a test case that did not compile in is *skipped*, not missing, and
  `-tc="name*"` reporting `0 passed` is what tells you.
- **A new translation unit gets a `clang++ -fsyntax-only` pass before it is pushed.** The local
  build and every CI job except one are GCC or MSVC; **the only Clang build in the matrix is the
  TSan job**, which sits behind the slowest matrix entry. m12.0-perf shipped a recursive type
  holding a `std::vector<std::pair<std::string, T>>` — legal-looking, accepted by libstdc++, and
  ill-formed (`std::vector` permits an incomplete element type; `std::pair` does not). A
  GCC-only loop cannot see that class of bug. Reconstruct the flags from
  `build/dev/compile_commands.json` so the check uses the real include paths.
- **A `.ps1` is only verified by RUNNING it under `powershell.exe`.** The default Windows shell is
  still PowerShell 5.1, which reads a BOM-less script in the machine's ANSI codepage (1252 on a
  Western install) rather than UTF-8. An em dash's third byte decodes to U+201D, which PowerShell
  honours as a double-quote delimiter: one em dash inside a double-quoted string truncates it, the
  quote meant to close it opens a runaway string that swallows the lines below, and the parse error
  is reported against an innocent line further down. `scripts/setup.ps1` was therefore unable to
  start from M0.4 (2026-06-17) until 2026-08-31, while its own header claimed CI exercised it — the
  Windows job runs `build.ps1` under `shell: pwsh` (PowerShell 7, UTF-8 regardless) and no job runs
  `setup.ps1` at all. `.ps1` files are ASCII-only now and the lint job greps for it. The trap within
  the trap: `[Parser]::ParseFile()` reports ZERO errors on a file `powershell.exe -File` refuses to
  run, because it decodes as UTF-8 — a parse check is not the proof, the same way grepping a build
  log is not the proof. Run it.
- **A red `format, lint & license` on a PR that touched no Rust is probably not yours.** CI pins
  `dtolnay/rust-toolchain@stable`, which **floats**: a new Rust release can start rejecting code
  that has been on `main` for a milestone (1.98.0 did, twice, on 2026-08-20). Read the failing
  STEP before believing the diff caused it, and `rustup update stable` to reproduce locally. Same
  rule as the SVT-AV1 ASan flake: read the log, then blame the PR.

### Brick delivery

Per-brick: own branch → build → lavapipe green → small commits → push → PR → CI (3-OS + format +
ASan/UBSan + TSan) → merge. **A perf-touching brick also commits a `docs/perf/` run**
(`scripts/perf.sh --commit`, Release, on hardware — ADR-0035 §2c): the regression gate is only as
good as the freshness of what it compares against, and putting the report in the diff is what makes
an absent measurement visible in review rather than merely absent. Stacked PRs are
**pre-retargeted to `main` before parents merge**
(`gh api repos/LucaSct/rime/pulls/<N> -X PATCH -f base=main`). **Drive PRs through the REST API**
rather than `gh pr edit`/`gh pr checks`: those wrappers need `read:org`, and whether you have it
depends on how this machine authenticated — an interactive `gh auth login` grants it, a bare
`GH_TOKEN`/PAT generally does not. The REST path works under either, so it is the one to reach for
by default. `gh auth status` prints the scopes you actually hold. **Run clang-format before
pushing** — it lives on the dev server at `~/.rime-tools/bin/clang-format` (v20.1.8, the exact
pinned CI version). Mirror CI's exact file set — `find`, **not** `git ls-files` (which skips
*untracked* new files and so silently misses a brand-new source file, the trap that red-CI'd M6.8):
`~/.rime-tools/bin/clang-format -i $(find engine tests \( -name '*.cpp' -o -name '*.hpp' -o -name
'*.mm' \))`. CI's format job is the backstop, not the first line of defence — skipping the local run
cost M6.3, M6.4 and M6.8 a red-CI round-trip each.

**Landing a stack: every child conflicts the moment its parent merges.** We merge to `main` by
**squash**, so `main` ends up holding the parent's *squashed* equivalent while each child branch
still carries the parent's *original* commits. Git's three-way merge then falls back to the
pre-stack merge base, sees both sides rewriting the same regions differently, and reports a
genuine conflict — with no rebase or force-push having happened. `mergeable: true` across the
whole stack before you start is therefore worthless: it is only ever true of the *next* PR.
Landing #114–#120 hit this five times in a row. Per child, after each parent merges:

```bash
git rebase --onto origin/main <old-parent-tip> <child-branch>
git diff <child-original-tip> HEAD          # MUST be empty — see below
git push --force-with-lease origin <child-branch>
```

That `git diff` is the load-bearing step, not a sanity check. `main`'s tree after the squash is
byte-identical to the old parent tip's tree, so replaying only the child's own commits is
*provably* tree-preserving — and an empty diff **carries the child's existing green CI onto the
new SHA**, because an identical tree cannot test differently. Check it; never assume it. Also
pass an explicit `commit_message` when merging: the repo's `squash_merge_commit_message` is
`COMMIT_MESSAGES`, which on a stacked PR concatenates all 10–17 commits, parents included.

One consequence worth expecting: `concurrency: ci-${{ github.ref }}` with `cancel-in-progress`
means each merge's `main` run is cancelled by the next merge, so a 7-PR stack leaves six
cancelled `main` runs and one completed. The tree-identity argument above is then the only thing
standing behind those intermediate commits.

### Recording conventions

When a pattern or gotcha recurs, record it without being asked — project conventions here, richer
context in the session memory. (A cross-project working-style file lives at `~/.claude/CLAUDE.md`.)
