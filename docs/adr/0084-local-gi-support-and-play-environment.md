# ADR-0084: finite local GI, stable quadrature, and the playable environment

- Status: Accepted for the opt-in playable scene
- Date: 2026-10-15
- Extends ADR-0032, ADR-0081, and ADR-0083

## Context

The houses continued changing brightness after the fast flicker fixes. In the actual
evening scene, 4,629 fixed facade points reprojected across frames measured mean
consecutive HDR luminance change 0.004562 while stationary. DDGI off reduced this to
0.000363; SSR off left 0.004130; sun shadows off left 0.004608. Raising hysteresis
from .85 to .97 helped stationary noise but did not fix the much larger jump when
the lattice scrolled: 0.083475 over one 2 cm camera step. The scroll retained 336 of
384 histories and primed only 48. Preserving history was insufficient because the
surface samplers extrapolated arbitrarily distant receivers onto a changing edge
plane. This was a spatial-support problem as well as temporal sampling noise.

The owner also requested working held aim, corrected fingers, textured houses,
trees, grass/dirt, clouds, and reflective water. These remain sample content; they
do not change the replicated destruction workload in headless/performance modes.

## Decision

Two appended `LightingSettings` opt-ins preserve existing authored-volume behavior:

- `ddgi_volume_fade` gives a scrolling local field finite influence. Both diffuse
  and SSR readers use `ddgi_support.glsl`: for spacing `s`, half extent
  `H=(dims-1)*s/2`, support ends at `H-s-margin` from the continuous camera position,
  and fades inward over one cell. The margin reserves the SSR surface bias. A ramp
  against snapped atlas bounds would itself jump. Thin grids with no safe interior
  return fallback when opted in; legacy clamp-to-edge remains the default.
- `ddgi_stable_rays` keys a Fibonacci quadrature rotation to the **world probe
  coordinate**, independent of frame and atlas slot. Every scheduled probe still
  traces every update; lighting/geometry changes remain live. This trades temporal
  convergence for fixed spatial integration error. Small openings can be missed by
  a finite ray set; the sample uses 128 rays instead of 64 to reduce that error.

Coverage, spatial influence, and measured radiance are distinct. A covered black
field stays black in its full-strength core. Diffuse blends one fallback (sky SH,
otherwise flat ambient) with one measured GI term, then applies albedo/AO once.
SSR blends its existing environment fallback with GI while retaining its existing
hit/Fresnel policy. At the finite boundary, fallback can brighten an enclosed room;
occluded far-field GI or multiple volumes remain later work.

Play uses an 8³ grid at 4 m spacing: all 512 probes fit the existing update budget,
with enough reach to influence the visible street facades after the finite fade.
The original 8×6×8/2 m volume was too small once unlimited extrapolation was removed.
These settings apply to play and its visual proofs. Other callers keep their settings.

`PlayEnvironment` generates 24 mipmapped albedo/height, normal, and roughness maps
once from fixed seeds. It updates existing palette material IDs, so destructible
leaves inherit the look without changing replication. Dominant-face planar UVs
are baked in original slab coordinates before COM rebasing, preserving phase across
intact chunks and following the geometry after fracture. Terrain weights use global
coordinates at every LOD; collision heights are untouched.

The scene adds 26 instanced trees with branching trunks and alpha-masked leaves,
bounded merged grass patches, dirt paths/verges, seven irregular puddles, a park
pool, and a broken cloud deck. Water is opaque surface shading with ripple normals,
screen-space object reflections, and the existing environment fallback. It is not
refractive water or a planar reflection camera. Scenery is visual dressing; trunks
are not new collision bodies. Clouds use the existing static slab model.

Held right mouse and Q aim all three weapons, including in drag-look fallback.
Rifles align sights without changing the parent FOV. MSR apparent magnification is
computed through the physical aperture: `(26mm/90mm)/tan(optical_half_fov)=5`.
The old fivefold feed focal ratio only produced about 2.36× once fitted into that
aperture. Opposed thumbs and weapon-specific wrist/index poses are authored in
the editable rigs; runtime IK continues solving the full wrist frames.

## Verification and consequences

Injected-atlas SSR tests distinguish covered black, absent coverage, remote
receivers, and continuous camera movement. Stable-ray tests use zero hysteresis,
repeated live updates, world-probe remapping, and light-change sensitivity.
The existing atlas-remap, GI response, shadows and reflection controls remain.

`--environment-smoke` runs without downloaded art. Actual texture removal, SSR
removal on projected puddle pixels, and cloud removal must visibly affect their
captures above unchanged controls. `--viewmodel-smoke` also exercises the live
input helper, all aim poses, apparent magnification, and real weighted arm meshes.
The native environment ships with the repository; licensed weapon/arm artwork and
editable Blender rigs remain outside git as before.

The second optical view renders the same dressing. Additional geometry, texture
memory, ray count and a broader probe lattice have costs; no unchanged-frame-time
claim follows from visual or sanitizer checks. Hardware measurement is recorded
separately in `docs/perf/`.
