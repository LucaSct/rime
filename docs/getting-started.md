# Getting started

From a fresh clone to a game you can walk around in. Two commands, then a third.

This page exists because it was missing: ADR-0038 named the on-ramp as m15.7 and the repository
shipped fifteen milestones of engine with no page telling a newcomer how to build it. If anything
here does not work on your machine, that is a bug in this page — the commands below are the ones CI
runs.

## 1. Toolchains

```bash
scripts/setup.sh
```

Safe to run repeatedly: it only acts on what is absent. It installs, user-local and reversibly,
**Conan** (into an isolated venv at `~/.rime-tools`) and **Rust** (via `rustup`, stable). It
*checks* — and tells you how to install — **CMake ≥ 3.24**, **Ninja** and a **C++20 compiler**,
because those are system packages and guessing your package manager is not this script's job.

**You do not need the Vulkan SDK to build.** Conan supplies the headers, volk, VMA and glslang
([ADR-0007](adr/0007-vulkan-backend-bootstrapping.md)). A Vulkan *runtime* is needed only to actually render:
a GPU driver, MoltenVK on macOS, or a software ICD — **lavapipe** — on a machine with no GPU. Every
rendering test in this repository is expected to pass on lavapipe, which is why CI can gate on them.

## 2. Build and test

```bash
scripts/build.sh
```

One command: `conan install`, configure, build the C++ engine, build the Rust tools, run both test
suites. Debug by default.

```bash
scripts/build.sh --preset release --no-tests      # optimised, build only
scripts/build.sh --cpp-only --sanitizer address   # ASan + UBSan, as CI runs it
scripts/build.sh --cpp-only --sanitizer thread    # TSan
scripts/build.sh --clean                          # start the preset over
```

The raw CMake presets also work, but only *after* a `conan install` has generated the toolchain in
`build/<preset>/` — which the script does for you:

```bash
cmake --preset dev            # configure (Ninja + the Conan toolchain)
cmake --build --preset dev    # build
ctest --preset dev            # run the C++ tests
```

**Judge a build by its exit status, never by grepping its output.** glslang reports a failed shader
compile as `ERROR:` and ninja as `FAILED:`, so `| grep "error:"` lets a broken build through — and
ninja leaves the previous binaries in place, so the tests that follow then run green against a stale
executable. This has cost this repository real time; the rule is in
[CLAUDE.md](../CLAUDE.md#verification-rhythm-calibrated-for-cost).

## 3. Play something

```bash
build/dev/bin/hello_game --windowed     # WASD to move, Esc to quit
```

[`samples/hello-game`](../samples/hello-game) is the smallest complete game here: walk an arena,
touch five markers, push a crate, win. **Read it first.** Every other sample demonstrates a
subsystem — a triangle, a render graph, a physics scene — and this one shows the shape of a *game*:
its rules live in one class with three methods and know nothing about devices, windows or frames.

No GPU, or working over SSH? Everything still runs:

```bash
build/dev/bin/hello_game --verbose      # the GPU-free self-check, with a report
build/dev/bin/hello_game --headless     # render off-screen and check the pixels
```

## Where to read next

| If you want to | Read |
| --- | --- |
| understand the intent | [VISION.md](../VISION.md) — it outranks every other document on questions of intent |
| understand the shape | [docs/ARCHITECTURE.md](ARCHITECTURE.md) |
| see what is built and what is next | [docs/ROADMAP.md](ROADMAP.md) |
| know why something is the way it is | [docs/adr/](adr/) — append-only decision records |
| decode a term | [docs/glossary.md](glossary.md) |
| run the other samples | [samples/README.md](../samples/README.md) |
| contribute | [CONTRIBUTING.md](../CONTRIBUTING.md) |

## Conventions you will meet immediately

- **Everything goes through the RHI.** Vulkan headers are included in exactly one place, the Vulkan
  backend. Graphics code targets `rime::rhi` interfaces — that is what lets D3D12/Metal land later
  without a rewrite.
- **Comments explain *why*.** This codebase is also a textbook; when a file implements a non-obvious
  technique it names the technique and the idea behind it. Do not delete explanatory comments to
  "clean up".
- **A milestone is done when its proof runs**, never when it compiles. Most proofs are samples, they
  are self-checking and headless-capable, and CI gates on them. A sample that can only be judged by
  eye is not a proof.
- **Every skip, drop and defer path gets a counter.** A proof that cannot see what it skipped reads
  exactly like a proof that passed.
