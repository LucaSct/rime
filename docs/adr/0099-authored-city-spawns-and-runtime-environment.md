# ADR-0099: Authored city spawns and runtime environment

- Status: Accepted for the local development city
- Date: 2026-10-19
- Extends: ADR-0097 and ADR-0098

## Context

A saved scene's camera is an editor view, while the full city previously started
the player at its hardcoded west viewpoint. Sources visible in the editor were
not consumed by the full runtime, which had an independent opt-in scripted FX
demonstration. These gaps made scene authoring fail to carry through Run game.

## Decision

The game and city host share registration of the sample-owned reflected
`rime::block_play::PlayerSpawn { enabled=false }`. Exactly one enabled marker
defines the avatar capsule centre in world metres and initial view yaw/pitch.
Roll is ignored; scale never sizes the avatar. The resolver copies placement
data before gameplay starts. It composes authored parent transforms, with an
explicit 64-ancestor limit; invalid references, nonfinite poses, non-unit or
degenerate rotations, vertical look ambiguity and multiple enabled markers
refuse startup with a reason. Serialized unit rotations admit rounding tolerance.
No marker retains the existing default spawn, view and recovery behavior.

The server places the avatar from its own resolved description, and the client
initializes its local look from the same map. Terrain preparation and recovery
use that spawn. Resolution diagnostics are distinct from observing actual
authoritative/predicted poses after the existing local startup handshake. This
does not create team spawns, spawn selection, overlap validation or a replicated
dynamic spawn service. Registry compatibility uses the existing schema hash.

A sample-owned `AuthoredEnvironment` consumes existing reflected VFX and thermal
descriptions through the same Effects/ThermalPreview/PhaseDetail adapters as the
editor. Fixed client ticks advance cosmetic state; rendering reconciles and
copies bounded rows without advancing time. Legacy scripted smoke and authored
smoke share a bounded 256+256 list. Authored flames/lights/surfaces keep their
existing 48/16/16 limits. Main and scope renderers consume the same publication.
The immutable phase atlas is allocated only when a validated enabled surface
needs it, and its owner outlives both borrowing renderers and their submitted
work. Inert scenes allocate no atlas or FX pass and retain legacy toggle ownership.

These runtime effects remain client-local presentation. Thermal state does not
apply forces, heat gameplay objects or decide collision/damage; no replicated
combustion or moving-water authority is implied. The city's existing flat
WorldTransform preparation remains the source pose contract for environment
rendering; this brick does not repair the general hierarchy/physics pipeline.

## Consequences

Creators can place an enabled spawn and enable smoke/fire/thermal descriptions,
save, then run/export the same map. The Outliner identifies player spawns and
offers a Gameplay filter. The fixed source defaults remain disabled. Authoring,
save/load, refusal, real runtime source counts, exact renderer clearing and
isolated export require separate witnesses. Existing user scenes are preserved;
demo experiments use copies. Production compatibility, multiplayer effects
authority, performance/collapse gates and in-viewport player possession remain open.
