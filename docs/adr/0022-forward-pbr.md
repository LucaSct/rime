# ADR-0022: Forward PBR — depth pre-pass → Cook–Torrance forward into HDR → tonemap (M5.6)

- Status: Accepted
- Date: 2026-07-05

## Context

M5's "done when" is *a lit PBR scene drawn through the render graph*. The graph (M5.4,
[ADR-0019](0019-render-graph.md)), the RHI it needs (M5.1–M5.3: UBOs, HDR format, blending, MRT,
mipmaps), and the scene layer (M5.5: mesh/material registries, ECS render components) are all
landed. This brick is the shading itself: a reusable set of graph passes that turn a world of
meshes, materials, and lights into pixels, plus the math note that makes them teachable.

The choices are load-bearing beyond M5 — this ADR's pass shape is where M10's shadows, clustered
lights, and GI will splice in — so they are recorded here, and the physics is derived term by term
in [docs/math/pbr.md](../math/pbr.md). The explicit non-goals for M5 stand: no shadows (the first
M10-prep brick), no IBL / environment lighting, no MSAA, no transparency policy.

## Decision

**A forward renderer: an optional depth pre-pass, then Cook–Torrance opaque shading into an RGBA16F
HDR target, then a fullscreen tonemap + sRGB encode — built as three independent, reusable graph
passes over a flat draw list, with a `SceneRenderer` as the standard (not the only) composer.**

### 1. Forward, not deferred

Opaque geometry is shaded in one pass, in world space, looping over lights per pixel. Deferred
shading (write a G-buffer, shade in screen space) wins when light counts are high, but it forecloses
on cheap MSAA and forward-friendly transparency, doubles the bandwidth (fat G-buffer), and is a poor
fit for the destruction-heavy, many-small-pieces scenes Rime targets. M5 has a handful of lights;
forward is simpler, and the clustered-forward path that scales it to thousands of lights is an M10
upgrade to *this* pass, not a rewrite. The render graph makes either reachable later — this is a
starting point, not a commitment against deferred.

### 2. The BRDF: Cook–Torrance, metallic-roughness

The industry-standard microfacet model: **GGX/Trowbridge–Reitz** normal distribution,
**height-correlated Smith** visibility (the geometry term with the projection Jacobian folded in),
**Fresnel–Schlick**, and a Lambert diffuse scaled by the energy specular did not reflect. Parameters
are the glTF metallic-roughness set (base color + metallic + roughness), so M6's glTF importer maps
onto materials with no translation. Every term and constant is derived in
[docs/math/pbr.md](../math/pbr.md) §2–§4; the shader cites, the doc explains (the teaching rule).
Linear-space discipline is absolute: base-color textures are sRGB-*format* so the sampler decodes
them to linear on input, all shading is linear, and encode happens once at the very end (§7).

### 3. HDR scene target, then tonemap + sRGB encode

The forward pass outputs **radiance** — unbounded, routinely $>1$ — to an **RGBA16Float** target
(float16 carries the range at half the bandwidth of float32). A separate fullscreen pass applies the
**ACES filmic** tone curve (Narkowicz's fit) and the **sRGB** transfer function into an 8-bit target.
Keeping scene radiance and display encoding apart is the entire reason to render in HDR: highlights
roll off on a filmic shoulder instead of clipping, and the pipeline is ready for bloom/exposure
(M10) which must read linear radiance. Encoding sRGB *in the shader* (rather than via an sRGB-format
target) keeps the choice visible and the pass usable with any Unorm target; a present pass can adopt
the free hardware encode later. The tonemap pass is also the **template for every post pass** — the
M5.8 vignette rides its exact shape (sample one input, fullscreen triangle, write one target).

### 4. Depth pre-pass — optional, two baked pipelines

An optional depth-only pass lays down nearest-surface depth so the forward pass shades each pixel
once (`CompareOp::Equal`, no depth write) instead of shading every overdrawn fragment. Because depth
state is baked into the pipeline ([ADR-0007](0007-vulkan-backend-bootstrapping.md)), the forward pass
carries **two** pipelines — *after-prepass* (load depth, test Equal, no write) and *standalone*
(clear depth, test Less, write) — and the caller picks per frame. The correctness hinge is an
**invariance contract**: both vertex shaders compute `gl_Position` with the textually identical
expression and declare `invariant gl_Position`, so the two pipelines produce bit-identical depth and
the Equal test never rejects a surface's own geometry (derivation: pbr.md §8). The proof asserts
pre-pass on/off are byte-identical; `LessEqual` is the documented fallback.

This required one **RHI addition**: a graphics pipeline with **no fragment shader and no color
attachment** (a color-less `RenderingInfo` paired with `color_format = Undefined`). Rasterizing for
depth alone is valid Vulkan and exactly a pre-pass's shape; the change is additive (a default,
never-assigned fragment handle means "depth only, on purpose") and does not touch existing pipelines.

### 5. Reusable passes over a flat draw list; `SceneRenderer` as one composer

Each pass (`DepthPrepass`, `ForwardPbrPass`, `TonemapPass`) is an object that bakes its pipelines
once and *declares itself* into a graph each frame. The geometry passes consume a flat
`SceneDrawData` (draw items + pre-filled uniform buffers) and know **nothing** about the ECS — so a
test drives them without a `World`, and `SceneRenderer` (which extracts a `World`, uploads the
uniforms, and declares the three passes) is *a* composer, not a hard dependency. Extraction copies
the world into flat arrays and then touches it no more, leaving the pipelined/parallel-frame seam
open (v0 runs serially). Lights ride a per-frame UBO with **fixed compile-time caps** (4 directional,
16 point); overflow drops with a one-time warning — per-view light culling into a larger structure is
the M10 many-lights problem, and the caps mark where it splices in.

## Consequences

**Good**

- M5's "done when" is met: a lit metallic-roughness scene renders through the graph, verified by
  *structural radiometric* proofs (lit ≫ unlit; highlight tightens as roughness falls; a global
  energy bound and a tight roughness-1 bound; pre-pass on/off byte-identical) — driver-portable in a
  way golden images never are, and green GPU-free on lavapipe.
- Adding a pass really is easy: the tonemap pass *is* the demonstration, and M5.8's extra post pass
  will prove it in ~10 lines.
- The metallic-roughness parameter set is glTF-native — M6 import needs no material translation.
- The seams that matter later are open, not bolted shut: clustered/deferred (graph reroute), shadows
  (a pass + a sampled depth map), IBL (an ambient-replacing term), exposure/bloom (they read the HDR
  target), parallel extract (the copy-then-declare split).

**Costs we accept**

- **Constant ambient** stands in for indirect light (pbr.md §6) — flat, occlusion-blind, obviously a
  placeholder. It keeps unlit sides readable until GI (M10); the code says so.
- **No shadows** — lights pass through geometry. Roadmap-faithful; shadow mapping is the first
  M10-prep brick and slots in as a pass feeding the forward one.
- **Fixed light caps** and an **unsorted, un-culled, un-batched** draw list — correct and simple for
  M5 scenes; frustum culling, instanced batching, and draw sorting are measured optimizations for
  when a real workload shows they pay (measure before optimize).
- **Two forward pipelines** double the pipeline count for the depth discipline — the price of baked
  state; trivial at this scale.

## Alternatives considered

- **Deferred / visibility buffer.** Better for very high light counts, but heavier bandwidth, hostile
  to MSAA and transparency, and a poor fit for many-small-pieces destruction. Forward first; the
  graph keeps deferred reachable. Rejected for v0.
- **Tonemap into an sRGB-format target (hardware encode).** Free, but hides the transfer function and
  ties the pass to sRGB targets. We hand-roll the encode so it is visible and target-agnostic; a
  present pass can switch later. (docs/math/pbr.md §7.2.)
- **No depth pre-pass.** Simpler (one pipeline), but no early-Z overdraw relief. Kept as the
  *standalone* path and made optional, so the choice is per-frame and measurable rather than baked.
- **A Reinhard tonemap.** The pedagogical baseline (pbr.md §7.1), but it desaturates and flattens the
  very highlights HDR exists to preserve. ACES ships; Reinhard stays in the doc as the derivation's
  first step.
- **Dynamic (uncapped) light lists.** Needs storage buffers and per-view culling — the M10
  many-lights problem. A fixed UBO budget is the honest v0; the caps are the splice point.

---

*This ADR is brick **M5.6**. Math: [docs/math/pbr.md](../math/pbr.md). Proof:
`tests/render/pbr_pipeline_test.cpp` — structural radiometric asserts on a metallic×roughness sphere
grid under one point light, plus a base-color-texture proof; GPU-free on lavapipe. Builds on
[ADR-0019](0019-render-graph.md) (the graph), [ADR-0020](0020-descriptor-model-v2.md) (UBOs +
binding layouts), [ADR-0011](0011-depth-attachment.md) (depth), [ADR-0004](0004-math-conventions.md)
(linear/RH/Vulkan-clip). Next: **M5.7** — `engine/app` and the fixed-tick loop (ADR-0023) — then
**M5.8** the proof samples that put this pipeline on screen. See [ROADMAP.md](../ROADMAP.md) → M5.*
