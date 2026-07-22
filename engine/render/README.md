# engine/render — the render graph & high-level renderer (M5)

`rime::render` is "where the picture comes from" ([ARCHITECTURE.md](../../docs/ARCHITECTURE.md)):
the layer that turns a *described frame* into recorded RHI commands. Its foundation is the
**render graph** ([ADR-0019](../../docs/adr/0019-render-graph.md)) — each frame, render code
declares passes and the resources they read/write; the graph compiles the execution order,
culls dead work, allocates transients from a cross-frame cache, emits every between-pass barrier
through the RHI's `texture_barrier` seam, and records the passes bracketed by GPU timestamps and
debug labels. This is the structure that makes UE5-class techniques tractable, and it is "the
home for M10": GI, virtual shadow maps, and many-lights arrive later as passes and resources on
this graph, not as a renderer rewrite.

## Status (built bottom-up — see [docs/ROADMAP.md](../../docs/ROADMAP.md) → M5)

| Brick | Provides | State |
| --- | --- | --- |
| M5.4 | **RenderGraph v0** — frame-declared raster/compute passes, virtual resources (`RGTexture`), `import`/`export`, compile (versioning → edges → topo order → cull), the transient cache, graph-owned barriers, per-pass GPU ms | landed |
| M5.5 | the **scene layer** — `OrbitCamera` graduated from the viewer (ADR-0016 rule 3, the first parallel-path promotion), procedural primitives (plane/cube/uv-sphere, analytically exact) + `MeshRegistry`/`MaterialRegistry` behind dense ids, reflection-registered ECS render components (`MeshRef`, `MaterialRef`, `Camera`, lights) | landed |
| M5.6 | the PBR forward pass library (depth pre-pass → HDR forward → tonemap) + `docs/math/pbr.md` | landed |
| m10.1 | **directional cascaded shadow maps** — cascade fit, layered depth array, hardware-PCF compare + `docs/math/shadow-mapping.md` | landed |
| m10.2 | **local (spot) shadows with a destructibility-aware cache** — a slot re-renders only when its light moves or a destruction event's AABB touches its frustum | landed |
| m10.3 | **clustered forward shading** — froxel light culling in compute + `RGBuffer` + `docs/math/clustered-shading.md` | landed |
| m10.4b | **the runtime SDF clipmap** — 3 nested 64³ R16Snorm levels composed from m10.4a's cooked per-mesh/part fields by a min-blending compute pass, camera-centred with per-level texel-snapping, dirty-tracked via C1 (instance move/add/remove) and C2 (destruction events) so a static scene settles to zero work and one broken wall recomposes only its own neighbourhood + `docs/math/sdf.md` §6–10. Sphere-traced by m10.5a's DDGI probes; gated off by default. | landed |
| m10.4c | **the GI feasibility spike** — ADR-0032 §2's named unknown, measured: sphere-trace cost at DDGI probe-grid scale, and what it fixes about m10.5's probe count / rays-per-probe / update policy. Findings and the recommended starting configuration in [`docs/design/gi-spike.md`](../../docs/design/gi-spike.md); the sweep itself is a shape-only test case in `tests/render/sdf_clipmap_test.cpp`, so it re-measures on real hardware at m12.0. | landed |
| m10.5a | **DDGI probes — the trace-and-store half** — a camera-centred, texel-snapped lattice of irradiance probes, each sphere-traced through the SDF clipmap every update (spherical-Fibonacci rays, a fresh random rotation per frame, SDF-based self-shadowing, a grey-world albedo — real per-surface colour is a named follow-up), folded into a temporally-blended octahedral irradiance atlas + a Chebyshev-visibility atlas (border-correct — `docs/math/ddgi.md` §6) via hysteresis. Round-robins past `kMaxDdgiProbesPerUpdate` probes. Closes m10.4b's own "nothing registers instances" gap (`SdfRef` component + change-detected extraction in `scene_renderer.cpp`). Sampled by m10.5b's forward-shading consumer; gated off by default, and requires `sdf_clipmap_enabled`. `docs/math/ddgi.md` has the full derivation. | landed |
| m10.5b | **DDGI probes — consume + react, and the M10 thesis proof** — `pbr_forward_shadowed.frag` samples both atlases as an indirect-diffuse term: an 8-probe trilinear cage (CLAMP_TO_EDGE, not extrapolated, past the lattice's own edge), weighted by the Chebyshev visibility test (stops a lit probe leaking through solid geometry onto a fragment on the far side) and the standard wrap/backface term. `DdgiProbes::invalidate` is the destruction-reactive hysteresis override (0.5 for 5 updates, vs. the ~30-frame default) — the last of the six ADR-0032 coupling contracts this milestone needed. `tests/render/gi_thesis_test.cpp` is the executable version of the milestone's own headline sentence: a floor patch lit only by DDGI reads dim-but-clearly-nonzero, then materially brightens (and its *isolated indirect contribution* measurably moves, not just the direct term) once the occluding wall is removed and a handful of fast-tracked updates run. `docs/math/ddgi.md` §10–§13 has the full derivation. | landed |
| m10.6 | **GI integration — the traced field replaces the ambient hack** — with DDGI on, `pbr_forward_shadowed.frag` now *substitutes* the traced indirect term for M5.6's flat ambient constant instead of adding to it: the sky the constant approximated already enters the probes through their escaped rays, so summing both double-counts it (the "ambient + GI" wash-out). Off, the shader is byte-for-byte the M5.6 baseline (the regression bridge, ADR-0032 §11). Adds the two-room walls-fall proof (`gi_thesis_test.cpp`): a covered dark room lit *only* by bounce from a sunlit room next door lights up ≈8.6× when the divider falls, while the constant-ambient control stays bit-identical — GI as the whole signal, not a correction. Derivation in `docs/math/pbr.md` §6.1. Colour bleed (grey-world albedo), indirect specular (m10.7), and a probe/indirect debug view (view-mode plumbing, m10.8) remain named follow-ups. | landed |
| m10.7a | **the thin SSR G-buffer** — with `ssr_enabled`, the shadowed forward pass writes a second colour attachment (octahedral world normal in RG, perceptual roughness in B, a geometry mask in A) that m10.7b's SSR march consumes. One shader source, two SPIR-V modules: a new `rime_add_shader_variant` compiles `pbr_forward_shadowed.frag` a second time with `-DWRITE_GBUFFER`, so the baseline module carries no unused second output (validation-clean) — the MRT the RHI has supported since M5.1, first used here. Off (default): the G-buffer is never allocated or written, byte-identical baseline (ADR-0032 §11). `gbuffer_test.cpp` decodes a rendered pixel's normal (a flat quad and a 25°-tilted one, dot > 0.997 vs ground truth) and its per-surface roughness (0.85 vs 0.30). | landed |
| m10.7b | **the SSR resolve** — a fullscreen pass (`ssr_resolve.frag`) that reconstructs each surface from the depth buffer + the m10.7a G-buffer, reflects the view ray about the normal, and fixed-step **linear-marches** it through the depth buffer in view space; on a shallow crossing (within a tuned `ssr_thickness`) it samples the frame's own colour, else falls back to the flat ambient the forward pass uses. Fresnel-weighted, roughness-faded (a sharp mirror sample is wrong for a rough surface, so SSR fades out — cone-blur resolve is m10.7c), edge-faded against the screen border. Writes a second HDR target the tonemap reads, so SSR-off is byte-identical (ADR-0032 §11). `docs/math/ssr.md` derives the march, the thickness problem, the miss classification, and the artifacts v1 accepts (screen-edge cutoff, thickness error, no rough/temporal — all named, not chased). `ssr_test.cpp`: a smooth dark floor's mirror pixel brightens ≈28× onto the cube's known reflection point, a matte floor's does not, and a sky-facing floor pixel does not (reflection, not global lift). | landed |

Deliberate v0 bounds (ADR-0019 records why): serial recording on one queue (pass boundaries keep
the parallel-recording and async-compute seams open) and no transient aliasing (measure first).
Buffers joined the graph as first-class resources at m10.3, with clustered forward's light lists as
the first GPU-driven consumer ADR-0019 predicted.

## Layout

```
include/rime/render/   # public interface
  render_graph.hpp     #   RenderGraph, RGTexture, RGBuffer, pass descriptors
  lighting/            #   the M10 lighting techniques (shadows, clustered, …)
src/                   # the compiler + executor
```

Depends on `rime::rhi` only (the scene layer adds `rime::ecs` at M5.5). Tests live in
`tests/render/` — pixel-verified, GPU-free on lavapipe like every proof since M3.3.
