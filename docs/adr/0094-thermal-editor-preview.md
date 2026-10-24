# ADR-0094: Authored thermal graphs and disposable editor diagnostics

Status: proposed implementation, 2026-10-18. Draft stack; no merge or milestone closure.

## Context

ADR-0091–0093 provide fixed-mass phase, contact and field references. The next
consumer must let creators author and inspect these states through the existing
editor pipeline without saving evolving solver buffers or assigning accidental
thermal authority to cosmetic fire.

## Decision

Add removable `fluids_scene` above ECS/fluids, with three independently disabled
reflected descriptions: `ThermalCell`, `ThermalContact`, `ThermalSource`. Reuse
real `ecs::Entity` references and existing scene/session remapping. The adapter
reflects material configuration; numerical fluids stays core-only.

Own one bounded graph preview outside ECS. Collect at most 16 cells, 32 contacts
and 16 sources, retaining lowest local entity keys in bounded arrays. Any
excess refuses the entire enabled graph. Validate all references, cell/material
initial conditions, undirected contact duplicates and duplicate source targets.
Do not clamp authored values or silently truncate. Invalid descriptions clear
old values; typed edits remain in the document for correction/undo.

Any change to an enabled description reconstructs the whole graph from authored
initial conditions, even while paused. Compare explicit fields by double bits
and generation-safe handles, not padding or hash alone. Ignore disabled
components and unrelated world edits. Equal descriptions preserve evolving state.
Play/Step creates disposable state, Pause freezes time, Stop resets it. Numerical
refusal retains the previous committed state and field tick, with explicit status.
A successful graph rebuild resets the field tick but does not rewind the host
clock. No automatic timestep retries or heat reservoirs.

Add optional read-only message `0x020A`, versioned and bounded independently of
component blobs. Include session, description revision, host/accepted-host/field
ticks, graph/problem and numerical-refusal diagnostics, plus finite EditorId-keyed
cell rows. Emit after each active viewport frame's PlayState, including one clear
message after Stop/disable. Empty scenes gain no diagnostic stream. Old peers
ignore unknown messages; existing handshake/schema/component layouts remain.

Rust accepts diagnostics only for its current phase/tick and a nondecreasing
session/revision, clearing old diagnostics on each PlayState. The inspector shows
selected-cell kelvin, liquid mass percentage and joules, graph status and human
refusal messages. It emits no commands. Runtime state cannot enter undo or save.

## Consequences and verification

Unit tests cover reset/edit semantics, stable NaN, whole-graph limits, recycled
handles, inverse edits, Play restoration and scene remapping, atomic numerical
refusal, and bounded/finite/exact-length cross-language codec fixtures. GUI tests
cover freshness and read-only selected-cell/refusal display. Real host smoke
checks saved-scene reload, melting/freezing on the latent plateau, Pause,
source-disabled sensitivity, numerical refusal, invalid descriptions and new Play.
Retain boundary CTest, Rust, sanitizer, SDK and Clang results separately from
these assertions. No performance or cross-platform success is inferred.

Astra supplied an independent proposal ($0.04958 response cost), not a code
review. Adopted whole-graph validation, correlation stamps and explicit stale
state tests. Declined NaN/signed-zero normalization: exact authored bit changes
have an explicit rebuild rule. Bounded lowest-key collection scans the world
instead of abandoning inspection at the first cap; it still never simulates a
truncated graph. Root owns implementation, self-review and local verification.

This adds no thermal renderer, moving water, liquid/solid mass transport, ice
geometry/collision, combustion/fuel/spread or multiplayer authority. Numeric
readouts do not close the visible-water/ice, clicked asset, export or network
proofs. M18/performance and city convergence gates remain open.
