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

An unruled item silently becomes scope, so the M12 plan ruled all 37 found across the ADRs, module
READMEs, the roadmap's named-gap trail and the session notes. Summarised:

- **In M12:** prediction (m12.4) · snapshot interpolation over the real interval (m12.5) · fx1a
  (m12.6) · audio as cuttable (m12.9) · the cook-cache schema-key fix (m12.7, the first cook-touching
  brick) · the `server.despawn` discipline backstop (m12.3, the first new replication consumer) ·
  physics-interpolation-into-alpha, folded into m12.4/m12.5.
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
  emitting `.rscene`, street props, and **view-frustum culling with its submitted/culled counters**.
  *Load-bearing.*
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
