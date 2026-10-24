<!-- SPDX-License-Identifier: Apache-2.0 -->
# Separated hulls: GJK containment repair

This follows the October 17 handoff's next collision brick. It corrects two
captured false contacts; it introduces no AABB filter, GPU path or speedup claim.
The rejected filter remains reverted. The M18 and city delivery gates remain open.

## Measured cause

The exact two first-pair snapshots from
`../environment-preview-2026-10-17/ghost-contact-hulls.json` reproduce on the
unchanged `e559cb5` private narrowphase. Both report overlap, a four-vertex
terminal simplex, valid EPA with zero depth, and one manifold point despite
positive world-AABB separation. The client simplex has all Y coordinates
negative; the server simplex has all Z coordinates positive. Neither convex
hull can contain the origin.

| snapshot | AABB separating gap | float triple product | product of edge lengths | double simplex distance |
|---|---:|---:|---:|---:|
| client | 0.0184495449 m | 1.49011612e-8 m³ | 2.25332379 m³ | 0.051349609 m |
| server | 0.00540733337 m | 1.86264515e-8 m³ | 103.921951 m³ | 0.0117478714 m |

The old absolute `kTinyVol = 1e-9` test accepts cancellation residue as solid
volume. The subsequent float face-side tests falsely certify containment. EPA's
zero-depth clamp and the clipping fallback then turn the false overlap into a
contact; changing that fallback alone would leave GJK wrong.

The repair scales the volume guard by the product of edge lengths, using the
existing `kDegenerateRel` arithmetic budget. A numerically flat tetrahedron searches
all faces instead of asserting containment. The existing double stall polish
remains the fallback if that search cannot make progress. Geometric touch,
speculation, clipping and broadphase tolerances are unchanged. See the derivation
in [the math note](../../math/gjk-epa.md#nearly-flat-tetrahedra-2026-10-18).

The corrected calls report no overlap and no manifold, with GJK distances
0.0513496 m and 0.0117479 m. The saved probe logs show the original and corrected
results. These explain the two retained first-pair snapshots, not every historical
contact, the angular-burst investigation or the city replication divergence.

The standalone probe needs only the repository headers:

```sh
c++ -std=c++20 -O2 -I engine/physics/src -I engine/physics/include \
  -I engine/core/include docs/perf/gjk-flat-tetrahedron-2026-10-18/probe.cpp \
  -o /home/next/reports/rime-separated-hulls-2026-10-18/probe
/home/next/reports/rime-separated-hulls-2026-10-18/probe
```

## Correctness and sensitivity

`tests/physics/separated_hull_test.cpp` embeds the exact runtime geometry. A
private-seam test is necessary to keep the captured vertex bits: registration
re-centres geometry and can mask this arithmetic defect. It exercises three
scales, both pair orders, coincident-centre overlap controls, all 24 vertex orders
of both terminal simplexes, and solid/thin enclosing tetrahedra.

The original header fails 76 of 690 assertions in two of the three new cases;
the genuine-enclosure case still passes. The corrected header passes all 690
assertions in all three cases with GCC, Clang, and a standalone ASan/UBSan build.
The baseline failure log is compressed alongside the passing compiler/sanitizer
logs. The source and input hashes are in `provenance.json`.

Reproduce the integrated regression proof after a successful build:

```sh
cmake --build build/release --target rime_physics_tests -j4
build/release/bin/rime_physics_tests \
  --test-case='captured collapse*,nearly flat separated*,GJK retains*'
```

For the negative control, copy the private physics headers into a persistent
scratch directory, replace only `gjk.hpp` with `git show e559cb5:engine/physics/src/gjk.hpp`,
and compile the new test standalone with that directory first on the include
path and `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`. Use the actual compile database
entry for the test's remaining flags. This avoids mutating the working tree or
measuring stale binaries.

## Delivery limits

This changes contact semantics and therefore can change the chaotic sample's
world hashes and destruction workload. An old/new collapse timing ratio would
not establish an optimization. A future conservative filter still needs its own
unchanged-contact/hash oracle against this corrected baseline, followed by a
fixed-workload timing comparison. No previously rejected timing is rehabilitated.

The release-preset full build passed. Full software-Vulkan CTest finished
**88/89** in 330.24 s. Physics, destruction/network, wall, environment, effects
and viewmodel checks passed. `the_block_headless` failed three collapse/locality
thresholds: far street untouched, most standing, and damage local to a few
buildings. It converged after 263 quiet ticks, with all 5,051 sent operations
applied and composition checks matching. This does not prove the separate city
burst-convergence gate. The failing thresholds are the chaotic gates documented
in `CLAUDE.md`; they are neither waived nor retuned here. The compressed full log
retains the failures. Pinned format, SPDX and diff checks passed; the new test
translation unit and probe also passed Clang syntax checks. Root reviewed every
hunk; no new independent model consultation ran.

The standard 1080p/600-frame hardware attempt **failed** (exit 1). It measured
frame p99 17.212 ms against 16.600 and sim.block p99 8.981 ms against 6.000.
Comparison was refused because the baseline uses NVIDIA 615.71.09 while this
machine uses 615.78.08. Graphics clocks spanned 967–1935 MHz (52.7% spread), so
automatic filing was refused. No override was used. Source stamp `e559cb5-dirty`
is disambiguated by the source and binary hashes in `provenance.json`; the new
untracked regression file did not affect runtime code. The release preset is
`RelWithDebInfo`/`-O2`, as configured in `CMakePresets.json`.

CI tests were observed active during the attempt even though the initial quiet
check admitted it. Thus CPU contention is an additional limitation, regardless
of the script's clock refusal. The retained `unqualified-1080p-e559cb5-dirty.json`
is diagnostic evidence inside this directory, not a qualified baseline. It
cannot establish a regression or a speedup; no further benchmark was attempted
while CI remained active. Ollama had no loaded model at the preflight check.

The parent preview's CI run `39066629972` has successful Linux build/test,
format, SDK-consumer and TSan jobs; Editor smoke reports failure with its active
step incomplete and its log blob returning 404. Its cause is unverified, and
ASan/export were still unfinished when checked. Those jobs cover `e559cb5`, not
this repair. Fresh candidate CI and the outstanding performance/city gates still
block delivery; this is an isolated draft candidate, not a merge-ready preview.
