# AI in Rime — a forward-looking design memo

- Status: **Exploratory — deliberately NOT an ADR.** Nothing here is decided. This memo exists so
  that, while we build M7–M12, we understand which foundations double as AI foundations and which
  cheap seams we should refuse to close. When AI work actually opens, its `.0` brick writes the
  real ADR (append-only, numbered then) and re-plans against the codebase of that day.
- Date: 2026-07-13
- Reads before this one: [VISION.md](../../VISION.md), [ARCHITECTURE.md](../ARCHITECTURE.md),
  [ADR-0026 (own physics core)](../adr/0026-physics-core.md),
  [ADR-0023 (fixed tick)](../adr/0023-app-fixed-tick-loop.md),
  [ADR-0018 (ECS storage)](../adr/0018-ecs-storage-model.md).

The project lead's framing, kept verbatim in spirit: *"It's not a request to include it now but to
think about it now to understand what we need."* So this memo answers three questions — **what**
"AI in a game engine" actually decomposes into, **what base** the engine needs underneath each
piece, and **when** each piece realistically sits on the roadmap — and it answers them the way
Rime answers everything: seams before features, honesty about cost, and the teaching voice.

---

## 1. Taxonomy — "AI" is four different systems wearing one name

The single word hides four systems with different owners, different budgets, and different
relationships to determinism. Conflating them is how engines end up with an "AI module" that is
really a behavior-tree runner with a marketing page. We separate them from the start:

| # | What it is | Runs where | Budget | Determinism relation |
| --- | --- | --- | --- | --- |
| **A** | **Game-AI / the opponent** — NPC decision-making, tactics, navigation, perception | CPU, sim-side | µs–ms per tick, thousands of agents | Must *feed* the deterministic tick as an input |
| **B** | **ML-augmented simulation** — learned/neural physics, controllers, deformation | CPU or GPU | per-tick or per-frame | The hard case — §5 exists for this |
| **C** | **The inference runtime** — the shared substrate A and B (and D) execute models on | CPU + RHI compute | whatever its caller has | Inherits its caller's constraint |
| **D** | **AI-assisted content & tooling** — cook-time ML, editor copilots (Rust side) | Offline, `tools/` | unbounded | None (offline) |

The order matters. **A is a game feature Rime must eventually have** (a Battlefield-class sandbox
ships combat bots — Frostbite's BF2042 fills 128-player servers with AI soldiers over destructible
terrain). **B is research-grade** almost everywhere in the industry. **C is plumbing** that only
earns its keep once A-learned or B exists. **D is where AI already works today**, and it lives on
the Rust side where none of the engine's hard constraints apply.

### 1a. The opponent (A): classical vs learned

**Classical game-AI is not legacy — it is the shipped state of the art.** What AAA actually runs:

- **Behavior trees** (popularized by Halo 2) — a tree of tasks/selectors/sequences ticked each
  decision cycle; the industry default for moment-to-moment behavior (Unreal's Behavior Trees +
  Blackboards are the reference implementation).
- **Utility AI** (The Sims' motives, many shooters' target selection) — score every candidate
  action against weighted considerations, pick the argmax; excels at "which of N similar options."
- **Planners** — GOAP (F.E.A.R.) and HTN (Guerrilla's Killzone/Horizon line): the agent *searches*
  for an action sequence satisfying a goal instead of authors enumerating every case. Powerful,
  harder to debug; a later layer, not the first.
- **Navmesh pathfinding** — Recast/Detour (zlib-licensed) is the de-facto industry bake+query
  stack (Unreal, Unity, O3DE, Godot's baker all derive from it): voxelize collision geometry →
  walkable regions → polygon mesh → A\* over polygons + funnel ("string-pulling") for the path.
- **Perception + blackboard** — sight/sound stimulus generation and per-agent (or per-squad)
  working memory the above read and write.
- **Director-level AI** (Left 4 Dead's AI Director) — pacing/orchestration above individual agents.

**Learned opponents exist but are exceptional:** Forza's Drivatars and GT7's GT Sophy (deep-RL,
shipped 2023+) are the famous ones; both are *policies for a narrow, well-instrumented task*
(driving) trained on enormous fleet data. Unity's ML-Agents and Unreal's Learning Agents exist as
training toolkits, used far more for QA bots and research than shipped opponents. The honest
industry read: **learned policies replace the *leaf* ("aim like this", "drive this corner"), not
the *tree* (mission logic, squad tactics) — and they need a training pipeline, which is an
engineering system of its own (§4.6).**

**Where LLM-driven agents realistically fit (and don't):** not in the tick. Latency (tens of ms to
seconds), memory footprint, and non-reproducibility disqualify per-decision LLM calls for combat
agents. Realistic uses are *out-of-band*: dialogue/barks generation, quest/mission scaffolding,
director-level pacing hints on a seconds-scale budget, and — most credibly — **dev-time authoring**
("compile this English description into a behavior tree / utility curve set" in the M9 editor,
category D). An LLM output that matters for gameplay must be reduced to plain data (a BT asset, a
parameter set) *before* it enters the sim, at which point the sim never knows an LLM existed.

### 1b. AI-driven physics (B): what "learned simulation" actually decomposes into

Ordered from shipped-today to research-frontier:

1. **Learned animation & character control** — the *real* production success. Learned motion
   matching (Ubisoft La Forge compresses motion-matching databases with networks), PFNN/MANN-line
   locomotion controllers, EA's HyperMotion (ML-generated transition animation in EA FC), and
   physics-based RL characters (DReCon/SuperTrack: a policy drives joint motors so a ragdoll
   *performs* the animation and reacts physically). Note the gate: RL ragdolls need **joints and
   motors, which ADR-0026 defers to m12.0** — so this whole family sits behind the m12.0 register,
   in the animation track's territory (an2+), not in `engine/physics` v1.
2. **Learned deformation/collision detail** — subspace neural physics (Ubisoft: real-time cloth as
   a learned low-dimensional model), Unreal's ML Deformer (offline muscle/cloth sims distilled into
   a runtime network). Pattern: an expensive offline sim becomes a cheap learned *approximation of
   its visual output*. Presentation-layer by construction.
3. **Neural super-resolution of simulation** — a coarse authoritative sim upsampled with learned
   detail (fluids/smoke research; the rendering twin — DLSS/FSR4/XeSS — is the proof that neural
   nets can live inside a hard real-time frame budget, on the *presentation* side of the fence).
4. **Learned dynamics replacing the solver** (graph-network simulators, "learning to simulate") —
   research. No production engine ships this for gameplay-coupled rigid bodies, and §5 explains why
   Rime, of all engines, should not be first: our destruction pillar *is* the deterministic solver.
5. **Differentiable physics** — a *training-time* tool (gradients through the sim for system
   identification / controller training), not a runtime feature. Worth remembering that we own our
   solver's source, so a differentiable twin of a subset (for offline tuning of contact/friction
   parameters against captures) is possible without touching the runtime. Research-grade; noted,
   not planned.

The pattern across all five: **ML augments simulation at the edges (animation in, visual detail
out) long before it replaces the core.** Rime's own ordering should copy that.

### 1c. The inference runtime (C) and tooling AI (D)

C is §4.1's subject. D, briefly, because it should stay secondary in this memo: the Rust
`tools/asset-pipeline` is the natural home for cook-time ML (auto-LOD, PBR-map inference,
convex-decomposition tuning for the m8.1 fracture cook, navmesh region hints), and the M9 editor
is the natural home for authoring copilots. Rust has serviceable native inference (`ort`, candle,
burn), so **D needs nothing from the engine at all** — it rides ADR-0001's existing boundary
(tools produce files; the engine loads cooked files). The one policy note worth writing down now:
generated *content* carries provenance/licensing risk that hand-authored fixtures don't (M6.10
deliberately hand-authored its glTF fixtures for exactly this reason); a future D effort needs a
provenance policy before it needs a model.

---

## 2. What the opponent needs from Rime (and mostly already gets)

Walk the classical stack against the modules we actually have, because the conclusion is
pleasant: **M7 was designed for destruction, and destruction's needs are a superset of game-AI's.**

- **Perception** = spatial queries. Sight checks are batched raycasts; hearing/proximity are
  overlap queries. That is *precisely* **m7.7** ("ray/overlap/shape-cast via the BVH, batched
  parallel-safe variants, filters — callable from sim-phase jobs"), which ADR-0026 already shapes
  for Tracks FX/FL. Perception is the *second in-repo customer* for the batched query API — worth
  remembering because ADR-0026 explicitly set "a second consumer with a measured need" as the
  trigger for promoting the AABB tree to a shared facility.
- **Stimuli** = events. Contact/trigger/sleep events with canonical per-tick order and
  double-buffering are **m7.8** — built as the M8 damage input, equally the AI stimulus input
  ("something hit this wall, impulse J, at P"). Keep the event stream consumer-agnostic (m8.4's
  one-event → many-listeners fan-out is already this shape).
- **Working memory (blackboard)** = components. Data-oriented from the start: blackboard entries
  and perception results are reflected ECS components in chunks, not per-agent heap objects. That
  buys, for free: M9 editor inspection (reflection), serialization/replay, and chunk-parallel
  behavior ticks via `Query::par_for_each` under declared `SystemAccess` sets — thousands of
  agents ticked across cores with the `Schedule` guaranteeing no data races *by construction*.
  Structural consequences of decisions (spawn a projectile, despawn a corpse) go through the
  existing `ecs::CommandBuffer` at phase boundaries, same as every other system.
- **Change detection** (**m7.6**, ADR-0018 §4 made real) = cheap perception invalidation. "Only
  re-evaluate what moved" is the same mechanism M10 uses for GI re-stamps; an AI that re-perceives
  only `changed_since(V)` transforms scales the way a settled debris field does — by doing nothing.
- **Navigation** = the one genuinely *new* substrate. Two halves:
  - **Bake (offline, Rust):** navmesh generation consumes the *collision* geometry — the cooked
    static triangle meshes, convex hulls, and compounds of m7.9/m8.1 — not render meshes. The bake
    is a classic Recast-shaped pipeline and belongs in `tools/asset-pipeline` as a new RMA1 asset
    kind (`NavMesh`), exactly per ADR-0024 (Rust cooks, C++ loads, files are the boundary).
    Remember the recorded gotcha: adding an `AssetKind` touches every kind-enumerating site.
  - **Query (runtime, C++):** polygon A\* + the funnel algorithm in a small `engine/nav` module.
    This is a *teachable, bounded* build — a few thousand lines, not a physics engine — so the
    ADR-0026 own-vs-integrate question will recur here at ai0 with a genuinely open answer
    (Recast/Detour is zlib and ship-safe; the funnel algorithm is also a lovely
    `docs/math/` chapter. Decide then, behind a seam either way).
  - **Destruction coupling — the honest hard part.** Rime's floors and cover *change*. A navmesh
    over destructible terrain needs tile-granular re-bake or runtime cutting driven by the m8.4
    destruction events. This is the single hardest classical-AI problem Rime inherits from its own
    pillar (BF-class bots on destructible terrain is proprietary art), and the reason the nav
    module must be **tiled from day one** — bake tiles offline, re-bake/patch tiles at runtime
    from the same code. Leave that seam in the *format* (tiles, not one monolith) even if runtime
    re-bake ships much later.
- **The decision layer itself** (BT/utility executors) = plain code over the above. No new engine
  substrate at all: an `engine/ai` feature module, removable per guardrail #2, depending on
  `core` + `ecs` + `physics`-queries + `nav` interfaces only.

**Learned opponents** add exactly two dependencies to the same list: the inference seam (§4.1) for
running the policy, and the data/training pipeline (§4.6) for producing it. The decision *surface*
is unchanged — a policy is just a different function writing the same intent components — which is
the architectural reason to build classical first: it defines the interface a learned policy later
plugs into (industry-standard practice, and it keeps the learned thing swappable/deletable).

---

## 3. What ML-augmented simulation needs from Rime

Mapping §1b onto our stack:

- **Learned animation/control (family 1)** — needs: the animation runtime (track an1/an2), joint
  motors (m12.0 register), the inference seam (CPU, small nets, per-character), and *training
  data* (mocap + sim rollouts — §4.6). It does **not** need to live inside `engine/physics`; a
  ragdoll policy is a *client* of physics (it writes motor targets pre-step via the ADR-0026
  force/impulse-accumulation seam), same as fluids' buoyancy is.
- **Learned deformation/detail (families 2–3)** — needs: GPU compute in-frame. That is the render
  graph's `add_compute_pass` + storage buffers/images (ADR-0021, shipped) plus the two seams the
  render graph already keeps deliberately open: **async compute** and **persistent (imported)
  GPU resources** across frames. Track FX's GPU particle substrate is the same shape — a neural
  cloth/smoke-detail pass is architecturally *an FX pass whose kernel happens to be a network*.
  Nothing new to build now; the constraint is only that these effects are **presentation-lane**
  (§5's rule) — they read sim state, they never write it back.
- **Learned dynamics in the tick (family 4)** — needs everything §5 says, which is why it is
  research-grade for Rime specifically, not just "later."

---

## 4. The shared substrate — "the base," enumerated

What the lead actually asked for: the concrete list. Six items; most already exist or are already
planned for other customers, which is the memo's central finding — **the AI base is ~80% the
destruction base.**

### 4.1 An inference-runtime seam (`engine/inference`, when it earns existence)

The RHI lesson applies verbatim: **callers target a thin interface; backends are swappable and
hidden.** Nobody above the seam names ONNX Runtime, GGML, or a compute shader. Candidate backends,
with the trade-offs stated honestly:

| Backend | Pros | Cons | Right for |
| --- | --- | --- | --- |
| **ONNX Runtime** (MIT) | Broadest op coverage; the industry default (Unreal's NNE wraps it); import any trained model | *Heavy* dependency (large build, its own threadpool + allocator worldview, runtime kernel dispatch by CPU features); overkill for tiny per-agent nets | Tooling-grade + big models; first CPU backend if we want reach over control |
| **GGML-class** (MIT, llama.cpp lineage) | Small, embeddable, quantization-first (int8/int4), no exceptions, easy to own | LLM-shaped op set; less turnkey for arbitrary policy/vision graphs | Small quantized policies; any (out-of-band) LLM use |
| **Custom tensor ops on RHI compute** | Full control: rides our render graph, our barriers, our timestamps; the only path that can be *in-frame GPU* with graph-owned sync; determinism knobs are ours | We own matmul/conv/activation kernels forever; a real cost, only sane for a small fixed op set | Family-2/3 neural sim passes inside the frame; the far future |

Three Rime-specific design stances, stated now so the eventual ADR starts from them:

1. **The engine owns threading.** Backends run inference synchronously on the *calling* thread
   (internal pools pinned to 1); batching across agents is `core::JobSystem::parallel_for`, like
   everything else. This keeps guardrail #4 (no hidden threads), keeps TSan meaningful, and is
   load-bearing for determinism (§5): a fixed partition of work = a fixed reduction order.
2. **Sessions are pure functions over spans.** Weights load once (cooked bytes); `run()` takes
   caller-owned input/output spans (allocator discipline; no hidden allocation on the hot path).
3. **Models are cooked assets.** Per ADR-0024 the engine parses no source formats: the Rust
   pipeline cooks ONNX/safetensors → an RMA1 `ModelWeights` kind (possibly wrapping a
   backend-native blob as an opaque payload), schema-hashed like every other asset, loaded through
   the same `AssetServer`/registry machinery. Training-side tools keep their native formats.

One cheap RHI note with a deadline attached: GPUs are growing first-class matrix acceleration
*inside graphics APIs* (Vulkan's cooperative-matrix extension family; D3D12's cooperative
vectors). When the RHI next grows a device-capability surface, **include a
cooperative-matrix/fp16/int8-arithmetic capability query** — one enum's worth of foresight that
keeps the custom-backend door open on future hardware.

### 4.2 The ECS surface: an `engine/ai` module's data model

Already covered in §2; the substrate summary: reflected components for agent state/blackboard/
stimuli (M4 + reflection, shipped), chunk-parallel ticks (`Query::par_for_each`, shipped),
declared-access scheduling (`Schedule`, shipped), deferred structural changes (`CommandBuffer`,
shipped), change detection (m7.6, landing this milestone). **AI LOD** — not every agent thinks
every tick — is a components-and-buckets pattern over the fixed tick, no new machinery. The module
itself is "build later"; the substrate is done or already scheduled.

### 4.3 Spatial queries & the navmesh

The direct dependency to protect: **m7.7's batched, filtered, parallel-safe queries are the
perception engine.** The nav bake consumes cooked *collision* geometry (m7.9 shapes; m8.1 fracture
hulls define what "intact vs breached" means spatially); the runtime nav module is new-but-small;
tiling is the destruction seam (§2). Nothing about m7.7's current plan needs to change — AI just
became its second customer, which strengthens the already-planned shape.

### 4.4 The job system

Behavior ticks, perception batches, and CPU inference batches are all `parallel_for` workloads
over chunked data — exactly what M1.6's work-stealing system + M4.4's scheduler were built for.
The one *addition* worth considering someday (not now): a **budgeted/amortized job class** ("spend
≤0.5 ms of leftover frame time on planning, resume next tick") for planners and background
inference. Note it for the backlog; do not design it before a consumer exists.

### 4.5 The GPU-compute seam in the render graph

Shipped: compute passes, storage resources, graph-owned barriers, per-pass timestamps (ADR-0019/
0021). Deliberately deferred and *shared with Track FX*: async compute, persistent GPU-resident
sim buffers. Neural-sim passes are FX-shaped passes; when Track FX opens those seams, B-family
work inherits them free. **No AI-specific GPU work is warranted now.**

### 4.6 Telemetry, record/replay, and the training-data pipeline — the sleeper seam

Every learned approach — policy, controller, deformer — starves without data, and this is where
Rime is accidentally *excellent*, because ADR-0026's determinism makes the gold-standard recording
format nearly free: **initial state + the per-tick input stream ⇒ the entire episode**, bit-exact,
replayable at (headless, lavapipe, `parallel_for`-scaled) faster-than-realtime. That is a training
gym: the fixed tick (ADR-0023) is the environment step; the C ABI (`engine/capi`, ADR-0001's
boundary) is where a Python/Rust trainer would drive rollouts without touching internals; Track S
already ships frames out of the process if pixels are ever the observation. The cheap actions
(§7) include the one paragraph of design that makes this real when M11 defines the input stream.
Richer telemetry (per-event outcome logs for imitation/analytics) can ride the same tap later.

---

## 5. Determinism & networking — the constraint that shapes everything

This is the section to internalize even if the rest of the memo ages badly.

**The pillar (restated):** ADR-0026 pins *same-binary* determinism — the world-state hash
(`core::fnv1a_64` over body state in body-index order) is bit-identical across runs and across
{1,2,8,16} thread counts. Preconditions: no unordered iteration in the sim path, fixed iteration
counts, no fast-math, islands strictly sequential inside / parallel across. M11 replicates
destruction *events* against a "damage → detach is a pure function" contract. Cross-platform
lockstep is an explicit non-goal. This determinism is not a nicety — it is how networked
destruction, replays, and CI-provable physics correctness exist at all.

**The threat:** ML inference is non-deterministic by default in exactly the ways our preconditions
forbid. Float results vary with reduction order (thread-count-dependent partial sums inside a
BLAS), with kernel selection (runtimes dispatch by detected CPU features, so *the same binary*
computes differently on AVX2 vs AVX-512 machines — a hazard class our own code doesn't have,
because we don't runtime-dispatch), with GPU scheduling (atomics, warp ordering), and across
drivers/hardware generations. GPU inference inside the deterministic tick is effectively
disqualified; naive library-CPU inference is disqualified-by-default until pinned.

**The architecture rule this memo proposes (the one sentence to keep):**

> **AI is an input source to the deterministic sim, never a stage inside it.**

Concretely — the two-lane model:

- **Lane A — the authoritative tick** (physics step, destruction, gameplay state transitions).
  Everything in it meets the ADR-0026 preconditions. It consumes *intents* (move, aim, fire,
  ability) sampled at tick start.
- **Lane B — advisory computation** (perception aggregation, planning, learned policies, anything
  LLM-shaped). Runs beside or across ticks, on the job system or truly async, with **no
  determinism requirement at all**. Its outputs become intent records, quantized into the same
  input stream player input already rides (ADR-0023 separated sim ticks from frames precisely so
  inputs are sampled data, not callbacks).

The elegance — and the reason this costs nothing — is that **an NPC becomes "just another player"
to the sim and to M11's network model.** The replicated per-tick input log doesn't care whether an
intent came from a human, a behavior tree, or a policy network. Replays reproduce bit-exactly
*including* every AI decision, because decisions were data in the log — even though the decider
was non-deterministic. Determinism of the *world* is preserved without demanding determinism of
the *mind*. (Classical BT/utility code, being ordinary arithmetic over ordered data, can trivially
run *inside* Lane A too — fine for simple reactive agents — but designing the boundary as
intents-in-the-log from day one is what keeps learned deciders and async planners plug-compatible
later. It also mirrors how BF-class titles integrate bots: server-side deciders producing
player-shaped input.)

**If something learned ever must live inside Lane A** (a learned contact model, a neural motor
controller stepped with the solver), the escape hatches, in order of preference:

1. **Don't** — reformulate as pre-step inputs (motor targets computed in Lane B from last tick's
   state; one tick of decision latency is invisible at 60 Hz and is exactly how human input works).
2. **Pin it** — integer-quantized weights, our own (or a frozen, audited) kernel path, single
   partition order, no runtime ISA dispatch on sim-path kernels, covered by the same
   cross-thread-count hash tests as the solver. Achievable for small nets *because* our
   determinism scope is same-binary, not cross-platform — the bar Rime actually has is one pinned
   inference can meet; the bar it doesn't have (cross-hardware float lockstep) is the impossible
   one.
3. **Fence it by authority** — server-only inside the sim, results replicated as state, never
   re-simulated on clients.

**Server-authoritative vs client-predicted AI:** with the client-server direction m11.0 is
expected to take, Lane-B deciders run **on the server**; clients receive replicated state and
never predict *other* minds (they don't predict other humans either — m11.3's
interpolation/prediction split applies unchanged). Purely cosmetic client-side behavior
(head-look, flinch, foliage-brush reactions) may run locally and non-deterministically, in the
same bucket as VFX. Determinism still pays on the server even with no lockstep anywhere:
reproducible bug reports, kill-cam/replay, regression-testable encounters, and §4.6's training
data all fall out of the same input-log design.

**And the GPU-neural rule, restated as networking:** anything that cannot meet Lane A's bar is
presentation — **gameplay-coupled ⇒ CPU + pinned + deterministic; presentation-coupled ⇒ GPU +
free.** Neural cloth detail, smoke super-res, learned secondary motion: clients may compute them
differently and nobody can tell, because nothing reads them back into state. This is the same
one-way data flow Track FX already commits to; AI adds no new principle, it inherits one.

---

## 6. Seams to leave NOW vs things to build later

**Must-not-preclude (the cheap insurance — mostly "keep already-made promises"):**

1. **Intents are sampled data.** Keep gameplay input flowing as per-tick sampled records (the
   ADR-0023 shape); never let a "think" callback run synchronously mid-tick from inside sim code.
   When m11.0 writes the network ADR, spend one paragraph defining the input stream so a non-human
   intent source is representable (an entity-addressed intent record, not "the keyboard"). This is
   the highest-leverage line in this memo and costs a paragraph.
2. **m7.7 stays batched, filtered, parallel-safe** (as planned) — perception is its second
   customer. If the AABB-tree-promotion question re-opens, ADR-0026's "second consumer with a
   measured need" trigger now has a named candidate; still measure first.
3. **m7.8 events stay consumer-agnostic** with canonical order + double-buffering (as planned);
   m8.4's fan-out registers listeners, none privileged.
4. **m7.6 change detection lands as public ECS surface** (as planned) — perception invalidation
   rides the same stamps as M10 GI dirt-tracking.
5. **Keep AI-shaped state in reflected components**, never module-private heaps — inspection,
   serialization, and replay come from reflection (this is just ADR-0018 discipline restated).
6. **Nav data is tiled in its file format** from the first bake, because destruction will
   invalidate tiles (runtime re-bake can come years later; a monolithic format would preclude it).
7. **RHI capability foresight:** when a device-caps query next grows, include cooperative-matrix /
   fp16 / int8-arithmetic bits (§4.1). One enum, zero code.
8. **Keep the no-fast-math + fixed-iteration CI guards** (they exist for physics; they are also
   exactly what makes pinned inference certifiable later).
9. **Module discipline as usual:** future `engine/ai` / `engine/nav` / `engine/inference` are
   removable feature modules depending on interfaces only; gameplay must never hard-link them
   (guardrail #2 — the engine builds with AI deleted).

**Build later (explicitly NOT now, listed so nobody starts early):** the `engine/ai` module and
its BT/utility executors; the navmesh bake + `engine/nav`; the inference seam and any backend;
custom tensor kernels; the record/replay capture tool and gym harness; budgeted planner jobs; any
model, of any kind. None of these unblock current milestones, and every one gets cheaper the later
it starts (the substrate beneath them is still moving).

---

## 7. When — roadmap placement

**Gate analysis.** Classical game-AI is unblocked once **m7.6 + m7.7 + m7.8** land (all M7,
in-flight) — assets (M6), the fixed tick (M5.7), and the ECS (M4) are already done. Nothing about
game-AI gates on M8–M10. But *mainline-first* ordering (destruction M8 → editor M9 → lighting M10
→ networking M11 → The Block M12) outranks it, and M12's demo is a destruction/lighting/networking
thesis, **not** an AI thesis — AI is not on the vision demo's critical path and must not pretend
to be.

**The forcing function that places it anyway: M11 needs bodies.** Soak-testing networked
destruction at 64+ players (m11.4, and M12's "feels right") needs synthetic players — headless
clients that roam, shoot walls, and generate honest load. A navmesh-walking, wall-shooting bot
*is* classical game-AI's first four bricks, and it doubles as the §4.6 data generator. So:

**Proposal: Track AI** — a cross-cutting track like FX/FL (interleaved under mainline-first rules,
never two track bricks while the milestone has unstarted work), opened in the **M9–M11 window**,
shaped so ai4 exists before m11.4's soak. Sketch (map-level ★; ai0 re-cuts it):

- **ai0 — the decision ADR** (this memo hardened): scope classical v1; own-vs-Recast for bake and
  runtime; the intent-record definition (joint with m11.0 if concurrent); defer the inference seam
  with named triggers. *Proof: the ADR.*
- **ai1 — navigation:** tiled navmesh cook in `tools/asset-pipeline` (new RMA1 kind — remember the
  kind-enumeration gotcha) from cooked collision geometry + `engine/nav` polygon A\* + funnel
  (+`docs/math/` derivation). *Proof: agents path across a cooked scene, headless, deterministic.*
- **ai2 — the behavior substrate:** `engine/ai` — blackboard/stimulus components, a BT executor +
  utility scoring as `Schedule` systems, LOD ticking. *Proof: thousands of agents ticking
  chunk-parallel; world hash identical across thread counts (the agents live in Lane A here —
  classical code can).*
- **ai3 — perception:** batched LOS/overlap via m7.7 + event-driven stimuli via m7.8/m8.4.
  *Proof: perception determinism + TSan (it's a threading surface).*
- **ai4 — the bot:** a combat bot emitting intent records into the M11 client input path — the
  load generator and the first opponent. *Proof: N bots soak a destructible scene over the net.*
- **ai5+ (unscheduled):** the inference seam + a CPU backend, gated on a real consumer (an2
  learned locomotion, a policy leaf for ai4, or a director) — not built speculatively.

**ML-augmented simulation: a research band, not a milestone.** Prerequisites, each with a named
home: GPU sim substrate (Track FX) · inference seam (ai5) · record/replay + gym data (the m11
input log + a capture tool) · joints/motors for anything ragdoll-RL (m12.0 register) · a real-GPU
validation lane (lavapipe proves structure, never performance — neural passes must be
performance-qualified on real hardware). Earliest honest slot: **post-M12**, opportunistic, explicitly allowed to fail —
with family 1 (animation-side) first because it is the industry-proven member, and family 4
(learned dynamics in Lane A) last or never, per §5.

**Cheap-now vs expensive-later, compressed:** everything in §6's first list is cheap now and
painful to retrofit (an input stream with no room for non-human intents, a monolithic nav format,
an RHI caps enum without tensor bits, event streams welded to one consumer). Everything in §6's
second list is *cheaper* later. That asymmetry is the memo's whole argument.

---

## 8. Cheap actions over the next few milestones (the complete list)

Explicitly not building AI — preserving its option:

1. **m11.0 (when it opens):** define per-tick intent records source-agnostically; note replay =
   initial state + intent log as a stated capability of the design (one paragraph each).
2. **m7.7 / m7.8 / m7.6 (in flight):** land as already planned — this memo adds a second customer,
   not a change request.
3. **m8.4:** keep the destruction fan-out consumer-agnostic (already its design).
4. **Next RHI caps surface:** add cooperative-matrix / fp16 / int8 capability bits.
5. **First nav bake (ai1, later):** tiled format from day one — recorded here so ai0 inherits it.
6. **Track AI's ai0 ADR when the M9–M11 window opens:** harden §5's two-lane rule and §6's list
   into decisions; number it then (ADRs are append-only; this memo deliberately isn't one).
7. **docs/glossary.md**, when Track AI opens: behavior tree, utility AI, GOAP/HTN, navmesh,
   blackboard, inference, quantization — the newcomer terms this memo assumes.

## 9. Honest uncertainties

- **The field moves faster than this repo.** Model architectures, runtimes, and even GPU API
  tensor support will look different by the time Track AI opens; that is *why* this memo commits
  to seams and lanes, not to backends and models. Re-survey at ai0; treat §1's industry citations
  as a snapshot (mid-2026), not scripture.
- **Own-vs-integrate for navigation** is genuinely open (unlike physics, where VISION #1/#3
  decided it): Recast is ship-safe, excellent, and *studying* it is also teaching. ai0 decides.
- **Destruction-aware navigation at BF scale** is the hardest thing named here; tiling is
  necessary, not sufficient. Expect ai-track re-planning contact with reality.
- **Whether The Block wants bots** (M12 scope) is a product call for its milestone, not this memo.
  The track is shaped so the option exists; m11.4's soak need stands either way.
- **Learned simulation may simply never clear Lane A's bar.** That would be a fine outcome — the
  presentation lane and the animation family are where the industry evidence is anyway.

*The frost does not think yet. But the surface it will form on is being poured now — crystal by
crystal.*
