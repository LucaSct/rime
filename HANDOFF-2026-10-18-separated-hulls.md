<!-- SPDX-License-Identifier: Apache-2.0 -->
# Rime continuation — 2026-10-18 separated hulls

Read the October 17 environment-preview handoff for integration scope. This
continuation implements its first collision brick on `fix/gjk-flat-tetrahedron`,
based on `integration/environment-preview` at `e559cb5`. The integration branch
and drafts #331/#332 remain unmerged.

## Finding and repair

Both retained first-pair hull snapshots reproduce the zero-depth ghost contact.
GJK falsely classifies a nearly coplanar four-point simplex as containing the
origin. The old absolute `1e-9` volume threshold accepts float cancellation
residue at `1.49e-8`/`1.86e-8`; the noisy face signs then certify containment.
EPA's zero clamp and clipping fallback turn that false overlap into one contact.
The snapshots' world-AABB gaps are 18.45 mm and 5.41 mm, and their simplexes
lie strictly on one side of a coordinate plane, so overlap is geometrically
impossible. This is confirmed on these two snapshots, not every past ghost.

`engine/physics/src/gjk.hpp` adds an edge-scale term using the existing
`kDegenerateRel` arithmetic budget. Numerically flat tetrahedra search all faces;
the existing double stall polish remains available. No collision/touch margin,
EPA posture, broadphase filter, physics authority or GPU path was changed.
Contacts/world hashes can intentionally change. The unsafe filter stays reverted.

## Evidence

See `docs/perf/gjk-flat-tetrahedron-2026-10-18/README.md`, its reproducible probe,
logs and source/binary hashes, and the updated GJK math note/roadmap pointer.

- Three regression cases, 690 assertions, pass GCC, Clang and standalone
  ASan/UBSan. Original-header sensitivity control fails 76 assertions in two
  cases; genuine enclosure still passes. Three scales, both pair orders,
  all 24 simplex vertex orders, coincident-centre and thin-enclosure controls.
- Full optimized release-preset build passes; CTest **88/89**, 330.24 s.
  The sole red test is `the_block_headless`: three collapse/locality thresholds.
  Its peers converge after 263 quiet ticks; all 5,051 operations are applied.
  Do not retune the chaotic gates or treat this as a city convergence proof.
- Exact pinned format, SPDX, diff checks and new-TU Clang checks pass.
- Standard hardware attempt fails: frame p99 17.212 >16.600 ms, sim.block p99
  8.981 >6.000 ms; driver mismatch refuses comparison, 52.7% GPU-clock spread
  refuses filing. CI activity was observed during the attempt, so its CPU
  measurements are also unqualified. No override, baseline or speedup claim.
- Root self-review ran. No new agent or premium consultation/spend.

## Next work and delivery limits

1. Establish a frozen-contact workload and an unchanged-contact/hash oracle
   against this corrected baseline before trying another conservative filter.
   Do not compare chaotic old/new trajectories for speed. Other ghost-contact
   poses may still need capture; these two fixes are not a universal oracle.
2. Keep #331's burst replication divergence and the qualified hardware budget
   open. Neither is explained by this fixture.
3. Parent preview CI run `39066629972` has an Editor-smoke failure whose step is
   incomplete and log returns 404; cause unverified. Linux build/test, format,
   SDK and TSan passed; ASan/export were unfinished when checked. Recheck exact
   candidate SHA; parent checks do not cover this repair. Keep the leaf draft.

Global rules are at `/home/next/.claude-switch/profiles/default/CLAUDE.md`, not
bare `~/.claude`. This is a shared CI desktop: no more benchmarks while its
tests/builds are active. Leave at least 15 GB free (22 GB observed). All existing
untracked user notes and other worktrees were preserved. Local working logs are
under `/home/next/reports/rime-separated-hulls-2026-10-18/`.
