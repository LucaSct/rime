# Handoff — 2026-10-13: three replication/proof bricks landed, and the solver-energy question closed

Session scope: Luca asked for all three bricks left open by
`HANDOFF-2026-10-12-TIER2-CONVERGENCE.md`. All three are done. The third one's result **inverted**
its own premise and uncovered an unrelated, more serious defect in `main`'s solver, which is now
the open thread.

## Landed on `main`

| PR | SHA | What |
|---|---|---|
| #317 | `e0601fa` | the block's settle report claimed agreement on failure |
| #318 | `7f69a4b` | a refused damage-op packet was a silent permanent loss |
| #319 | `7da85c8` | island-parallel bit-identity with non-diagonal inverse inertia |

All three green on all 8 real checks (3-OS + format + ASan/UBSan + TSan + Editor smoke + SDK
consumer). The 5 "skipped" entries per PR are the hosted duplicates of the self-hosted jobs.

### #317 — the diagnostic that claimed success on failure
The drain-to-quiescence loop in `samples/99-the-block/main.cpp` has two exits and printed
"peers agreed" for both. The *claim* was always correct (`settled_after < kSettleBound`) and is
untouched; only the message lied. Added: a truthful exhausted-path message, a `settle obs:` line
counting how many quiet ticks moved the **server's own** `shared_state_hash` plus the stable tail,
and an `op ledger:` line with the outstanding `sent − applied` delta. Output only — `kSettleBound`
stays 600, the claim list stays at 29, nothing gates on the new lines.

Both exits were **exercised before merge** (a throwaway `kSettleBound = 5` build, reverted and
rebuilt), because an unexercised branch is precisely what the brick was fixing. The loop rewrite is
behaviour-identical by measurement: 69 settle ticks in the archived pre-change baseline, 69 after.

### #318 — the hole, with its size measured
`DestructionServer::publish` incremented nothing when `send_reliable` refused. Unlike the
replicator's diffs (`server_replicator.cpp:311`) and the gameplay server's deferred announcements
(`gameplay_server.cpp:289`), both of which re-offer next tick, `committed_ops()` is a **per-tick
list with no re-offer** — so a refused packet is a permanently lost damage transition for that
client, and the reliability guarantee never applied because it never entered the channel.

Added `op_packets_refused()`, `ops_refused()` (the ops *inside* those packets) and
`composition_checks_refused()`, plus a test that starves a connected client until the
256-message backlog (`net/reliable_channel.hpp:75`) refuses, asserting the conservation identity
`d(ops_sent) + d(ops_refused) + d(ops_unaddressable) == committed_ops().size()`.

**Mutation-checked**: with the counters neutered the identity reads `4923 == 11520` — **6,597 of
11,520 committed ops, 57 %, were invisible** — and the test fails 71 of 133 assertions. Restored,
it passes 387/387 under ASan. No retry, no queue, no disconnect: the repair policy is still Luca's
call and was deliberately left out.

### #319 — closing a structural blind spot
The two existing `M7.5:` determinism proofs drive `build_multi_island_scene`
(`islands_test.cpp:60`): axis-aligned **cubes and spheres**, whose isotropic inertia keeps
`R·(1/k)I·Rᵀ = (1/k)I` diagonal under every rotation. They cannot express an off-diagonal
divergence at all. #319 adds the suite's first scene that can — 16 tilted, spinning, elongated
boxes in 16 islands — with `islands_solved_parallel` as the sensitivity control (0 sequential,
16 parallel) so it cannot pass vacuously. Green on `main`.

## Brick 2: the Tier2 matrix is PRICED and EXONERATED

Interleaved A-B-A-B, 5 pairs, on a **provably idle box** (the run waited for
`Runner.Worker`/`ninja`/`ctest` to clear for 60 consecutive seconds first — this workstation IS the
self-hosted runner and this session's own PRs were building on it).

| witness | baseline `d47e1bf` | matrix | ratio |
|---|---:|---:|---:|
| velocity phase, min of pair-mins | 11004.3 µs | 5521.9 µs | **1.993×** |
| velocity phase, median of pair-mins | 11033.9 µs | 5522.8 µs | **1.998×** |
| whole solve, min | 12988.8 µs | 7202.8 µs | **1.803×** |

Within-sitting spread 1.1 % (baseline) and 0.29 % (matrix); workload identical in both arms
(448 bodies, 3846 constraints, 1 island); each arm produced exactly **one** distinct hash pair
across 5 repeats, so the instrument reproduces itself. The historical **2.2×** predates #316 and
must not be quoted as current. The same baseline binary read 11436.6 µs last session versus
11004–11130 today — **2.7–3.8 % cross-sitting drift**, which is why only the interleaved ratio counts.

Report: `/home/next/reports/rime-tier2-reprice-2026-10-13/` (REPORT.md, ab-report.json, all 10 raw
runs, `ab_bench.py`). **`ab_bench.py` is the harness last session lacked** — `solver_bench` prints
its payload on a *labelled* `solver_bench_json: ` line even under `--json-only`, so parsing stdout
whole fails every time; it also checks exit status and keeps raw streams, because a parse failure
and a crashed run are indistinguishable from a missing number.

Then validation (`/home/next/reports/rime-tier2-validate-2026-10-13/`): determinism exact across 4
worker counts with the control firing; all of `tests/physics/` passing (176 cases, 134,099
assertions); TSan clean. But 2 of 82 ctest entries failed, both behavioural: the wall pile never
sleeps, and the block's peers never converge.

**#317's new diagnostic resolved that in ONE run** (it took a four-experiment freeze control last
session): `server hash changed on 591 of 600 quiet ticks`, `outstanding 3`, zero dropped, zero
malformed. Delivery was fine; the authority simply never stopped producing damage.

## ~~THE OPEN BUG: a settling pile gains energy, on `main`~~ — SUPERSEDED, see the ledger section at the end

> **This section's conclusion is wrong and is kept only for the measurements in it.** The
> phase-energy ledger ran later the same day and showed the energy is gravitational, not
> injected: no solver phase adds net work on any step. Read
> "RESOLVED: the ledger ran" below before acting on anything here.

A numerical probe (2.8M samples) refuted the suspicion against the matrix: it is symmetric by
construction, positive definite in **every** sample, median error 6e-8 against a double reference —
indistinguishable from the quaternion path, which is itself the slightly non-symmetric one (~4e-7).

The real finding, from perturbing each body's velocity-phase principal frame by **1e-6 rad**
(rounding scale, 6 seeds) on the **unpatched** solver:

| seed | outcome | peak awake KE |
|---|---|---|
| eps=0 control | settles t=530, 0 awake | 5.5e2 |
| 1 | **FAILS** 1500-tick cap, 15 awake | 3.26e4 |
| 3 | **FAILS** 1500-tick cap, 5 awake | 2.44e4 |
| 5 | settles t=1044 | 2.60e4 |
| 6 | settles t=1273 | 2.42e4 |
| 2, 4 | settle t=258 / t=254 | 5.5e2 |

**4 of 6 rounding-scale seeds burst to ~50× the scene's total energy input; 2 of 6 fail the gate.**
Growth is **exponential, ~1.9× per 8 ticks** (KE ~2 → 2e4 between ticks ~860 and ~1008), peak body
speed 4 m/s. The zero-perturbation control is clean and reproduces main's exact 530-tick settle, so
the instrument has a verified zero point.

Consequences:
- **`destructible_wall_headless` is a luck gate.** Its pass on `main` is one draw. It belongs in the
  same category as the 29 `--headless` claims in CLAUDE.md, and had not been identified as such —
  which is how the matrix got condemned on it. **Do not read its red as a defect without a
  perturbation sweep alongside.**
- **The matrix cannot be re-judged until a non-luck gate exists**, and building that gate is part of
  this work.
- **Which subsystem injects the energy is NOT determined.** Nobody has isolated it.

Evidence + the sweep harness: `/home/next/reports/rime-tier2-diagnose-2026-10-13/`
(`probe.cpp`, `probe.log`, `sweep.sh`, `sweep.log`, `temp-diag-instrumentation.diff`).

## Consults on the energy bug

**astra** (`codex exec -m gpt-6-astra`, high, read-only, 28,780 tokens) — every code claim it made
was verified against the source by hand:

1. **Warm-started friction clamped against the current normal impulse** (`solver.hpp:413-422`). When
   the normal impulse shrinks, the clamp forces a large negative correction that can do **positive**
   work: for k=1, v_t=−1, λ_old=2, new bound 0.5 → Δλ=−1.5, ΔK = 1.5 + 1.125 = **+2.625**. Arithmetic
   checked. It correctly calls this a possible local source, not net amplification.
2. **NGS changes the energy metric at fixed ω** — and this is the orchestrator's own error, stated
   plainly: I had cleared NGS because it writes no velocity, conflating velocity with energy.
   `KE_rot = ½ω·I(q)·ω` depends on **orientation**, so rotating the pose at fixed ω changes
   rotational energy for **anisotropic** bodies (`solver.hpp:526-535`), and its translation changes
   gravitational potential. The pile is box chunks, and the perturbation was applied to the principal
   inertia frame — so this explains the anisotropy link that the orchestrator's hypothesis did not.
3. Restitution bias, gated at 1 m/s (`solver.hpp:69`, `:287`).
4. Integrator, lower priority (`world.cpp:1407`).

**The test that should have caught #2 is triple-blind** (astra cited it, verified here):
`tests/physics/solver_test.cpp:374-390` uses two **cubes** (isotropic ⇒ `I(q)` rotation-invariant),
**at rest** (ω=0 ⇒ ½ω·Iω is zero regardless) and with **gravity disabled** (no potential term). It
asserts velocities stay zero, a different property. Same blindness class as #319's fixture — found
twice in one day.

**Disagreement worth keeping:** astra never raised the orchestrator's candidate, the **uncapped**
speculative-contact bias at `solver.hpp:296-298`, where `velocity_bias = penetration/dt` turns a
0.07 m gap into a commanded 4 m/s approach — matching the measured 4 m/s peak. That remains an
unverified coincidence, and astra ranks restitution third. Do not treat it as eliminated or as
established.

**fable** (`claude-fable-5-1` over the devpass gateway, 3rd attempt, 7235 completion tokens,
**$0.896 total across all three**). Its first two calls returned `finish_reason: "length"` having
spent an entire 4000-token budget inside an encrypted `reasoning_details` block without emitting a
word: **Fable is a thinking model over this gateway, so `max_tokens` must cover the thinking AND the
reply or you pay in full for nothing.** At 20000 it answered in 118 s.

1. **Warm start applying stale cached impulses to churned contact points** — its top suspect, and
   **its stated premise is WRONG**: it inferred that "nothing ties a cached scalar to a feature
   identity" because `store_impulses` writes by slot index (`solver.hpp:441-449`). But
   `contact.hpp:33-41,54` has `feature_id`, "a stable hash of the shape features", and the
   persistent manifold cache "matches this frame's points to last frame's **by feature id** and
   carries their accumulated impulses forward". **The fault is the prompt's, not the model's** — only
   `solver.hpp` was inlined, and fable correctly hedged the claim to "nothing in the excerpt".
   Its *measurement* survives the correction and is still worth running: count points whose cached
   impulse is applied with |r_a − r_a_prev| > 5 mm or a flipped tangent basis.
2. **NGS injects POTENTIAL energy** — up to `kNgsMaxCorrection` 0.2 m per point per iteration x 2
   iterations, which is large for wall chunks: a body is lifted, falls, re-penetrates deeper, and is
   lifted further. Toggle `kPositionIterations = 0` (`solver.hpp:48`); the burst persisting refutes it.
3. **Restitution multi-contact over-count** — sharper than astra's version: a body wedged between two
   neighbours gets **two independent separating targets** computed from the same pre-solve state
   (`solver.hpp:283-285`). And since growth begins at KE~2, likely below the 1 m/s gate, restitution
   is more plausibly the **amplifier than the trigger**.
4. It **engaged with the orchestrator's speculative-bias candidate** (`solver.hpp:290-292`) and
   ranked it LOW with a mechanism: a cached positive normal impulse warm-started onto a point whose
   bias flipped to `penetration/dt` can be fully retracted by the clamp to zero. astra ignored that
   candidate entirely. **Taken together the orchestrator concedes it is unlikely to be primary**, and
   the 4 m/s coincidence stays a coincidence.
5. Velocity solve proper is dissipative (PGS with a consistent effective mass); integrator low,
   inconsistent with an onset at KE~2.

### Where the two consults agree, and where they do not

- **Agreement: warm start ranks first in both** — but by *different* mechanisms (astra: the friction
  clamp doing positive work; fable: stale impulses on churned points). Two opinions converging on a
  phase, which is **weak evidence** — not two independent measurements.
- **Disagreement, and astra is right:** fable asserts NGS "cannot inject KE directly" because it
  writes no velocity. That is exactly the orchestrator's own error. `KE_rot = 1/2 w.I(q).w` depends on
  orientation, so rotating an anisotropic body at fixed w *does* change rotational energy. **Two of
  the three analyses made the same mistake; astra's correction stands.**
- **Fable adds what astra lacked: cheap falsification TOGGLES** — zero the cached impulses, set
  `kPositionIterations = 0`, set restitution to 0 — plus the right framing that **exactly one ledger
  term should grow at 1.9x/8 ticks**, and a tight tick window (seed 1, ticks 850-1010).

### astra's decisive experiment (the recommended next brick)
Both consults converge on this, and it should now be run as the **union** of their two designs:
a **phase-energy ledger**, in double precision, on a failing seed, across the onset and growth:
sum translational KE + rotational KE **using the live pose** + **gravitational potential**, sampled
before/after gravity-damping, warm start, velocity solve, pose integration and NGS. Phase
boundaries named at `world.cpp:1212, :1386, :1391, :1406, :1415`. Keep warm-start and velocity-solve
deltas both separate and combined. **The cumulative ledger must account for the growth — a transient
positive spike is not enough.** The gravitational-potential term is astra's addition and was missing
from the orchestrator's own design. Add fable's three toggles as the confirmation step, and its
expectation that **exactly one** term grows at 1.9x/8 ticks — a ledger where several terms drift is
a ledger that has not isolated anything.

## Environment findings recorded this session

- Global `CLAUDE.md`: the devpass gateway now **refuses provider-prefixed model ids** —
  `aws-bedrock/claude-fable-5-1:global` answers **403** *"Direct provider routing is not available on
  coding plans"*. Use the bare `claude-fable-5-1`. And a **non-streamed** `/v1/chat/completions` call
  with a ~28 KB prompt dies at exactly **60 s** with "Remote end closed connection without response"
  — a gateway idle timeout, not the silent-stall hazard. Pass `"stream": true`.
- Session memory: `wall-test-is-a-luck-gate.md` and `solver-bench-json-parsing.md`.
- `code`'s login is expired (read twice, 20 min apart). Luca: its week is at 100 % anyway, so it does
  not matter until the 16 Oct 23:00 reset.
- CI's clang-format job covers **`engine tests` only** — `samples/` is not in it.

## Budget and state

Personal: ~20 % of the 5-hour window (reset 13:00, runs to 18:00), week ~36 %. devpass: 9.64 plan
credits left of 87; premium weekly pool 10.44 with ~2.6 used after the Fable calls, resets
2026-10-14 09:36 UTC; PAYG balance 0.

Worktrees: four of five removed after checking each for untracked files; `agent-aba92cb6c50d66c72`
is deliberately retained (the validation checkout) and its matrix work is archived as a patch
either way. `main` checkout is at `7da85c8`.

## RESOLVED: the ledger ran — nothing injects energy, and the real question changed

Evidence: `/home/next/reports/rime-tier2-ledger-2026-10-13/` (`REPORT.md`,
`phase-energy-ledger.patch`, `analyse.py`, both traces). Instrumented worktree
`.claude/worktrees/energy-ledger`, retained; the patch is gated on `RIME_LEDGER=1` and is **not**
proposed for merge.

Both consults' designs were unioned and run: KE and gravitational potential in double precision,
rotational energy from the **live pose**, sampled around gravity/damping, `warm_start`,
`solve_velocities`, pose integration and NGS, accumulated in per-island slots (race-free by the
island-timing-slot argument) and summed after the join. Failing seed 1 at eps=1e-6 against the
eps=0 control, over the 300 steps containing the scripted charge:

| term (J) | control | failing seed |
|---|---:|---:|
| gravity + damping | +15,305 | +53,628 |
| `warm_start` | +2,986 | +24,775 |
| `solve_velocities` | −18,419 | −78,433 |
| pose integration, KE | +5.2e-5 | −1.6e-2 |
| pose integration, PE | −1,518 | −43,897 |
| NGS, KE | −0.118 | −11.16 |
| NGS, PE | +201 | +974 |
| **steps where the solver's own work is net positive** | **0 / 299** | **0 / 299** |

- The ledger **closes**: `grav + warm + vel + int_ke + ngs_ke` equals the step's ΔKE to a median
  **5.7e-6 J**, with the residual confined to steps where bodies spawn or are removed. Separately,
  gravity's work (+38,016 J) and the potential drop at integration (−37,391 J) agree to 1.6 % over
  the burst, which validates the sign and scale of the PE term without another experiment.
- **The solver is net dissipative on every single step of both runs**, cumulatively −55.7 kJ over
  the burst, with a maximum positive cumulative excursion of exactly zero. Warm start is a positive
  term — and the velocity solve removes more in the same step, every step.
- **The energy is gravitational.** The failing seed admits 3.5× more gravity work because 29× more
  potential is released. The earlier "~50× the scene's energy input" framing was wrong: 5.5e2 was
  the *control's peak*, not an input, and the wall's own potential energy is tens of kJ.
- **astra was right that NGS's rotational-metric term is real** — it measures −11.2 J, and it is
  negative. Fable's "NGS cannot inject KE directly" is false in principle and harmless here.
  Fable's NGS potential-lift loop is also real: +974 J, two orders below gravity's work.
- All four hypotheses both consults ranked (warm start, NGS, restitution, speculative bias) are
  **refuted as the energy source**. Fable's three falsification toggles were deliberately not run:
  they discriminate between candidate injectors, and there is no injector.

**What actually differs** between the control and a failing seed: nothing in the destruction
facts. `parts_died 17`, `islands_detached 1`, `debris_count 18`, `bodies_final 19`, peaks
77/256/18 — identical. Only `settled` (yes/no), `awake_final` (0 vs 15) and `ticks_run` (530 vs
1500) differ. The 1e-6 rad perturbation does not change what breaks; it changes whether the 18
debris chunks come to rest. In the failing tail total KE oscillates between 7e-3 and 2e-1 J across
15 awake bodies, so something re-excites them just above the sleep threshold rather than energy
accumulating. **The open question is a sleep limit cycle.**

One trap for whoever reads these traces: `destructible_wall --headless` replays the scene **5
times** and steps physics **5 times per sample tick**, so a per-world step counter reaches 7500 for
`sim.ticks_run: 1500`, and the world's stack address is reused across replays (the tell is a
`PE → 0` discontinuity every 1500 steps). The "growth at ticks 860–1008" quoted above is a step
index inside one replay, not a sample tick.

## If resumed, in order

1. **Sleep starvation** — renamed, because "limit cycle" claims more than a finite run can show.
   Why do 15 of 18 debris chunks never fall below the sleep thresholds when the solver is provably
   dissipative? Two things to **unlearn first**, both established 2026-10-13:

   - **The stage-5 island rejoin is NOT the mechanism.** This handoff named it as the prime
     candidate. Its reset is guarded by `if (p.asleep[b] != 0)` (`world.cpp:1369`), so an
     already-awake body's timer is never touched there. Reactivation can only contribute if a
     component actually slept and then reconnected, or got an explicit wake. Refuted by reading
     the code — astra found the guard, and I checked it.
   - **"The 18 debris chunks form one island" was an over-read of a PEAK.** `awake_final` 15–16
     cannot coexist with sleeping bodies inside one unchanged 18-body island, because stage 7's
     predicate is all-or-nothing per island. `physics.largest_island_peak: 18` is a peak, not the
     final partition. The tail's island topology is **unmeasured** and is part of this question.

   What stage 7 actually requires (`world.cpp:1501-1534`): every member of the island under both
   thresholds (0.05 m/s, 0.05 rad/s) for `kTimeToSleep` = 0.5 s, and the sample substeps 5× at
   60 Hz, so that is **150 consecutive substeps with all members simultaneously quiet**. One
   violation zeroes that body's timer and vetoes the whole island. More bodies, more chances to
   veto.

   The design to implement, and why, is in `/home/next/reports/rime-sleep-consult-2026-10-13/`
   (`REPORT.md`, astra's raw answer beside it): a **bounded-pose** rest criterion OR-ed with the
   velocity one — per-body anchor pose, 1 mm / 1 mrad envelope held for `kTimeToSleep`, anchor
   replaced wholesale on violation and never crept. Its cheapest falsifiable measurement first: a
   **read-only shadow detector** over one failing tail recording the violating body ids, their
   violation magnitudes, the minimum sleep timer, the **actual island membership**, and the
   pose-envelope durations the proposed predicate would have accumulated. The ledger harness is
   reusable — swap the energy terms for these. One thing astra flagged unprompted that belongs
   here: `world_hash()` hashes poses and velocities and **omits even the existing sleep timers**
   (`world.cpp:1907`), so the cross-worker bit-identity test proves nothing about sleep decisions
   today.
2. **The matrix is now PR #320**, with both sides of the sample question measured rather than
   argued. Velocity phase **1.997×**, whole solve **1.805×**, 5 interleaved pairs on an idle box,
   committed as `docs/perf/2026-10-13-solver-bench-matrix-908b31b.json`. It brings a test of the
   new path with a sensitivity control, and `solver_test.cpp`'s #314 cache test does **not** go
   vacuous after all: `inertia_orientation` remains the materialized tensor's input and remains
   the NGS path's operand. Open question on it, and the owner's call: `destructible_wall_headless`
   and `the_block_headless` are green on `87b02c8` (13/13 with their cook fixtures, measured) and
   **red on the branch**, so merging it reds trunk until item 1 lands.
3. **Wire #317's `op ledger:` to #318's refusal counters.** Both are on `main` now, and the ledger
   currently cannot see a refused send at all. Small.
4. Record the wall-test luck-gate rule in the repo's `CLAUDE.md` beside the 29-claims rule, so the
   next session finds it where it will look.
