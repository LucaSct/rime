# Bounded thermal field verification — 2026-10-18

Implementation: `0f8df6d40ab78291375bfe3a6ed3be59fd96ff40` on
`feat/bounded-thermal-field`. Parent contact PR #338 received a separate Debug
warning fix at `55e5fb57f1eabaa79bd18a1d2bf678d1f3e17396`; this field is based on
that fixed parent. Nothing merged. No enabled frame workload or native benchmark.

| Check | Result |
| --- | --- |
| Full RelWithDebInfo build | exit 0 |
| Fluids C++ tests | 46 cases / 25,061 assertions pass |
| New field tests with ASan/UBSan | 12 cases / 777 assertions pass |
| New field runtime and test TUs with actual flags, Clang syntax | pass |
| Exact field diagnostic, actual GCC and Clang runtime archives | 38 fields / 544 calls / 4,057 operations per compiler |
| C++ allocations interposed during advances | zero, including refusals |
| Partial publication / missing receipt / tick allocation mutants | all rejected |
| Python -O | explicitly refused |
| Full lavapipe CTest | 89/90, exit 8, 336.55 seconds |
| SDK smoke / separate installed field consumer | both pass |
| Pinned formatting / license headers / whitespace | pass |

The field sanitizer binary instruments the field, pair contact and phase runtime,
plus the field tests. The contact test's prior sanitizer pass is not substituted
for the new field pass. The actual new translation units also pass Clang syntax.
The separate installed consumer links public `rime::fluids`, runs 160 ticks and
checks multi-hop melting. Sources and CMake input are retained as text.

The exact diagnostic interposes ordinary, array and aligned C++ allocation
operators only while `advance()` runs. Creation/output allocations are excluded.
The current implementation uses preallocated STL buffers; full diff self-review
also checked for other allocation paths. A deliberate capacity-discarding mutant
causes counted allocations and is rejected.

The 544 calls include 509 accepted calls (including zero-duration no-ops), 30
invalid inputs, 2 source failures, 1 contact failure, 1 receipt budget refusal and
1 work budget refusal. Each accepted positive call's exact journal telescopes
against actual stored energy endpoints; rejected/zero calls retain the previous
publication. There are 1,097 witnesses of source product rounding and 96 of
duration-partition rounding. Retained powers, requested dt, h and N permit exact
accounting through these effects to ideal P*dt, without claiming the primitive
receipt alone contains them.

GCC/Clang input SHA-256:
`be5d2ba14052020927e830f1cf56c2889766af49a35f0277903661d0c23f1973`.
Matching output SHA-256:
`227337ec3af0a27bcdaeb43a0106ed9c9c924af5522c73c67a5b8d80f3e50985`.
`digests.json` identifies committed sources and actual tested binaries. The Clang
runtime archive uses O1; the ordinary GCC archive uses the normal O2 build flags.
No cross-device or network determinism follows from this local comparison.

## Numerical controls

The portable cases cover input canonicalization, malformed descriptions, source
and contact ordering, distinct latent reservoirs at the same temperature, zero
sources/links, disconnected cells, multi-hop freezing/melting, integer ticks,
move semantics and unsupported FP mode. A failure in the fourth substep rolls
back seven earlier operations and preserves the previous journal. A final
contact refusal rolls back successful earlier sources and contacts.

Source-free global extrema are checked over substeps. The three-cell analytic
mode checks first-order temporal convergence and detects node-relabeling order
sensitivity. The insulated cosine rod's N=4/8/16 RMS errors are
0.0312724 / 0.00808035 / 0.00203663 K. Halving the fine timestep changes its error
to 0.00200659 K, about 1.5%. See `spatial-refinement.log` and the
[method/consultation note](../../research/2026-10-18-thermal-field.md).
This is a single-phase spatial witness, not a moving phase-front proof.

## Full-suite and parent CI limitations

Local `the_block_headless` still fails the same three inherited claims: far side
untouched, most block still standing, and damage confined to a few buildings.
The field is not consumed by that sample. No threshold was loosened. Full local
output is retained; no other local test failed.

Parent PR #338's actual Linux log (run `39157706199`, job `117764263470`)
confirms those same claims. Its SDK job `117764263487` instead found our contact
TEST's const `std::pair` range-copy warning in Debug, promoted by Werror. This
was a real parent defect, not dismissed as an inherited physics failure. Commit
`55e5fb5` changes the loop to a const reference; local contact tests (12/1,048)
and Clang Debug syntax with explicit range-loop warning checks pass. The separate
parent CI rerun started as `39161178783`; SDK was queued at the recorded snapshot.
Its editor job `117774649879` still fails the inherited collapse/locality claims.
Filtered actual failures are retained. No final parent/new-PR CI success or
three-OS proof is claimed. Draft hosted jobs may be skipped.

M18, city convergence, clicked editor asset proof, water displacement/transport
and exported multiplayer/authority remain open. No Rust changes or new Cargo run.

## Reproduce

From the configured checkout, use an external scratch directory:

```sh
cmake --build --preset release -j 3
build/release/bin/rime_fluids_tests --no-colors
python3 tests/fluids/verify_thermal_field.py --library build/release/lib/librime_fluids.a --work-dir /path/to/scratch/exact-gcc
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json RIME_REQUIRE_VULKAN=1 ctest --test-dir build/release --output-on-failure -j 2
CMAKE_BUILD_PARALLEL_LEVEL=3 scripts/sdk-smoke.sh --preset release
```

Copy `sanitize.py` and `mutants.py` to scratch before running them; they generate
objects/logs beside themselves. Their checkout path is the one used for this
measurement; adjust it if relocating. The first reconstructs compile-database
flags, checks new TUs, instruments all three thermal runtime objects and field
tests, and builds a separate Clang archive. Point the exact diagnostic at that
archive with `--compiler clang++`. The second changes only scratch source copies
and requires a failing checker with no success JSON. The diagnostic checks process
exit statuses and removes old success metadata before building. Assertions must
remain enabled. Raw bit-pattern input/output and all binaries remain in the
persistent scratch folder rather than being committed.

Root performed the full diff self-review. Astra supplied an independent design
proposal, not a final code review. Its response cost was $0.06254; shared gateway
usage rose $0.10313 during the window, including $0.04059 not attributable from
this response. The conservative task spend records the full $0.10313. Public
prompts/answer/usage are retained; private account snapshots and credentials are
excluded.

Scratch: `/home/next/reports/rime-thermal-field-2026-10-18/`.
