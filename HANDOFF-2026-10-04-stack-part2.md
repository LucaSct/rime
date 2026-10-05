# Handoff — 2026-10-04 (evening): five of eight landed, lazy mode at 90% of the window

## Merged today, in this order (each rebased onto the previous, local full-suite verified)
#239 toolchain fix (857f898) → #236, #231 earlier → then:
| PR | squash | note |
|---|---|---|
| #230 | c3975f3 | phone sign-in, ADR-0055 |
| #232 | 725df16 | full-window stream + page redesign |
| #233 | 2d1486e | GameDefinition, ADR-0056 |
| #234 | 70cbd15 | page streaming, ADR-0057 — **ADR-0043 gate 5** |
| #235 | 3b07cc1 | micro-raster, ADR-0058 — **ADR-0043 gate 6** |

**None of the eight originally contained 857f898**, so every one was genuinely red; all eight were
rebased onto it and pushed, and all eight went green on all 8 jobs. That round is the CI evidence.

## Still open — DO THESE NEXT, same recipe
#237 (m18.6-vg-dag, ADR-0059) → #238 (m19.1-heightfield, ADR-0060) → #240 (m19.2, ADR-0061,
stacked on #238, already retargeted to `main`). All three were green on 857f898 before the merges.

Recipe per PR, in its worktree under `/home/next/projects/rime-wt/`:
1. `git fetch`, then `git rebase --onto origin/main 857f898` (for #240: `--onto m19.1-heightfield <old m19.1 tip>`).
2. Conflicts are ALWAYS `docs/adr/README.md` (+ sometimes `docs/glossary.md`). Two shapes:
   - **additive** (two new rows): keep both sides.
   - **the branch's own renumber commit**: keep both sides MINUS the stale low-numbered row for
     this branch's own ADR. A blind keep-both leaves a dead `ADR-00xx` row — it happened twice.
   Then `grep -rn '005[6-9]-<brick>' docs/ engine/ tests/` for stale refs before continuing.
3. `cmake --build --preset dev` as ITS OWN command, check `$?`; full `ctest --preset dev`;
   clang-format `--dry-run --Werror` over CI's `find` set. All three were clean for #233/#234/#235.
4. `git push --force-with-lease`, then merge by REST with an explicit `commit_title`/`commit_message`
   (the repo's squash default concatenates every commit).
5. `mergeable` stays `false` for a while after a push and the API still merges — poll it, but a
   405 "has merge conflicts" right after a force-push is usually just a stale flag.

**Why local verification stood in for a fresh CI round:** each PR was green on 857f898, the only
new content underneath is sibling merges, and a fresh 8-job round per PR is ~40 min of queue (the
runner takes ~3 at a time). main's own runs cancel each other (`cancel-in-progress`), so the local
full suite is what stands behind the intermediate commits. Watch main's run after the LAST merge.

## Three finished bricks sitting LOCAL and unpushed (deliberate — CI queue)
| branch | what | state |
|---|---|---|
| `docs/roadmap-refresh` | ROADMAP.md reconciled with reality (5 commits, docs only) | verified, needs PR |
| `m19.3-terrain-render` | **m19.3: the terrain DRAWS** — R16_UNORM upload, vertex puller, ADR-0062 | verified, on m19.1 → deliver after #238 |
| `m19.1a-seam-guard` | the seam guard is now reachable AND counted (3 commits) | verified, on m19.1 → deliver after #238 |

- **m19.3** (Opus agent + my own re-run): vert's `offset + scale*q` is character-identical to
  `HeightfieldShape::h()`; worst GPU-vs-physics divergence 9.54 µm (NVIDIA) / 7.63 µm (RADV) over 36
  probes against a 1 mm margin; a flipped diagonal is rejected 36/36 by up to 96.8 mm. **Lavapipe was
  never exercised (no ICD on this box) and no sanitizer ran** — CI's Linux legs see it first.
  Its first fixture was nearly vacuous (separable height ⇒ planar cells ⇒ both diagonals identical);
  the fixture was made harder, not the test weaker. Report: `rime-wt/specs/report-B.md`.
- **m19.1a**: a seeded search over ~4M grazing rays found 12 that fire the watertightness guard; 5
  are pinned as hex floats, and a `RIME_PHYSICS_SEAM_COUNTER` hook (test target only, inline
  namespace so the library keeps the plain hot path) makes the test assert the guard FIRED — without
  it a differently-contracting toolchain would pass vacuously. Report: `rime-wt/specs/report-C.md`.
- **ROADMAP**: m17.4 landed in #181, m17.7a–d landed inside #183 (so `git log 3182fe2..` cannot see
  the M17 work at all), m17.7e/m17.9 cut by ADR-0041, M17's frame-rate clause unpaid (p99 28.376 ms
  vs 16.6) and carried into M18. Report: `rime-wt/specs/report-A.md`.
  **Loose end it found: ADR-0040 and ADR-0041 still say `Status: Proposed` although their code landed.**

## Needs Luca
- **ADR-0043 gate 7** — the last M18 clause once #237 lands: a clean-tree, clock-pinned **Release**
  `scripts/perf.sh --sample the-block --commit` on the RTX 3060. Cannot be done unattended.

## Housekeeping done
9 merged worktrees removed + their branches deleted, a dead 3.4 GB `target-pair` dropped:
`rime-wt` 28 GB → 20 GB. New worktrees: `wt-roadmap`, `wt-m19.3`, `wt-seam`. Agent specs live in
`rime-wt/specs/` (NOT /tmp — a reboot ate the last set).

## Global rulebook updated
codex + astra are back (probed live: `ASTRA-OK`), and Luca's instruction is **codex for consulting
only** — `--sandbox read-only`, never `--worktree`, no implementation runs. Entry appended to
`~/.claude-switch/profiles/default/CLAUDE.md`.
