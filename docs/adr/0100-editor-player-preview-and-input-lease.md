# ADR-0100: Editor player preview and input lease

- Status: Accepted for the local development city
- Date: 2026-10-19
- Extends: ADR-0031, ADR-0098 and ADR-0099

## Context

The city editor could simulate sources and fly its camera, but could not walk a
map before launching the full weapon runtime. Possession needs explicit input
ownership, collision admission and teardown: an inferred mouse grab or a restored
camera with stale movement would make Preview unpredictable.

## Decision

The app host accepts optional sample-owned EditorPlayHooks. Begin borrows the live
world/physics and admits or refuses a session; fixed Tick and lifecycle callbacks
own transient sample state. Stop cleanup runs before the borrowed physics world
is destroyed. Existing hosts require no hooks and keep Inspect Preview.

Two optional messages advertise capability and acknowledge mode, phase, session,
capture and refusal reason. Commands include the current session generation.
Only a matching Active/Playing Player capture acknowledgement permits GUI key/look
forwarding or cursor confinement. Capture uses relative motion accumulated into
a fresh virtual pointer origin, reusing the existing input wire. Esc, focus loss,
Pause, modifiers, panel closure, disconnect and Stop release every held key and
capture. Resume does not respawn or recapture. Stale session commands are ignored.

The city adapter reuses the query-only gameplay character step, look and eye
helpers. Exactly one enabled PlayerSpawn and one active unparented camera are
required. Transient static collision uses the authored GroundSurface, existing
physics bodies, known CookSpec dimensions, generated city convex parts and cube
props with validated rotation/positive scale. It has explicit body/hull/pattern
bounds. Unsupported collision geometry refuses admission instead of supplying
an invisible fallback floor. Explicit colliders take precedence over visual roles.
Phase sheet is a new appended SlabRole kind 13: the two showcase water/ice sheets
are cosmetic; a ThermalSurface on an ordinary wall never removes its collision.
The initial penetration check uses existing physics queries, including their
known shallow-overlap limitations on very large boxes.

PlaySession owns the complete reflected baseline. Only the camera pose is changed
by the adapter. Stop restores the source snapshot and releases all owned bodies
before hulls. Player editing and saving are disabled in the GUI and refused by the
server because transient static collision cannot track arbitrary authoring edits.
Inspect retains live edits and saving. Step from Pause advances one fixed tick.
The editor stream remains 960×540.

## Consequences

Creators can walk and jump in the saved city without weapons or a separate game
window. The real-host proof compares exact serialized snapshots before Play and
after Stop; UI tests prove capture intent/acknowledgement and input release.
Walking is not swimming, ice physics, dynamic-body pushing or network authority.
Terrain heightfield queries, arbitrary imported-mesh collider cooking, parented
collision, general input/action authoring and weapon possession remain separate.

The companion renderer leaf caches five full texture/sampler handle pairs only
within one record_draws invocation. First attachment always binds, including null;
no pipeline/callback crosses the lifetime. Draw ordering, UBO offsets, descriptors
and GPU draw counts are unchanged. Fixed-state controls and integrated Vulkan
renderer tests verify behavior; hardware receipts retain qualification failures
and do not establish an FPS improvement or relax existing budgets.
