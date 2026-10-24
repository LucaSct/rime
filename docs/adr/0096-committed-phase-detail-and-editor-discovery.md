# ADR-0096: Committed phase detail and editor discovery

Status: proposed opt-in implementation, 2026-10-19. Extends ADR-0095; no physical
water/ice, gameplay authoring, performance or shipment gate closure.

## Context

The user found opaque flat ice concrete-like and asked for modest waves/motion,
editor navigation, source selection and an explanation of professional workflows.
A day-end demo needs recognizable phases without a new deformation/rendering ABI.
Right mouse was released before WASD: no camera translation defect was reproduced.

## Decision

Extend copied whole-entity SurfaceAppearance rows with an explicit optional detail
map pair (borrowed base-colour/normal textures) and bounded normal scale. False
inherits authored maps; true with invalid handles explicitly selects the existing
white/flat fallbacks. Apply only to private resolved material copies, including
all submeshes. Other authored factors, geometry and shader layouts remain unchanged.
The caller owns resource validity/lifetime, like existing PBR texture descriptors.

A removable PhaseDetail owner above render/fluids_scene uploads 64 immutable
128² looping water normal frames, a static frost normal, and nine static tint maps
whose veins fade with liquid mass fraction. Total 74 maps with complete mip chains,
approximately 6.5 MB before backend overhead. Maps never change in flight; startup
uses existing blocking RHI uploads. Partial creation is never published and falls
back to factors only. RHI upload itself has no error return; do not claim injected
upload-failure coverage. Destroy after renderer references and GPU work: declare
the owner before the renderer; its cold destructor waits idle before releasing maps.
The host initializes it once, even for scenes without active thermal bindings;
it is removable, but its startup allocation is not zero-cost/default-off.

The preview holds a cosmetic accepted-duration accumulator outside authored/wire
state. Accepted steps add requested dt; relevant graph rebuild/Stop reset it to
zero. Pause and numerical failure retain it. It is explicitly not a thermal
receipt or energy oracle. Animation reads this clock with the same committed
fraction; it does not use wall clock or assume fixed timestep/host tick continuity.
At zero liquid, the selected frost map is static: water ripple amplitude is zero.
The normal scale for that different static material is 0.35. No moving geometry,
flow, refraction, solid volume, crack physics or collision is inferred.

Add wheel dolly along the sight line and visible right-mouse-held navigation help.
Continue translation during an owned right drag even if its hover flag drops.
The Outliner gains Effects/Thermal filters using reflected component identity;
semantic role names outrank MeshRef/MaterialRef regardless of component order.
Small code-native vector icons (20 px list / 32 px tiles) support a Display →
List/Tiles switch; icons and labels both select, with accessible icon buttons.
Selection/undo remains generation-safe and filtering never mutates the world.
Assets identifies the component path for creating effects. A documented creator
workflow separates current controls from future prefab/event/animation/export UI.

## Independent consultation and verification

Astra was asked for its own minimal design, cost $0.03019. Adopted immutable maps,
private per-draw overrides, lifetime/fallback semantics and a committed-duration
clock rather than host-tick multiplication. It recommended deferring base maps
unless normal detail stayed ambiguous; root chose a bounded nine-map static tint
ramp because ice veins need visible contrast under ambient lighting. Local images
confirm recognizable veins and moving water detail. Root owns edits and review.

Structural checks cover real forward/G-buffer motion sensitivity, both triangles
of a multi-submesh plane, exact paused/refused/frozen frames, shared neighbor
isolation, factor restoration and invalid scale. CPU checks use variable dt and
nonconsecutive host ticks. GUI checks cover the held right-drag, wheel routing,
filter reset and moving/undoing a bodyless source. Existing source/thermal editor
smoke remains the real-host lifecycle boundary. No depth/shadow geometry or shader
code is changed, but no new independent depth/shadow readback proof is claimed.
See the verification record and handoff for final checks and limitations.
