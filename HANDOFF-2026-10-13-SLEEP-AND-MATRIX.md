# Handoff — the sleep starvation fix (#321) and the Tier2 matrix (#320)

Written 2026-10-13, late. Supersedes nothing; it continues
[`HANDOFF-2026-10-13-REPLICATION-PROOFS-AND-SOLVER-ENERGY.md`](HANDOFF-2026-10-13-REPLICATION-PROOFS-AND-SOLVER-ENERGY.md),
whose resume item 1 is **done** and whose item 2 is now unblocked.

## State in one table

| PR | branch | what | CI |
|---|---|---|---|
| **#321** | `sleep-pose-rest` | the pose rest route — the sleep starvation fix | opened, running at handoff |
| **#320** | `tier2-matrix` | the materialized world inverse inertia, **1.997×** | 3 jobs red, **and that is a draw, not a defect** |

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

| worktree | contents |
|---|---|
| `.claude/worktrees/sleep-pose` | branch `sleep-pose-rest` — #321 |
| `.claude/worktrees/tier2-matrix` | branch `tier2-matrix` — #320, with `build/release` for the bench |
| `.claude/worktrees/matrix-base` | detached at `87b02c8` + detector + perturbation — the control arm |
| `.claude/worktrees/sleep-detector` | detached at `924be48` + detector + perturbation — the matrix arm |
| `.claude/worktrees/stack-check` | detached, `#321 + #320` — the stack proof; disposable |
| `.claude/worktrees/energy-ledger` | the earlier phase-energy ledger; retained |

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
