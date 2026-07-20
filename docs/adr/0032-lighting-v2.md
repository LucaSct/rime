# ADR-0032: Lighting v2 — shadows, many lights, SDF-probe GI, and the destruction-coupling contracts

- Status: Accepted
- Date: 2026-07-20

## Context

Milestone 10 is Rime's lighting milestone. [VISION.md](../../VISION.md) fuses **Frostbite-class
destruction** with **Unreal-class lighting**; M10 is where those two meet. The milestone's whole
thesis is one sentence: *break a wall between the sun and the floor, and on the next frame the floor
lightens — the shadow moves, and the bounced light updates.* Everything below is chosen to make that
sentence true, structurally provable on lavapipe, and cheap enough to pile on the render loop we
already have.

This ADR settles the architecture the M10 bricks (m10.1–m10.8, plus the floating m10.i) all cite:
the GI approach, the shadow strategy, the many-lights strategy, reflections, exposure, the RHI
top-ups the milestone needs, and — load-bearing for the thesis — the **contracts by which lighting
couples to physics and destruction without ever linking against them**.

**What M10 rests on (all merged, all proven):** the render graph (M5.4/[ADR-0019](0019-render-graph.md)),
forward PBR (M5.6/[ADR-0022](0022-forward-pbr.md)), compute pipelines (M5.2/[ADR-0021](0021-compute-pipelines.md)),
the fixed tick ([ADR-0023](0023-app-fixed-tick-loop.md)), ECS change detection
([ADR-0018](0018-ecs-storage-model.md) §4, landed M7.6), and the destruction event channel
(M8.4/[ADR-0029](0029-destruction-model.md)). The two seams M10's destruction thesis depends on —
change detection as public API (C1) and a destruction event channel carrying world-space bounds
(C2) — are **built, tested, and in production use** by `PhysicsSync` and the `10-destructible-wall`
sample; ADR-0029 §7 folded the "M10-C2 lighting listener" ask into its own decision record verbatim.
This ADR records decisions against a verified foundation, not a hoped-for one.

**The planning trail:** this ADR ratifies the direction argued in two design passes —
`rime-plans/m10/REFRESH-2026-07-12-lighting-family.md` (the deep technique pass) and
`REFRESH-2026-07-20-m9-done-kickoff.md` (the kickoff reconciliation that re-verified every claim
against `main` at M9 close, with file:line citations). Where those passes left an open question, the
lead ruled on it; those rulings are decisions 3, 8, and 9 below.

**The platform floor (a hard constraint, not a preference).** CI proof runs on **lavapipe** (a CPU
Vulkan rasterizer) forever — this box has no hardware GPU, and real-GPU numbers are a Mac/hardware
**sign-off channel, never a CI dependency**. Two consequences bind every technique choice here:
**no hardware ray tracing** and **no mesh shaders** anywhere a proof runs. The SDF/DDGI/clustered/SSR
stack below satisfies this by construction — it is pure compute plus rasterization — but the
constraint is stated so no later brick reaches for `VK_KHR_ray_tracing_pipeline` or task/mesh shaders
and quietly breaks CI portability.

## Decision

### 1. Module placement — lighting lives *inside* `engine/render`, not a new `engine/lighting`

Lighting is a family of render-graph passes and the settings that gate them; it is not a module with
its own object lifetime. It lives under `engine/render/` (new `include/rime/render/lighting/` +
`src/lighting/`). The decisive property: `engine/render/CMakeLists.txt` links only
`rime::rhi rime::ecs rime::assets` today — **zero link-time coupling to physics or destruction** —
and a lighting-in-render design *must preserve that*. All destruction/physics coupling flows through
**data seams** (ECS components, change-detection versions, the event channel), never a
`target_link_libraries` edge. [ADR-0025](0025-gpu-asset-bridge.md) is the precedent: a standalone
`engine/asset_gpu` was rejected because "a new module boundary bought nothing." Reversal trigger: if
the render graph ever becomes its own module, revisit.

### 2. Global illumination — SDF-traced DDGI probes

Dynamic diffuse GI is a grid of irradiance probes (DDGI-style), lit by rays **sphere-traced through a
global signed-distance field** composed from cooked per-mesh / per-part SDFs into a clipmap around the
camera. This is the choice that makes the walls-fall thesis reachable: it is fully dynamic (no
lightmap bake), it sees off-screen geometry (unlike screen-space GI), and re-stamping the SDF where a
wall broke is a bounded, change-driven update rather than a global rebuild.

The **critical unknown is the SDF-trace feasibility spike** — rays/second at probe-grid scale on
lavapipe. It cannot be answered by reading code; it must be measured. **Run the spike before
finalizing the ADR's proof *numbers*** (probe count, grid extent, rays/probe/frame). Per the plan's
own framing, the spike moves the *proof strategy* (smaller scenes, fewer probes, amortized re-trace),
not the *architecture* — so it gates m10.5's scope, not this decision. The spike itself is scheduled
ahead of **m10.5** (not m10.0, which this ADR closes); its notes will land as
`docs/design/gi-spike.md`.

### 3. Shadows — CSM first, local-light shadows second, VSM deferred; **stored in real array/cube textures**

Staged: **m10.1** directional cascaded shadow maps (the sun), **m10.2** shadowed local lights
(spot/point), **VSM/virtual shadow maps deferred** behind a named trigger (when texel density under a
Nanite-class geometry budget forces it — not before). m10.1 reuses `render::DepthPrepass` verbatim as
the per-cascade depth pass: its `depth_only.vert` computes `gl_Position` from nothing but
`frame.view_proj` and `draw.model`, so a cascade render is the identical pass aimed from the light —
no new pipeline, no new shader (detail in the m10.1 plan). The one **new RHI primitive** shadows need
is a depth-compare sampler (`SamplerDesc.compare_enable`/`compare_op`, reusing the existing
`rhi::CompareOp` enum) so the shader's `sampler2DShadow` gets hardware PCF.

**Storage — the lead's ruling (reverses the refresh's atlas-first default).** Cascades and cube faces
are stored in **real texture arrays and cube images**, and we **build the RHI lift to support them**.
The kickoff reconciliation found this is a genuine, moderate lift — not a "verify and go":
`TextureDesc` has no `array_layers` field, the Vulkan backend hardcodes `arrayLayers = 1`, and
`RenderingInfo`/`ColorAttachment`/`DepthStencilAttachment` cannot yet target a single layer of a
layered image. The refresh therefore *recommended* the atlas / N-separate-targets workaround (reusing
`ScenePicker`'s viewport-offset trick) to avoid the lift. **We decline the workaround and build the
general primitive** (see decision 10 for the exact top-up). Rationale for the investment: an atlas is
a per-technique hack that CSM, cube point-light shadows (6 faces genuinely want a cube image), and
later VSM/reflection-probe capture would each re-improvise; a proper array/cube capability is built
once and every layered-render technique in M10 and beyond uses it. It costs more now (m10.1a, a small
RHI pre-brick) and buys a clean seam for the rest of the milestone.

### 4. Many lights — clustered forward (froxels + compute light culling)

The fixed light caps in `pbr_forward.frag` (4 directional / 16 point, [ADR-0022](0022-forward-pbr.md))
are replaced by **clustered forward shading**: a froxel grid over the view frustum, a compute pass
that culls the light list into per-froxel index lists, and a forward shading pass that reads only its
froxel's lights. This is where ADR-0022 already said the many-lights splice goes. Its first RHI
forcing function is a render-graph **buffer resource** (`RGBuffer`, decision 10) for the light list
and index lists — call it out as **m10.3's** dependency, not a prerequisite of this ADR.

### 5. Reflections — screen-space reflections with probe fallback

SSR for what the screen can see; a probe/environment fallback (reusing the DDGI probe infrastructure
of decision 2) for what it cannot and for glancing rays that leave the screen. Planar reflections and
an SDF-specular second trace are **deferred** with named triggers. Nothing in the live tree constrains
this scope; it lands at m10.7.

### 6. Exposure — physically-based auto-exposure

A histogram/average-luminance auto-exposure driving the existing tonemap. Placement is a **cut-in-brick
question** (it rides inside m10.5/m10.6, or a micro-brick, depending on how the GI HDR range lands) —
recorded here as a v1 deliverable, not scheduled as its own numbered brick.

### 7. The destruction / simulation coupling contracts

M10 couples to a moving, breaking world **only through data seams**. These six contracts are
load-bearing; each is annotated with its verified state and the file that grounds it (verification
from the 2026-07-20 kickoff reconciliation).

| # | Contract | State | Grounding |
|---|---|---|---|
| **C1** | ECS change detection is public API: `for_each_changed(since)` / `mark_changed<T>()`, chunk-grain skip filters. A settled world stamps nothing, so lighting caches key invalidation off *what moved*. | **BUILT** | `ecs/query.hpp`, `world.hpp`; forcing consumer `physics/src/sync.cpp:102,126` writes back **awake bodies only**. `docs/design/simulation-tick.md` §4. |
| **C2** | A destruction event channel whose every payload carries a **world-space AABB** — the "what region changed" signal for shadow/SDF/probe invalidation. Kinds: `PartDamaged`/`PartDied`/`IslandDetached`/`DebrisSettled`. | **BUILT** | `destruction/events.hpp` (`world_bounds` populated unconditionally), `destruction/world.hpp::events()`, ADR-0029 §7. Consumption demonstrated in `samples/10-destructible-wall/main.cpp`. |
| **C3** | A tick/frame-phasing substrate with a deterministic per-tick order; lighting invalidation drains in the sequential tail after `PhysicsSync::write_back`. | **BUILT** (substrate); the tracker object is M10's to build | ADR-0023; `docs/design/simulation-tick.md` §2 (7-step order). See **decision 8** for where the drain hook lives. |
| **C4** | The walls-fall pipeline: re-stamp → probe re-trace → the frame-`f+k` CI assert. | **M10's own product** | The mechanical composition of C1+C2+C3; built by m10.6/m10.8. No unmet pre-M10 dependency. |
| **C5** | The transient-light "push" seam (fire-as-light for Track FX) + a shared `lighting_data.glsl` shader include. | **PARTIAL** — data shape ready, API + include to build | `render::ExtractedScene::point_lights` is an uncapped `std::vector` populated before caps apply; no named `push_point_light` yet, no `lighting_data.glsl` yet. Both start at m10.1. |
| **C6** | Debris visual lifetime is bounded, and retirement emits a world-bounds event so lighting caches invalidate the region. | **NEW — M10's to build** (the lead's ruling, decision 9) | Extends the m8.5 lifecycle; closes the SDF/shadow-caster growth found at kickoff. |

The C1–C5 wording is carried from `REFRESH-2026-07-12-lighting-family.md`; C6 is added by this ADR.

### 8. Where the per-tick lighting hook lives — `Application` owns the sim stage (the lead's ruling)

**Problem (C3's concrete gap).** Today the per-tick simulation wiring — `PhysicsSync::step`,
`DestructionWorld::update` — is assembled **by hand in each sample's `main.cpp`**;
`engine/app::Application` does not own destruction at all, and owns physics only via the single
generic `fixed_tick_` callback (`application.cpp:53`). A `WorldChangeTracker` that drains physics +
destruction deltas each tick and hands the render half a `WorldDelta` has no home in a *generic* app —
only in one sample.

**Decision.** `Application` grows an **ordered sim stage** it owns, replacing per-sample hand-wiring.
This generalizes the existing `fixed_tick_` seam from one opaque callback into a small ordered list of
sim steps run in the canonical `simulation-tick.md` order (systems → transform propagation → physics
sync → destruction update → **change-tracker drain** → deltas), each step optional. It mirrors exactly
how `Application` *already* optionally owns `rhi::Device`/`RenderGraph` ([ADR-0023](0023-app-fixed-tick-loop.md)
§4): present when configured, absent (and zero-cost) when not. Samples stop re-deriving tick order;
they register the stages they use. The `WorldChangeTracker`'s sim half is one such stage; its render
half hangs off the extract seam. This keeps determinism intact — the drain runs in the **sequential
tail**, never inside a parallel `Schedule` system, so it cannot perturb the cross-thread world hash
(the same discipline M7 events/CCD/stats followed). Scoped concretely into **m10.0's C3 section /
m10.6's tracker brick**; the `Application` API change itself is small and lands with the first
consumer.

### 9. Debris visual lifetime is bounded, and retirement feeds lighting invalidation (the lead's ruling)

**Problem.** In destruction v1, the m8.5 lifecycle's `freeze_debris()` reclaims only the *physics*
body under budget; the per-part **render leaf is deliberately kept in place** (ADR-0029 §8 — "a render
leaf can outlive the physics body at its last pose"). Correct for v1 rendering, but it means **debris
never despawns visually**. A lighting cache that tracks visible geometry (SDF bricks, shadow casters)
would therefore only ever *grow* across a long session — nothing ever tells it a piece is gone,
because for rendering nothing *is* gone. Left alone this is a slow leak, invisible at M10/M12 scene
scale and real over hours.

**Decision — the fix.** Add a **visual-retirement stage** to the debris lifecycle, past a second,
larger budget than the physics-freeze budget (by count, oldest-first / LRU, and/or age):

1. When a frozen debris leaf crosses the **visual budget**, it is retired — a brief dissolve/fade
   (one shader parameter, no new pass) then the render leaf is removed.
2. Retirement **emits a `DebrisEvicted` event carrying the leaf's world-space AABB** on the existing
   C2 channel (a new `DestructionEventKind`, same `world_bounds` shape — no new plumbing).
3. Lighting caches subscribe to it exactly as they subscribe to `PartDied`/`IslandDetached`: the
   evicted region's SDF bricks and shadow-caster entries are invalidated and reclaimed.

This **bounds the visible-geometry set** (≤ the visual budget) and therefore bounds the SDF/shadow
caster working set over any session length, using only the seam C2 already provides. The visual
budget is deliberately **larger** than the physics-freeze budget so debris stays visible well after it
stops simulating (the cheap, static, "rubble stays" look) but is still ultimately bounded. Lands as a
small **m8.5 follow-up brick** consumed first by m10.2 (shadow-caster set) and m10.4 (SDF set);
recorded here so M10's caches are designed against a *bounded* caster set from day one, not
retrofitted once the leak is noticed.

### 10. The RHI top-up ledger (accepted)

The techniques above need these RHI additions. Each is scoped to its forcing brick; none is a
rewrite.

| Top-up | For | Size | Forcing brick |
|---|---|---|---|
| **Depth-compare sampler** — `SamplerDesc.compare_enable` + `compare_op` (reuses `CompareOp`); one `VkSamplerCreateInfo` wire-up. No new `BindingType`. | Hardware-PCF shadow sampling (`sampler2DShadow`). | Small | m10.1 |
| **Array + cube textures** — `TextureDesc.array_layers` + cube flag; Vulkan image-creation (`arrayLayers`, `VK_IMAGE_CREATE_CUBE_COMPATIBLE`) and view-type selection; **per-layer render-target attachment** in `RenderingInfo`/`*Attachment` (a `layer`/`layer_count` field). | Cascade storage + point-light cube faces (decision 3, the lead's ruling — we build this rather than atlas-hack around it). | **Moderate** | **m10.1a** (RHI pre-brick), consumed by m10.1 + m10.2 |
| **`RGBuffer`** — a render-graph buffer resource alongside `RGTexture`, with barrier/hazard tracking for `add_compute_pass`'s storage read/write sets. (The RHI primitive it wraps — `BufferHandle` + `BufferUsage::Storage` — exists since M5.2.) | Clustered-forward light + froxel index buffers. | Moderate | m10.3 |
| **3-D storage-image writes** — verify `depth > 1` + `TextureUsage::Storage` + compute `imageStore` composes on lavapipe/MoltenVK. Primitives exist; the *combination* is untested. | SDF clipmap writes, froxel volumes. | **Spike** (not new code unless the spike finds a gap) | m10.4 |
| **Snorm formats** — `R16Snorm`/`R8Snorm` in `rhi::Format`. | The SDF clipmap's signed-distance storage (3× 64³ R16Snorm). | Small | m10.4 |
| Uint color target (`R32Uint`) | Any id/material G-buffer channel. | **Already built** (m9.6 `ScenePicker`) | — |

### 11. Performance governance — relative fences now, and idle work is a bug

Two halves.

**Regression fences (unchanged policy).** M10 techniques are proven with **relative** structural
assertions on lavapipe (properties the physics guarantees, checked with margins — never golden
images), plus per-pass GPU-timestamp fences via the existing `RenderGraph::resolve_timings`. **Absolute
frame budgets are deferred to real hardware (m12.0)** — lavapipe timings gate *regressions*, never
absolute ms. Every technique is a graph pass (or passes) behind `LightingSettings`, and **off ==
M5.6/ADR-0022 baseline, byte-identical** — the same on/off regression-bridge discipline the depth
pre-pass proof already uses. This is what lets M10 add shadows + GI + clustered + SSR without any one
of them being able to silently regress the others: each has an off switch that returns to a proven
baseline.

**Idle work is a bug (a CPU-cost decision, prompted by a real complaint).** The editor-host
render/stream loop (`engine/app/editor_host_main.cpp`) currently **renders, reads back, LZ4-encodes,
and sends a full RGBA frame every ~33 ms unconditionally** — including in Edit/Paused mode with zero
ticks, no edits, and a stationary camera, where the frame is byte-identical to the last one. An
*idle* editor should cost ≈0% CPU, not a full-frame pipeline 30×/second. Decision: the render/stream
loop must **skip render + capture + encode + send when nothing observable changed** (no applied edit,
no camera delta, no gizmo-state change, no pending pick, not Playing), and only wake on change or a
low-rate keepalive. This is a governance rule, not just a bug fix, because **M10 is about to add
substantial per-frame GPU work** (shadow cascades, clustered culling, SDF trace, probe update) —
introducing that on top of an already-wasteful loop would multiply the cost of doing nothing. Fixed as
an early perf brick (**m10.0-perf**, ahead of m10.1) so the CPU/GPU baseline is clean before the
lighting passes land. The synchronous `submit_blocking` per frame and the per-frame GPU readback are
noted as **secondary** targets (pipelining / async readback via the s1.1 `SubmitTicket` the picker
already uses) — real wins, but lower leverage than not doing the work at all when idle, and deferred
to a measured follow-up rather than front-loaded.

## Consequences

- **The walls-fall thesis has no external blocker.** Every input the acceptance pipeline (C4) consumes
  — change detection, event bounds, tick order — is proven on `main` today. What remains is M10's own
  composition, not a bet on unbuilt infrastructure.
- **We take on a real RHI lift early** (decision 3/10: array + cube textures, m10.1a) that the atlas
  workaround would have avoided. We pay it once and every layered-render technique in M10+ (CSM, cube
  shadows, later VSM and probe capture) inherits a clean primitive instead of re-improvising an atlas.
  The risk is a slightly heavier m10.1 opener; the mitigation is that it is a *bounded, well-understood*
  Vulkan capability with an obvious lavapipe proof (render into two layers, read both back).
- **`Application` grows a small public API** (the ordered sim stage, decision 8). This is a deliberate
  generalization of the existing `fixed_tick_` seam, and it retires per-sample tick-order duplication —
  a net simplification for every consumer, at the cost of one API addition to land and document.
- **Destruction gets one new event kind and a visual-retirement stage** (decision 9). Small, additive,
  on the existing channel — but it means the m8.5 lifecycle is no longer "physics-only," and the
  destruction docs/tests grow to cover visual retirement. The payoff is that M10's lighting caches are
  bounded by construction.
- **The GI proof strategy is spike-gated** (decision 2). If SDF-trace throughput on lavapipe is low,
  m10.5's proof shrinks (fewer probes, amortized re-trace across frames) — the architecture holds, the
  numbers move. We accept discovering this at the spike, not at m10.5.
- **CI stays lavapipe forever; the platform floor is now ADR-recorded.** No technique may reach for
  hardware RT or mesh shaders. Real-GPU numbers remain a Mac sign-off channel.
- **An idle editor stops burning a core** (decision 11), and M10's added GPU work lands on a loop that
  does nothing when nothing changed — the right order to add cost in.

## Alternatives considered

- **Voxel-cone-traced GI (VXGI).** Rejected: light leaking through thin geometry (exactly the walls we
  break), and a heavy voxel volume in memory. SDF tracing gives sharper occlusion for the destruction
  case at lower memory.
- **Screen-space-only GI.** Rejected outright: it cannot see the off-screen bounce the walls-fall
  thesis requires. Kept only as SSR's on-screen *specular* contribution (decision 5), never as the
  diffuse GI.
- **Lumen / surfel-class fully-dynamic GI.** Rejected as a one-person research risk for v1. DDGI is the
  known-good, single-developer-tractable point on the fully-dynamic curve; the SDF/probe seams do not
  preclude a later surfel layer if it earns its place.
- **Atlas / N-separate-targets for cascades (the refresh's recommended default).** Considered and
  **declined by the lead** (decision 3): it avoids the array/cube RHI lift but is a per-technique hack
  each layered-render pass would re-improvise. We build the general primitive instead.
- **A separate `engine/lighting` module.** Rejected (decision 1): it buys no boundary that the
  data-seam coupling doesn't already give, and it would tempt a link-time edge to physics/destruction —
  the one thing the design must not have. Same reasoning as ADR-0025.
- **Forward+ (tiled) instead of clustered.** Rejected for the froxel (3-D cluster) form: tiled culling
  over-includes lights along depth discontinuities; clusters bound light count per froxel far tighter,
  which matters once destruction fills a scene with transient fire-lights (C5).
- **Leaving debris visual-lifetime unbounded** (accept the slow leak, "revisit at M11"). Rejected by
  the lead in favor of fixing it now (decision 9): the fix is cheap (one event kind on an existing
  channel), and designing M10's caches against a *bounded* caster set from the start is far cheaper
  than retrofitting a bound after several passes assume its absence.
- **Keeping the per-frame editor stream unconditional** (decision 11's status quo). Rejected: it wastes
  a core at idle and would compound as M10 adds per-frame GPU work.
