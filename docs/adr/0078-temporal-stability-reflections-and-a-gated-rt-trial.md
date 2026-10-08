# ADR-0078: temporal stability first, then reflections, then a gated hardware-ray-tracing trial — and the world the player cannot fall out of

- Status: **Provisional** — the order and the gates are decided; every millisecond below is an
  estimate until a pinned RTX 3060 run says otherwise
- Date: 2026-10-07

## Context

The engine has shipped a large lighting stack — cascaded shadow maps, local shadows, clustered
forward, an SDF clipmap, DDGI, SSR, a physical sky — and a virtual-geometry path with a software
micro-rasterizer. It has **no anti-aliasing of any kind**. Measured 2026-10-07:
`VK_SAMPLE_COUNT_1_BIT` is hardcoded at `engine/rhi/src/vulkan/pipeline_vulkan.cpp:121` and
`engine/rhi/src/vulkan/resources_vulkan.cpp:121`; there is no TAA, FXAA, SMAA, camera jitter,
motion-vector target or history buffer anywhere in `engine/render/`; the entire post chain is
`tonemap.frag` and `present.frag`. The image crawls and sparkles in motion, which is what the owner
reported after walking through `99-the-block` on the 3060.

Two further gaps were measured the same day. The sky is sampled along the mirror direction only —
ADR-0065 already records "no prefiltered radiance and no sky occlusion yet" — so a *rough* metal has
no environment to reflect, and SSR, being screen-space, returns nothing for a surface whose
reflection is off screen. And `samples/99-the-block` references the terrain system **zero times**:
the vision demo is a street in a void, while M19 shipped a terrain system that streams a 4 km world
with a six-level LOD chain at a measured 300 m/s travel envelope (ADR-0073).

Budget, from ADR-0077's pinned run: `frame` p99 **13.705** against 16.6 (passes), but `sim.block`
p99 **8.328** against **6.0** — *over*, and M18's gate 7 is still open on it. **The frame is CPU-
limited, not GPU-limited.** GPU headroom at 1080p is roughly 2–3 ms.

### What was consulted, and what it was worth

Three models were asked independently for their own plan: `gpt-6-astra` (through `codex`, with
repository access), `claude-opus-5-5` and `claude-fable-5-1` (through devpass, prompt only, $0.68 of
the premium pool). A separate study then checked their shared claims against primary sources. **The
study contradicted two things all three asserted as fact**, which is the whole reason it was run:

1. **"MSAA is foreclosed by a visibility buffer" is false.** Burns & Hunt (JCGT 2013) §3.3 exists
   to make multisampling cheap; Schied & Dachsbacher (HPG 2015) do 8x; The Forge (Apache-2.0)
   shipped "Triangle Visibility Buffer with Programmable MSAA" in release 1.63 (2025-03-20,
   verified). For a *software* rasterizer resolving by 64-bit atomic min there is no published
   treatment, so for us it is a **cost argument, not an impossibility**. It does not change the
   decision — MSAA does not address shading or specular crawl, which is most of what was seen — but
   the premise was wrong and is recorded as wrong.
2. **The fracture motion-vector trick is not attested anywhere.** All three models independently
   proposed it; nothing in Frostbite's destruction work, Rainbow Six Siege, O3DE or the TAA
   literature contains it. That convergence was three models answering one prompt, not three
   measurements. It is *plausible* — the ordinary "previous model matrix" rule applied through a
   hierarchy — and it is **unproven**, which is why §2's reprojection gate is mandatory rather than
   advisory.

Three things the sources had that no model mentioned: **Rainbow Six Siege**, the closest shipped
destructible game, uses 2x MSAA checkerboard *plus* jittered TAA *plus* a 3D G-buffer velocity
target, and its GI is **static**, with parallax-corrected local cubemaps as SSR's primary fallback —
which undercuts the categorical claim that baked probes are wrong under destruction. **Specular
anti-aliasing must run before the temporal stage**, because TAA's clamping removes the highlights it
is meant to filter. And visibility-buffer shading needs **analytic screen-space derivatives** for
specular AA to work at all. **Checked, and we already have them:** `vg_resolve.frag` implements
analytic barycentrics (citing Schied & Dachsbacher 2015) and derives explicit gradients from them
"with no 2x2 quad", precisely because neighbouring pixels in a visibility buffer can be different
triangles with different materials, so hardware derivatives there "would be garbage"
(`engine/render/shaders/vg_resolve.frag:10-26`, `analytic_barycentrics` at :85). So the one
precondition the study raised against this plan is already satisfied, and specular AA can be fed
correct gradients on the virtual-geometry path without new work.

## Decision

The order below is forced by dependency, not preference: each step produces what the next consumes.

### 1. Motion vectors, camera jitter and TAA — first, and alone

Sub-pixel Halton jitter in the projection, an RG16F velocity target, an HDR colour history with
neighbourhood clipping and disocclusion rejection, resolved **before** tonemapping, reset on camera
cuts. Estimated 0.4–0.8 ms including velocity generation.

- **The micro-rasterizer must apply the same jitter as the hardware raster path**, or the boundary
  between the two shimmers.
- **Do not rasterize velocity in the micro-rasterizer.** The visibility buffer already holds
  instance and triangle id; the resolve pass refetches the vertices, reconstructs the position and
  transforms it by a per-instance previous transform. Clusters are static in object space, so
  streaming LOD changes do not matter — the *current* surface point is reprojected, not a vertex
  identity.
- **Fracture.** A part that just broke off takes
  `prevTransform = parentPreviousRenderedTransform x partRestOffsetInParent`. Its exterior surface
  **was** on screen last frame as the intact parent, so the reprojection is exact rather than
  approximate. Newly exposed **interior** faces genuinely did not exist: the cook tags them, and
  they enter a reactive mask with zero history weight for one frame.
- **Previous transforms are the previous RENDERED pose, never the simulation tick**, and they are
  snapshotted when the client half consumes the sim result — **never read from sim memory**, because
  ADR-0077 made the two halves overlap and that read is now a race.
- **Lineage is written in the same sim tick that spawns the body.** One frame late and the child
  gets an identity previous transform and smears the intact facade across the debris for several
  frames, in every destruction clip, on the headline feature.
- **Specular anti-aliasing (NDF/roughness filtering) ships in this brick and runs BEFORE the
  temporal resolve.** TAA alone does not fix specular sparkle; both Kaplanyan et al. (HPG 2016) and
  Tokuyoshi & Kaplanyan (JCGT 2021) say so outright.

**The gate, because the technique is unattested:** a test reprojects frame N-1 through the velocity
buffer and diffs it against frame N over a scripted fracture, failing if any instance exceeds 1 px.
A velocity debug view ships on day one — this failure is otherwise dismissed as "TAA blur".

### 2. Prefiltered sky specular, then SDF specular occlusion

A split-sum GGX mip chain plus a DFG lookup table, built at **runtime** rather than baked: Filament
ships a runtime GPU filter and Frostbite did runtime pre-integration for dynamic probes, so a static
environment is not assumed. Then cone-traced specular occlusion against the existing SDF clipmap, so
interior metals stop reflecting sky they cannot see. Estimated 0.1–0.35 ms.

### 3. Reflections: SSR first, probes as the fallback

SSR accumulated through the new history; on miss, a **sparse world grid of runtime-relit probes**,
traced against the SDF clipmap and shaded from DDGI, invalidated per cell by destruction events;
DDGI remains the rough-surface fallback. **No probe is baked on or inside destructible geometry.**

This is the middle route. The sources do not settle it: no publication covers a probe whose proxy
volume is *removed*, and Siege ships static GI with parallax-corrected cubemaps, so the categorical
objection to baked probes is not supported by the one shipped game in our genre. Runtime relighting
is chosen because it reuses the SDF clipmap and DDGI we already have, and because it forecloses
nothing.

**Collapse policy — decided by the owner, 2026-10-07: no local probe reflection while collapsing.**
On the destruction event (deterministic, so free to key off) the affected probes' weight fades to
zero within about 6 frames, and the surface falls back to DDGI irradiance plus the prefiltered sky,
with whatever SSR legitimately has on screen. **"No reflection" means no STALE LOCAL PROBE, never
black** — a metal that goes black mid-collapse is a worse artifact than the one being fixed.
Showing nothing local is less wrong than showing a wall that is no longer there.

Re-converge **within 1 s (60 frames) of the last part death**, at **≤0.5 ms GPU per frame**,
re-rendering six faces at low resolution amortised one per frame and refitting the parallax proxy to
the surviving collision hull; probes are processed in priority order by screen proximity and the
rest queue. Capture need not be deterministic — the geometry it samples already is.

Three corrections to the "motion hides it" intuition that prompted this, all from the consults and
all accepted: (a) **the dominant error is illumination, not geometry** — a collapse opens a room to
sky, and a stale probe keeps a rough metal reflecting a dark interior while sunlight floods in,
which is low-frequency, large-area and visible *during* motion; (b) **rough metals have no SSR path
at all**, so the probe is their only environment and a stale one is a lie at rest for the rest of
the match; (c) **the parallax proxy box stops bounding the space**, so the correction itself becomes
wrong rather than merely stale. The deadline is therefore keyed to when dust occlusion drops, which
happens *before* the debris settles — "at rest" is too late a trigger.

### 4. FSR upscaling — a separate brick, after the above

FSR rather than DLSS, because the hosted tier's GTX 1060 cannot run DLSS and FSR reuses exactly the
inputs step 1 produces. Deliberately **not** in the same brick: an upscaler hides velocity bugs as
"upscaler artifacts", so our own velocity buffer is debugged against our own simple TAA first.

### 5. Hardware ray tracing — a GATED TRIAL, not a commitment

`VK_KHR_ray_query` only, called from the existing SSR compute shader. **No RT pipelines and no
shader binding tables**, which would leak into the RHI; the seam gains an acceleration-structure
resource, build/refit commands and a capability bit. Reflections only — not shadows (CSM exists),
not AO or GI (SDF and DDGI cover them). Rays are cast only where SSR has no hit, on surfaces below a
roughness threshold, at reduced rate.

**Acceleration structures under destruction:**

- One BLAS per unique part, **built once at load** from a fixed coarse proxy LOD, decoupled from
  streaming LOD.
- **An intact building is one merged static BLAS. On fracture only the damaged island swaps to
  per-part instances; untouched islands keep the merged one** — so 2000 standing parts never means
  2000 instances.
- **A debris spawn costs no BLAS build**: the cooked part already has one, and a spawn is a single
  instance record.
- **TLAS fully rebuilt each frame, not refitted** — refit degrades tree quality fast under hundreds
  of tumbling bodies. Refit is for skinned geometry only.

**The abort conditions, which are the point of calling this a trial.** Stop and ship without RT if
acceleration-structure work exceeds **0.5 ms**, or total RT exceeds **1.5 ms p99**, or any
destruction frame misses 16.67 ms. Ship **off by default** as a quality toggle in any case.

The evidence says this is hard: Battlefield V's reflection pipeline totalled **6.29 ms**, and its
acceleration structures cost **64 ms naive** for 20,200 instances and ~5,000 BLAS rebuilds, reaching
**1.15 ms** only through 4-degree projected-angle culling, staggered incremental rebuilds,
skip-if-unchanged and overlap with the G-buffer — at the admitted cost of popping and missing
objects. Developer-reported, 2019, hardware and resolution not stated. Our whole GPU headroom is
2–3 ms and our CPU half is already over budget. The gates are therefore real, and failing them is an
acceptable outcome of this ADR, not a defeat.

### 6. The world the player cannot fall out of

The owner's requirement, recorded as a requirement: **terrain must exist everywhere the player can
look, including when they leave the playable area by cheating or accident, and maps should be large
enough that leaving is not reachable in the first place.**

What was measured: there is **no kill plane, no world-bounds check and no respawn** anywhere in the
samples, and nothing is drawn below the horizon, so a player who walks off the street falls forever
through dark atmosphere. But the capability is not missing — it is unconnected. The terrain system
streams a 4 km world with a six-level LOD chain to the horizon (ADR-0073) and the vision demo does
not reference it at all.

So: **place the block on streamed terrain** rather than in a void, and extend the world beyond the
playable area so the horizon is terrain. A boundary policy — soft turn-back, hard kill volume with
respawn, or a world large enough that neither is reachable — is still **undecided** and is left to
its own ADR rather than guessed at here.

## Consequences

- The RHI gains a capability bit and an acceleration-structure resource only if step 5 passes its
  gates. Steps 1–4 need no new RHI capability.
- Step 1 touches every pass that writes depth, plus the cook, which must emit interior-face tags and
  part lineage. It is the largest brick here and it is deliberately first.
- Estimated GPU cost of steps 1–3 is 0.6–1.6 ms against 2–3 ms of headroom, so step 5 does not fit
  until step 4 buys budget back.
- `samples/15-terrain` has no window and no screenshot path, so none of this can be judged by eye on
  the newest content. A windowed fly-through is owed.
- **The frame is CPU-limited.** Nothing in this ADR addresses `sim.block` p99 8.328 against 6.0, and
  no amount of it closes gate 7.

## What was checked

- No anti-aliasing: `pipeline_vulkan.cpp:121`, `resources_vulkan.cpp:121`, and a search of
  `engine/render/` for TAA/FXAA/SMAA/MSAA/jitter returning nothing but font and comment matches.
- The block never references terrain: zero matches for `terrain` in `samples/99-the-block/main.cpp`
  outside an SSR comment.
- No bounds or respawn: no kill-plane, world-bounds or respawn symbol in the block or hello-game.
- The Forge 1.63 release notes, for programmable MSAA on a triangle visibility buffer.
- Analytic derivatives already present: `engine/render/shaders/vg_resolve.frag:10-26` and :85.
- "TAA is a prerequisite for denoised reflections **of any kind**" is false as stated — Godot's SSR
  (MIT) keeps no history, trading noise for a roughness-driven mip blur because it is not
  stochastic. It is true of the stochastic and ray-traced designs that shipped, which is the only
  version this ADR's ordering relies on.
- ADR-0077's own table, for `sim.block` p99 8.328 against 6.0, "still over".

Every millisecond figure in this ADR is an **estimate** — from the consulted models or from
published slides on other hardware — except ADR-0077's pinned numbers. None has been measured on
this engine. The first brick that lands must bring its own `docs/perf/` run.

## Implementation record (appended; the Decision above is unchanged)

**Step 2, first brick: the split-sum half.** Landed **switched off** (`SceneRenderer::
set_sky_specular_prefilter_enabled`, default `false`). The SDF specular-occlusion half of section 2
is a separate, later brick and is not started.

- *What exists:* `SkySpecular` (`lighting/sky_specular.hpp`) builds, through the render graph, a
  6-layer GGX-prefiltered array (level 0 is the sky-view LUT itself, levels 1-6 are perceptual
  roughness k/6) and a 64x64 DFG table, and `pbr_forward_shadowed.frag` reads them in place of the
  m19.6b mirror-faded-to-SH blend. Both bakes are GPU compute; the DFG table is built once per
  device, the chain only on a frame where the sky changed.
- *Why off:* `ssr_resolve` still reflects the sky-view LUT with plain Schlick, and with SSR on the
  forward pass compiles its sky term out, so the chain has no reader there. Turned on, SSR on/off
  would disagree about a rough metal by the single-bounce energy the new path restores (measured
  about 2x at roughness 1), and `sky_lighting_test`'s m19.6b SSR on/off bridge (bound derived from
  the old analytic fit) would fail. With SSR or DDGI on the setting builds nothing and counts the
  frame as disabled. Section 3 (SSR on the new history) is where `ssr_resolve` moves to the same
  chain; the switch flips then.
- *A deviation from "split-sum" as the ADR names it:* the DFG lookup is followed by Filament's
  single-line energy compensation (`1 + f0 (1/E - 1)`), because a single-bounce table loses 55% of a
  white metal's energy at roughness 1 (measured), which makes the white-furnace property false
  without it.
- *Cost, measured:* the chain rebuild is 1.6-1.8 ms of GPU on the reference RTX 3060 (Debug build,
  validation on, clocks not pinned) -- **above this ADR's 0.1-0.35 ms estimate if the sky changes
  every frame** (a scrolling cloud field does). It is zero on a frame where the sky did not change.
  The estimate and the measurement answer different questions: the per-pixel lookup is unmeasured.
  No `docs/perf/` JSON was filed: with the switch off the frame is byte-identical, and the bake is
  event-driven rather than per-frame. The first brick that turns it on owes the Release run.

**Step 2, second brick: `ssr_resolve` reads the chain, and the switch flips ON.**

- *What changed:* `ssr_resolve.frag` includes the same `sky_specular_eval.glsl` the forward pass does
  (bindings 9 and 10; `GpuSsrUniforms::params.z` is the live flag). With the chain live, a reflection
  that misses the screen reads `sky_specular_prefiltered(sky_specular_dominant_direction(n, r, rough),
  rough)` at the G-buffer's roughness, and the whole reflection (hit and miss alike) is weighted by
  `sky_specular_environment_brdf` instead of plain Schlick. The screen *hit* colour is untouched. With
  DDGI on, the resolve's probe is still the DDGI field and the weight is still Schlick, exactly as the
  forward pass reads no chain in that configuration. Exactly one pass mirrors the sky: the forward pass
  with SSR off, the resolve with SSR on; so the chain is built whenever DDGI is off (it used to be built
  only with SSR off too).
- *Why this closes the gap:* the old disagreement was two different BRDFs (Karis fit against plain
  Schlick) over two different skies (the SH-blended LUT against the raw LUT). Both sides now evaluate the
  same product from the same header, so for a pixel whose reflection leaves the screen they differ only by
  what the resolve can read back out of the G-buffer (RGBA16Float normal and roughness, RGBA8Srgb base
  colour). Measured on the 96x96 near floor, sky on, no lights: on/off mean ratio 0.993-1.008 and worst
  pixel 1.1% on both the RTX 3060 and lavapipe, for a red metal at roughness 0.05, a warm metal at 0.3 and
  0.6, a grey metal at 1.0 and a grey dielectric at 1.0; the chain-off pair on the same frames reads 1.03,
  1.23-1.33, 1.72-2.10, 3.5-3.6 and 1.22. `sky_lighting_test.cpp` asserts 3% per pixel and 2% on the mean.
- *What is NOT the same:* the forward pass looks the chain up at `sqrt(alpha)` where alpha carries the
  geometric specular AA widening (step 1a); the G-buffer holds the unwidened roughness and the resolve
  deliberately does not recompute the widening (a G-buffer normal's derivatives step across silhouettes,
  not curvature). On a high-curvature surface the forward pass therefore looks up a blurrier level than
  the resolve. The proof's floor is flat, so it does not exercise this; it is a named gap.
- *m19.6b's SSR on/off bound:* unchanged and still asserted, now pinned to the chain-off path its
  derivation (env_brdf_approx against Schlick) describes. The chain-on counterpart is the new case
  "ADR-0078 s2: with the prefiltered chain on, SSR on and SSR off mirror the sky alike".
- *Default ON, and the cost it owed:* no committed perf sample exercises the chain -- `the-block` and
  `lit-rooms` run DDGI, where neither reader touches it, and a `scripts/perf.sh --sample the-block` run
  on this branch shows no `sky-specular-prefilter` pass at all (and failed its gate on an unstable GPU
  clock and CPU-side sim time, neither related) -- so no `docs/perf/` JSON is filed and none would
  say anything about this change. What was measured instead is a probe, `sky specular: per-pass GPU
  cost of the lookup` (`RIME_PERF_PROBE=1`, Release, RTX 3060, 1080p, a rough-metal floor under a clear
  sky, median of 40 frames after 8 warm-up, clocks NOT pinned, so read it as a bound, not a figure):
  the forward pass 0.075 ms -> 0.082 ms (+0.007 ms) and `ssr-resolve` 0.80 ms -> 0.79 ms (no difference
  outside the noise) with the chain on. The bake itself is a one-off on an unchanged sky and 1.6-1.8 ms
  on a frame where the sky changed (previous brick's measurement, unchanged); a scrolling cloud field
  still re-bakes every frame, and the levers named there still apply.


**Step 2, third brick: SDF cone-traced sky specular occlusion, default OFF.**

- *What exists, measured by source inspection:* `SceneRenderer::set_sdf_specular_occlusion_enabled`
  defaults false (`scene_renderer.hpp:388,479`). The shared `sdf_trace.glsl` carries DDGI's original
  sampling/hit/normal helpers plus Quilez's running-minimum cone visibility. The enabled forward
  variant multiplies only sky specular by this visibility, replacing its diffuse AO factor; diffuse
  AO stays on diffuse. The enabled SSR variant occludes only the sky fallback before the screen-hit
  blend, so local hit radiance is untouched (`pbr_forward_shadowed.frag:553`, `ssr_resolve.frag:287`).
  Neither adds a pass. Pipelines are created only on their first live frame; the OFF shaders and
  DDGI compile to byte-identical SPIR-V against the pre-brick versions. Missing/disabled clipmap
  levels and frames without a sky reader have distinct cumulative counters (`scene_renderer.cpp:774`).
- *Cone derivation, inferred:* GGX's slope CDF is s²/(alpha²+s²), so the median half-vector slope is
  alpha. Reflection doubles its angle; the small-angle cone slope is therefore approximately
  2 alpha, extended to broad lobes as a finite approximation. Alpha is the square of the actual
  prefiltered lookup argument: forward uses sqrt(AA-widened alpha), resolve uses stored roughness
  (`sdf_trace.glsl:124`). Both lift the origin two finest voxels along the normal and start the ray
  two voxels away, to escape reconstruction uncertainty at the surface (`sdf_trace.glsl:87,131`).
- *What the proof measured:* lavapipe, a six-slab sealed box containing a roughness-0.6 metal plane
  and an identical outdoor plane, same world geometry viewed from two camera positions, clear sky,
  no lights. In all four SSR/prefilter on/off combinations the enclosed mean falls to zero and the
  outdoor mean ratio is exactly 1.0; every outdoor sample stays within 2%. The proof requires at
  least 10x enclosed suppression and less than 2% outdoor change (`sky_specular_test.cpp:1366`).
  Returning 1.0 from the cone falsifies all four enclosed cases with "enclosed metal sky must drop
  by at least 10x" (the mutation is restored). OFF after ON restores every HDR byte, and unavailable
  fields are counted and byte-identical (`sky_specular_test.cpp:1427,1437`).
- *Why off / what is not the same, inferred:* sixteen samples and an 8 m reach cannot guarantee
  distant/thin blockers, and a narrow-band saturated value is a lower distance bound, not a measured
  distance; using it in d/radius would darken empty outdoors. Such samples advance the ray but do
  not lower visibility (`sdf_trace.glsl:95,113`). This is a GGX-median cone approximation, not the
  full GGX integral. The preceding brick's forward/resolve AA gap remains: the resolve reconstructs
  position/direction without derivatives but still does not have the widened alpha. The proof is
  flat and pure sky fallback, so it does not prove curved surfaces or mixed SSR hits.
- *Cost, measured on the RTX 3060:* Release, 1080p partial-screen outdoor metal, median of 40
  frames after 8 warmups, three runs agreeing to within 0.008 ms: forward 0.0512 -> 0.1597 ms
  (**+0.109**), SSR resolve 0.1976 -> 0.2847 ms (**+0.087**). That is the bottom of this section's
  own 0.1-0.35 ms estimate, so the estimate holds; the switch stays OFF for the approximation
  limits above, not for its cost. Raw output and procedure:
  `docs/perf/sdf-specular-occlusion/rtx3060-release-probe.txt` and its README. The probe is opt-in
  (`sky_specular_test.cpp:1496`).
- *The same probe on lavapipe costs ten times as much* -- +1.294 ms forward, +1.334 ms resolve
  (`lavapipe-release-probe.txt`) -- and that software figure was the only one this brick's first
  pass could reach, because it ran where no `/dev/dri` or `/dev/nvidia0` existed. Filed beside the
  hardware run deliberately: taken for the brick's cost it would have read as a factor of ten over
  budget. A software rasterizer's per-pixel loop does not predict a GPU's.
- *Binding constraint, measured by source inspection:* appending four slots to the existing 21
  would exceed the RHI's 24-slot limit. The enabled forward variant reuses the inactive DDGI atlas
  slots 14/15 and adds 21/22 (23 total), a deliberate exception to the previous comment's request
  to split the next technique into a second set (`passes.cpp:558`, `pbr_forward_shadowed.frag:125`).
  The original layout and bindings remain the OFF path; no RHI seam change is made.
- *SAA-widened roughness vs the SSR resolve, measured (no fix yet):* a unit smooth metal sphere
  (perceptual roughness 0.1, camera z = 4, 256^2, clear sky, chain ON, TAA off, ambient 0).
  *Measured, analytic* (finite-difference normals; `sky_specular_test.cpp`, "SAA roughness
  divergence: analytic bound"): the forward pass reads the chain at `sqrt(alpha')` = 0.115 at the
  centre (0.69 levels vs the resolve's 0.60, a 0.10-level gap; the screen-space term is never
  zero), 0.14-0.19 in the 0.90-0.97 R rim band (up to 0.55 levels apart), and up to 0.312 at the
  outermost pixels (1.87 vs 0.60 levels, 1.27 apart); the 0.18 cap bounds the lobe at 0.651
  (3.9 levels). *Measured, rendered on lavapipe* (SSR-on / SSR-off mean luminance): centre disc
  1.0004, rim band 1.0051, outer ring (0.97-0.99 R) 1.0072. *Inferred:* against this smooth clear
  sky the artefact is below 1% in luminance even where the level gap exceeds one, so the choice
  between "store the widened value in B" and "carry it in A" is not forced by this scene; a
  sharper sky (sun disc, horizon) or a normal-mapped surface may show more and was not tried. The
  fix is pending Luca's choice between those two options.
