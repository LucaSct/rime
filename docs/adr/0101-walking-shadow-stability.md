# ADR-0101: Stable walking shadows and depth-aware local caches

- Status: Accepted for the development renderer
- Date: 2026-10-19
- Extends: ADR-0032, ADR-0078 and ADR-0100

## Context

Walking and turning changed the directional shadow texel grid despite its nominal
sphere fit and centre snapping. The measured 600-pose city-coordinate sweep moved
a fixed world's shadow-UV phase by up to 0.425367355 texel and the XY scale by
0.000325352 relatively. Cascade selection also switched abruptly between different
resolutions. Stationary spotlight maps only knew explicit invalidation/light edits,
so moving or removed world geometry could retain old silhouettes. Editor lighting
only enabled clustering, leaving sun/local shadow gates off unlike the full game.

## Decision

Fit each cascade sphere analytically in camera-local space using lens/splits,
transform only its centre to world space, and retain one fixed light basis. Round
the centre to texels, with a half-texel extent guard; tiny resolutions skip snapping.
The same sweep now measures 0.000122070 texel maximum residual and zero XY scale
variation. These are fixed-world grid measurements, not screen-image constancy
while a camera or light moves.

Mesh and terrain receivers include one sun-shadow implementation. Derive its
sampling guard from the configured PCF footprint. Near a map boundary, compose
its visibility over the already-composed coarser result, including overlapping
fade bands. Interior receivers keep one nine-tap PCF sample. No map-resolution,
shadow-bias or PCF-radius increase and no screen blur is used. A GPU wrapper of
the production function tests full-contrast disagreement, depth boundaries,
overlapping bands and enabled/disabled controls. Original hard switching and an
intermediate raw-coarse blend fail the corresponding continuity bounds.

A private SceneRenderer caster tracker keys the complete world draw set by full
entity generation and mesh subrange. Models, silhouette/cull flags and masked
alpha/cutoff/full texture handles determine depth identity. Additions, changes and
removals invalidate both old/new conservative regions. Camera culling partitions
rather than deletes the set, and list reorder or opaque cosmetic changes leave
static maps cached. Unknown deformation bounds and ambiguous identities refresh
all local slots conservatively. Re-enabling local shadows invalidates retained
maps before restarting tracking, including an empty world. Explicit C2/regional
invalidation remains available for external changes such as in-place GPU uploads.

Derivative-dependent material reads/normal filtering run before alpha discard,
keeping cutout foliage's pixel footprint defined. A camera-free render breaks
world TAA/motion history as well as first-person history. The native editor now
enables authored directional and spot shadows; its stream stays 960×540.

## Verification and limits

Stable-phase/scale/coverage tests include walks, turns, translated scene copies,
multiple suns/counts/resolutions and deliberately broken projection controls.
GPU checks cover unchanged-map reuse, camera-only draw reorder, old/new moving
rigid-caster shadows, removal, alpha-textured double-sided foliage and cascade
handoffs. These synthetic casters exercise the common world-mesh paths; they do
not establish that every tree/person asset or skeletal animation system exists.
The actual editor and full city use the shared renderer.

SSR reconstruction, velocity sign, jitter and widened-roughness propagation were
audited; the tracing algorithm is unchanged. Binary screen-space hit/miss popping,
previous-depth disocclusion rejection, point-light cube shadows, terrain casting,
full-body first-person actors and volumetric particle shadows remain separate
capabilities/limitations. The existing local-light off gate also removes spot
shading; a spotlight control therefore keeps the light on and explicitly refreshes
a caster-free reference. Its sun-only counterpart uses a shadows-off control.

Astra independently reviewed the paths and caught the overlapping-band defect in
an intermediate sampler; root corrected it and added a failing-before GPU case.
Claude implemented the bounded stable-fit leaf. Consultation availability and
integrated run evidence are recorded with this brick, not assumed from model names.
Performance measurements and inherited collapse/shipment gates stay separate.
