# ADR-0090: Reuse reflected scene components for environment authoring

Status: proposed implementation, 2026-10-18.

## Context

Smoke and fire have bounded numerical/presentation seams, but the preview's
sources are sample scripts. The owner requested the next water, smoke, fire and
ice bricks and their editor pipeline, and selected temperature-driven freezing
and melting as ice's first result. The M18 and multiplayer gates remain open.

## Decision

Use the existing reflected component → editor schema → typed inspector commands
and undo → engine-owned `.rscene` save/load path. Start with independently
disabled `SmokeEmitter` and `FireEmitter` descriptions in a removable
`vfx_scene` adapter above ECS and VFX. Do not introduce an alternative effect
graph, binary asset format or a shared water/smoke solver for these sources.

The host owns one bounded, disposable presentation consumer. Only Play/Step
advances particles; Pause reconciles removals without time advancement; Stop
discards presentation. Derive missing world transforms after successful edits,
so a source spawned through the editor works before a save/reload cycle.
Fire light rows are copied into rendering, never written into saved ECS state.
These are cosmetic source descriptions, not authoritative ignition or heat.

Record ice as **M-FL2 Temperature-driven freezing and melting**, independently
verifiable from M-FL1 buoyancy. Its next design/CPU brick is an energy/phase
oracle, followed by conservative heat exchange, coupling to water mass and
transport, visible phase/thickness, and editor controls. The detailed numerical
contract and material parameters must be researched and reviewed before that
solver is implemented. This does not make the existing basin freeze.

## Consequences and verification

Reflection supplies ordinary fields without Rust per-component special cases.
Runtime pools, RNG state, light rows and tick clocks stay out of scene saves.
One preview admits 16 sources and 256 particles; invalid, capped and discarded
work has counters. The adapter itself mutates no ECS data. A game's authority
consumer can later use canonical network identities without changing VFX's
core-only boundary.

Tests cover inert defaults, inverse edits, engine-owned save/reload into a fresh
world, no baked light, strictly increasing ticks, paused removal/extinguish,
Stop/restart, generation replacement, transformed bounds, moving-source emission
debt and observable caps. A software-Vulkan light test proves a visible response
and exact clearing with temporal accumulation disabled. Clicked authoring,
enabled-scene hardware capacity, export/play integration and network relevance
remain distinct evidence; this brick does not waive them.
