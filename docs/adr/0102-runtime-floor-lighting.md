# ADR-0102: Receiving-side diffuse GI and bounded city shadows

- Status: Accepted for the development renderer
- Date: 2026-10-19
- Extends: ADR-0101

## Context

The owner still observed changing floor darkness in Run game after ADR-0101;
the editor looked fine. The runtime enables SDF/DDGI and SSR, unlike the editor.
Real RTX 3060 window recordings and replicated-city captures show a sharp floor
band that remains with SSR disabled and disappears with DDGI disabled. Settled
world-anchored yaw samples at the basin spawn were already stable; those samples
alone do not reproduce the reported motion symptom.

The forward diffuse query samples exactly on the receiving surface. At a probe
plane, the next layer has zero trilinear weight. Tiny accepted contributions can
normalize to a full estimate, while no accepted support uses a different fallback.
The observed band implicates forward DDGI; the runtime visibility atlas was not
read back, so the precise rejected/accepted-probe mechanism remains an inference.

Run game also uses a 3072-unit terrain far plane for its cascade fit. With the
current lens/splits this gives roughly 0.54-unit near-shadow texels and a 2.48-unit
constant depth offset. This is a separate local-shadow quality defect.

## Decision

Query forward diffuse GI along the unperturbed interpolated surface normal by
`max(0.01, probe_spacing * 0.025)` world units, matching the existing SSR bias
formula. Keep the finite support envelope at the actual receiver position. This
does not change atlas contents, visibility thresholds or covered-black semantics.
Legacy authored-volume sampling also uses this receiving-side policy.

Add `LightingSettings::shadow_max_distance`, default zero. A finite value beyond
the camera near plane caps cascade coverage at the lesser of that value and the
camera far plane. Unset/invalid values preserve existing fits. The city runtime
uses 160 units while terrain rasterization retains 3072; map resolution, count,
bias and PCF settings are unchanged. No serialized scene contract changes.

## Acceptance and limits

In matched 1280×720 city captures, the maximum adjacent-row mean encoded-luma
step across the original floor band falls from 0.0412105 to 0.000644755 with all
features enabled; SSR-off controls fall from 0.0468022 to 0.000273772. These are
local image measurements, not physical radiance or a proof of all temporal errors.
Actual before/after walking and turning videos are retained separately.

Production forward-pass tests render real floor/ceiling geometry against fixed
real-format probe atlases. They cover exact/near plane positions, anchored yaw,
authored/local modes, covered black, absent support and positions beyond the bias.
The original fragment shader fails the bright receiving-side checks while the
occlusion controls pass. Shadow-range tests preserve default fits bit-for-bit and
compare a capped long-range camera against an uncapped short-range reference.

The bias has no local-clearance knowledge: narrow gaps, very small spacing and
interpolated/backface normals remain limitations of this policy. Existing closed
room/leak regressions remain required. SSR hit/miss changes, camera-relative GI
support, approximate sample SDF proxies and authored lighting differences remain
separate issues. This correction does not establish that every shadow is fixed.

See [recorded evidence](../verification/runtime-ground-2026-10-19/README.md).
