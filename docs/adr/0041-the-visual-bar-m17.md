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
