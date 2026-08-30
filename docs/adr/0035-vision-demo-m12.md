# ADR-0035: The vision demo (M12) — falsifiable "feels right", a work ledger for performance, and the player who was never built

- Status: Accepted
- Date: 2026-08-20

## Context

M12 is the last milestone on the map and the only one whose "done when" is written in the language of
feeling: *"a destructible urban block (M8+M10+M11) runs at a playable frame rate and **feels right**."*
[VISION.md](../../VISION.md) §5 says the same thing at more length — buildings that fragment under
fire, debris that falls and hurts you, dust and sound from the same destruction event, GI that
updates as walls come down, all at a playable frame rate.

Every previous milestone entered with a falsifiable proof. This one enters with an adjective. Three
facts make closing that gap harder than writing a bigger sample:

**1. It is the first milestone that can, and must, make an absolute performance claim.** Every proof
this project has shipped is *structural on lavapipe* — properties the physics guarantees, checked
with margins, never golden images — because absolute time on a CPU rasterizer is meaningless. M10
deferred every frame budget to "m12.0, when we have real hardware." That hardware now exists (an RTX
3060 on the primary workstation), and CI stays lavapipe forever. So M12 must answer a question the
project has never answered: **what is the durable, regression-catching form of a performance claim
when the machine that can make it is not in CI?** "60 fps on my 3060" rots the day the scene changes,
the driver updates, or nobody re-runs it.

**2. There is no player.** M11 replicated input as *intent* and delivered the prediction seam as
**state rather than an interface** ([ADR-0033](0033-networking-v1.md) A20) precisely because no
character controller existed to shape it, and a guessed `virtual void replay(...)` in a header is
inherited as a constraint. Nothing consumes the replicated commands. A ground-truth pass for this ADR
also found that [ADR-0026](0026-physics-core.md)'s deferred register still promises *"a capsule mover
still ships at m11.3"* — false against the tree, because the M11 decomposition reassigned m11.3 to the
replication core and the mover was never re-homed (see that ADR's 2026-08-20 amendment). **The
controller is new work, and client-side prediction with reconciliation is the hardest problem in this
area.**

**3. The record disagrees with the tree in places M12 would have built on.** The same pass found: no
**view-frustum culling exists anywhere** (`ExtractedScene::draws` is literally "every
`{WorldTransform, MeshRef, MaterialRef}`" — fine at six walls, not at a city block of per-part render
leaves); `engine/vfx` is one CPU dust stub with **no draw pass**, so the roadmap's "Track FX
hard-gates M12" gates on a bounded subset rather than the whole GPU-particle dream; the windowed
present path's excuse (*"this box is headless"*) has expired; and the cross-cutting note promising
"real audio ~M8–M9" never happened. This is the A9/A10 failure mode again — a record outliving its
code — and it is why §0 of this ADR is a ground-truth list rather than a decision.

This ADR fixes the architecture and the ladder. It deliberately does **not** fix the budget *numbers*:
those come from measurement at m12.0, the same move [ADR-0032](0032-lighting-v2.md) §2 made with the
GI spike — *the spike moves the numbers, not the architecture*.

---

## Decision

### §1. The thesis, made falsifiable

`samples/99-the-block` is a dedicated headless server and two clients on one city block. Every clause
of "feels right" becomes a number, and — following m11.7's discipline — **every way the proof could
be vacuously green is itself an assertion.**

**Scale floors, asserted by the proof, because too-small is the quiet failure.** ≥ 8 destructible
structures composed from cooked slabs, ≥ 1,500 destructible parts, ≥ 400 peak live debris bodies, a
sun plus ≥ 32 active local lights. Below those floors the byte budget never binds, relevancy never
differs between the two clients, the debris lifecycle never triggers, and half of M11 is dead code in
the run — the exact failure m11.7 caught in its own first draft, where 228 dead parts were 0 and the
whole m11.5 half measured an empty world.

**The clauses:**

| clause | the assertion that lets it fail |
|---|---|
| the fusion actually runs | destruction, GI and replication all demonstrably active in the same frame — each with a non-zero work counter |
| own-input response | ≤ 1 tick, against a **prediction-off control** showing ≥ RTT ticks, so prediction is provably *the reason* |
| the shot feels connected | fire → visible feedback in the same frame; the damage rides M11's committed-op path |
| remote motion is continuous | max rendered per-frame step ≤ a bound that v1 measurably violates; never rewinds |
| the peers still agree | bit-exact `shared_state_hash` convergence at quiescence, at ~10× m11.7's scale |
| it is playable | p99 within budget through the collapse window (§2) |

**And a human gate, named as procedural rather than dressed up as a proof:** a recorded play session
on the workstation. "Feels right" has an irreducible subjective core; the honest move is to state
that the recording is evidence for a human judgement, not a passing test.

### §2. Performance governance: split *how much work* from *how fast it runs*

**This is the ADR's load-bearing decision.** The performance equivalent of "structural properties,
never golden images" is **asserting the frame's work, not its wall-clock.**

**§2a. The work ledger — the CI-gated half, on lavapipe, forever.** Work counts are
machine-independent, exact, and deterministic (M7's rule: *counts not clocks, so the tick stays
reproducible*), so lavapipe can check them bit-for-bit. Most of the instrument already exists:
`RenderGraph::resolve_timings`, `LocalShadowStats::rendered/reused`, the `SdfClipmap` stamp counters,
the DDGI round-robin cap, physics `WorldStats`, the m11.3–m11.6 replication counter suite,
`RIME_PROFILE_ZONE`, `DustField::count/capacity`. **Missing and added at m12.7: draws submitted vs.
draws after frustum cull** — because no cull exists yet.

m12.0-perf aggregates these into one per-run ledger, and the block's headless CI proof asserts ledger
budgets with margins: draws-after-cull ≤ N, awake bodies return to 0 at quiescence, SDF re-stamp work
proportional to the break region rather than the world, shadow-cache reuse above a floor on a static
frame, replication bytes/tick inside the m11.5 budget, particles ≤ cap. **A change that doubles draw
calls, wakes a sleeping pile, or silently turns the shadow cache off fails CI deterministically, with
no hardware in the loop** — which is the class of change that later surfaces as "the demo got slow."

M11's counting rule applies in its harder half — *count the skips that stop happening*. The cull
counter exists so that culling degrading to "culled 0 of 4,000" is a red number rather than a warm
frame.

**§2b. The hardware gate — self-gating, distribution-based, committed.** `99-the-block --perf` runs a
deterministic scripted timeline (fixed seeds, scripted camera, the scripted match) and emits one JSON
report:

- **frame-time distribution — p50/p95/p99/max, never the mean.** Destruction is bursty and the
  fracture tick is precisely the moment that must stay smooth; a hitch storm vanishes into a mean.
  Plus the same distribution restricted to the collapse window, and the worst single frame with its
  per-pass breakdown.
- **sim-tick distribution separately** — the tick must fit its own budget or 60 Hz simulation dies
  regardless of the GPU.
- per-pass GPU ms and per-stage CPU ms; the machine fingerprint (GPU, driver, resolution, preset,
  commit, date); **and the same run's ledger totals, so a fast run on a scene that did no work is
  self-evidently invalid.** The vacuity guard applies to performance runs too.

The run **self-gates** — non-zero exit if p99 or max exceeds budget — and compares against the last
committed report *with the same fingerprint*, failing on relative regression.

**§2c. The numbers live in the repo, append-only.** `docs/perf/` gains one small JSON per recorded
run plus a README defining the schema and the reference machine. This is the ADR culture applied to
measurements: dated, fingerprinted, diffable, citable. *"Measure before optimizing; note the
measurement in the PR"* finally gets a ledger instead of a habit, and CLAUDE.md's brick-delivery list
gains one line — **perf-touching bricks commit a `docs/perf/` run** — so absence is visible in review.

**Proposed headline budget, to be ratified at m12.0 against the baseline, not before:** p99 ≤ 16.6 ms,
max ≤ 33 ms, 1920×1080, Release, all lighting gates on, through the collapse window; sim tick
p99 ≤ 6 ms, collapse-tick max ≤ 12 ms. **If the baseline shows the block sailing under trivially,
tighten it at m12.0** — a gate nothing can fail is not a gate.

**Explicitly not done, with reasons.** *No hardware CI runner*: a self-hosted runner on the one desk
machine makes every merge hostage to one box's uptime and launders thermal and driver noise into
red/green. Revisit when a second, dedicated box exists. *No absolute gates on lavapipe*: ADR-0032 §11
stands. *The hardware gate is procedural, and that is a real weakness* — humans skip checklists. It is
mitigated by the committed-report convention and by the CI ledger, which catches the work regressions
that cause most time regressions; it is not mitigated away.

### §3. The player: a pure move function, not a solver client

**`step_character` is a pure function over physics *queries*** — capsule collide-and-slide using
`shape_cast`, with slope, step-up and ground-snap rules — not a body the solver integrates. Purity is
what makes it replayable, and replay is what prediction is built out of. The player occupies a
**kinematic** capsule so debris can hurt it through ordinary contact events; knockback is out of v1.

**Module placement, per guardrail 2 and the `destruction_net` precedent:**

- **`engine/gameplay`** (new) — `CharacterController`, reflected `CharacterState`/`CharacterConfig`,
  and `Weapon` v1 (deterministic hitscan via `raycast`, with `RayHit::child` naming the part; emits
  `WeaponFired`/`HitResult` **events**). It depends on `core + ecs + physics` **only**, and
  deliberately **does not link `destruction`**: the hit → `apply_damage` glue lives in the consumer,
  exactly as the M8.4 fan-out kept `vfx` from ever linking `destruction`. A single-player game gets
  this module with `net` deleted.
- **`engine/gameplay_net`** (new) — the `destruction_net` argument verbatim: it exists so `gameplay`
  and `replication` need not know each other. It holds `PlayerRegistry` (session ↔ player entity), the
  server consume loop, and the client `Predictor`. Its message tags claim a **new block of the shared
  tag registry** in `snapshot.hpp` — M11 learned that lesson the expensive way, when two modules would
  both have started their enums at 1 and one would have silently never received its mail.

The controller consumes `replication::InputCommand` **by value shape, not by link**: `gameplay`
defines its own `CharacterInput` and `gameplay_net` converts, keeping the wire contract out of a
module single-player builds use.

### §4. Prediction and reconciliation, inside A19/A20's fence

**Server, per tick:** drain each session (advancing `consumed_through`, the only honest ack); run
`step_character` per command in order; resolve fire; write the transform and `CharacterState`; and
**stamp a replicated `LastProcessedInput{sequence}` component on the player entity.**

That last point is the design's pivot. The pairing between authoritative state and input position
crosses **as data on the entity, riding the ordinary snapshot path** — reusing A15's `DebrisOrigin`
move. No new message kind, no new framing, and no new completeness rule to get wrong.

**Client, per tick:** sample → `record()` (stamping the sequence, joining `unacked()`) → run *the same*
`step_character` on the predicted player immediately → store `{sequence, state}` in a bounded ring.

**On player-state arrival with `LastProcessedInput = q`:** compare **resulting state at q, never a
diff of command lists.** This is the constraint m11.6c wrote down and it is not a stylistic
preference: `consumed_through` steps over permanent gaps, so a command leaving `unacked()` means
*"the server will never act on it"*, **not** *"the server acted on it"*. A predictor that diffs command
lists is subtly wrong exactly under loss — the condition it exists for. Within tolerance: done, and
**count the skip**. Otherwise: rewind to the server state, replay every `unacked()` command with
sequence > q, restamp the ring, and count the correction and its magnitude.

**The properties, each with a proof:** a lost command leaves `unacked()` via the ack jump, so replay
excludes it and both sides agree the input never happened; the server moves the player only via
commands or explicit teleports, so predicted-at-q vs. state-at-q is like-for-like; and **at quiescence,
predicted state equals authoritative state bit-for-bit** — same binary, same function, same inputs, so
exact equality is the assertion and epsilons are confined to the mid-flight tolerance gate.

**The GPU-free proof** runs on M11's scripted-loss harness with negative controls that make the
mechanisms provably the reason: prediction-off shows ≥ RTT ticks of latency, reconciliation-off
diverges, and a lossy run with **zero** corrections fails — because that would prove the comparison is
dead rather than that the network was kind.

**Not built, and named:** lag compensation and the clock offset it needs. The block's targets are
buildings, which do not dodge.

### §5. Scope rulings

- **Track FL (water): out of M12.** The roadmap parked this as *"decided at M12.0"*; **this is that
  decision.** No water in the block. Heightfield water plus two-way buoyancy is a whole track, and it
  does not earn a slot in a demo whose thesis is destruction, lighting and networking at scale. The
  ADR-0026 substrate seams stay intact.
- **Track FX: in, at its true size.** **fx1a** is load-bearing — the GPU draw pass for the existing
  deterministic CPU sim, with three families (impact dust, lingering smoke, muzzle flash) driven from
  destruction and weapon events through consumer glue, and **off must be a byte-identical baseline**,
  the `LightingSettings` gate discipline applied to FX. **fx1b** (compute-sim scale-up) is contingent
  on the ledger showing the CPU sim bind. Fire-as-light stays deferred behind its seam.
- **Audio: in as cuttable polish.** VISION's sentence says "dust *and sound*", and a silent demo
  undercuts "feels right" — but the CI proof is deaf either way, so audio is **first on the cut list**
  and the "sound" clause then narrows honestly to the M8.4 event witness.
- **Cut order if the milestone runs long:** audio → fx1b → m12.p items beyond the narrowphase cache →
  m12.5. **Never cut:** m12.4 (the feel thesis dies), fx1a (the vision sentence dies), the perf gate
  (the milestone's defining question dies).

### §6. Every deferred item is ruled

An unruled item silently becomes scope, so this ADR rules all **38** found across the ADRs, module
READMEs, the roadmap's named-gap trail and the session notes. Summarised:

- **In M12:** prediction (m12.4) · snapshot interpolation over the real interval (m12.5) · fx1a
  (m12.6) · audio as cuttable (m12.9) · the cook-cache schema-key fix (m12.7, the first cook-touching
  brick) · the `server.despawn` discipline backstop (m12.3, the first new replication consumer) ·
  physics-interpolation-into-alpha, folded into m12.4/m12.5 · **ADR-0032 C6, debris *visual*
  retirement** — see below.

**C6, and what its discovery says about this section.** [ADR-0032](0032-lighting-v2.md) declared C6
*"NEW — M10's to build"* and promised destruction "one new event kind and a visual-retirement stage".
Neither shipped: `DestructionEventKind` still has exactly m8.4's four kinds. m8.5 bounds debris in the
*physics* world, which is why no M10 proof ever noticed — those run for a few hundred ticks — but the
*visual* population (render leaves, and with them the SDF stamp and shadow-caster sets) grows without
limit. M12 is where that first bites: the block runs long, at ≥ 400 peak debris, in a mode a human
plays. **Ruling: in M12, in two halves — m12.0-perf adds the ledger counter that *detects* unbounded
caster/draw growth, and m12.7 builds the retirement stage that fixes it.** Detect before fix is the
right order here, and it makes C6 the work ledger's first real customer: a population that climbs
monotonically across a long run is exactly what §2a exists to catch.

The honest note is *how* C6 was found. This section's first draft claimed all 37 items were ruled and
that nothing remained — and C6 was missing from it, surfaced only by a **second, independent review
pass** run against the same tree. That is this ADR's own thesis turned on itself: a completeness claim
is only as good as the search behind it, and "I found everything" is precisely the assertion that
cannot fail on its own terms. The count is 38 because someone looked again; it should be read as a
floor, not a total.
- **In M12 only if measured (m12.p):** the every-tick narrowphase cache (M7's named first hot spot,
  likely at 400+ debris) · TGS solver mode *iff* the stack-quality review shows building-scale wobble
  · hi-Z SSR *iff* SSR binds on the 3060 · transform quantization *iff* the ledger shows budget
  starvation · DDGI tuning · parallel command recording / async compute *iff* CPU submission binds ·
  the relevancy spatial index *iff* client count grows · fx1b.
- **After M12:** m10.i virtualized geometry · GI colour bleed · pre-filtered specular probe · cube
  shadows · virtual shadow maps · late-join baselines and composition repair · lag compensation and
  clock offset · the dev-server scale run · debris velocity · the A18 fragment brick ·
  entity-reference replication · static trimesh · triggers/sensors · an1 GPU skinning · the editor
  destruction cameo · asset hot reload · stream S2 · `t_queue_index` · the Mac portability lighting
  fault · chunk-grain over-inclusion.
- **Closed:** the AI track stays closed for M12 — no bots in the demo, and when it opens its own `.0`
  brick writes the real ADR.

---

## The brick ladder (m12.0–m12.10)

Instruments first, because everything else steers by them; then the controller line m12.1→m12.4,
which is a strict dependency chain and the schedule's long pole; FX and content beside it; the
measured perf pass late, because it needs the block to measure; the proof last.

- **m12.0** — this ADR + the ladder + the **hardware true-up**: the first baseline session on the
  3060 (re-run the GI spike sweep, physics stress, `11-lit-rooms`, `12-networked-destruction
  --transport=udp`), the first `docs/perf/` entries, and the budget numbers ratified against them.
  *Load-bearing. No engine code.*
- **m12.0-perf** — the perf harness + **work ledger v1**, wired into `11-lit-rooms` and
  `10-destructible-wall` so the harness is proven before the block exists. *Proof: a deliberately
  doubled draw count fails the ledger — the gate can fail.* *Load-bearing.*
- **m12.1** — physics top-up: **`shape_cast`** (sphere/capsule sweeps through the BVH, exposing the
  GJK-distance machinery CCD already uses) + **kinematic push-in** in `PhysicsSync`. *Load-bearing.*
- **m12.2** — `engine/gameplay`: the character controller. Proofs include analytic slope/step/slide
  bounds and **replay determinism** (same tape twice ⇒ bit-identical trajectory) — proven *here*, so
  m12.4 debugs reconciliation and never the mover. *Load-bearing.*
- **m12.3** — the networked player, **server-authoritative, no prediction yet**: `gameplay_net` v1,
  the consume loop, `LastProcessedInput`, weapon→destruction glue. Proof records **own-input latency
  = RTT ticks — the number m12.4 must beat.** *Load-bearing.*
- **m12.4** — **prediction + reconciliation.** The hardest brick in the milestone, flagged now the
  way m8.3 was. *Load-bearing.*
- **m12.5** — snapshot interpolation v2 + presentation timing: interpolate over the interval a value
  actually covers (the delta header already carries the server tick), a small jitter buffer, predicted
  -player smoothing. *Load-bearing-lite — cut last, and say so in the README.*
- **m12.6** — Track FX brick fx1 (**fx1a** load-bearing, **fx1b** contingent). The M8.6-deferred
  coverage-delta pixel proof lands at last.
- **m12.7** — block content + render scale: the building prefab pattern, a procedural assembly script
  emitting `.rscene`, street props, **view-frustum culling with its submitted/culled counters**, and
  **ADR-0032 C6's debris visual-retirement stage** (§6) — the fix for the growth m12.0-perf's counter
  will have made visible. *Load-bearing.*
- **m12.8** — the playable client: **windowed present** (filling ADR-0023's seam; the RHI side has
  existed since M3.4) + first-person camera on the predicted player. *Load-bearing — a vision demo
  nobody can play is not one.*
- **m12.9** — audio v1: an engine mixer behind the existing `AudioBackend` seam, proven GPU-free by a
  deterministic offline mixdown, with a **Linux sink first** (the demo machine; power beats
  portability, and CI is deaf either way). *Polish — cuttable.*
- **m12.p** — the measured perf pass, **its scope chosen by the ledger rather than guessed**. The slot
  is load-bearing; each candidate is contingent.
- **m12.10** — the proof: `samples/99-the-block` (scripted CI mode, `--play`, `--perf`) + the docs
  true-up. *Load-bearing.*

---

## Consequences

- **Two new modules**, `gameplay` and `gameplay_net`, both placed by the guardrail-2 argument that
  created `replication` and `destruction_net`. The engine must still build with either removed.
- **A new artefact class in the repo**: `docs/perf/` JSON reports. They are append-only and
  fingerprinted, and they are the only place an absolute performance claim is allowed to live.
- **CI gains ledger assertions but no timing assertions**, and stays lavapipe-only. The hardware claim
  is deliberately outside CI, and its proceduralness is a named risk, not a solved problem.
- **The tick stays the ordering key it became at A11.** Prediction adds no clock synchronisation:
  the motion clock is the command sequence, so M11's "no clock offset" gap does not block M12.
- **A single machine is the sole fingerprint for perf claims.** Any second GPU adds a second
  fingerprint cheaply; until then, "it is fast" means "it is fast on that box", stated that way.
- **`99-the-block` will be the first sample a human is expected to *play*.** Every prior proof was
  self-checking and headless; this one has a mode that is neither, and the ADR says which claims come
  from which mode.

## Alternatives considered

- **A self-hosted hardware CI runner.** Rejected for M12: it makes merges hostage to one desk
  machine's uptime and converts thermal and driver noise into red/green. The ledger gets most of the
  regression coverage without it.
- **Gate on mean frame time, or on a single "average FPS" number.** Rejected: destruction is bursty,
  and the fracture tick is exactly the frame that must not hitch. A mean hides precisely the failure
  the milestone is about.
- **Golden-image or golden-timing constants checked into the repo.** Rejected for the same reason
  m11.7 refused a golden hash: it converts a proof of *this run's* behaviour into a cross-platform
  determinism claim this project has already recorded as false.
- **Put the character controller in `physics` as a solver client.** Rejected: a body the solver
  integrates is not replayable as a pure function, and replay is the substrate prediction is built
  from. It would also make `physics` know about gameplay concepts it has no business holding.
- **Ship the prediction interface at M11 and implement it at M12.** Already rejected by A20, and this
  ADR is the vindication: the signature the controller actually needs (`step_character` over queries,
  a bounded ring keyed by sequence, a replicated `LastProcessedInput`) is not the shape a
  `virtual void replay(...)` would have guessed a milestone earlier.
- **Reconcile by diffing command lists.** Rejected on m11.6c's evidence: `consumed_through` steps over
  permanent gaps, so the two lists disagree under loss in a way that looks like a state error and is
  not.
- **Include Track FL (water) so the block has a river.** Rejected: a whole module and a two-way
  physics coupling, for a clause the vision statement does not make.
- **Split the milestone in two — "The Player" and "The Block".** Genuinely arguable, and *not*
  rejected on the merits: M12 as specified bundles two proof regimes that behave differently, one
  GPU-free and CI-gateable (move/aim/shoot, predicted and reconciled under loss) and one that only a
  single machine can judge (the block holds frame rate). A second independent review of this same
  question proposed exactly that cut. It is **deferred rather than taken** because the milestone table
  and VISION both name one final demo, and re-cutting the map is a deliberate act that wants its own
  decision rather than a side effect of this ADR.

  **The seam is left where the split would go.** The ladder already divides cleanly at
  **m12.5 / m12.6**: everything up to and including m12.5 is the player line and is provable on the
  scripted-loss harness with no GPU; m12.6 onward is content, scale and the hardware claim. If the
  milestone runs long, cutting there is a re-labelling, not a re-plan — which is the whole point of
  naming it now.

---

## Amendment (2026-08-20, m12.0-perf): the first baseline exists, and it does not ratify what §2 hoped it would

§2 ended with *"Proposed headline budget, to be ratified at m12.0 against the baseline, not before"*
and *"If the baseline shows the block sailing under trivially, tighten it at m12.0 — a gate nothing
can fail is not a gate."* The baseline now exists. It says something the instruction did not
anticipate, so the ruling is recorded rather than fudged.

### A1. The headline budget is NOT tightened at m12.0, because the thing it measures does not exist yet

Measured on the reference machine (RTX 3060, NVIDIA 610.43.03, Release, 1920×1080, no sanitizer;
`docs/perf/2026-08-20-*.json`):

| run | frame p50 | p95 | p99 | max | sim p99 | sim max |
|---|---|---|---|---|---|---|
| `11-lit-rooms` — the whole M10 lighting stack | 7.46 | 7.81 | **7.96** | **8.05** | — | — |
| `10-destructible-wall` — a 60-part collapse | 3.15 | 3.60 | **3.79** | 3.86 | **0.394** | **0.526** |

Against the proposed p99 ≤ 16.6 ms / max ≤ 33 ms, that is 2.1× of frame headroom; against the
proposed sim-tick p99 ≤ 6 ms it is 15×. By the letter of §2, tighten.

**We are not tightening, and the reason is that the measurement is of the wrong subject.** The
headline budget is a claim about `99-the-block` — §1's floors put ≥ 8 structures, ≥ 1,500 parts,
≥ 400 peak debris and ≥ 32 local lights in one frame. `11-lit-rooms` has four boxes and four lights;
`10-destructible-wall` has sixty parts and two. Tightening a budget for the block against a scene
two orders of magnitude smaller would produce a number with a measurement behind it and no meaning
in it — the *appearance* of ratification, which is worse than an admitted estimate because it stops
the question being asked again.

So the ruling is a re-timing, not a re-numbering:

> **The headline budget stays as proposed (p99 ≤ 16.6 ms, max ≤ 33 ms, sim-tick p99 ≤ 6 ms,
> collapse-tick max ≤ 12 ms) and is ratified at m12.7, against the block's own content**, when
> there is something of the right size to measure. m12.0's baseline is what the block's cost is
> *budgeted out of*, not a margin already proven.

What the baseline does establish is that the numbers are not fantasy: on this GPU the engine has
~8.6 ms of frame and ~5.5 ms of tick to spend on the difference between these samples and the block.
That is a budget, and m12.7 will find out whether it was enough.

### A2. What m12.0 actually ratifies is the RELATIVE gate, which had nothing to compare against until now

§2b asked the run to *"compare against the last committed report with the same fingerprint, failing
on relative regression"*. Before this brick there were no committed reports, so that clause could
not fire — it was, precisely, a gate that could not fail.

It can now. With `docs/perf/2026-08-20-*.json` committed, a 10% slide on the reference machine fails
at 8.76 ms rather than at 16.6 ms, which is where the real protection lives for the rest of the
milestone. The absolute ceiling remains the *product* bar — loose today by design, because it says
"60 Hz at 1080p" and not "as fast as this commit" — and the relative check is what bites.

The corollary is a working rule, now in CLAUDE.md's brick-delivery list: **a perf-touching brick
commits a `docs/perf/` run.** A regression gate is only as good as the freshness of the thing it
compares against.

### A3. Correction to §2a's inventory: `RIME_PROFILE_ZONE` had no callers at all

§2a lists the instruments that "already exist" and names `RIME_PROFILE_ZONE` among them. The macro
and its swappable sink have existed since M1.6; a grep across the tree found **zero** zones placed
anywhere in `engine/` or `samples/`. What existed was the hook, not the measurement — the same shape
of drift m11.8 found in ARCHITECTURE and the glossary, and worth recording for the same reason.

m12.0-perf places the first zones: `sim.tick` and its five stages, plus `frame.declare` /
`frame.execute` / `frame.submit`. They sit at stage granularity because `report_zone` takes a lock
and copies its sink, so a zone in a per-entity loop would cost more than it measures — a limit of
the current implementation, named here so the next person does not discover it by benchmarking it.

One consequence is visible in the committed reports and is honest rather than tidy:
`10-destructible-wall` drives physics *outside* `Application`'s fixed tick, so its `sim.tick`
timeline reads ~0.001 ms while the real work sits in `sim.physics`. The sample predates the ordered
sim stage (ADR-0032 §8) and has a determinism proof pinned to its current loop; moving it is a
change to make deliberately, not as a side effect of measuring it.

---

## Amendment (2026-08-26, m12.3): the prediction-off baseline exists, and two corrections to §3/§4

m12.3 built the networked player: `gameplay_net`, the consume loop, `LastProcessedInput`, Weapon v1
and the weapon→destruction glue. Three things are worth recording — one number the next brick is
measured against, and two places where building the thing changed what §3/§4 said about it.

### B1. The number m12.4 must beat

§1 makes "own-input response" falsifiable as *"≤ 1 tick, against a prediction-off control showing
≥ RTT ticks, so prediction is provably the reason."* That control now exists and is measured, not
asserted. `tests/gameplay_net/latency_test.cpp`, counting the client's OWN ticks between stamping a
sequence and seeing a mirrored `LastProcessedInput >= q` — both endpoints on one machine's clock, so
no offset can enter (the ADR-0030 §5 trick; there is still no clock synchronisation anywhere):

| link | own-input latency | visible position lag, walking at 6 m/s |
|---|---|---|
| 48 ms one-way = 6 round-trip ticks at 60 Hz | **6 ticks** (best and worst of 8 samples) | **0.30 m** |
| zero-latency loopback | 2 ticks | — |

The zero-latency row is the control on the control: it is the tick-boundary quantum the harness
costs, so the 6 is the network and not the fixture. **Recording it before m12.4 rather than after is
the point** — a baseline measured afterwards is a baseline chosen to be beaten.

### B2. §4's tick loop needed a rate budget it did not mention, and dropping is what keeps the ack honest

§4 says *"drain each session (advancing `consumed_through`, the only honest ack); run
`step_character` per command in order"*. Taken literally that is a speed hack: one step per command
means a client sending two commands a tick moves two ticks a tick, for free, by editing a number in
its own send loop.

The fix is a per-player allowance that refills by one per tick and saturates at a burst ceiling, and
the interesting part is what happens to the surplus. **Dropping it is honest; deferring it is not.**
`consumed_through` is explicitly not a completeness claim — input.hpp already documents that it steps
over permanent gaps — so a client retiring a dropped command has learned the truth: *the server will
never act on this*. Deferring instead would advance the frontier over commands still sitting in a
queue, which is corollary 1 of [the replication invariant](../design/replication.md) pointed
upstream, and m12.4's predictor would then drop from its replay set commands the server had not yet
applied. The divergence a throttled client does suffer is detected and repaired by the very
comparison m12.4 builds, because that comparison is on resulting *state* and never on command lists.

This is the same shape of reasoning §4 already uses about loss, arriving in a place §4 did not look.

### B3. Correction to §3: the weapon's damage scale, and what a wrong default cost

§3 specifies *"`Weapon` v1 (deterministic hitscan via `raycast`, with `RayHit::child` naming the
part)"* and says nothing about magnitudes, which is reasonable for an ADR and left one trap. A cooked
destructible part stands at **1.0 health** (`destruction/world.hpp`), so damage in this engine is a
normalized quantity; the header's first draft defaulted to `45.0f`, an "hit points" number from a
health scale Rime does not have. The m12.3 end-to-end proof deleted all sixteen parts of its test
wall on the opening shot, and then spent 399 further shots measuring rubble.

Recorded rather than quietly fixed because it is a class, not an incident: **an engine default that
is not scaled against the engine's own cooked assets is a design claim nobody checked.** The
defaults are now `damage = 0.5` and `impulse = 25` (the destruction suite tips a wall's upper slab
with 30), and the proof asserts that breaching the wall took several damaging shots — the assertion
that fails if the default drifts back.

### B4. §6's `server.despawn` backstop shipped, and it repairs rather than merely complains

§6 ruled the backstop into m12.3 as *"the first new replication consumer"*. It runs once per
`publish` — not per client — walks the live NetId slots, and for any whose entity is no longer alive
it **retracts the id properly, warns with the id, and counts** (`net_ids_orphaned()`). Repairing
matters: without it a bare `world.despawn()` leaves every client holding a mirror of an entity that
no longer exists, permanently, with no message that repairs it. Counting matters too — a silent
repair would make the mistake invisible and therefore permanent in the source. It should read zero
in any healthy game.

---

## Amendment (2026-08-26, m12.4): prediction lands, §4's replay set was wrong, and m12.3 shipped a silent transform bug

m12.4 built the `Predictor`. Three things to record: the number §1 asked for, a correction to §4's
sketch that only building it revealed, and a defect in m12.3 that its own green proofs could not see.

### C1. §1's "own-input response" clause is met, and its control is measured beside it

§1 asks for *"≤ 1 tick, against a prediction-off control showing ≥ RTT ticks, so prediction is
provably the reason."* Both arms run the same tape over the same 48 ms link and differ in one
boolean (`tests/gameplay_net/prediction_test.cpp`):

| | own-input response |
|---|---|
| prediction **on** | **1 tick** |
| prediction **off** (m12.3's baseline) | **6 ticks** — exactly the round trip |

Supporting numbers, each with its own negative control: worst prediction-vs-authority distance while
walking **0.20 m** (the prediction runs *ahead*, which is the point); under 25% loss over 300 ticks,
124 pairings produced **2 corrections** and 122 within-tolerance skips; divergence after 300 lossy
ticks was **0.2 m with reconciliation and 1.1 m without**; and a run with 13 permanently-lost
commands took 10 corrections and then agreed **bit for bit**.

§4's "a lossy run with zero corrections fails" is enforced as an assertion, not a hope: every other
case in the file would still pass with `reconcile` hard-wired to return false, and that one would
not.

### C2. §4's replay set is wrong: `unacked()` is retired on the wrong frontier

§4 sketches the replay as *"replay every `unacked()` command with sequence > q"*. That has a hole,
and it is invisible on a clean link.

`unacked()` retires on `ClientInputSender::acked_through`, which comes from the `InputAck` message.
That ack rides an unreliable **superseding** stream; the snapshot carrying `q` rides a *different*
one, and relevancy or a byte budget can hold the player's record back further still. So
`acked_through >= q`, often strictly — **measured on 60 of 300 ticks under 30% loss**. Replaying
only `unacked()` therefore skips every command in `(q, acked_through]`: commands the server *did*
consume and the client *did* predict. The prediction slides a few ticks backwards on every
correction, under exactly the conditions prediction exists to smooth over.

**Ruling: the `Predictor` keeps its own `{sequence, command, state}` ring, retired on the reconciled
`q`** — the only frontier that makes the replay set complete. `unacked()` remains what m11.6c built
it to be (the bound on the client's send buffer) and is not the predictor's input.

Everything §4 promised survives. A lost command still sits below the ack jump, so `q` moves past it,
the ring is trimmed past it, and the replay excludes it — both sides end up agreeing the input never
happened. What changes is only *where the replay set comes from*.

A related property, found the same way and worth writing down because it looks like a bug: **a
permanently-lost command stops mattering only when a later one arrives.** If the last thing a client
ever sends is dropped, `consumed_through` stops just short of it and the two sides sit exactly one
tick of travel apart — measured at 0.1 m, which is 6 m/s × one tick, on the nose — until somebody
says something else. A real client keeps sending while standing still, so this is a property of
contrived silence rather than of play; the proofs settle *with* input because of it.

### C3. m12.3 shipped a silent transform bug, and its proofs were green throughout

The controller wrote its pose to `WorldTransform` only. `propagate_transforms` runs one step later
in the canonical tick order and **recomputes `WorldTransform` from `LocalTransform`** for every
entity carrying both — so the write was discarded every tick, by a pass doing exactly its job.

Measured: an avatar whose `CharacterState` had walked to z = −3.29 had a `WorldTransform`, a
`LocalTransform` and a physics body all still reading z = 0. The consequences are all one layer out
from where the tests were looking: the kinematic capsule never moves, so the player pushes nothing
and debris cannot hit them where they are; and **the replicated transform is wrong, so every other
client mirrors that avatar standing at its spawn point forever.**

It was silent because `CharacterState` is what a controller test naturally asserts on, and
`CharacterState` was perfectly correct the whole time. Every m12.3 proof stayed green.

The fix is one function — `gameplay::write_character_pose` — used by both the m12.2 ECS wrapper and
the m12.3 consume loop, writing `LocalTransform` (the one propagate reads) as well as
`WorldTransform`. A parented character is explicitly unsupported and keeps the old behaviour, since
a world-space pose assigned to a child's local transform would be wrong in a different way.

The lesson is the milestone's own, turned on itself once more: **a proof that asserts the value a
system computes has not thereby asserted that the value reached anyone.** m12.3's counters covered
every skip inside the consume loop and none of them could see a write being overwritten downstream.
The new case (`consume_loop_test.cpp`, "the avatar's transform and its physics body follow the
controller") asserts the handoff instead of the state, and was checked against a deliberately
reverted fix to confirm it fails — on all three consequences, including the client's mirror.

---

## Amendment (2026-08-27, m12.5): what "interpolate over the interval a value actually covers" was hiding

m12.5 built the two halves the ladder names — snapshot interpolation v2 in `replication`, and
predicted-player smoothing in `gameplay_net`. The brick was listed as *load-bearing-lite, cut last*.
It turned out to be the one that fixed a defect the milestone had been carrying since m11.6.

### D1. v1's interpolation was wrong for every case except the one its proof exercised

m11.6 blended `previous → current` over **exactly one tick period**, then expired the pair. That is
correct if and only if a value arrives every tick — and m11.6's proof moved its entity every tick,
so it was the only case ever exercised.

A real session is never in that case. Loss drops snapshots; relevancy holds distant entities back;
the byte budget defers records; and the server sends nothing at all for an entity that did not
change. So a mirror routinely receives one value carrying N ticks of motion — and v1 played all of
it in one tick and held still for the other N−1. **The mirror lurched and froze, at a rate set by
how badly the link was behaving**, which is when a player is least forgiving.

Measured over a 3-tick interval, sampling four frames a tick: v1 showed **79 motionless frames out
of 120**; v2 shows **0**, and its worst single-frame jump is a third of v1's.

This is the third time in this milestone that a proof asserted the right value and missed the thing
that mattered (m12.3's transform write was the second — amendment C3). The pattern is worth naming
because it is not carelessness, it is a property of what is easy to assert: **state is easy to check
and motion is not**, so a suite grown from convergence proofs is systematically blind to how
anything *moves*. Every position v1 produced was correct. The bug was in the timing, and only a test
that sampled frames rather than ticks could see it.

### D2. The span needs no clock, and that is why it was available all along

The fix needs to know how many ticks a value covers, which sounds like it needs a clock — and
ADR-0033 A11 rules one out, with M12 inheriting the ruling (§4). It does not: the Delta header
already carries the server tick, and the span is a **difference of two server ticks**. A difference
has no origin, so it says nothing about what time it is on either machine and needs no offset. The
information was already on the wire; nothing was reading it.

Two bounds go with it, each argued at its site: a gap above `kMaxInterpolationSpan` (8 ticks) snaps
rather than crawling, and is counted; and a value arriving mid-blend retargets from where the mirror
is *being drawn* rather than from `current`, so jitter does not reintroduce the jump v2 removes.

**Not built, and named:** a full playback clock — render at `server_tick − delay` against a
multi-sample ring, which is what handles a value arriving out of *order* rather than merely late. v2
holds two samples and cannot reorder them. That wants a per-client clock and a real jitter buffer,
and it should be built when the ledger shows the tail it addresses actually binds.

### D3. Smoothing is presentation-only, asserted rather than intended

A correction is a rewind: the state jumps, correctly. Drawing that jump is rubber-banding, so the
displacement is absorbed into a visual offset that decays away (~90% in eight ticks) and is
**bounded** — a correction above `max_smoothing_distance` is shown at once, because sliding a player
across two metres of a firefight draws them somewhere they demonstrably are not, and they will shoot
from there.

The claim that matters is that none of this reaches the simulation. `state()` never carries the
offset; only `visual_position()` does. A smoothing layer that leaked would be a hidden accumulator
inside a function m12.2 proved pure, and it would surface as a rare desync rather than a failing
test — so it is asserted directly: two runs on the same seed and tape differing only in
`smoothing_decay` produce **bit-identical simulation trajectories across 250 lossy ticks** while
their drawn poses differ on 156 of them. Setting `smoothing_decay = 0` restores m12.4's behaviour
exactly, which is both the negative control and the cut path if the brick has to go.

---

## Amendment (2026-08-27): the deferred split was taken — see ADR-0036

The "Alternatives considered" entry above left this open:

> **Split the milestone in two — "The Player" and "The Block".** … It is **deferred rather than
> taken** because the milestone table and VISION both name one final demo, and re-cutting the map is
> a deliberate act that wants its own decision rather than a side effect of this ADR.
> **The seam is left where the split would go.** The ladder already divides cleanly at
> **m12.5 / m12.6**.

That decision has now been made, at that seam, in
[ADR-0036](0036-milestone-split-player-and-block.md). **M12 is "The Player" (m12.0 – m12.5, plus a
new closing proof m12.6); M13 is "The Block".** M13's "done when" is this ADR's §1 thesis verbatim —
the demo did not move.

**Reading this document after the split.** Everything it decides still stands; only labels below the
seam changed, and this file is append-only so they could not be edited in place. The mapping:

| this ADR says | now |
|---|---|
| m12.6 (Track FX fx1) | **m13.1** |
| m12.7 (block content, culling, C6) | **m13.2** |
| m12.8 (playable client) | **m13.3** |
| m12.9 (audio) | **m13.4** |
| m12.p (measured perf pass) | **m13.p** |
| m12.10 (the `99-the-block` proof) | **m13.5** |

So the A1 amendment's promise that the headline budget is "ratified at m12.7, against the block's
own content" now reads **m13.2**; §6's C6 ruling splits across m12.0-perf (landed) and **m13.2**;
and the cut order in §5 lies entirely inside M13.

One label is reused: this ADR's ladder had no "m12.6 milestone proof", so a reference to **m12.6
here means Track FX fx1**. ADR-0036 §"The brick ladders" carries the same table and says so too.


## Amendment (2026-08-29, m13.2c/d): §2's brick list was missing a system, and the scene format must not carry a MaterialRef

### E1. There is no render-leaf system, and §2's ledger never noticed because nothing was drawing parts

§2a inventoried the render work M13 must ADD and named exactly one entry: view-frustum culling,
"because no cull exists yet". It missed a larger one. **Nothing in the engine turns a destructible's
parts into anything a renderer can draw.** `destruction/world.hpp` has said so since M8 — "per-part
render leaves land with the [sample]" — and `10-destructible-wall` accordingly hand-rolls
`build_leaves`/`refresh_leaves` against one instance of 60 parts.

That is a defensible v1 for one wall. It is not one for §1's floors: 140 instances totalling 2,016
parts, with m13.3 and m13.5 each about to copy the loop. So m12.7's remainder split into **m13.2c**
(the block as content) and **m13.2d** (`engine/destruction_render`, the bridge).

The bridge is its own module for the reason that produced `replication` and `destruction_net`:
`destruction` must not depend on `render` (it sits below it and is proven GPU-free), and `render`
must not know what a fracture is. Its load-bearing property is **one mesh per (pattern, part), not
per (instance, part)** — 148 uploads for the whole block rather than 2,016 copies of identical
geometry, which is the difference between the naive loop being affordable and not.

### E2. `.rscene` must carry a ROLE, not a MaterialRef — and this is a general property of the format

§2 assumed the "procedural assembly script emitting `.rscene`" would author the block's appearance.
It cannot, safely. `render::MeshRef` and `render::MaterialRef` hold **dense indices into runtime
registries**. A scene file storing `MaterialRef{7}` is correct only for as long as every loader
happens to build its registries in the identical order; insert one material at the front of the
palette and every entity in the file shades as something else, with nothing failing and no counter
moving.

So the block's `.rscene` carries placement plus a reflected `blockkit::SlabRole{building, storey,
kind, tint}`, and `apply_palette()` derives `MeshRef`/`MaterialRef` at load. This is the same split
`.rdest` already makes — cooked destruction geometry carries no material either — and it has a
pleasant consequence the ADR did not anticipate: **the entire appearance of the vision demo is one
translation unit of floats**, re-tintable without regenerating the scene or re-cooking anything.

Stated generally, for any future `.rscene` author: *the format may carry content ids and authored
intent; it must not carry a dense runtime registry index.*

### E3. `rime fracture` could write geometry the runtime refuses, and had been doing so since M8.1

Building the block hit a `register_hull` rejection: a cooked part whose faces disagreed about a
shared edge (a duplicated directed edge, two edges with no twin). The `.rdest` decoded *cleanly* —
every count in range, every index valid, the asset schema fully satisfied — and only the physics
validator caught it, at load, one process boundary away from the cause.

Two things came out of that, and they should be read separately.

**The guarantee, which shipped.** `rime fracture` now runs the same closed-manifold check
`build_convex_hull` runs and refuses to write a file whose *topology* it would reject. That word is
load-bearing: the guard mirrors the two topology rules only, not the runtime's planarity, convexity
and volume checks. Empirically that is enough today — across a 200-seed sweep no topology-clean cell
failed a metric check — but the worst planarity residual measured is 7.2e-5 against a 1e-4 limit,
which is 28% of headroom and not a margin to rely on. Mirroring the metric checks too is cheap and
should happen when someone next touches this. The cost of *not* having it is
the shape of failure this repository keeps legislating against: late, silent, and attributed to
whatever tried to use the artefact. `every_part_is_a_valid_convex_shape` also now checks topology,
which its name has claimed since M8.1 while it verified only counts and index ranges.

**The defect, which did not.** It is not a new-configuration problem, and the first draft of this
amendment said it was. Measured over 60 seeds per config: 4/60 at 28 parts in a thin 8x3x0.3 wall,
**3/60 in a thick 8x3x3 one** — so thinness is not the driver — and 3/60 at the 2x1.5x0.3 / 16-part
config this cooker's own tests have used since M8.1. The rate tracks part count, not shape.

The cause is in `build_cell`'s epsilon handling, and three hypotheses have been **refuted by
measurement**: duplicate faces from near-coincident planes (deduping them changes no outcome), an
inverted winding (every face agrees with its plane normal to dot > 0.9), and the thin-box framing
above. Every failure observed contains a *duplicated directed edge*, never an untwinned one alone —
which points away from the T-vertex story and toward **rival copies of a single geometric corner**:
where several planes pass within EPS of one point, the triple enumeration emits each rival, they can
end up further apart than the dedup tolerance, and the +/-EPS membership slack then seats both on
both incident faces. The same slack admits points genuinely outside the cell by up to 1e-4.

If that reading holds, the fix is not a bigger epsilon — it is **sequential half-space clipping**:
start from the box as a closed vertex/edge/face structure and clip plane by plane, computing each
edge-plane crossing once and sharing it between the truncated face and the new cap. Closure becomes
an invariant by construction rather than an agreement among ~34 independent epsilon tests. It is
also the foundation ADR-0027's deferred mesh-sourced fracture needs.

**Schedule it in the m13.3 window rather than later**, for a reason that is about cost and not
elegance: the rewrite changes cooked bytes for *every* seed, so it should land before more `.rdest`
fixtures and replay baselines accumulate against the current output. m13.2c's own cooks are
generated at test time and deliberately uncommitted, which is what keeps that cost small today.

The finding that matters most for calibration: **this was already on `main`.** Seed 2 of
`a_different_seed_gives_a_different_partition` has cooked two malformed parts since M8.1, and the
test never noticed because it compares cooked BYTES and never registers a hull. A proof that cannot
see what it skipped reads as passing — the guardrail-5 lesson, arriving from the asset pipeline
rather than from replication.

### E4. A brick is added to M13: the engine has no text rendering at all

Luca ruled on 2026-08-29 that the vision demo gets a native HUD rather than stats in the Rust
editor's egui shell. There is **no font atlas and no text pass anywhere in the C++ engine**, so this
is a rendering subsystem, not a detail of the playable-client brick. §2's m12.8 (now m13.3)
therefore splits:

- **m13.3a** — windowed present + the first-person camera on the predicted player (ADR-0035's
  original m12.8). *Load-bearing.*
- **m13.3b** — text/HUD: a font atlas, a text pass on `GizmoRenderer`'s always-on-top overlay model,
  and a designed overlay. *Load-bearing — it competes with m13.4 (audio, already cuttable) for the
  milestone, and that trade should be made deliberately rather than discovered.*
