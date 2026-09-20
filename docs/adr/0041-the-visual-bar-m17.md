# ADR-0041: M17 — "The Visual Bar", and the four rulings a milestone with two clauses needs

- Status: Proposed
- Date: 2026-09-03

## Context

M17 carries two clauses, and they pull against each other.

The first is the UE5 column of [VISION.md](../../VISION.md) §3: virtualized geometry (Nanite-style),
real-time GI and reflections (Lumen-style), virtual shadow maps, and a many-lights pipeline
(MegaLights-style). The second is M13's frame-rate clause, carried forward unchanged by
[ADR-0038](0038-platform-proof-m15.md) and renumbered here by
[ADR-0039](0039-authored-surfaces-m16.md): `frame` p99 **35.60 ms** against a ratified 16.6.

Every technique in that first list is a *performance* technique. Nanite is not prettier geometry, it
is geometry whose cost stops scaling with triangle count. Virtual shadow maps are not softer
shadows, they are shadow resolution that stops scaling with cascade area. MegaLights is not more
lights, it is many lights that stop scaling linearly. So the milestone's two clauses are the same
clause read twice, and the order is forced: **a frame that misses its budget by 2.1× cannot
demonstrate a scaling technique, because there is nothing to compare the scaling against.**

This ADR is late. M17 opened with two bricks and no plan — m17.0 (the procedural sky) and m17.1 (the
editor's fly camera) both landed before anything in the repository said what the milestone was made
of. [ADR-0040](0040-sky-and-atmosphere.md) was written afterwards to close five code sites that
already cited it, and it rules on the sky alone. That is recorded in `docs/ROADMAP.md` rather than
tidied away, and it is the reason this ladder starts at m17.2.

### What the one committed measurement actually says

`docs/perf/2026-08-30-99-the-block-nvidia-geforce-rtx-3060.json` — RTX 3060, NVIDIA 610.57.04,
Linux, RelWithDebInfo, no sanitizer, 1920×1080, preset `block-all-lighting-gates`, 600 frames.
Ratified budget from [ADR-0035](0035-vision-demo-m12.md) §2 as amended 2026-08-20: `frame` p99
≤ 16.6 ms, `frame` max ≤ 33 ms, sim-tick p99 ≤ 6 ms, collapse-tick max ≤ 12 ms.

| timeline | p50 | p99 | max | budget | over by |
|---|---|---|---|---|---|
| `frame` | 18.842 | **35.598** | **37.974** | p99 16.6 / max 33 | 2.14× / 1.15× |
| `frame.collapse` (n=90) | 24.986 | 36.939 | **36.939** | max 33 | 1.12× |
| `sim.block` | 14.242 | **25.491** | 26.242 | p99 6.0 | **4.25×** |
| `sim.client` | 7.482 | 16.593 | 17.022 | — | — |
| `sim.server` | 5.828 | 8.971 | 9.714 | — | — |
| `frame.player` | 13.398 | 27.507 | 28.968 | — | 1.66× of 16.6 |
| `frame.render` | 5.230 | 12.690 | 12.982 | — | — |
| `frame.submit` | 3.520 | 10.604 | 10.872 | — | — |
| `frame.execute` | 1.470 | 1.675 | 1.850 | — | — |
| `frame.declare` | 0.299 | 0.516 | 0.582 | — | — |

Four gate rules breach. Three readings matter more than the totals:

- **The largest single breach is the simulation, not the renderer** — `sim.block` at 4.25× of a
  ratified budget, against the frame's 2.14×.
- **`frame.player` is 27.51 ms**, so removing the authoritative server this demo runs in-process
  still misses 16.6 by 1.66×. The topology is not the whole answer; the engine is too slow.
- **The roadmap's "the whole M10 stack renders in 5.23 ms" is the p50 quoted against a p99
  budget.** At p99 the renderer costs 12.69 ms of a 16.6 ms frame — 76% of it. That number has been
  repeated four times in `docs/ROADMAP.md` as if it bounded the render half. It does not.

### Five facts found while planning, each verified against the tree

**1. The engine has ten profile zones, all in one file, and none of them is inside the work.**
`grep -rn RIME_PROFILE_ZONE engine samples` returns exactly `sim.tick`, `sim.pre`, `sim.schedule`,
`sim.transforms`, `sim.post`, `sim.publish`, `frame.declare`, `frame.execute`, `frame.present`,
`frame.submit` — every one of them in `engine/app/src/application.cpp`. There is no zone anywhere in
`engine/physics`, `engine/destruction`, `engine/render`, `engine/net` or `engine/ecs`.

The consequence is visible in the committed report: `sim.pre`, `sim.schedule`, `sim.transforms`,
`sim.post` and `sim.publish` all read **0.000 ms** at p50, p99 *and* max, while `sim.block` reads
25.49 at p99. That is not a bug — `samples/99-the-block/main.cpp:1878-1880` already says the demo
drives a `Session` rather than the `Application` schedule, so the stage zones fire over empty
stages — but it means **the frame's single largest cost has no attribution of any kind.** Two
stopwatches in the sample split it into client and server, and that is where the decomposition ends.

So [ADR-0035](0035-vision-demo-m12.md) §6's prediction — "the every-tick narrowphase cache (M7's
named first hot spot, likely at 400+ debris)" — has never been measured, and `docs/ROADMAP.md`
currently repeats it as "ADR-0035 §6's predicted narrowphase cache, now with its measurement." The
measurement says *the simulation costs 25 ms at 667 debris*. It does not say narrowphase. Nothing in
the repository does.

**2. Same-named render-graph passes collapse, and the committed artifact has duplicate JSON keys.**
`CascadedShadowMap::add` renders each cascade by calling `DepthPrepass::add`
(`engine/render/src/lighting/shadows.cpp:178-183`), and `LocalShadowMap::add` does the same per spot
slot — so a frame declares one `"depth-prepass"` for the camera and one more per cascade and per
invalidated spot. `PerfReport::observe_frame` folds by name into a single accumulator
(`engine/core/src/diagnostics/perf_report.cpp:589-598`), and the worst-frame writer emits one key
per instance with no deduplication (`:784-788`). The committed file therefore contains, verbatim:

```json
"depth-prepass": 0.172,
"depth-prepass": 0.077,
"depth-prepass": 0.071,
"depth-prepass": 0.058,
```

Every ordinary JSON reader keeps one of those four. The pass table sums to **5.604 ms** in worst
frame #558; read through `jq` or `serde_json` it sums to **5.284 ms**. And the aggregate line
`depth-prepass p50 0.058 / max 0.398` is the median of ~2,400 individual cascade renders, not a
per-frame cost — a reader will take it for one.

**There is no pass named for shadows anywhere in the report.** The milestone that must decide
whether to build virtual shadow maps cannot currently see what the shadow maps it already has cost.

**3. Roughly half the GPU's wall time is unattributed.** `frame.submit` wraps
`device_->submit_blocking` (`engine/app/src/application.cpp:322`) and its own comment calls it "the
GPU's wall time as the CPU experiences it — the honest number for a headless run". In worst frame
#558 that is ~10.87 ms against 5.60 ms of summed pass timestamps. Where the other ~5 ms goes —
barriers, layout transitions, queue idle, the pass-timestamp resolve itself — nothing in the tree
says.

**4. The regression gate's protection has already lapsed on this machine, silently.**
`MachineFingerprint::comparable_to` requires string equality on `driver` among the eight fields
(`perf_report.cpp:515-519`). The workstation's driver moved **610.43.03 → 610.57.04** between
2026-08-20 and 2026-08-30, and both facts are on disk:

| baseline | date | driver |
|---|---|---|
| `2026-08-20-10-destructible-wall-…json` | 2026-08-20 | NVIDIA 610.43.03 |
| `2026-08-20-11-lit-rooms-…json` | 2026-08-20 | NVIDIA 610.43.03 |
| `2026-08-30-99-the-block-…json` | 2026-08-30 | NVIDIA 610.57.04 |

`scripts/perf.sh --all` runs all three, and picks each one's baseline as the newest matching file in
`docs/perf/`. The workstation reports **610.57.04** today (`nvidia-smi`, 2026-09-03), so two of the
three now return `BaselineStatus::FingerprintMismatch`,
which adds no violation — the absolute rules still fire, but the 10% regression comparison does not
run. ADR-0035's own amendment says where the real protection lives: "a 10% slide on the reference
machine fails at 8.76 ms rather than at 16.6 ms". A driver update turns that off and reports it as a
note. This is the "gate that cannot fail" shape the codebase keeps finding, arriving through the
calendar rather than through a diff.

**5. m16.1's frame-in-flight ring never reached the passes, and the gap is seven passes wide.**
m16.1 gave `SceneRenderer`'s own frame and draw uniform buffers a ring of `frames_in_flight + 1`,
for the reason written at `scene_renderer.cpp:332-338`: the handle is baked into a descriptor set,
so `write_buffer` overwrites mapped memory the GPU may still be reading, "ordered by nothing a
pipeline barrier can express". Every lighting pass still has the pre-m16.1 shape — a `write_buffer`
inside the per-frame `add()`, into a buffer created once in the constructor:

| pass | buffers written per frame |
|---|---|
| `clustered` (m10.3) | `light_buffer_`, `uniforms_` |
| `ssr` (m10.7b) | `uniforms_` |
| `sky` (m17.0) | `uniforms_` |
| `shadows` / CSM (m10.1) | `cascade_vp_ubo_`, `shadow_ubo_` |
| `local_shadows` (m10.2) | `spot_vp_ubo_`, `local_ubo_` |
| `sdf_clipmap` | one params buffer |
| `ddgi` (m10.5) | `hysteresis_buffer_`, `clipmap_levels_ubo_`, `trace_params_ubo_`, `blend_params_ubo_`, `sample_params_ubo_` |

Fourteen buffers, none ringed. **Reasoned, not reproduced** — no corruption has been observed, and
it cannot be: every test path uses `submit_blocking`, which finishes the frame before the next
write. It bites only under `present()`, which is the editor viewport and every `--windowed` sample.

Two smaller ones, recorded so they are not rediscovered. The ratified **collapse-tick max ≤ 12 ms**
has no rule in `99-the-block`'s gate — it gates `frame.collapse` max at 33, which is the frame
budget during the collapse window, not the tick budget (`samples/99-the-block/main.cpp:1936-1942`).
And the block baseline was measured on a **dirty tree**: its `run.commit` is `68bcfd7-dirty`.

## Decision

### Ruling 1 — the budget is earned before the bar is spent

M17's ladder does the frame-rate clause **first** and the UE5 column **second**, and no brick that
adds GPU or CPU cost lands before `99-the-block` meets its ratified budget.

This is not conservatism, it is the only order in which the milestone's second clause means
anything. The UE5 column is four scaling techniques; a scaling claim needs a baseline whose cost is
known and whose budget is met, or "Nanite made it faster" is unfalsifiable. It is also the same
argument [ADR-0039](0039-authored-surfaces-m16.md) used to insert M16 — a visual bar cannot be
judged on content that renders grey, and it equally cannot be judged on a frame that stutters.

The budget itself **does not move**. ADR-0035 §2 was ratified against a baseline and re-ratified
when that baseline came in 2.1× under; moving it now, in the milestone that must meet it, would be
the goalpost move that `samples/99-the-block/main.cpp:1897-1899` already refuses in code.

### Ruling 2 — the frame becomes attributable before anything is optimised

No optimisation brick lands until the report can say *where the time went*. Concretely, three
properties, each of which is false today and each of which is checkable:

1. **Every pass instance is separately attributable.** Four `depth-prepass` rows must not collapse
   into one, and the shadow work must appear under a name that says it is shadow work. The seam is
   the pass name: `DepthPrepass::add` already takes a `layer`, and its callers already know whether
   they are drawing the camera view, cascade *c*, or spot slot *i*.
2. **The artifact has unique keys.** A committed `docs/perf/` report that no ordinary JSON reader
   can read faithfully is not evidence. Whatever the naming scheme, the writer emits each key once.
3. **The parts account for the whole.** The simulation gets zones inside it, and the gap between
   `frame.submit` and the summed pass timestamps gets a name rather than being left as a
   subtraction. Where a residual genuinely remains, it is *reported as a residual* — an unnamed
   5 ms that a reader must compute for themselves is how "the renderer costs 5.23 ms" became a
   sentence in the roadmap.

**The proof for this brick is falsification, not coverage.** A test that asserts "the report has
zones" passes on a report whose zones are all zero — which is exactly the state of the committed
one. The gate must fail a report whose sub-zones do not account for their parent within a stated
margin, and that rule must be shown to fail on the 2026-08-30 file.

### Ruling 3 — what the UE5 column means for M17, and what it defers

M17 delivers **three of the four** and defers the fourth by name:

- **Real-time GI and reflections** — DDGI and SSR ship today. The gap ADR-0040 §6 names is that
  *the sky does not light the scene*: all three consumers of sky radiance (forward ambient, the DDGI
  miss, the SSR miss) read one constant, `SceneRenderer::ambient_`, whose own comment still calls it
  "the crude GI stand-in until M10". Closing that is the single largest visual change available for
  its cost, and it closes an ADR that deliberately left it unscheduled.
- **Many lights** — clustered culling shipped in m10.3. The MegaLights claim is not "add clustering"
  but "find the light count where the current path falls over, and raise it". That is a measurement
  followed by a decision, and it is only possible after Ruling 2.
- **Shadows** — CSM and cached spot maps ship today. Whether M17 builds *virtual* shadow maps is
  explicitly **not decided here**: the RHI has no sparse-binding capability, so true VSM is an RHI
  extension plus a page-table allocator plus a sparse depth atlas, and we do not yet know what the
  shadows we have cost. The brick is scoped by m17.3's numbers, and the ADR that rules on VSM proper
  is written when they exist.
- **Virtualized geometry — deferred to M18, with its own ADR. Approved by Luca, 2026-09-03.**
  Nanite-style rendering is a cluster hierarchy in the cook, a LOD DAG, GPU-driven culling, and a
  visibility-buffer path with software rasterization for sub-pixel triangles. It is a milestone, not
  a brick. [ADR-0035](0035-vision-demo-m12.md) §6 already parked it as "m10.i virtualized geometry"
  under *After M12* and it has never been scheduled since. Attempting it inside a milestone that
  also owes a 2.1× frame-time reduction is how both get done badly.

Saying this out loud is the point. M17's "done when" is now three of four plus the budget, and
VISION §3 is not satisfied at the end of M17. A milestone that quietly reinterprets its own scope is
worse than one that states the cut.

### Ruling 4 — a baseline that cannot be compared is a failure, not a note

`BaselineStatus::FingerprintMismatch` must stop being a silent pass. Two changes, and this ADR takes
the first:

- **`scripts/perf.sh` fails when a baseline exists and is not comparable.** Today the run prints a
  note and exits on the absolute rules alone. A milestone that will re-measure repeatedly cannot
  have its tightest protection lapse on a driver update.
- **The fingerprint keeps `driver`.** The alternative — dropping it so baselines survive driver
  bumps — trades a loud, correct refusal for a quiet, wrong comparison. Driver revisions change GPU
  performance; that is the entire reason the field is in the fingerprint. The fix is to re-baseline
  deliberately when the driver moves, and for the tool to say so rather than shrug.

The immediate consequence is that the two 2026-08-20 baselines are stale and M17 re-measures all
three samples on the current driver before it optimises anything. That re-baseline is m17.3's
deliverable, not a chore attached to a later brick — a fresh baseline measured *after* an
optimisation cannot prove the optimisation.

### Ruling 5 — the ground is a *surface* in M17; a ground *module* is not

Raised by Luca while reviewing the ladder, and the ladder was wrong to omit it. What the block
stands on today, verified:

- **The visual ground is two triangles.** `make_plane(0.5f, 24.0f)` (`engine/render/src/mesh.cpp:22`,
  four vertices, six indices), scaled to a 76 m square by a `LocalTransform`
  (`engine/blockkit/src/block.cpp:315-326`).
- **It has no textures at all.** `palette.street` is `opaque(0.10f, 0.10f, 0.11f, kStreetRoughness)`
  — a `PbrMaterialDesc` carrying base colour, metallic and roughness and nothing else
  (`palette.cpp:16-24, 48`). The mesh's `uv_tiles = 24` addresses a texture that does not exist.
- **The collider is a different object, authored by hand, once per sample.** `99-the-block` builds a
  44 m half-extent static box in `add_street` (`main.cpp:273-279`) and says in its own comment that
  the street "is NOT in the scene file". `13-networked-player` builds a 3×3 grid of 10 m boxes
  because "10 m per box is a measured GJK limit" (`main.cpp:112-114`) — a limit review pass 2
  **measured as gone** (168 configurations, half-extents 10 m to 500 m, zero misses), and which the
  block's own single 44 m box already contradicts in the same repository.
- **No module owns the concept.** `engine/worldkit` is the component profile, not a world.

That is the wrong surface to judge a visual bar on, and it is wrong in a way that flatters. The
ground is the largest thing in almost every frame — it is what a fly camera mostly sees, what every
shadow lands on, what SSR reflects and what the sky's ambient term (m17.7) most visibly changes. A
flat untextured plane makes DDGI bounce, SSR and shadow quality all *unjudgeable*: there is no
normal variation for a reflection to bend around, no albedo for a bounce to tint, and no detail for
a shadow to read against. It also flatters the measurement — two triangles cost nothing, so every
render number in this ADR was taken over a floor no game would ship.

**So M17 gets a ground brick (m17.8), and it lands after the budget bricks**, which is Ruling 1's
first real test: a tessellated, textured ground *adds* cost, so it may not precede m17.5 and m17.6.
The brick's scope is deliberately modest — one owned ground surface with a cooked material, and a
collider derived from it instead of hand-authored beside it. It is the first thing to use M16's
asset path over a large area, which is a proof M16 never got.

**A terrain module is NOT in M17, and its ADR is not written here.** A real
`engine/terrain` — heightfield, quadtree or clipmap LOD, splat/material blending, a heightfield
collider, streaming — is a milestone, not a brick, and adding the largest new GPU *and* CPU cost
source to the milestone that owes a 2.1× reduction is exactly the mistake Ruling 1 exists to
prevent. There is also a genuine ordering dependency: **terrain LOD is the part of terrain that
virtualized geometry most naturally subsumes**, and building a bespoke terrain LOD in M17 and a
virtualized-geometry pipeline in M18 risks building the same thing twice. The ground module is
therefore ranked with M18 and the question "does virtualized geometry subsume terrain LOD?" is
answered in *that* ADR, from a geometry pipeline that exists, rather than guessed at here.

What m17.8 must not do is foreclose it. The ground gets an owner and a seam — one place that
answers "what is the ground", producing both the drawn surface and the collider — so that a later
heightfield replaces an implementation rather than being retrofitted into eight call sites. That is
guardrail 3 applied to ground the way it is already applied to destruction: leave the seam.

## The brick ladder (m17.2–m17.10)

m17.0, its proof, and m17.1 are already on the branch; the table in `docs/ROADMAP.md` records what
they were and that they preceded this plan.

| brick | what it is |
|---|---|
| **m17.2** | this ADR and the ladder. Decision brick, no engine code |
| **m17.3** | **the frame becomes attributable.** Four sub-bricks, because each is separately falsifiable and the re-baseline must come last — a baseline committed before the instrumentation exists cannot carry it. **m17.3a** per-instance pass identity and unique report keys · **m17.3b** stage zones inside the simulation, recorded per call *and* per frame · **m17.3c** the gate rule that fails an unaccounted frame, falsified against the 2026-08-30 file, and the 32-slot timestamp pool that m17.3b found the block already overflowing · **m17.3d** a supplied-but-incomparable baseline fails, and all three samples re-baselined on the current driver in Release |
| **m17.4** | **the pass-owned buffer ring** — all seven lighting passes write host-visible buffers inside their per-frame `add()` with no frame-in-flight ring: `clustered` (2), `ssr` (1), `sky` (1), CSM (2), `local_shadows` (2), `sdf_clipmap` (1), `ddgi` (5) — **14 buffers**. m16.1 fixed this one level up and the reasoning is already in-tree at `scene_renderer.cpp:332-338`. It lands here because from m17.5 on the bar is judged **windowed**, under `present()`, which is the only path that can bite |
| **m17.5** | **the simulation budget** — `sim.block` p99 25.49 against a ratified 6.0, the largest breach on the board. What lands is decided by m17.3's zones, not by ADR-0035 §6's guess |
| **m17.6** | **the GPU budget** — `frame.submit` p99 10.60. `ssr-resolve` (max 4.455) and `forward-pbr shadowed` (max 4.051) are the attributed half; ~5 ms is not attributed at all until m17.3. Hi-Z SSR is ADR-0035 §6's named candidate, *iff* the measurement agrees |
| **m17.7** | **the sky lights the scene** — ADR-0040 §6, scheduled. The three `ambient_` consumers read a sky radiance |
| **m17.8** | **the ground becomes a surface** — see Ruling 5. Two triangles and a flat colour today; a tessellated, cooked-material ground owned in one place, with the collider derived from it rather than hand-authored beside it |
| **m17.9** | **the shadow bar** — scoped by m17.3's shadow numbers, which do not exist yet. Its own ADR if it reaches virtual shadow maps |
| **m17.10** | **the re-measured demo** — a fresh `docs/perf/` run on a clean tree against the ratified budget, and M13's frame-rate clause closed or its number restated with the reason |

**Cut order: m17.9 → m17.7.** **Never cut: m17.3, m17.5, m17.8, m17.10.** A milestone that cut the
measurement, the largest breach, the surface most of its pixels land on, or the final re-measurement
would be claiming a visual bar it never weighed.

**The ordering is load-bearing.** m17.3 comes first because every brick after it is a decision that
needs a number, and three of those numbers do not currently exist. m17.4 comes before the windowed
work because a hazard that only appears under `present()` will otherwise be found by looking at the
bar and mistaking corruption for a rendering bug. m17.8 comes after the two budget bricks because it
adds cost and Ruling 1 forbids spending before earning. m17.10 comes last because a baseline measured
before the optimisations cannot judge them, and one measured on a dirty tree — as the current block
baseline was — cannot be reproduced.

## What M17 does not fix, said plainly

- **VISION §3's UE5 column is not complete at the end of M17.** Virtualized geometry is deferred to
  M18 by Ruling 3.
- **There is no terrain at the end of M17.** m17.8 gives the ground an owner, a cooked material and
  a derived collider; it does not give it height, LOD, material blending or streaming. A scene whose
  ground is not flat is not authorable, and `docs/authoring-blender.md:138` already tells authors to
  "split by material at the object level instead" of blending — that instruction stands through M17.
- **Nothing here addresses the two review passes' open findings**, which live on their own list in
  `docs/ROADMAP.md` — the heap-write from a network packet, the out-of-range `MeshRef`, the cook
  cache's colorspace bug, and the rest. Several are more urgent than anything in this ladder. They
  are not folded in because a milestone that absorbs an unrelated defect list stops being a
  milestone, and because listing them here would make them look scheduled when they are not.
- **The physics work in m17.5 is unbounded until m17.3 runs.** This ADR deliberately does not name
  the fix. ADR-0035 §6 named one ("the every-tick narrowphase cache") and the roadmap has since
  quoted it as measured; repeating that pattern with a second guess would be worse than the first.
- **`frame.player` is a diagnostic, not a target.** M17 does not move the gate to the flattering
  measurement. If the engine meets 16.6 for one machine's work but the two-worlds-in-one-process
  demo does not, that is a result to state, not a budget to rewrite.

## Consequences

- The milestone's first three bricks produce **no visible pixels**. That is the cost of Ruling 1 and
  it should be expected rather than treated as a stall.
- The three re-baselined reports in m17.3 will make the current numbers in `docs/ROADMAP.md` stale.
  That is the point: they are quoted from a dirty-tree run on a driver two revisions old.
- Making `FingerprintMismatch` fail will break `perf.sh --all` on any machine whose driver has moved
  since its last committed baseline — including, today, this one. The break is the feature.
- Per-instance pass names change the shape of `docs/perf/` JSON. The in-tree parser reads duplicate
  keys into a vector and is unaffected; anything downstream that keyed on `"depth-prepass"` is not.
  Nothing in the tree does today.
- Deferring virtualized geometry means M18 is now spoken for before M17 finishes — and Ruling 5
  ranks the ground module alongside it, so M18 opens with two candidate tracks and an ordering
  question between them. Written down here so the next planning brick inherits it rather than
  rediscovering it.
- **m17.8 will make the numbers worse before m17.10 measures them.** A tessellated, textured ground
  costs more than two triangles; it lands after m17.5 and m17.6 precisely so that the budget is met
  first and the ground's cost is visible as a delta rather than absorbed into an unmet budget. If
  the ground alone breaks the budget again, that is a result about the budget worth having.
- `13-networked-player`'s 3×3 tiled floor exists for a GJK limit that no longer exists. Whether
  m17.8 unpicks it is out of scope for this ADR — it is a physics fixture, not a rendered surface —
  but the ground's new owner is where that decision will land.

## Alternatives considered

**Do the UE5 column first and the budget last.** Rejected by Ruling 1's argument: the column is four
scaling techniques and a scaling claim needs a known baseline. It also front-loads the fun work and
leaves a 2.1× reduction as the last brick of a long milestone, which is how frame-rate clauses get
carried a third time.

**Split M17 the way ADR-0039 split M16** — a "Frame Budget" milestone, then a "Visual Bar" milestone.
Genuinely tempting, and the precedent is exact. Rejected because M13's clause has already been
carried across two renumberings (M16 by ADR-0038, M17 by ADR-0039), and moving it a third time into
a milestone that does not exist yet is indistinguishable from not doing it. Keeping both clauses in
M17 with a stated internal order costs one paragraph and forecloses that.

**Move the budget.** The demo runs a server and a client in one process; `frame.player` p99 27.51 is
the honest one-machine number and 16.6 was ratified before the block existed. Rejected on ADR-0035's
own terms — the budget was re-ratified in the 2026-08-20 amendment *after* the baseline came in
under it, precisely so that it could not be renegotiated later. And 27.51 misses 16.6 by 1.66×
anyway, so the flattering number does not rescue the clause; it only obscures which half is slow.

**Drop `driver` from the fingerprint** so baselines survive driver updates. Rejected in Ruling 4: it
converts a loud correct refusal into a quiet wrong comparison, over the one variable the field
exists to control for.

**Skip m17.4 (the UBO ring) as speculative.** The hazard is reasoned, not reproduced — no corruption
has been observed. Rejected because m17.5 onward is judged by *looking at* a windowed frame, and an
un-ringed UBO under `present()` produces exactly the kind of intermittent artifact that would be
chased as a shading bug. The brick is small; the misdiagnosis it prevents is not.

---

## Amendment (2026-09-04, m17.3c): an adversarial review of this ADR and of m17.3a/b

The plan above and the two bricks that followed it went out for an adversarial pass (Fable 5,
read-only, its own judgement rather than a grade on mine). It found no landed bug and it found six
things worth writing down. Recorded here rather than folded into the text above, because ADRs are
append-only and a ruling that quietly changes is a ruling nobody can audit.

**What the reviewer actually ran**, since the strongest item below is an empirical one: the two new
doctest cases, all fifteen physics determinism witnesses on the m17.3b binary (5866/5866), and a
600-frame run of the *2026-08-30 RelWithDebInfo* `the_block --perf` under an LD_PRELOAD SIGPROF
PC-sampler (7,084 samples, resolved against the binary's own DWARF). That run reproduced the
committed baseline's work ledger exactly, which is what makes its profile a measurement of the same
tape rather than of a different run.

### Four corrections to the rulings above

1. **Ruling 1's headline argument is overclaimed, and its real support is stated more quietly.**
   "A frame that misses its budget by 2.1× cannot demonstrate a scaling technique" conflates two
   prerequisites. A scaling claim needs an *attributable, stable* baseline — you can show "cost
   stops scaling with triangle count" on a 35 ms frame by sweeping the count and getting a flat
   curve. What it strictly requires is **m17.3**, not m17.5/m17.6. The load-bearing arguments for
   budget-first are the two the Alternatives section makes: the clause is debt already carried
   across two renumberings, and ADR-0039's precedent. The conclusion stands; the argument for it
   is narrower than written.

2. **Ruling 1 never addresses replace-vs-optimise**, and it should, because the next milestone will
   inherit whichever it finds. The test is: *optimise only what the bar will not replace.* M17
   happens to pass it — m17.5 targets the simulation, which no UE5 technique touches; m17.6's named
   candidate (Hi-Z SSR) survives any VSM/Nanite future; shadow-path spending waits for m17.9 — but
   that is luck of the targets, not reasoning, and luck does not transfer.

3. **The cut order is inverted.** `m17.9 → m17.7` makes the sky-lighting brick cuttable while m17.8
   is not, and Ruling 3 itself calls that brick "the single largest visual change available for its
   cost". Cutting it re-defers exactly what ADR-0040 §6 already deferred once — which is the same
   debt argument Ruling 1 uses to justify its own ordering, applied against it. **Amended: the cut
   order is m17.9 only; m17.7 joins m17.3, m17.5, m17.8 and m17.10 as never-cut.** A ground that
   nothing lights well is the worse of the two half-milestones.

4. **M18 is at risk of being two milestones wearing one label** — the overload M17 just refused.
   Ruling 3's premise that virtualized geometry "most naturally subsumes terrain LOD" is a
   hypothesis with mixed industry evidence: UE5 shipped Nanite for years while Landscape kept its
   own LOD, and Frostbite keeps terrain dedicated. The epistemic posture (answer it in M18's ADR,
   from a pipeline that exists) is right. **Added as a pre-commitment: if both tracks survive M18's
   opening ADR, terrain becomes M19.** Ranking is a scheduling statement, not a promise, and saying
   so now is what stops the deferral becoming a pile-up.

### Two findings the ladder did not have, both inputs to m17.5

5. **The block never hands its `PhysicsWorld` a job system.** `set_job_system` is called by samples
   09 and 10 and by seven tests; the block's `Peer` builds a `JobSystem` for `propagate_transforms`
   and never gives it to either world. Verified by grep here, and by the reviewer's sampler: across
   600 frames, **zero** solver samples on worker threads, while two full worker pools spin-wait
   through the run. So an unknown part of the 4.25× `sim.block` breach is demo wiring rather than
   engine speed — and the engine's parallel solve path already has its bit-identity across worker
   counts proven by the ADR-0026 witnesses. m17.5 must establish which part before it optimises
   anything; a fix here would also move every number m17.3d is about to commit, so it lands after
   the re-baseline, not before.

6. **The zone decomposition folds client and server together.** `physics.*` merges both worlds while
   `sim.client`/`sim.server` split them, so the two decompositions do not compose and m17.5 would be
   optimising a merged distribution. Zones need a per-world tag or prefix before that work starts.

### What was refused, and what the profile settled

The m17.3b summary declined to state that its Debug probe puts the solver ~2× ahead of ADR-0035 §6's
predicted narrowphase. The refusal was right, and the reviewer's Release profile says why twice
over. In RelWithDebInfo the two stages are the **same order of magnitude** — direct solver chain
~31%, direct contacts+broadphase ~28%, with ~32% in shared out-of-line math, dominated by a
not-inlined `rime::core::rotate` (`quat.hpp:99`) and its interior `cross`, which `apply_inv_inertia`
in the solver and `PolySupport` in the narrowphase both hammer. (`rotate` is `inline`, not forced,
and is reached from `solver.hpp`, `support.hpp`, `narrowphase.hpp` and `hull.hpp` — verified here;
the sample counts are the reviewer's measurement and are not independently reproduced.) The Debug
ranking was an artifact of the build type *and* of the mix: at 40 frames `charge_frame` clamps to
20, so half the probe sat inside the collapse window, and under 100 samples nearest-rank p99 *is*
max. Quoting it would have planted a wrong finding in the same place ADR-0035 §6's last one was
planted. **m17.5's target is therefore still open, and the shared math is a third candidate** — a
flattening or SoA pass there helps both stages at once.

### One question this ADR must answer before m17.5, and does not

**Is the gated `frame` allowed to be a serialized loop?** It measures sim, declare, execute and
`submit_blocking` *in series*, so meeting 16.6 means CPU + GPU ≤ 16.6 with no pipelining — a
materially harder bar than "60 FPS windowed". That may be deliberate honesty; no ADR says. Pipelining
(sim of frame N+1 overlapping the GPU of N) is the largest single architectural lever toward the
number and at today's costs would turn 35.6 into roughly max(25, 11) on its own. **Ruled on in
writing before m17.5 commits effort, either way** — accepted as the definition of `frame`, or taken
as a brick — so that m17.10 cannot relitigate what the number meant.

### Accepted, rejected, and carried

Accepted and landed in m17.3c: the per-frame residual as a first-class named timeline gated through
the existing `Missing` machinery; the worst frame carrying its zone totals; the collapse-tick gate
(ADR-0035 ratified two collapse numbers and only one had a timeline); the timestamp pool raised
before the re-baseline rather than after; the SDF clipmap's per-dispatch names.

Rejected: **suppressing the all-zero `.per_frame` rows.** They double the report's zero-noise, which
is a real cost, but an all-zero row is the *proof* that a stage never ran — and a subsystem quietly
reporting no work is this engine's most repeated silent failure. The readability half was taken
instead: timeline keys are written in name order, so a zone sits beside its `.per_frame` twin.

Carried to m17.3d: **the GPU-side residual.** `frame.submit` is CPU wall and Σpass is GPU clock, so
their difference mixes domains and its margin has to absorb calibration skew; it must also stay off
any path a timestampless device reaches. Now that the pool brackets 128 passes the question is at
least clean, and the ~5 ms that ADR-0041's own §"five facts" could not attribute is worth a name.
Also carried: **pass-level gates** (m17.6 would otherwise optimise passes with nothing holding them
afterwards), and **m17.8 naming the heightfield-collider shape** its "derived collider" is a
placeholder for, so the physics side is not retrofitted later.

---

## Amendment (2026-09-04, m17.4/m17.5a): the pipelining question is answered — build it

The previous amendment left one question open: *may the gated `frame` be a serialized loop?* Luca
ruled to build the pipelining. What that ruling cost, and what it bought, both belong here.

### It was not a one-brick change, and the order was forced

`submit_blocking` was hiding two things, not one.

**m17.4 came first because it had to.** Seven lighting systems owned **fourteen** host-visible
buffers they rewrote every frame. GPU work is ordered against GPU work by the graph's barriers and
by queue order; a CPU write to host-visible memory is on neither. The moment the loop stops waiting,
frame N+1's `write_buffer` lands while the GPU still reads frame N — and the frame renders with next
frame's numbers. This ADR called that hazard "reasoned, not reproduced"; pipelining makes it
structural rather than hypothetical. `RenderGraph::push_frame_data` / `push_frame_buffer` is the
ring, owned once by the graph rather than copied into seven systems, and thirteen of the fourteen
moved onto it. The fourteenth — `ClusteredLights::empty_lists_` — stays owned, because it is written
once at construction and never again, and that distinction is the whole design: ring what the CPU
rewrites, not everything.

**The measurement problem was the real work.** `post_submit_`'s contract is "submitted AND
completed, with the graph still holding this frame's passes", and pipelining breaks it by
construction: when frame N's fence signals, the graph holds frame N+2's passes. Worse, a command
buffer OWNS its timestamp query pool, so `Device::wait()` — which waits and reclaims in one call —
would free every number in `docs/perf/` before anyone could read it. Two seams close that:
`Device::wait_and_borrow`/`release` opens the window between "the GPU finished" and "the submission
was reclaimed", and `RenderGraph::TimingPlan` snapshots at submit what only the current frame can
answer. A borrow deliberately **outranks completion** — `is_complete()` still answers true but no
longer reclaims — because otherwise any other subsystem politely polling its own ticket would free a
buffer a borrower is reading.

### Opt-in, and the fingerprint is why

Pipelining is `set_headless_frames_in_flight(n)`, default 1. Not caution — governance. Pipelined,
`frame` stops meaning "sim + render + GPU wall in series" and starts meaning "CPU wall, with the GPU
alongside". ADR-0035 decision 3 says the fingerprint decides what may be compared, and it has no
field for this, so the block writes `+pipelined-N` into its preset. A pipelined run therefore
**fails comparison against a serialized baseline by itself** — Ruling 4's refusal, applied to the
variable this ruling introduced, rather than left to whoever reads the diff.

### What it buys, stated before anyone hopes for more

From the committed 2026-08-30 baseline: `frame` p99 **35.60**, of which `frame.submit` — the CPU's
wait for the GPU — is **10.60**. Perfect overlap therefore lands near **max(CPU, GPU) ≈ 25 ms**.

> **Pipelining does not meet the budget, and was never going to.** 35.6 → ~25 still misses 16.6 by
> 1.5×, because `sim.block` p99 is 25.49 and the CPU frame very nearly *is* the simulation. This
> ruling removes the GPU from the critical path; it does not touch the thing on it. **m17.5 proper —
> the simulation budget — remains the brick that decides whether M13's clause closes**, and the
> review's finding that the block never hands its `PhysicsWorld` a job system is still the first
> thing it should check.

A Debug run of the block (120 frames, ratios only — Debug inflates the CPU far more than the GPU)
shows the mechanism doing what it claims: `render` p99 118.33 → 106.79 ms, the GPU wall disappearing
behind CPU work, with both runs producing a complete artifact.

### Consequences for the ladder

- **m17.4 is done, early**, and its scope grew: it was "the pass-owned buffer ring", and it also
  had to become the RHI's borrow seam and the graph's timing plan.
- **m17.3d's re-baseline must be taken SERIALIZED**, before any pipelined number is quoted. It is
  the before-picture, and a before-picture measured after the optimisation proves nothing — this
  ADR's own Ruling 4 note, applied to itself.
- **`on_post_submit` is superseded for measurement** by `on_frame_timings`, which hands over
  resolved, owned timings tagged with the frame they describe. The old hook remains for callers
  that want the graph itself; it simply does not fire on the pipelined path, and the block now says
  so out loud rather than printing the zero its unmeasured counter holds.

## Amendment (2026-09-06, m17.5): the budget is earned, and two ladder entries change

m17.5 is answered. Ruling 1 said the budget is earned before the bar is spent; this records what
earning it cost, and the two consequences Luca ruled on afterwards.

### What m17.5 found, in the order it found it

Four candidate levers, each settled with an interleaved A/B on a clock-pinned, guarded box, and
three of the four came back negative:

| lever | result |
|---|---|
| wire the block's `PhysicsWorld` to a job system | **nothing** (+0.27%) |
| gate the island dispatch on ACTIVE islands | nothing (+0.07%), kept for correctness |
| force-inline `core::rotate` | **−19.6% on `physics.solve`**, bit-identical |
| one physics step per TICK, not per destruction batch | **−6.4 ms `frame`, −6.7 ms `frame.player`** |

**The job-system finding is the one worth keeping, because the obvious reading of it is wrong.**
The previous amendment's finding 1 — the block never hands its `PhysicsWorld` a job system — was
true, and wiring it changes nothing measurable. Not because there is no parallelism available: at
the tick that sets the tail there are 25–31 active islands and every step is dispatched. It is that
**~92% of the awake bodies are in ONE island** (602 of 656 when first measured, 628 of 678 on the
committed baseline). Amdahl's ceiling on island-level parallelism there is **1.09×** — under a
millisecond of a 29 ms frame, at the edge of the rig's run-to-run spread. A measurement finding
nothing is exactly what that ceiling predicts.

**So island-level parallelism cannot close `sim.block`, and that is a structural fact rather than a
tuning one.** A collapsing building is one island *because its parts are in contact*, and contact is
what the partition is made of. Splitting the tail means splitting *within* an island — graph
colouring, or a Jacobi/hybrid velocity solver — which carries ADR-0026's determinism contract.
**Ranked with M18.**

**The lever that worked was not a physics optimisation at all.** The client took a full
`physics.step` per queued destruction batch, so the frames setting its p99 ran one server step plus
two client steps of ~8 ms. The fracture boundary that ADR-0033 A12 forbids merging is the
`DestructionWorld::update()`, **not** the step: two mirrors fed an identical pair of *remote* batches
— one stepping between them, one not — produce equal composition hashes and equal debris rosters,
while merging both into a single `update()` still hashes differently. The step belongs to the wall
clock, not to the batch; taking one per batch also ran the client's physics permanently *ahead* of
the server's (704 steps against 600).

*A correction worth recording, because it nearly went the other way.* The first version of that test
used `apply_damage` and **failed**, 13 chunks against 14 — local damage carries a world POINT that
`update()` resolves against the current pose, so for a Local instance a step between the blast and
the update genuinely does change which parts are hit. The client never takes that path. Testing the
convenient path rather than the real one would have "proved" the step load-bearing while measuring
something else entirely.

### The result against the clause

Committed baseline, median of three guard-passed 600-frame runs (`docs/perf/2026-09-06-99-the-block-…`):

| | opening | now |
|---|---|---|
| `frame` p99 | 28.915 | **20.976** |
| `frame.player` p99 (client + render) | ~20.2 | **12.556** |

**`frame.player` meets the ratified 16.6 ms.** M13's playable-frame-rate clause is met **for one
machine**. The gated `frame` is 20.976 — 1.26× over — and it **stays** the gated number: it hosts an
authoritative server *and* a predicting client in one process, which no shipped configuration does,
and moving the goalposts to the flattering measurement is what a ratified budget exists to prevent.
The two numbers now say different things, and the ADR says which is which rather than choosing the
kinder one.

### Ruling 6 — m17.6's premise no longer exists; it is re-scoped, not cut

Every number scoping m17.6 in the ladder above was measured on a GPU the driver was parking mid-run:

| | m17.6's premise | clock-pinned |
|---|---|---|
| `frame.submit` p99 | 10.600 | 2.785 |
| `ssr-resolve` max | 4.455 | 0.902 |
| `forward-pbr shadowed` max | 4.051 | 0.664 |

All twelve GPU passes together are **1.841 ms at p50 and 2.407 ms at max against a 16.600 ms
budget — 11% of the frame at p50 (1.841 / 16.600), 14% at max.** [Corrected 2026-09-17: the prior
text said "8%", which does not follow from the 1.841/16.600 figures stated in the same sentence —
an arithmetic slip, not a re-measurement; the 1.841 and 2.407 ms figures themselves are unchanged
and are not in question.] There is no GPU budget breach to close, and there never was one on a
machine whose clocks were pinned; m17.3d's guard is what made that visible.

**m17.6 is therefore re-pointed from cost to correctness.** Its brick is no longer "reduce the GPU
budget" but "the passes are right, and the headroom is recorded" — the cloud layer's unmeasured
per-pixel cost (named in ADR-0040's consequences) belongs here, and so does establishing what
headroom m17.8's textured ground and any later sky work are spending *into*. Cutting it outright was
considered and refused: the ladder is ratified, and a milestone entry that vanishes without a record
is exactly what Ruling 4 objects to elsewhere.

### Ruling 7 — m17.7 defers behind m17.8; the ground comes first

The ladder ordered the sky (m17.7) before the ground (m17.8). That order is reversed.

ADR-0040 §6 — the decision m17.7 was scheduled to take — says of the Hillaire atmosphere that it
"is not scheduled here… a milestone-sized brick" which "should not land before there is authored
content worth judging it against." **The ground is that content.** Ruling 5 already says why: the
ground is the largest thing in almost every frame, it is what every shadow lands on and what SSR
reflects, and a flat untextured plane makes DDGI bounce, SSR and shadow quality *unjudgeable* —
"there is no normal variation for a reflection to bend around, no albedo for a bounce to tint, and
no detail for a shadow to read against." Making the sky light that surface first would be tuning a
light against a floor that cannot show what the light does.

The dependency Ruling 5 asserted also now resolves the other way round: it said a textured ground
"may not precede m17.5 and m17.6" because it *adds* cost. m17.5 is closed with 7.9 ms of headroom on
the gated frame and the GPU at 11% of it (§ above, corrected 2026-09-17), so Ruling 1's test is
passed and the ground brick is unblocked. m17.7's scope — minimal analytic radiance versus the full
four-LUT model — is deferred with it and decided against a ground worth judging.

### Consequences for the ladder

- **m17.5 is done**, and it closed the clause on one machine rather than on the gated number.
- **m17.6 is re-scoped** to pass correctness and headroom accounting. Its original budget target is
  recorded as met-on-arrival once the clocks were pinned.
- **m17.7 moves behind m17.8** and its scope is decided later, against authored ground.
- **m17.8 is next**, at Ruling 5's scope: one owned ground surface with a cooked material, a
  collider *derived* from that surface rather than authored beside it, and a seam that a later
  heightfield replaces rather than is retrofitted into.
- Cut order and never-cut list are unchanged.

## Amendment (2026-09-07, m17.8): Ruling 5's title, and what the ground brick actually found

**Ruling 5's title says "a ground *module* is not [in M17]" and its body asks for "one place that
answers 'what is the ground', producing both the drawn surface and the collider", shared by two
samples and the editor. In this repository those cannot both be satisfied**: `samples/` has no
shared code, and a thing two samples and an engine host all link is a library. The title is read as
what its own next paragraph says it means — *"A **terrain** module is NOT in M17"* — and the brick
was built on the body. `engine/ground` is deliberately named `ground` rather than `terrain` so M18's
question (does virtualized geometry subsume terrain LOD?) stays open, and it holds no heights, no
LOD, no splat blending and no streaming.

**The defect was worse than Ruling 5 counted.** It listed three places; there were five, and three
of them were live in `99-the-block` at once:

| | half-extents | spans |
|---|---|---|
| drawn | 38 x 38 | x[-16, 60] z[-38, 38] |
| collided | 44 x 44 | x[-22, 66] z[-44, 44] |
| GI-traced (SDF clipmap instance 0) | 26 x 14 | x[ -4, 48] z[-14, 14] |

So a player could stand six metres past the visible edge of the world, and the DDGI probes lit a
street a third the size of the one being drawn. The fourth was `13-networked-player`'s 3x3 grid of
10 m boxes; the fifth was the editor host's own floor, where the comment reads
`make_plane(half_extent, uv_tiles)` as width/depth — there is no depth argument — so a 20 m square
plane sat over a 10 x 4 m collider whose top face was 10 cm *above* it.

**Ruling 5's other factual claim checked out, and was worth checking.** It says the "10 m per box is
a measured GJK limit" justifying `13`'s tiling was "measured as gone". This machine's own notes said
the opposite — an open collision-core defect losing 10 cm of overlap at ≥30 m half-extents — and a
76 m ground collider was about to be built on the answer. Re-measured over 324 configurations
(half-extents 10 to 50 m, depths 1 mm to 20 cm, aim points from the box centre to 99% of the
half-extent): **zero misses**. Ruling 5 is right and the notes were stale;
`tests/gameplay/character_fixture.hpp` now says so rather than arming the next reader with a
superseded constraint.

**Two things the ladder did not name, recorded so they are not rediscovered as surprises.**
`99-the-block` has no asset runtime at all — no `AssetServer`, `Manifest` or `GpuAssetBridge` — so
"the first thing to use M16's asset path over a large area" means standing that path up in the demo
for the first time, which is plausibly a larger job than the ground module was. And M16 cannot
express a standalone material (a carrier glTF is mandatory) or a uv transform in the material
record, which is why tiling lives on `GroundSurface::tile_metres` and not on the material.

**A follow-up this brick declined to fold in.** Adding one component to blockkit broke *three*
private component-registration lists (`blockkit_test`, `block_standup_test`, `block_render_test`),
and `editor_host_app.cpp`'s `build_viewport_scene` keeps a fourth. That is precisely the drift
ADR-0037 built `worldkit` to end, still live in the places worldkit does not reach. Registering the
new component in each was the small fix; switching them to the profile is the right one, and it is
its own brick rather than a rider on this one.

## Amendment (2026-09-07, m17.8b): the ground's material is generated, not authored

Ruling 5 asked for "one owned ground surface with a cooked material" and called it "the first thing
to use M16's asset path over a large area, which is a proof M16 never got". It did not say where the
material would come from, and the answer turned out to be a decision rather than a lookup.

**There is no texture art in this repository.** The whole of the tree's source art is four
hand-authored glTF files — `cube`, `sphere`, `rig`, and three test fixtures — and not one image.
So "the ground gets a cooked material" had three possible readings, and two of them are bad:

1. **A factors-only material.** Cooked, and satisfies the letter. But Ruling 5's argument is that a
   constant surface makes SSR, DDGI bounce and shadow quality *unjudgeable* — "no normal variation
   for a reflection to bend around, no albedo for a bounce to tint". A cooked flat colour is the
   same flat colour with more steps, and would have closed the brick while changing nothing the
   ruling cared about.
2. **Commit authored art.** A carrier glTF plus PNGs, cooked through the existing `#materialN`
   path — the smallest engine change, and the first design tried. It needs images nobody has
   authored, and a hand-painted tiling asphalt is *worse* than a generated one at the property that
   actually matters here: whether it wraps.
3. **Generate it in the cook.** Taken.

`rime ground` (`tools/asset-pipeline/src/ground.rs`) synthesises albedo, normal and
metallic-roughness from a periodic value-noise height field and cooks them with a standalone
material. It follows the `fracture:` precedent exactly — the *config is the source*, so there is no
input file and nothing for the cook cache to stat — and it keeps the ground derived data the
manifest can regenerate rather than binary art the repository has to carry.

**Wrapping is the property, and it is proved rather than eyeballed.** The ground tiles every
`tile_metres` (4 m on the block), so a texture that does not wrap does not show *a* seam, it shows a
grid of dozens across the frame — visibly worse than the flat colour it replaced. Every octave is
therefore value noise on a lattice wrapped modulo its own period, and `seam_is_invisible` measures
the step across the wrap against the texture's own largest interior step. Comparing against the
interior rather than a fixed tolerance is what makes it falsifiable in both directions: it fails for
a non-wrapping generator at any noise amplitude, and it cannot be passed by making the texture
flatter. Removing the wrap fails it (seam 10 against interior 5); the other three generator tests
still pass, which is what says the seam test is testing the seam.

**The mean is deliberately unchanged.** Albedo and roughness are centred on the palette's old flat
values (`0.10, 0.10, 0.11` at roughness `0.45`). `palette.cpp` records why the road is dark and
comparatively smooth — "at dusk the road is what carries the lamp highlights, and a matte road at
this light level is a black hole with buildings floating on it" — and moving the mean would have
quietly invalidated that tuning together with every lighting number measured against it. This brick
adds variation; it does not relight the scene.

**The entity-owned reference it needed:** see
[ADR-0039](0039-authored-surfaces-m16.md)'s m17.8b amendment. The short version is that a derived
mesh has no cooked mesh to hang a `#materialN` join off, so the ground names its material directly.

### What the brick found: m16.7's BC7 never reached the GPU

The most valuable thing m17.8b produced is not the ground. It is that **the ground was the first
scene asset ever cooked with `--bc`**, and doing so revealed that block compression had never worked
end to end.

m16.7 shipped the BC7 encoder, the container support, the reader and an RHI test. What it did not
ship was the one line joining them: `GpuAssetBridge`'s `to_rhi_format` mapped only `Rgba8Srgb` and
`Rgba8Unorm`, and answered `RGBA8Unorm` from a `default:` label for the three block formats. So a
512x512 BC7 texture — 350 KB of blocks — was described to the GPU as a 1 MB RGBA8 image, and
`write_texture_mips` copied a megabyte out of a 350 KB staging buffer, level after level. The
validation layer said so ten times per run (`VUID-vkCmdCopyBufferToImage-pRegions-00171`), the
albedo lost its sRGB decode, and the sampler read whatever followed the staging buffer in host
memory.

**Every proof stayed green**, including this brick's own. `render: the street wears its COOKED
material` asserts that the material's base-colour texture is resident and is not the magenta
placeholder — both true. The upload happened; it just read past its source. This is one level
deeper than the failure the placeholder check was written for (`valid-is-not-resident`): the handle
was valid *and* not the placeholder *and* the pixels were garbage. A structural claim about
residency cannot see a byte-level error, and the only thing that did see it was the validation layer
writing into a log nobody was reading.

Three things follow, and they are the general lesson rather than the specific fix:

1. **A `default:` label in a format switch is a silent wrong answer waiting for its first caller.**
   The block cases are now spelled out and `default:` is gone, so a seventh texture format is a
   compile error rather than a mis-described image.
2. **A capability shipped without a consumer is a capability that has not been tested.** m16.7's RHI
   test proved the RHI could create a BC7 texture; nothing proved the asset path could deliver one.
   The gap survived a milestone.
3. **Validation-layer output belongs in the pass/fail decision.** The evidence was sitting in
   `build/dev/Testing/Temporary/LastTest.log` through every green run of this brick.

The device-capability half of ADR-0039's rule is now honoured too: `AdapterInfo::block_compression`
is checked before a block-compressed upload, and a device without BC gets a warn-once and a named
counter (`GpuAssetBridge::textures_refused_unsupported`) rather than a silent placeholder, with
`99-the-block` refusing to start rather than measuring a magenta road.

### What it cost: real on the GPU, and it reaches the frame — Ruling 1 was right

The A/B is unusually clean because the brick shipped its own control: the ground falls back to the
palette's flat material when no cook is present, so **the same binary, the same scene and the same
frame** can be run with the cooked material on and off by adding or removing one `manifest.txt`.
Three interleaved pairs on a clock-pinned RTX 3060, 600 frames each, `parts.alive_end` 1314 and
`draws.submitted` 1834 in all six runs — an identical workload, not merely a similar one.

**Which arm a report came from is now recorded rather than remembered.** `ground.materials_bound`
is in the work ledger: 1 in all three textured runs, 0 in all three flat ones. Before it, the only
evidence of which arm a report belonged to was which file the operator had moved — and a toggle that
silently stopped working would have published as "the cost is nil", which is the exact shape of the
wrong answer this section carried in its first draft. The summariser voids the comparison if the
arms do not separate.

| | flat | textured | delta |
|---|---|---|---|
| `frame.submit` p50 | 2.509 | 2.639 | **+5.18%** |
| `frame.submit` p99 | 2.873 | 3.067 | +6.75% |
| `frame.render` p50 | 4.215 | 4.337 | **+2.89%** |
| `frame.render` p99 | 4.616 | 4.769 | **+3.31%** |
| `frame` p99 | 19.717 | 19.849 | **+0.67%** |
| `frame.player` p99 | 11.879 | 12.080 | **+1.69%** |
| `sim.block` p99 | 15.454 | 15.468 | +0.09% |

Median of three per arm. **Bold** marks the rows whose flat and textured ranges do not overlap at
all — a stronger statement than any percentage, because it does not depend on a noise model. Only
`frame.submit` p99 and the `sim.block` control overlap.

**The GPU cost is real and separable.** `frame.submit` p50 moves +0.130 ms and `frame.render` p50
+0.122 ms, both with disjoint ranges: about **0.13 ms of GPU time** for three BC7 samples over the
largest surface in the frame. That is the cost Ruling 1 predicted would exist, and it does.

**And it reaches the frame. An earlier draft of this section said it did not, and that was wrong.**
`frame` p99 moves +0.132 ms with disjoint ranges — the worst flat run (19.727) is faster than the
best textured one (19.832). To within a thousandth of a millisecond, the delta on the whole frame is
the delta on `frame.submit`: the GPU cost passes **through** to the frame rather than being absorbed
by it. The earlier draft claimed the opposite — "`frame` p99 moves 0.23% and its ranges overlap
completely… the block is CPU-bound, so 0.14 ms of extra GPU work disappears into it" — and that was
an artefact of a run measured while another process saturated the CPU. Contention inflates tails, and
inflated tails swallowed a real 0.13 ms effect. The *mechanism* was wrong, not merely the number, and
it was wrong in the direction that flattered the brick.

**`sim.block` is the control, and it behaves.** The ground's material cannot touch physics; its p99
moves +0.09% with overlapping ranges. That is a direct read of the noise floor, and it is why the
frame-level separation above can be believed.

**So Ruling 1's ordering was correct, and for the reason it gave.** A tessellated, textured ground
does add cost; it may not precede the budget bricks. What the measurement adds is *where* the cost
lands — on `frame.submit`, m17.6's budget, not on the simulation — and that it does not stop there.
`frame.player` carries +0.201 ms of it, which is the number m17.10 should be re-measured against.

**Two methodological notes, because this section went the wrong way twice.**

*The first measurement was taken on a broken configuration.* It said the ground was free
(`frame.submit` p50 2.526 flat against 2.533 textured), and it was taken while BC7 was being
uploaded as RGBA8 (see above): the sampler was reading a broken mip chain, which is not the work the
shipped path does. **A perf result measured on a broken configuration is not conservative, it is
meaningless** — it happened to under-report here, but it could as easily have over-reported. What
caught it was reading the validation log, not re-reading the numbers.

*The second was taken on a contended box, and nothing in its exit code said so.* `99-the-block`
always fails `perf.sh`'s budget gate (`sim.block` p99 ~15 ms against a 6 ms budget), so **every** run
exits 1 — and a run abandoned for CPU contention also exits 1. The two are distinguishable only by a
line in the log, and the numbers reached this ADR before anyone read that line. The A/B is now driven
by a script that checks for the contention marker and refuses to accept a run that filed no report;
all six runs above are certified by it, and by a clock trace that never left 1785/7501.

The sample remains gated for the reasons this ADR already records, and this brick does not change
them: `frame` p99 19.849 against 16.600 and `sim.block` p99 15.468 against 6.000, neither of which
this brick touches. `frame.player` — one machine's share — is 12.080 against the ratified 16.600, and
still meets it.

The committed baseline for this brick is filed by its own `--commit` run on the brick commit rather
than on a dirty tree, the convention every report here but `2026-08-30` follows. The A/B above stands
on its own regardless: six runs, one workload, both guards green, and the arms proved apart by a
counter rather than by recollection.

## Amendment (2026-09-17, m17.8b): the table above is superseded — clock pinning is gone, the
re-take needed a different method

Luca withdrew clock pinning (`-lgc`/`-lmc`, both root-only) between the previous amendment and this
one: the workstation is now genuinely shared, and re-pinning it for every perf run was costing more
than the milestone could ask for. The table two sections up was taken clock-pinned; it is **not
reproducible as measured** and the driver script that took it is gone (wiped `/tmp`). This amendment
does not retract its finding — see below, it confirms the same effect — but the table itself is
superseded rather than re-typed, per this ADR's own append-only rule.

**Unpinned, the standard `--perf` loop does not separate the arms.** Three independent interleaved
T/F sessions were run on the unpinned box (`h1`: 8 runs, `h2`: 8 runs, both 2026-09-16 (`h2`'s last
two completed 2026-09-17, continuing the same series — see the h2-completion note below); `r2`: 8
runs, 2026-09-17). Taking the adjacent-pair T-F difference on `frame` p50 the same way the withdrawn
table did:

| session | `frame` p50 A/B median (T−F) | `frame` p50 A/A noise floor (median \|Δ\|) |
|---|---|---|
| h1 | −0.276 ms | 1.348–2.124 ms |
| h2 | +1.934 ms | 2.073–3.387 ms |
| r2 | +0.984 ms | 0.014–1.599 ms |

The sign is not even consistent across sessions, and the effect is smaller than or comparable to the
same-arm noise floor in all three. **This is not the ground's material being free — it is the
unpinned GPU's boost/park behaviour dominating a 0.1 ms effect inside a 15–20 ms frame that also
carries a full physics step.** A table built from this data would be reporting noise.

**The fix a Fable-designed, Qwen-audited harness proposes: freeze the simulation and force the GPU
to stay busy.** `--hold N` (a scratch-only patch to `samples/99-the-block/main.cpp`, applied in a
disposable worktree, never in this tree — see below) appends N render-only frames after the measured
loop, with the simulation not stepped, so every hold frame re-renders the same post-collapse scene.
This is what makes the GPU boost at all on this box: the sim-bound `--perf` loop parks it between
frames, and a park-to-boost transition costs far more than the 0.1 ms this brick is trying to see.
Full design and its own audit: [`docs/perf/m17.8b-hold/QWEN-ESTIMATOR-VERDICT.md`](../perf/m17.8b-hold/QWEN-ESTIMATOR-VERDICT.md).

**Where the evidence lives.** The raw per-frame data — 24 runs, 13 MB of hold-loop CSVs plus clock
and contention traces — stays on the reference workstation at `~/rime-perf-harness-m17.8b/`: too big
for the repository, and outside it by size rather than by accident. What *is* committed, in
[`docs/perf/m17.8b-hold/`](../perf/m17.8b-hold/), is a per-run summary (`runs.csv`, one row per
run) from which `pool.py` regenerates every number in this amendment, plus the original drivers and
analysis scripts verbatim. `docs/perf/`'s own rule (ADR-0035 §2c) is that the numbers live in the
repo; the summary is how they do without the megabytes.

**The clock-boost filter that harness first shipped with is a collider, but the bias it introduces is
small enough to ignore, and `r2` confirms that finding on fresh data rather than re-quoting it.**
Filtering hold frames on a `.clk`-trace boost gate (kept `gr≥1700 ∧ mem≥7000`) selects differently
by arm — F drops more frames than T, because the flat arm's lighter GPU load lets the card park more
readily — which is exactly the shape that can bias an estimate. The audit measured how much on
`h1`/`h2`; `r2` re-measures it independently. This table's estimator is the **difference of per-arm
medians** (all four T runs against all four F runs), not the adjacent-pair estimator the pooled table
below uses, which is why its unfiltered `frame_p50` and `submit_p50` do not equal that table's `r2`
column:

| metric | r2 filtered (clock-gated), median(T) − median(F) | r2 unfiltered (all post-settling frames), median(T) − median(F) |
|---|---|---|
| `gpu_sum_p50` | +0.1021 ms | +0.1025 ms |
| `frame_p50` (hold) | +0.1072 ms | +0.1026 ms |
| `submit_p50` (hold) | +0.1096 ms | +0.1084 ms |
| `ssr_p50` | +0.0973 ms | +0.0973 ms |

Filtered and unfiltered agree to within 0.005 ms on every row — the same order-of-magnitude agreement
the audit found on `h1`/`h2`. **The primary estimator adopted here, per the audit's recommendation, is
the unfiltered one**: per-run median over all frames after a 100-frame settling skip (no clock
conditioning, so no collider bias), with the adjacent-pair T-F difference as the effect and adjacent
same-arm pairs as the empirical noise floor. The clock trace is kept as a per-run diagnostic only —
no run in any session was boosted by that gate for more than half its hold frames (40–50% across all
24, `pct_boosted` in `runs.csv`), which is reported, not filtered on; it is the unpinned box's
ordinary behaviour, not a bad run.

### The re-taken table (hold-loop, render-only, sim frozen)

Three interleaved sessions, all on the unpinned box, all analysed by the same unfiltered estimator.
Each session's rows are in `runs.csv`; the raw data sits in the named directory under
`~/rime-perf-harness-m17.8b/` on the reference workstation (see *Where the evidence lives*, above):

| session | order | runs | date | raw data | original analysis |
|---|---|---|---|---|---|
| h1 | T F F T T F F T | 8 | 2026-09-16 | `h1/` | `analyze_unfiltered.py h1 100` |
| h2 | F T T F F T T F | 8 | 2026-09-16 + 2026-09-17 (last 2 runs) | `h2/` | `analyze_unfiltered.py h2 100` |
| r2 | T F F T T F F T | 8 | 2026-09-17 | `r2/` | `analyze_unfiltered.py r2 100` |

All three sessions ran the identical workload in every run (`draws.submitted` 1834, `parts.alive_end`
1314) and separate cleanly on `ground.materials_bound` (1 in every T run, 0 in every F run) — the same
control this ADR's withdrawn table used, still doing its job. `h2` was completed to its full designed
8-run order (`F T T F F T T F`, matching `h1`'s design mirrored) on 2026-09-17 — see the h2-completion
note at the end of this amendment for the two added runs' own details.

Per-session and pooled effect (12 adjacent T/F pairs across all three now-complete 8-run sessions)
against the pooled noise floor (9 adjacent same-arm pairs):

| metric | h1 effect | h2 effect | r2 effect | **pooled effect (n=12)** | **pooled \|noise\| (n=9)** | ratio |
|---|---|---|---|---|---|---|
| `gpu_sum_p50` | +0.1087 | +0.1065 | +0.1025 | **+0.1058 ms** | **0.0089 ms** | 11.9× |
| `frame_p50` (hold) | +0.1150 | +0.1078 | +0.1272 | **+0.1087 ms** | **0.0383 ms** | 2.8× |
| `submit_p50` (hold) | +0.1089 | +0.1077 | +0.1116 | **+0.1089 ms** | **0.0171 ms** | 6.4× |
| `ssr_p50` | +0.1009 | +0.0988 | +0.0973 | **+0.0983 ms** | **0.0020 ms** | 48.0× |

Every cell regenerates from the committed summary (`python3 docs/perf/m17.8b-hold/pool.py`). The
ratios are at full precision. The original `pool_sessions.py` pooled per-pair deltas that had already
been rounded to four decimals; at this table's precision that moves exactly one cell, `ssr_p50`'s
ratio, which its saved output (`pooled-analysis.txt`) prints as 49.1×. `pool.py` reproduces that
output byte-for-byte by replicating the rounding, and says so in its comments.

Every row's pooled effect is a small positive number, consistent across three independently-run
sessions — within 0.004–0.006 ms of each other on `gpu_sum_p50`, `submit_p50` and `ssr_p50`, and
within 0.019 ms on the hold loop's `frame_p50` — and at least 2.8× its own noise floor. **This confirms the
withdrawn table's headline finding — a real, separable ~0.10–0.11 ms GPU cost for the cooked BC7
ground material — by an independent method, on an unpinned and genuinely shared box, across three
separate sessions on two different days, not merely by re-quoting the pinned-box number.** (Completing
`h2` to 8 runs moved the pooled ratios by ≤2 points on every row except `ssr_p50`, whose noise floor
happened to shrink — see the h2-completion note for the reproduction command; the conclusion is
unchanged, this is the same effect measured with one more independent pair.)

### The claim this table supports, and the one it does not

**This is a render-only, hold-loop measurement.** The hold frames are produced with the simulation
frozen in its post-collapse state — no physics step, no collapse, no destruction. It measures the
GPU pass-time cost of the material under those conditions. It does **not**, by itself, measure the
cost as it reaches a gameplay frame, which also carries culling variance, frame-pacing and a live
simulation the hold loop deliberately removes.

The non-hold `--perf` data gathered in the same three sessions was checked for whether it could
independently support the stronger "reaches the gameplay frame" claim the withdrawn table made. It
cannot, on this unpinned box: pooling the same adjacent-pair method over `frame` p50 from the
standard (non-hold) loop across all three now-complete sessions gives a pooled effect of **+0.98 ms**
against a pooled same-arm noise floor of **1.60 ms** — the purported effect is smaller than the noise
it would have to be measured against. The same pattern holds for `frame.submit` p50 (effect +0.31 ms,
noise 0.58 ms) and `frame.render` p50 (effect +0.54 ms, noise 0.99 ms). **The wording is therefore
scoped to what was actually measured:**

> The hold-loop protocol measures the GPU pass-time cost of the cooked BC7 ground material under
> render-only conditions (simulation frozen, scene static): approximately **0.11 ms** per frame
> (`gpu_sum_p50` +0.1058 ms, `frame_p50` +0.1087 ms, pooled over 12 T/F pairs across three sessions),
> against a same-arm noise floor an order of magnitude smaller for the GPU-side metrics. **This is
> the material's isolated rendering cost. It is not, on the data gathered so far, shown to reach the
> gameplay frame** — the non-hold measurement on this unpinned box cannot currently separate a
> ~0.1 ms effect from its own noise, which is a statement about this measurement's resolution on a
> shared, unpinned machine, not a claim that the cost disappears.

This requalifies the withdrawn table's "and it reaches the frame" claim. That claim was made on a
clock-pinned box where the non-hold frame noise floor was much smaller; unpinned, the same claim is
not currently supported and is not repeated here. Closing that gap — either by re-pinning for a
single confirming run, or by a longer non-hold series whose noise floor the pooled 0.1 ms effect can
clear — is left as an open item, not asserted.

### Methodology notes, so the numbers are not re-derived from a different tree by accident

- **The binary measured is not the tree's own `build/release`.** The `--hold`/`--hold-out` flags are
  a scratch-only source patch (`docs/perf/m17.8b-hold/patch_hold.py`), applied to a disposable
  `git worktree` built from this branch's tip *plus* the uncommitted m17.8b brick patch, never to the
  tracked tree. `r2`'s worktree was built at commit `4a8383a` (this branch's tip at measurement time)
  with the dirty brick's 22 files applied on top; `h1`/`h2` were built the same way in now-deleted
  session-scoped worktrees — `h2`'s last 2 runs (2026-09-17) used a freshly rebuilt worktree at the
  same commit and patch, since the original had not survived (see the h2-completion note). The real
  `build/release/samples/99-the-block/cooked/manifest.txt` was never moved — every run reads a
  `cooked_T`/`cooked_F` directory **copy**.
- **Every run's identical `draws.submitted`/`parts.alive_end`** is what makes the pairing valid: three
  sessions, twenty-four runs (12 T, 12 F), one workload — the withdrawn table's own guard against a
  run silently measuring less, still holding.
- **`ground.materials_bound` separates every run correctly** (1 for all 12 T runs, 0 for all 12 F
  runs across h1+h2+r2) — the toggle did not silently stop working in any of the three sessions.
- **This sample always exits 1** on the perf gate (`frame`/`sim.block` breach the ratified budget
  regardless of arm) — every run above exited 1, and that is expected, not a failure of the run.

### h2 completion note (2026-09-17)

`h2` was left at 6 of its designed 8 runs (`F T T F F T`) when the 2026-09-16 series stopped — a
fresh `r2` series was judged higher value at the time. The 2 remaining runs were taken on 2026-09-17
to complete `h2`'s original design (`F T T F F T T F`, mirroring `h1`'s `T F F T T F F T`), in a
freshly rebuilt disposable worktree (the original had not survived, like every other worktree from
2026-09-16 — see the methodology note above):

- `r07-T` — textured arm (raw: `h2/r07-T.{json,clk,cpu,hold.csv,log,meta}` on the workstation;
  row `h2,r07-T` in `runs.csv`). `ground.materials_bound=1`, `draws.submitted=1834`,
  `parts.alive_end=1314`, `foreign_max=211`, 43% clock-boosted.
- `r08-F` — flat arm (raw: `h2/r08-F.*`; row `h2,r08-F`). `ground.materials_bound=0`,
  `draws.submitted=1834`, `parts.alive_end=1314`, `foreign_max=127`, 45% clock-boosted.

Both runs used the same worktree binary, the same `--frames 600 --hold 1500` parameters, and the same
`cooked_T`/`cooked_F` directory copies (byte-identical to the fresh worktree's own `cooked/` output,
verified by checksum before use) as every other run in this amendment. The control properties and
clock-boost percentage are in the same range as the other 22 runs — nothing about these 2 runs looks
different from the rest of the dataset.

Completing `h2` added one same-arm pair (`r06-T`, `r07-T`) and one T/F pair (`r07-T`, `r08-F`),
moving the pooled hold-loop numbers from n=11 A/B pairs / n=8 A/A pairs to n=12/9. Every pooled
number above is the pooling script's output, not a hand-edit of an earlier draft's numbers
(reproduce: `python3 docs/perf/m17.8b-hold/pool.py`; the original script's saved output is
`pooled-analysis.txt` beside it). As `pool_sessions.py` computed them, the shift was small:
`gpu_sum_p50` +0.1065→+0.1058 ms, `frame_p50` +0.1092→+0.1087 ms, `submit_p50` +0.1122→+0.1089 ms,
`ssr_p50` +0.0983→+0.0983 ms (unchanged to 4 decimals); ratios moved by ≤2 points except `ssr_p50`'s,
whose pooled noise floor shrank from 0.0036 to 0.0020 ms once the new, very low-noise same-arm pair
joined the pool, taking that ratio from 27.7× to 49.1× (48.0× at full precision — see the note under
the table). **None of this changes the amendment's conclusion or its blockquoted claim** — the same
effect, now measured with one more independent T/F pair.

## Amendment (2026-09-20, m17.7): Ruling 7's deferred scope, decided — the full Hillaire model, staged

Ruling 7 deferred m17.7's scope on purpose: *"minimal analytic radiance versus the full four-LUT
model — is deferred with it and decided against a ground worth judging."* m17.8b landed the ground
and its cooked BC7 material, so the deferral is discharged and the decision is due. **Luca's call:
the full Hillaire-2020 four-LUT model is the destination, with the sky-view LUT + SH irradiance
built first as its foundation.**

Recorded plainly, because the decision was taken with the cost in front of it and a later reader
should see that: [ADR-0040](0040-sky-and-atmosphere.md) §6 calls the Hillaire model "a
milestone-sized brick" and explicitly does **not** schedule it, and aerial perspective touches every
lit pixel against a frame whose p99 already misses 16.600 ms unpinned. A published figure for a
complete atmosphere of this shape — multiple scattering, aerial perspective, dynamic volumetric
clouds, god rays — is **under 1.5 ms on an RTX 4080**; this workstation is a 3060, so the
aerial-perspective brick should expect to be arguing for several milliseconds it does not currently
have. That is why m17.7 is a **ladder of five**, and why the expensive tail is last and separately
cuttable rather than bundled into one brick that can only be taken whole.

| brick | what lands | cuttable? |
|---|---|---|
| **m17.7a** | **shader `#include`** — the enabling brick. `-I` plus a **depfile**, so a shared `.glsl` cannot leave dependent SPIR-V stale | no (blocks everything after) |
| **m17.7b** | **the sky lights the scene** — a sky-view LUT and SH irradiance, filled by the *existing analytic* sky. All three `ambient_` reads become sky-derived. Delivers the milestone's visual claim on its own | no |
| **m17.7c** | transmittance + multiple-scattering LUTs — the two view-independent ones | no |
| **m17.7d** | the sky-view LUT's body becomes physical. Nothing downstream moves — this is m17.7b's payoff | no |
| **m17.7e** | aerial perspective (the froxel volume) — touches every lit pixel | **yes, first** |

**If the frame cannot afford the whole model, m17.7e is what gets cut**, and the claim lost is "the
landscape reads as kilometres deep" — not "the sky lights the scene", which m17.7b already owns.
That separation is the point of the staging.

### Why the ladder gained an enabling brick it did not have when the plan was approved

The plan as approved put shader includes *second*. Writing the SH projection showed that wrong: the
sky-view mapping — direction ↔ texel and its solid-angle weight — is needed by **both** the LUT-fill
shader and the SH-projection shader, so the copy-paste hazard the includes brick exists to prevent
bites one brick earlier than written. `ssr_resolve.frag:52` already records what that costs in this
repo ("a verbatim COPY of the forward shader's… no shader-include mechanism exists"); `oct_decode`
has three copies, `fibonacci_direction` three, `ddgi_sample_irradiance` two, `sdf_sample` two.
ADR-0040 §2's promise — that a physical atmosphere replaces `sky_radiance()`'s **body** "without any
other file moving" — only holds while there is exactly one body to replace.

The **depfile** is the half that makes includes safe, and omitting it would have been the real
defect: without it, editing a shared `.glsl` leaves every dependent `.spv` stale while the build
still reports success, which is the stale-artifact failure this repo already paid for once with a
stale executable in m15.6.

### One correctness finding, recorded because it would not have looked like a bug

`sky_radiance()` includes the sun's **disc**. The sun already reaches every shaded pixel as the
world's first `DirectionalLight` — by ADR-0040 §4 the *same* light the sky couples its disc to — so
feeding that function into ambient, DDGI and SSR would have lit every frame with the sun **twice**,
with the error growing with the sun's brightness. It would have read as "the new sky lighting is a
bit strong" and been tuned away rather than fixed.

The split is now named at the seam: `sky_full_radiance()` is the sky you **see** (disc included),
`sky_lighting_radiance()` is what the scene is **lit by** (disc excluded). The forward-scatter glow
stays in both, and the distinction is physical — the glow is light the atmosphere scattered *out* of
the beam, which no directional light accounts for. `sky_radiance(vec3)` keeps its exact signature,
because it is the seam §2 names. The disc is independently the one term a 192×108 table cannot
resolve (~0.7° against a ~1.8° texel), which is why Hillaire also keeps it analytic in the final
pass.

### The descriptor-slot budget, spent deliberately

`passes.cpp:308` recorded that the forward pipeline holds 17 of `rhi::kMaxBindings` = 24 slots, and
named 18 as the trigger for "a second descriptor set or a bindless table". m17.7b spends the 18th on
the SH storage buffer and updates that comment rather than doing the refactor: 18 < 24, the refactor
is milestone-scale, and doing it here would bury the brick's actual claim. The `enabled` flag folds
into the SH buffer's own payload rather than costing a 19th.

### Consequences for the ladder

- **m17.7's scope is decided** and is no longer a deferred question.
- The never-cut list gains nothing and loses nothing: m17.7 was already never-cut, and its
  **m17.7e** sub-brick is the one cuttable piece.
- m17.9 (the shadow bar) remains the milestone's first cut, ahead of m17.7e.
