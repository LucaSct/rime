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
