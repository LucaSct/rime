# Handoff — the sleep starvation fix (#321) and the Tier2 matrix (#320)

Written 2026-10-13, late. Supersedes nothing; it continues
[`HANDOFF-2026-10-13-REPLICATION-PROOFS-AND-SOLVER-ENERGY.md`](HANDOFF-2026-10-13-REPLICATION-PROOFS-AND-SOLVER-ENERGY.md),
whose resume item 1 is **done** and whose item 2 is now unblocked.

## State in one table

**BOTH MERGED, 2026-10-14.** `c9bbe5f` (#321, the pose rest route) then `37c18d0` (#320, the
materialized world inverse inertia, **1.997×** on the velocity phase). Both landed with **all
eight CI jobs green**, macOS and Windows included. The "to finish" list below is therefore done;
it is kept because the *method* is the part worth reusing.

| PR | merged as | what |
|---|---|---|
| **#321** | `c9bbe5f` | the pose rest route — the sleep starvation fix |
| **#320** | `37c18d0` | the materialized world inverse inertia, **1.997×** velocity phase |

Two things from the landing worth keeping:

- **#320's CI evidence did NOT carry over the rebase, and that was correct.** `main` had gained a
  real change (#321) beneath it, so `main`'s tree was not byte-identical to the old parent tip's
  and the stacked-PR carry-over argument did not apply. What was verified instead is that the
  rebase preserved #320's *own* contribution: the diff-of-diffs between `87b02c8→924be48` and
  `c9bbe5f→d09adc4` is **12 lines, all blob-index and hunk-header line numbers, zero `+`/`-`
  payload**. Then the pushed state was rebuilt and retested (179/179 cases, `ctest` **82/82**)
  rather than trusting the earlier local stack proof.
- **One job came back `cancelled` with `runner_name: ''` and `runner_id: 0`** — assigned to no
  machine at all, after 42 minutes queued. Runner starvation from landing two PRs and four `main`
  pushes onto the one self-hosted runner in quick succession; nothing to diagnose in the code. It
  passed on a rerun once the queue drained (0 in-progress, 0 queued).

**The owner's call, recorded and not to be re-asked: #321 lands first, then #320 rebases onto
`main` and merges green.** Priced against merging red (which spends trunk's CI signal), merging
with the gates quarantined, and parking the branch.

**Verified locally, not argued:** #320's commit cherry-picked on top of #321 gives
`destructible_wall_headless` PASSED (settles tick 179, `awake_final` 0), `the_block_headless`
PASSED 29/29, `rime_physics_tests` 179/179 cases / 134 127 assertions.

## To finish, in order

1. Read #321's CI. If green, merge it (squash, explicit `commit_message` — the repo's
   `COMMIT_MESSAGES` setting concatenates otherwise).
2. `git rebase --onto origin/main 043e0a6 tier2-matrix`, then **`git diff 924be48 HEAD` MUST be
   empty** — that is what carries CI evidence onto the new SHA — then
   `git push --force-with-lease`. #320's base is already `main`.
3. Read #320's CI on the rebased SHA. The three previously-red jobs
   (`destructible_wall_headless`, `the_block_headless`, and the self-hosted **Editor smoke**, whose
   authoring round-trip runs the block demo) should all be green. Merge.
4. Remove the four worktrees below once their PRs show `merged_at`, and `git status --porcelain`
   each one first — the `??` files are where some of this evidence lives.

## What was measured, and the three things it refuted

Everything here is in `/home/next/reports/rime-sleep-detector-2026-10-13/REPORT.md` with the
harness beside it (`sleep-trace.patch` — a `RIME_SLEEP_TRACE=1`-gated **read-only** detector, not
for merge — plus the analysis scripts and three raw traces).

**The detector is proven read-only:** with it enabled the baseline still settles at tick 530 with a
byte-identical work ledger.

- **The bug is sleep STARVATION.** The speed test is sampled once per step and one tick above the
  bound zeroes that body's timer, while the island needs every member under the bound
  simultaneously. Over a 900-tick failing tail the island's *minimum* timer never exceeds
  **0.267 s** of the 0.5 s needed.
- **Refuted: island churn.** 9 membership changes in 400 steps; the longest *stable* run is
  **289** steps, nearly twice the 150 substeps sleep needs.
- **Refuted: stage 5's rejoin reset as the mechanism.** It fired exactly once in 400 steps (a body
  genuinely `asleep=1`, `timer=0.5`, rejoining), and its reset is guarded by `asleep != 0`.
- **Refuted: "the 18 debris chunks form one island".** The tail is **4 islands**, signature
  (1, 1, 1, 15). The old claim read `largest_island_peak`, which is a peak.
- **Refuted: #320's reds as a defect.** Seven rounding-scale draws per build give base and the
  matrix branch an **identical 5-of-7 settle rate**, and tail bursts of 3–9 rad/s appear in both
  (and in both are compatible with settling).
- **Refuted: the consult's tolerance, not its design.** astra proposed a 1 mm / 1 mrad pose
  envelope; measured, no failing draw ever cleared it. 25 mm / 25 mrad —
  `kLinearSleepThreshold × kTimeToSleep`, i.e. exactly the displacement the speed bound already
  permits over the same window — cleared **all four** failing draws, including #320's.

## Still open

1. **What kicks a chunk to 7.6 rad/s every ~30 substeps.** The energy ledger says no phase injects
   energy net, and the sweep says the kicks belong to the scene rather than to any recent change.
   But a settling pile that rattles at 7 rad/s is a physics question, not a sleep question, and it
   is unanswered. The detector traces in the report directory are the place to start (body 10 in
   `matrix-eps0.err`).
2. **`world_hash()` omits the sleep bookkeeping** — both routes, and it already omitted
   `sleep_timer` before this change. So the cross-worker bit-identity test observes the
   *consequences* of a sleep decision, never the decision. Adding it would change a wire-visible
   value, so it is a separate owner call; noted in the code.
3. **No `docs/perf/` run for #321.** `scripts/perf.sh` refuses unpinned GPU clocks and pinning
   needs `sudo nvidia-smi`. The settle-tick figures (530 → 179) are the measurement that matters,
   but a frame-time number for the block is unmeasured.
4. **The 25 mm envelope is derived, not validated against collision resolution or intended object
   scale**, which the consult asked for.
5. Wire #317's `op ledger:` to #318's refusal counters. Small, independent, still untouched.

## Worktrees and reports

All five worktrees this work used are **gone**, reclaiming 4.0 GB (the box was at 83 % of /home),
and both merged branches are deleted locally and on the remote. Nothing was lost: each arm's full
instrumentation is archived as a patch, so the sweep is reproducible in one build.

| to rebuild | apply | onto |
|---|---|---|
| the control arm | `rime-sleep-detector-2026-10-13/arm-base-87b02c8.patch` | `87b02c8` |
| the matrix arm | `rime-sleep-detector-2026-10-13/arm-matrix-924be48.patch` | `924be48` |

Each patch carries the `RIME_SLEEP_TRACE=1` detector **and** the `RIME_PERT_EPS`/`RIME_PERT_SEED`
perturbation, applied to the same quantity in both arms, plus the `RIME_SLEEP_POSE_POS`/`_ANG`
envelope overrides. `.claude/worktrees/energy-ledger` is the one still standing (18 MB), holding
the earlier phase-energy instrumentation.

Reports: `rime-sleep-detector-2026-10-13/` (this one), `rime-sleep-consult-2026-10-13/` (astra's
design and where it was right and wrong), `rime-tier2-ledger-2026-10-13/` (the energy question,
closed), `rime-tier2-matrix-2026-10-13/ab/` (#320's A/B raws),
`rime-tier2-reprice-2026-10-13/`, `rime-tier2-validate-2026-10-13/`.

**One archived verdict is now known to be wrong**, and it is worth saying so plainly because it is
persuasive and it is in the file tree: `rime-tier2-validate-2026-10-13/REPORT-ADDENDUM.md`
concludes *"as patched, this is not shippable"* and blames a velocity/position phase disagreement
over the inverse inertia. The premise is false — the NGS pass has **always** read the live pose
while the velocity phase reads the pre-integration composite (`solver.hpp`, the 4-argument
`apply_inv_inertia`), so the matrix change introduces no disagreement that was not already there;
it changes round-off only. The behaviour it measured was real, and it was the draw.
