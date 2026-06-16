# Contributing to Rime

Thank you for wanting to build an open AAA engine with us. Rime is for people who want
to ship great games **and** for people who want to *learn how engines work by reading
and writing real engine code*. Both are first-class here.

> Read [VISION.md](VISION.md) for intent and [CLAUDE.md](CLAUDE.md) for the day-to-day
> standards. This file is the contribution workflow.

## Ways to contribute

- **Code** — pick up a "brick" from the [roadmap](docs/ROADMAP.md) or a `good-first-issue`.
- **Docs & comments** — Rime's code is a textbook; improving explanations is real,
  valued work. Filling in [glossary.md](docs/glossary.md) counts.
- **Research** — extend the [engine survey](docs/research/engine-survey.md) with
  well-sourced findings about techniques we should adopt.
- **Testing, profiling, bug reports** — measurements and reproductions are gold.

Because we're pre-alpha, the **foundations** (core, platform, RHI, ECS) are where the
highest-leverage and most educational work is right now.

## Ground rules

1. **The code teaches.** Comment the *why*. If you implement a known technique, name it
   and explain the idea so a learner can follow. Don't strip explanatory comments.
2. **Respect module boundaries and the RHI seam** (see [CLAUDE.md](CLAUDE.md) and
   [ARCHITECTURE.md](docs/ARCHITECTURE.md)). Don't include Vulkan headers outside the
   Vulkan backend; don't reach across module internals.
3. **Measure before optimizing**, and put the numbers in your PR.
4. **Significant decisions get an ADR** ([docs/adr/](docs/adr/)).
5. **Be excellent to each other** — see the [Code of Conduct](CODE_OF_CONDUCT.md).

## Workflow

1. **Discuss first for anything non-trivial** (open an issue) so we agree on the
   approach before you invest time. For a roadmap brick, comment that you're taking it.
2. **Branch** from `main`: `feat/<area>-<short-desc>`, `fix/...`, `docs/...`.
3. **Build & test locally** (see [CLAUDE.md](CLAUDE.md) for commands as they come
   online). Add tests for new behavior.
4. **Format & lint**: `clang-format` for C++ (per [`.clang-format`](.clang-format));
   `cargo fmt` + `cargo clippy` for Rust. CI denies warnings.
5. **Commit** in small, focused steps. Conventional-commit prefixes encouraged:
   `feat:`, `fix:`, `docs:`, `refactor:`, `perf:`, `test:`, `build:`, `chore:`.
6. **Add the SPDX header** to every new source file:
   `// SPDX-License-Identifier: Apache-2.0` and the copyright line (see CLAUDE.md).
7. **Open a PR** describing *what*, *why*, how you tested, and any measurements. Link the
   issue/brick. Note anything left as a stub.

## Developer Certificate of Origin (DCO)

Contributions are accepted under [Apache-2.0](LICENSE). Sign off each commit
(`git commit -s`) to certify you have the right to submit it under that license — this
adds a `Signed-off-by:` line. By contributing you agree your work is licensed under
Apache-2.0.

## Review

Reviews look for: correctness, that module/RHI boundaries hold, performance sanity,
tests, and — specific to Rime — **whether the code and comments teach well**. Expect
questions; they're how we keep the codebase legible.

## A note on scope and pace

Rime is enormous by design, and we build it **brick by brick**. Small, complete,
well-explained contributions beat large unfinished ones. Don't be intimidated by the
size of the vision — the whole point is that many hands, each laying a few crystals,
build the frost.
