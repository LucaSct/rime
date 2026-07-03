# Architecture Decision Records (ADRs)

An **ADR** captures one significant technical decision: the context we were in, what we
decided, and the consequences we accepted. They explain *why the engine is the way it
is* to anyone who arrives later — including our future selves.

## Rules

- **Append-only.** Never rewrite history. If a decision changes, write a *new* ADR that
  supersedes the old one, and mark the old one `Superseded by ADR-XXXX`.
- **Numbered** sequentially: `NNNN-short-kebab-title.md`.
- **Small and honest.** Record the real trade-offs, including what we gave up.

## Format

```
# ADR-NNNN: Title
- Status: Proposed | Accepted | Superseded by ADR-XXXX
- Date: YYYY-MM-DD
## Context      — the forces at play, constraints, what we knew.
## Decision     — what we chose, stated plainly.
## Consequences — what follows (good and bad), what we now must live with.
## Alternatives considered — what else we weighed, and why we passed.
```

## Index

- [ADR-0001](0001-cpp-core-rust-tooling.md) — C++ core + Rust tooling
- [ADR-0002](0002-vulkan-first-rhi.md) — Vulkan-first behind a thin RHI
- [ADR-0003](0003-apache-2-license.md) — Apache-2.0 license
- [ADR-0004](0004-math-conventions.md) — math conventions (float, column-major, RH, Vulkan clip space)
- [ADR-0005](0005-rotation-representation.md) — rotations as unit quaternions (Hamilton, RH, active)
- [ADR-0006](0006-native-windowing.md) — native windowing & input (no GLFW/SDL)
- [ADR-0007](0007-vulkan-backend-bootstrapping.md) — Vulkan backend bootstrapping (volk + VMA, 1.3 baseline)
- [ADR-0008](0008-offline-shader-compilation.md) — offline GLSL→SPIR-V shader compilation
- [ADR-0009](0009-swapchain-and-presentation.md) — swapchain, presentation & frame pacing
- [ADR-0010](0010-textures-and-descriptors.md) — textures, samplers & the descriptor model
- [ADR-0011](0011-depth-attachment.md) — depth attachment & the depth test (pulled ahead of M5)
- [ADR-0012](0012-push-constants.md) — push constants for small per-draw data (MVP, …)
- [ADR-0013](0013-3d-textures.md) — 3-D (volume) textures (field colormaps; pulled ahead of M5)
- [ADR-0014](0014-stencil.md) — stencil state (for the cross-section cap)
- [ADR-0015](0015-imgui-free-ui.md) — a from-scratch, imgui-free immediate-mode UI
- [ADR-0016](0016-editor-is-a-client-of-the-engine.md) — the editor is a client of the engine (+ parallel-path rules)
- [ADR-0017](0017-streaming-codec.md) — the S0 streaming codec (JPEG on the wire, LZ4 for lossless)
