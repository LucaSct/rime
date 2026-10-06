# Handoff — 2026-10-05: eleven PRs landed, M19 draws, and M18 is one gate from done

`main` is at **453866b** and green on all eight CI jobs. Nothing is open.

## Landed today (squash, in merge order)
| PR | brick | squash SHA |
|---|---|---|
| #230 | m18/track-H-pair — phone sign-in (ADR-0055) | c3975f3 |
| #232 | m18/track-H-page-design — full-window stream + redesign | 725df16 |
| #233 | m20.1 — engine-owned `GameDefinition` (ADR-0056) | 2d1486e |
| #234 | m18.5 — bounded page streaming (ADR-0057) = ADR-0043 **gate 5** | 70cbd15 |
| #235 | m18.4 — hybrid software raster (ADR-0058) = **gate 6** | 3b07cc1 |
| #237 | m18.6 — replacement-DAG cook (ADR-0059) = **gate 3's cooker half** | d5f2e0c |
| #238 | m19.1 — terrain heightfield + collision (ADR-0060) | cf8376d |
| #240 | m19.2 — terrain convex queries + speculative CCD (ADR-0061) | 3c760b0 |
| #243 | m19.3 — the terrain **draws** (ADR-0062) | 31cc618 |
| #241 | m19.1a — the seam rule proven by its own counterfactual | bca66d6 |
| #242 | docs — the ROADMAP tells the truth again | 453866b |

**#239 (857f898) was the unblocker**: a floating `dtolnay/rust-toolchain@stable` deprecated
`AtomicUsize::fetch_update`, `clippy -- -D warnings` made it fatal, and all eight PRs were red on a
file none of them touched. Rebasing the stack onto it turned them all green.

## Where the milestones stand
- **M18: gates 1–6 are closed.** Only **gate 7** remains — a fresh, clean-tree, clock-pinned
  **Release** `99-the-block` report meeting the unchanged ratified `frame` p99 ≤ 16.6 ms /
  max ≤ 33 ms, via `scripts/perf.sh --sample the-block --commit`. **It needs Luca's RTX 3060** and
  cannot be produced unattended. The only clean-tree number on file is still M17's, 28.376 ms
  (`docs/perf/2026-09-22-99-the-block-nvidia-geforce-rtx-3060.json:19`).
- **M19: cooked, collidable and drawn.** m19.1/m19.2/m19.3 are in. **Splat-material blending and
  terrain streaming are the remaining scope** — those are the next bricks.
- **M20:** m20.1's `GameDefinition` landed; the rest of the mode/export work is untouched.
- `docs/ROADMAP.md` is now accurate as of 2026-10-05 and carries an M18 gate table; it is the map
  again rather than a trap.

## Needs Luca
1. **ADR-0043 gate 7**, above. It is the last thing between M18 and done.
2. **ADR-0040 and ADR-0041 still say `Status: Proposed`** for code that has landed. ADRs are
   append-only, so this wants a deliberate status edit.

## Two things CI taught, worth keeping
- **The seam proof's first version was a statement about one compiler.** It asserted the guard fired
  at least once per ray (`fired >= 5`) and went red on **macOS alone with 3 of 5**: on arm64 the
  ordinary per-piece root catches two of the five grazing rays that x86-64/GCC misses. How many rays
  land in the seam depends on the target's rounding and FMA contraction. The fix was not to relax
  the number but to run the **counterfactual** — a test-only switch turns the seam rule off and
  recasts the identical rays, so the test proves the firing is *load-bearing*, not merely that it
  happened (`fired >= 1`, plus "with the rule off at least one misses", plus a re-cast with it
  restored so the global flag cannot leak into later cases). ADR-0060 carries a dated addendum.
- **The PLI flake is still live on Windows after #236.**
  `transport::tests::a_pli_from_the_peer_becomes_a_keyframe_request_event` panicked at
  `rime-gateway/src/transport.rs:982` ("PLI did not reach the transport event seam") on a PR whose
  diff was physics-only; a re-run of the identical commit went green. So #236's fix was partial, and
  this test will keep costing re-runs until the seam is made deterministic. Same discipline as the
  floating-toolchain trap: **read the failing step before believing the diff caused it.**

## Verification policy used for the sibling merges — stated openly
The `brick-delivery` skill's `git diff <child-original-tip> HEAD` tree-identity shortcut carries
green CI forward **only in a true stack**, where `main`'s tree after the squash equals the old
parent tip's. These were siblings, so after each rebase the tree genuinely changed and the shortcut
did not apply. Rather than wait ~40 min of queue per PR — and `main`'s intermediate runs cancel each
other anyway under `concurrency: ci-${{ github.ref }}` — each rebased branch ran **CI's own gates
locally**: the build as its own command with `$?`, full `ctest --preset dev` (79/79, 80/80 once
m19.3's target existed), `clang-format --dry-run --Werror` over CI's exact `find` set, and for
Rust-touching branches `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings` and
`cargo test`. Every PR had also been green on all eight jobs at 857f898 before its rebase, and
`main` has since gone 8/8 green on `3c760b0`, `31cc618` and `453866b`. The two PRs opened fresh
today (#241, #243) took full CI rather than the local substitute.

Conflict shapes seen, for the next stack: ADR-index collisions where the branch's own renumber
commit leaves a **stale low-numbered row** (drop the branch's row, keep the incoming renumbered one,
then grep for stale refs), and two independent test cases appended at the same spot in one file
(keep both, and mind that the closing brace is shared across the conflict region).

## Housekeeping done
Eleven merged worktrees removed, `/home` 93% → 89%; only the main checkout remains. Local branch
refs kept — a ref costs nothing, a directory costs gigabytes. The merged PRs' remote head branches
were **not** deleted; that is outward-facing and left for Luca.

## Routing note (global rulebook, updated today)
There are **two independent Claude subscriptions** via `claude-switch`: `code`
(lucaschuttler@gmail.com) and `personal` (ls@peekstar.eu) — separate budgets, not a fallback.
`personal` was re-logged-in at 11:40 and its week reset at 12:00, so it is the place to put
delegated coding (Opus 5.5 for design-bearing work, Sonnet 5 for bounded bricks) while the
orchestrating session spends `code`. This session's `code` window ended the day at ~94% used
(resets 14:59); the week at 56%. An unreadable usage file on a profile means **expired credentials**,
not an exhausted budget — read the message before concluding a profile has no room.
