# ADR-0042: Track FL reopens; the substrate ruling; and the fluids track queues behind M18

- Status: Accepted
- Date: 2026-09-17

## Context

Luca asked to watch water, fire and smoke in the block, and guessed the three might share one
substrate. That ask is itself the reopening event for a decision this repo already made and
explicitly left reopenable.

**What ADR-0035 §5 ruled.** At M12.0 (2026-08-20), [ADR-0035](0035-vision-demo-m12.md) §5 ruled:

> **Track FL (water): out of M12.** The roadmap parked this as *"decided at M12.0"*; **this is that
> decision.** No water in the block. Heightfield water plus two-way buoyancy is a whole track, and it
> does not earn a slot in a demo whose thesis is destruction, lighting and networking at scale. The
> ADR-0026 substrate seams stay intact.

`docs/ROADMAP.md`'s cross-cutting tracks paragraph recorded the same ruling with the reopening
condition attached in its own words:

> Track FL (`engine/fluids`) — CPU heightfield water with two-way buoyancy coupling into physics;
> *decided at M12.0, and the decision was **no**:* no water in the block (ADR-0035 §5). A whole
> module plus a two-way physics coupling does not earn a slot in a demo whose thesis is destruction,
> lighting and networking at scale; the ADR-0026 substrate seams stay intact **for whenever it
> opens**.

(`docs/ROADMAP.md:1635-1639` as it stood at `4a8383a`, before this ADR's companion edit replaced the
sentence; emphasis added on the clause this ADR now exercises.) The condition named was never a date
or a milestone — it was the owner asking. The owner has now asked (2026-09-16: a water + fire +
particle/smoke track, and *"I love to watch smoke and want to see it in the engine"*), so the seams
ADR-0035 kept intact are the ones this ADR opens.

Nothing about the 2026-08-20 ruling was wrong and nothing here reverses it as a past fact: M12/M13
correctly did not carry a whole module and a two-way physics coupling for a clause the vision
statement did not make. What changed is the input, not the argument — the ask itself is new
information, exactly as ADR-0035's own wording anticipated.

**Why a substrate question rides along.** The owner's ask names three domains — water, fire,
particle/smoke — and one intuition: that they share a common simulation base.
[`docs/design/fluids-track-plan.md`](../design/fluids-track-plan.md) (written by GLM, 2026-09-17) is
the planning report this ADR is built from: it adjudicates that intuition, prices the two ways to
answer it, and designs the three bricks that follow from the answer. This ADR records the owner's
ratification of that plan's recommendation and the sequencing decision layered on top of it. The
plan is committed beside it as the working paper carrying the brick-level detail this ADR
summarizes rather than repeats; **where the two disagree, this ADR governs**, and the paper's header
lists every claim of its own that this ADR corrects.

**What Track FX already carries, and what it does not yet do.** `docs/ROADMAP.md`'s Track FX
definition (the *Cross-cutting tracks* paragraph) scoped a GPU particle substrate for fire and
dust/smoke, spawned from the M8 destruction event fan-out, with two clauses named and deferred:
`fx1b`'s compute scale-up was made "contingent on the ledger" and fire-as-light was "deferred behind
its seam." `fx1a` (the draw pass, m13.1a) shipped and is load-bearing, exactly as ADR-0035 §5
required. Verified against the tree while writing this ADR: **`fx1a` has never had a consumer
outside its own test suite.** `samples/10-destructible-wall/main.cpp` declares an M8.4 `vfx::DustField`
(:166) and calls its family-less `emit_burst(min, max, intensity)` overload (:200, :205;
`dust.hpp:97`) — headless dust with no draw pass, predating `fx1a` — and `samples/99-the-block` has no `vfx`, no
`FxParticlePass`, and no `FxSettings` reference anywhere in it. The only callers of the m13.1a/b
machinery are `tests/render/fx_particle_test.cpp`, `tests/destruction/events_test.cpp`,
`tests/vfx/dust_test.cpp` and `tests/vfx/families_test.cpp`. The owner who asked to watch smoke has
never seen this engine draw any — which is this repo's own recorded lesson turned on itself
(`docs/adr/0041-the-visual-bar-m17.md:812-813`: "a capability shipped without a consumer is a
capability that has not been tested"), now 21 days old (`fx1a` landed 2026-08-27,
`docs/ROADMAP.md:621`).

## Decision

### Ruling 1 — the substrate: fork now, hybrid gated later, not built now

Two ways to answer "do water, fire and smoke share a base?" were priced.

**Option A — the ratified fork, reopened.** Track FX stays a GPU particle substrate (no pressure
solve — buoyancy-velocity, curl noise, per-particle growth); Track FL stays a CPU-designed, GPU-run
heightfield (shallow-water on `h(x,z)`, two-way buoyancy into the CPU physics via ADR-0026's named
seam). Two modules, two machinery sets, no shared solver.

**Option B — the unified particle-grid substrate.** One hybrid solver carrying water, smoke,
fire-heat and granular dust as fields over one state: particles plus a background grid, PIC/FLIP/APIC
for momentum exchange (Jiang/Schroeder/Selle/Teran/Stomakhin, *The Affine Particle-in-Cell Method*,
TOG 34(4) 2015), MPM above it for phase-change/heat transport (Stomakhin et al., *Augmented MPM for
Phase-Change and Varied Materials*, TOG 33(4) 2014).

Both priced (all figures are **estimates**, so labeled by the plan and re-labeled here, and none of
this has been built or measured):

| | A: particles + heightfield | B: hybrid particle-grid |
|---|---|---|
| **GPU sim cost** | fx1b **est. 0.25–0.9 ms** at ~64k particles; water **est. ≤0.1 ms** at 256² | **est. 3–8 ms**: P2G/G2P at ~1M APIC-class particles (est. 2–4 ms) plus a pressure/divergence solve at 128³ (est. 1–2 ms); 256³ is **est. 8–20 ms**, not 60 Hz on the reference 3060 |
| **CPU sim cost** | fx1b ≈ 0 net (removes more than it adds); heightfield **est. +0.1–0.3 ms** into `sim.block` | **est. +0.2–0.5 ms** (dispatch, emission, witness readbacks) |
| **engineering** | days-scale: every piece exists (pass, families, event glue, the m17.4 buffer-ring seam); the heightfield collider is a named, waited-on placeholder | a new subsystem class — 3D grid allocator, boundary conditions, particle↔grid-consistent reseeding, a divergence-free projector, a GPU-desync surface extraction, multiple barrier-dense dispatch chains — **est. 3–6 bricks, 1–2 milestones** at this repo's demonstrated pace |
| **what it forecloses if built now** | pour/splash/fill-a-room *in this system* (2.5D water: waves and flood-filling, no vertical structure); and, if the hybrid is built later, a second water stack to maintain | **M18's ratified slot** (virtualized geometry, approved 2026-09-03, `docs/adr/0041-the-visual-bar-m17.md:212`) or any water for roughly 1–2 months, whichever it competes with |

The comparison's shape does not depend on the estimates' precision: A's total added cost
(~0.3–1.0 ms GPU, ~0.1–0.3 ms CPU) is a third to a tenth of B's, and B's GPU cost lands in a gated
frame last measured at `frame` p99 **19.849 ms** against a ratified **16.600 ms**, with `sim.block`
p99 **15.468 ms** against a ratified **6.000 ms** — the largest breach on the board
(`docs/adr/0041-the-visual-bar-m17.md:892`, the 2026-09-07 m17.8b amendment, clock-pinned). ADR-0041's
2026-09-17 amendment withdrew that amendment's A/B *table* when clock pinning went away, but it did
not re-measure these p99s — its retake is a render-only hold loop, p50 pass times with the
simulation frozen — so they remain the latest gated-frame figures. Nothing below leans on their
third decimal: the argument rests on the sign and scale of the two breaches. `sim.block`'s stated paydown (graph colouring or a Jacobi/hybrid
velocity solver, under ADR-0026's determinism contract) is explicitly **ranked with M18, not
schedulable here** (`docs/adr/0041-the-visual-bar-m17.md:599`).

**Ratified: Option A, now.** Land the two-substrate fork at its originally-priced scope. Option B is
**not cut** — it is recorded as this track's destination, gated rather than built, because `fx1b`'s
particle half (SoA state, emission, witnesses, draw consumption) *is* the hybrid's particle half
either way: PIC/FLIP/APIC is "particles plus a grid," and the particle side is what `fx1b` builds
regardless of which substrate answer wins. Building the hybrid's grid stage first buys nothing the
fork does not already require building.

**The hybrid's trigger — testable criteria, not appetite.** All four must hold before Option B is
scheduled as work: the plan's three (§4.1), plus a fourth that makes the CPU's headroom explicit,
because the plan's own numbers argue for it:

1. The gated `frame` p99 is at or under the ratified 16.600 ms budget (M18's paydown has landed).
2. M18's virtualized-geometry-vs-terrain ordering question is answered, so the hybrid's GPU cost is
   priced against a settled geometry pipeline rather than a moving one.
3. The owner still wants the unified formalism after watching `fx1b` + `fl1a` + `fx1c` running — a
   preference nobody can know today, and this ADR does not try to.
4. `sim.block` p99 is at or under its ratified 6.000 ms budget. Clause 1 does not imply it — the two
   budgets are ratified separately, and ADR-0041:893-894 records `frame.player` meeting 16.600 while
   `sim.block` breaches 6.000 — and the hybrid adds CPU cost of its own (est. +0.2–0.5 ms: dispatch,
   emission, readbacks), which must land in headroom rather than deepen a breach.

Every clause is checkable — three against the ledger, one by asking the owner — and none holds
today, which is why this is a condition rather than a schedule.

### Ruling 2 — sequencing: strictly after M18, no interleave

All three bricks below queue **after M18 finishes entirely**. This is stated without hedging because
Luca chose it explicitly, as the more conservative of two options offered: M18's ratified slot
(virtualized geometry / terrain, approved 2026-09-03 — ADR-0041:212 and the M18 row of
`docs/ROADMAP.md`'s milestone table) is **fully protected** — nothing about this track competes with
it, interleaves with it, or lands concurrently with it. M17 precedes M18, so none of the three
interleaves with M17's remaining ladder either. The tracks' usual cross-cutting latitude
("interleave under mainline-first", ROADMAP's *Cross-cutting tracks* paragraph) is simply not
exercised until M18 has closed; from then on it applies as it does to any track. `fx1b`, `fl1a` and
`fx1c` are ordered strictly behind M18's close, full stop.

### The three bricks, at ADR scope (full detail: [the plan](../design/fluids-track-plan.md) §3)

Order: **fx1b → fl1a → fx1c**, house rule cut order if any runs long: fx1c → fl1a; never-cut:
fx1b.1 (it is the owner's ask), the witnesses, the covenant (every brick files its own `docs/perf/`
A/B, per ADR-0035 amendment A2's relative gate).

**Brick 1 — `fx1b`: the compute-sim scale-up, un-cut, and finally consumed.** `fx1a`'s draw pass has
had zero non-test consumers for 21 days (Context, above); this brick's first half (`fx1b.1`) wires the
block's existing, tested destruction/weapon-event glue to the pass and turns on `FxSettings::enabled`
— no compute yet, just consumption, plus the counters the CPU-side 200-particle cap has never had
(`engine/vfx/include/rime/vfx/dust.hpp:91-92` says it "silently drops the rest" today — a drop path
with no counter, which CLAUDE.md's guardrail 5 says every skip/drop/defer path must have; fixed in
this brick's first hour rather than left standing). The
second half (`fx1b.2`) moves the simulation to a compute dispatch — SoA particle buffers, GPU
integration/retirement, curl-noise + buoyancy + Wavelet Turbulence detail octaves (Kim/Thürey/
James/Gross, SIGGRAPH 2008) — and adds lit shading so the billboards read as smoke, not glowing fog.
**Done when (headline):** in `--headless`, after a scripted collapse, `vfx.particles_drawn > 0`
against a byte-identical `enabled=false` negative control (the ADR-0032 §11 proof, re-run over the
new compute path); the GPU-computed drop/draw counts reconcile with emissions minus retirements; the
A/B shows the GPU delta in `frame.submit`/`frame.render` with disjoint ranges. **Budget answer:** est.
+0.25–0.9 ms GPU (expected ~0.4); CPU delta ≈ 0 (the CPU sim it removes costs more than the dispatch
it adds). Whether that GPU delta reaches the gameplay frame 1:1 is **not** assumed: the plan leaned on
ADR-0041's 2026-09-07 m17.8b coupling (`frame.submit` p50 +0.130 ms → `frame` p99 +0.132 ms), and
ADR-0041's 2026-09-17 amendment withdrew exactly that claim — unpinned, the gameplay-frame noise
floor (1.60 ms at `frame` p50) swamped m17.8b's ~0.11 ms. An effect at the low end of this brick's
estimate sits inside that same noise, so its A/B must be designed to resolve it (the hold-loop
protocol of [`docs/perf/m17.8b-hold/`](../perf/m17.8b-hold/), or better) rather than inherit a
coupling nobody has currently shown.

**One correction to `docs/ROADMAP.md`'s current wording, discharged by this brick's design rather
than left standing:** `fx1b`'s contingency was written at ADR-0035 §5 as triggered by "the ledger
showing the CPU sim binding" (`engine/render/include/rime/render/fx_pass.hpp:48-50` repeats it
verbatim: "if and only if the work ledger shows the CPU sim binding — not on appetite"). Taken at that
wording, the condition **cannot be met**: the CPU sim is capped at 200 particles
(`dust.hpp:87`), costs effectively nothing, and nothing has ever measured it "binding" — it was left
contingent twice, at M12.0 (ADR-0035 §5) and again at the M12/M13 split (ADR-0036:80, :106, second on
the cut list), and never built or measured. What actually binds is the *feature*: 200 CPU particles
and a 4096-draw cap (`FxSettings::max_particles`, `fx_pass.hpp:70`) against an owner who asked to
watch smoke. The criterion transfers from "the clock binds" to "**the caps bind the ask**" — made a
ledger fact, not an appetite, by the drop-counter `fx1b.1` adds. Concretely: `fx1b.1` (consumption
and counters) goes ahead as soon as Ruling 2 allows; `fx1b.2` (the compute move) goes ahead when
`fx1b.1`'s counter shows the 200-particle field dropping emissions during the block's scripted
collapse. See the ROADMAP.md edit, below.

**Brick 2 — `fl1a`: the heightfield and the buoy — Track FL reopened at its ratified scope.** This
*is* ADR-0035 §5's Track FL, executed rather than reversed: a new removable module `engine/fluids`
(depends on `core` plus the ADR-0026 physics *interface* only — physics never learns it exists, the
dependency direction ADR-0026:147-149 already commits to), carrying a GPU-free-provable 256²
shallow-water field (mass conservation, an energy-decay bound, an analytic buoy-at-rest test) and
two-way coupling (deposit velocities to GPU uniforms; retrieve buoyant-body forces via the m17.4
`wait_and_borrow` seam, one tick stale, which is physically invisible for buoyancy and
deterministic-input by construction). The heightfield collider shape closes the named,
waited-on placeholder `docs/adr/0041-the-visual-bar-m17.md:496-497` carried into m17.8's wake. One
re-read the plan calls out and this ADR ratifies: ADR-0035's "CPU heightfield water" carried the
M12-era *GPU-free-proof* convention, not a *GPU-free-production* mandate; with the CPU now the
2.58×-over binding resource and the GPU idling, the production field computes on GPU (est. ≈0.05 ms)
with the CPU implementation kept as the proof oracle, agreeing within a stated margin — an all-CPU
fallback (est. +0.1–0.3 ms into `sim.block`) is documented as the honest alternative if the margin
does not hold. **Done when (headline):** `Σh·ΔA` drift bounded over a 10,000-tick disturbance run
against a deliberately leaky control that fails the same bound; a dropped buoyant body exchanges
momentum and re-bobs (asserted as an energy/decay bound); the cross-peer convergence witness,
`destruction_net::shared_state_hash`, still converges bit-exactly with buoyant bodies added. **Budget answer:** CPU
est. +0.1–0.3 ms (or ≈0 on the compute path) into `sim.block`; GPU est. 0.05–0.1 ms; explicitly does
not attempt to close the 9.5 ms `sim.block` overage, and inherits the wait for M18's paydown the same
way `fx1b`'s GPU delta does. Visible water (a displaced-grid surface entity reusing `fx1a`'s
vertex-storage-buffer-read pattern and the palette's fresnel trick) is its own later brick, `fl1b`,
out of this ADR's scope.

**Brick 3 — `fx1c`: fire drives lights — the deferred clause, landed.** The third clause of Track
FX's own sentence (its ROADMAP definition: "fire drives lights, smoke reads the M10 lighting data") —
the second half of which `fx1b.2` discharges — closes here: a reflected `FxLight` component adds
≤8 rows of `LightDesc` to the clustered pipeline's existing, CPU-written-every-frame light buffer
(muzzle-flash → a 1-frame radiance spike; `IslandDetached` → a 2–3-frame decaying fireball light), and
an ignition-state curve on struck flammables that **drives `fx1b`'s `LingeringSmoke` emission rate**
— fire, smoke and light closing one loop in a single demo. No new pass, no new GPU technique: the
cluster cull, forward-PBR shading and DDGI trace all already read the shared buffer, so fire's
contribution to global illumination is a consequence of the buffer write, not new machinery.
**Done when (headline):** a lit wall's post-flash radiance measurably exceeds its pre-shot sample
against a no-`FxLight` control; the DDGI irradiance checksum moves by more than its measured noise
floor when fire lights are present versus absent; two runs differing only in ignition state produce
measurably different `LingeringSmoke` coverage curves (against `fx1b.1`'s counters — why this brick
is sequenced third). **Budget answer:** the cheapest of the three — est. +0.05–0.15 ms GPU (8 extra
lights through cull+shade, measured not asserted), est. ≤0.1 ms CPU.

### What this ADR does not decide, condensed from the plan's §4

Explicitly deferred, not built, not scheduled by this ADR: the hybrid particle-grid before its
trigger fires (Ruling 1); volumetric fog / froxel smoke (no froxel pass exists; correct only if
billboards demonstrably stop satisfying, not pre-emptively); sorted/alpha-blended FX and the FX
texture atlas (`fx_pass.hpp:41-46` already reserves both as separate decisions); SPH water (a
predecessor technique to the grid, sharing nothing with `fx1b`'s particles the way FLIP does);
ML/regression-forest super-resolution of the sim (right technique, no inference dependency this repo
carries — revisit inside the hybrid); FFT-ocean synthesis (wrong tool for an interactive, disturbable
40 m basin); MegaLights-class FX lighting (`fx1c` emits ≤8 rows; where clustered falls over is
m17.6's measurement, not this track's); a multiplayer FX wire (FX is outside replicated state by
design, `engine/vfx/README.md:53`, "nothing depends on it" — the replicated destruction/weapon events
are the shared cause, each peer's FX is a locally computed, cosmetic effect); the M18
virtualized-geometry-vs-terrain ordering question (adjacent to `fl1a`'s bounded 256² basin, not
coupled to it — this ADR does not resolve it); flame propagation / a heat-field solver (the fx1c
"fire" is event-ignition plus a light curve, not cellular heat transport — that wants the hybrid's
MPM-carried temperature scalar and is deferred with it); and MoltenVK compute performance (untested,
unmeasured, not gated on until something ships on the Mac — Power > portability, VISION #2).

## Consequences

- **`docs/ROADMAP.md`'s cross-cutting tracks paragraph changes** (in the same change as this ADR):
  Track FL's status moves from "decided at M12.0, and the decision was no" to "reopened, see
  ADR-0042," at the scope this ADR ratifies; `fx1b`'s contingency wording is corrected from
  "contingent on the ledger [showing the CPU sim binding]" to the criterion transfer stated under
  Brick 1 above (the caps binding the ask, not the clock); the three brick names (`fx1b`, `fl1a`,
  `fx1c`) and Ruling 2's sequencing are recorded in the same paragraph. Nothing else in the roadmap
  changes — see the note below on why the M17/M18 ladder entries are untouched.
- **`engine/fluids` becomes a new removable module**, depending on `core` and the ADR-0026 physics
  interface only, discharging that ADR's named seam (ADR-0026:96-99, 142) four weeks of repo-time
  after Track FL was ruled out.
- **`fx1a` gets its first non-test consumer since m13.1a landed** (21 days), closing the
  capability-without-a-consumer gap this repo's own m17.8 amendment names as a repeated failure mode.
- **The hybrid particle-grid substrate is now a recorded, gated future** rather than an unranked
  possibility — the next time someone proposes it, this ADR's trigger is what they check against,
  not appetite.
- **M18's ratified slot is unchanged and unthreatened.** No M17 or M18 ladder text needs to change:
  this track's own cross-cutting convention is that such tracks *interleave* with milestone work
  rather than edit the ladder (the *Cross-cutting tracks* paragraph), and Ruling 2 above goes
  further than that default — this track does not even interleave with M18, it queues fully behind
  it. There is therefore nothing in M18's entry for this ADR to touch; if a future reader is tempted
  to add fluids language to M18's row of the milestone table, that temptation is exactly what
  Ruling 2 forecloses.
- **No brick here lands without its own `docs/perf/` A/B**, filed on its own commit, arms separated
  by a ledger counter rather than memory — the covenant m17.8b established and this track inherits
  without exception (ADR-0035 amendment A2; `docs/perf/README.md:178`).

## Alternatives considered

- **Build the hybrid now (Option B, un-gated).** Priced above: est. 3–8 ms GPU, 3–6 bricks, 1–2
  milestones. Rejected for the reasons Ruling 1 gives — the budget asymmetry (`sim.block` 2.58× over
  with its paydown ranked into M18, while all twelve GPU passes take 11% of the frame budget at p50
  and 14% at max, per ADR-0041's Ruling 6 as corrected 2026-09-17), the ask
  being three watchable domains rather than one formalism, and the load-bearing continuity fact that
  `fx1b`'s particle half is the hybrid's particle half regardless of which substrate wins — so
  sequencing the fork first costs the hybrid nothing it would otherwise have bought.
- **Park everything (do nothing until M18 closes, decide fluids then).** Rejected: the ask is real,
  and Option A's total added cost (~0.3–1.0 ms GPU, ~0.1–0.3 ms CPU across all three bricks) is
  affordable inside the fork-now-sequence-later shape Ruling 2 already establishes; there is no
  reason to also delay the decision itself. Parking the decision is a different thing from parking
  the work's schedule, and only the latter serves M18's protection.
- **Water-only or smoke-only subsets.** Not separately argued by the plan and not adopted here: the
  three bricks are independently priced and independently gated by their own `docs/perf/` A/Bs, so a
  future session choosing to land `fx1b` without `fl1a` (or vice versa) is already free to do so
  inside this ADR's ruling — nothing here requires all three to land as one unit. What Ruling 2 does
  require is that none of the three interleaves with M18.
- **Reopen Track FL by editing ADR-0035 §5 to say "reopened."** Rejected: ADRs are append-only. §5's
  ruling was correct for the information available on 2026-08-20 and stays on the record as written;
  this ADR is the newer decision that supersedes it for the current tree, exactly the mechanism
  `docs/adr/0036-milestone-split-player-and-block.md`'s own "VISION.md is not edited, deliberately"
  section models for a sibling case.

---

## A note on what else this ADR does not touch, and why

Following [the plan](../design/fluids-track-plan.md)'s §5 argument, verified rather than assumed:

- **Not `VISION.md`.** This changes neither intent nor scope — VISION's "feels right"/UE5-class
  language already covers fluids, the same argument ADR-0036 §"VISION.md is not edited, deliberately"
  made for its own re-labelling. Re-labelling churn is what that precedent warns against.
- **Not ADR-0026, ADR-0035 or ADR-0036.** ADR-0026's fluids seam (§"The core as a universal
  substrate," ADR-0026:96-99) is *consumed* here, not amended — nothing in it became false. ADR-0035
  §5 is *superseded*, not edited: its ruling was right for 2026-08-20 and stays intact as the
  historical record; this ADR is the newer decision, which is exactly what append-only means. ADR-0036
  is untouched because nothing here re-cuts a milestone boundary.
- **Not `CLAUDE.md`.** The brick-delivery covenant (a `docs/perf/` run on every perf-touching brick)
  already covers all three bricks above without a fluids-specific addition; adding one would be the
  habit-vs-ledger mistake ADR-0035 §2c already named in miniature.
