# Handoff — 2026-10-04: the stack, the toolchain blocker, and M19's first two bricks

## The blocker (FOUND, FIXED, MERGED)
`main` and all eight open PRs were red in `format, lint & license` on a file none had touched.
A 2026-10 Rust stable deprecated `AtomicUsize::fetch_update`; CI pins `dtolnay/rust-toolchain@stable`
which FLOATS, and `clippy -- -D warnings` made it fatal in `rime-gateway`.
**#239 merged (857f898)** — `Slot::take` now runs the compare-exchange loop `fetch_update` runs
internally (APIs stable since 1.0, so it cannot be deprecated again). `try_update` was rejected: it
would raise the toolchain floor silently. NOT reproducible locally (this box is rustc 1.98.0, which
predates the deprecation); CI was the proof, and `format, lint & license` went green on it.

## Merged today
- **#236** PLI test flake, **#231** browser keys+mouse (ADR-0054), **#239** the toolchain fix.

## Open, ALL REBASED AND PUSHED, all now need CI re-run on top of 857f898
| PR | branch | note |
|---|---|---|
| #230 | m18/track-H-pair | phone sign-in (ADR-0055) |
| #232 | m18/track-H-page-design | full-window stream + redesign; retargeted to main |
| #233 | m20.1-game-definition | GameDefinition (ADR-0056) |
| #234 | m18.5-page-streaming | page pool/cache (ADR-0057) = ADR-0043 **gate 5** |
| #235 | m18.4-micro-raster | hybrid raster (ADR-0058) = **gate 6**; Luca to confirm the CPU oracle's changed fill rule |
| #237 | m18.6-vg-dag | replacement DAG (ADR-0059); its editor-smoke red was a 49-min timeout, logs expired — re-run on the rebase |
| #238 | m19.1-heightfield | terrain heightfield + collision (**ADR-0060**, renumbered from 0056) |
| #240 | m19.2-terrain-queries | terrain shape_cast/penetration/CCD (**ADR-0061**); stacked on #238 |

**Merge order:** #239 is in. Then land them one at a time, and after EACH squash rebase the rest —
every sibling conflicted the moment #231 landed (ADR-index collisions, and a
`tests/app/CMakeLists.txt` link-set clash between #231 and #233). The `brick-delivery` skill's
`git diff <child-original-tip> HEAD` check only carries CI forward when main's tree equals the old
parent tip's; it did not here, because #236 landed too.

## Review findings worth keeping
- **m19.1's watertightness seam rule is a guard NO test reaches.** Instrumented the branch: the full
  167-case physics suite, including the 3,300-cast "watertight at every edge and vertex" test,
  fires it **zero** times, and deleting it leaves everything green. Stubbing the ordinary per-piece
  root DOES redden it, so the watertightness is real but empirical. Comment corrected; in ADR-0060.
- **m19.1 heap-allocates per terrain pair per step** (`build_heightfield_contacts`). The narrowphase
  loop is serial so persistent `mutable` scratch is race-free — deliberately NOT done, nothing
  measured it, no shipped scene has terrain. Named at the declaration + ADR-0060.
- **m19.2: depenetration must not be per-triangle EPA.** A triangle is zero-thickness, so for a sunk
  shape the shortest separation is often DEEPER INTO THE GROUND (measured: a sphere 0.1 below a flat
  tile separates 0.4 down vs 0.6 up). It reuses the contact build's "straight back up, never
  sideways" rule instead. See ADR-0061 §4.
- **m19.2 known limitation, now gated:** a shape sunk exactly on a grid vertex or cell diagonal gets
  no depenetration (no triangle's interior contains it). Inherited from ADR-0060, depenetration-only.
  A test pins it, so a later fix goes red rather than passing unnoticed.

## Roadmap: `docs/ROADMAP.md` IS A MONTH STALE and misled this session
Its last commit is m15.8. It describes as open two things that are DONE:
- **m17.4** (the frame-in-flight ring for 14 pass-owned buffers) — landed; `clustered.cpp:137`
  says "everything the CPU rewrites per frame moved to the graph's ring" and cites m17.4.
- **m17.7** (the sky lighting the scene) — landed through m17.7d; `samples/99-the-block/main.cpp:1483`
  has the sky's solar source lighting the street and calls `set_ambient` "the sky-less fallback".
**Fixing ROADMAP.md is a real task** — it is what new contributors are pointed at.

## What is actually next
- **ADR-0043 gates 1–6 close when #234/#235/#237 merge.** Only **gate 7** then remains for M18: a
  fresh, clean-tree, clock-pinned **Release** `99-the-block` report meeting the unchanged ratified
  `frame` p99 ≤ 16.6 ms / max ≤ 33 ms. `scripts/perf.sh --sample the-block --commit`, on the
  RTX 3060 — **needs Luca's hardware**, cannot be done unattended.
- M19 continues: terrain **rendering** (the asset is cooked and collidable; nothing draws it yet —
  upload `samples` as R16_UNORM with the same scale/offset so drawn and collided surfaces come from
  identical integers), splat blending, streaming. ADR-0061 also defers the **banded enumeration**
  that would retire m19.2's triangle budget instead of raising it.

## Traps re-confirmed today (both cost real time)
- **A piped build hides its exit status.** `cmake --build ... | tail` reported BUILD_EXIT=0 while
  ninja had failed; the tests then ran green against a stale binary. Twice. Run the build as its own
  command and check `$?`.
- **`git checkout <file>` on uncommitted work destroys it.** Lost the whole m19.2 query
  implementation that way mid-falsification; reconstructed it and verified by matching assertion
  counts. Commit a checkpoint before falsifying.
