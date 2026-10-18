# Thermal contact verification — 2026-10-18

Implementation: `9d50cb843226b93180b9de98457b27056afe3f09`, on
`feat/thermal-pair-exchange`, based on thermal phase reference
`c1416037b8e59f4583910bbfef08e99712f71908`. This is an unmerged CPU reference.
No new default workload or hardware performance claim is made.

## Results

| Check | Result |
| --- | --- |
| Full RelWithDebInfo build | exit 0 |
| Fluids tests | 34 cases / 24,284 assertions, all pass |
| New contact ASan + UBSan | 12 cases / 1,048 assertions, all pass |
| New runtime and test TUs, Clang syntax | pass with actual compile-database flags |
| Exact Fraction ledger/bracket diagnostic | 1,225 cases per GCC/Clang archive; matching output |
| Deliberately missing second commit | rejected by exact stored-delta check |
| Deliberately discarded second residual | rejected by exact receipt identity |
| Python assertions disabled (`-O`) | explicitly refused |
| Full lavapipe CTest | 89/90, exit 8, 288.20 seconds |
| Installed SDK consumer | pass |
| Separate installed `rime::fluids` contact consumer | pass |
| Pinned clang-format, license headers, whitespace | pass |

The exact diagnostic has 1,042 exchanged pairs, 170 valid unchanged contacts and
13 atomic refusals: 4 invalid inputs, 1 temperature limit, 5 unrepresentable and
3 iteration limits. Of the accepted pairs, 954 have a nonzero exact change in
stored total energy. Both returned rounding residuals balance it exactly. This
is evidence for the six-component ledger, not exact conservation of stored
energy alone. The exact bracket-width check also includes subnormal scales.

Input SHA-256:
`e2f4b227a479ee75792f8537d11cb9e1cfd1423c41f8da8a4018f02d8dea7746`.
GCC and Clang output SHA-256:
`2e735109d82bbf4f2baa0d2fe36126f4642ee0e2aff95b629cb6d2fc2c4b45b6`.
See `exact-gcc.json`, `exact-clang.json`, `digests.json` and `validation.json`.
Source hashes were checked against both the working files and `git show` of the
implementation commit. Binary hashes identify actual tested artifacts; full
binaries remain in the persistent scratch folder, not this repository.

## Inherited red and external gates

`the_block_headless` still fails the same three inherited collapse/locality claims:
far side untouched, most block still standing, damage confined to a few buildings.
The contact object is not consumed by this sample. No assertion or physics
threshold was changed. `full-ctest.log` retains the entire local result.

Parent PR #337's actual ASan and editor logs were read, not inferred from check
names: run `39154495346`, ASan job `117754779529`, editor job `117754779497`.
Both fail those same claims; the editor fails its authoring-round-trip demo
consumer. The relevant excerpts are retained here. Parent format, TSan and SDK
checks passed at the observed snapshot; Linux was still running. Hosted jobs on
drafts were skipped, so this is not a completed three-OS CI proof. The new PR's
CI is separate and starts after publishing this evidence commit.

M18 performance, city convergence, clicked editor asset proof and exported
multiplayer/authority gates remain open. No native GPU benchmark was run.
No Rust code changed; the existing SDK smoke ran, without repeating Cargo tests.

## Reproduce

Use a fresh scratch directory outside the repository. In the configured checkout:

```sh
cmake --build --preset release -j 3
build/release/bin/rime_fluids_tests --no-colors
python3 tests/fluids/verify_thermal_contact.py --library build/release/lib/librime_fluids.a --work-dir /path/to/scratch/exact-gcc
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json RIME_REQUIRE_VULKAN=1 ctest --test-dir build/release --output-on-failure -j 2
CMAKE_BUILD_PARALLEL_LEVEL=3 scripts/sdk-smoke.sh --preset release
```

`sanitize.py` and `mutants.py` preserve the local flag reconstruction and fault
experiments. **Copy these helpers to your scratch folder before running them**;
they write objects and logs beside themselves. Their checkout path is the path
used in this measurement; adjust it if relocating. `sanitize.py` checks both new
TUs with Clang, builds an entirely ASan/UBSan-instrumented contact/phase runtime
plus the contact tests, and builds a separate Clang runtime archive (O1 versus the
ordinary archive's O2). The exact diagnostic can then be pointed at that archive
with `--compiler clang++`. `mutants.py` makes scratch-only runtime changes and
requires nonzero checker exit with no success report. Production code is never
mutated. The diagnostic deletes a previous success JSON before compilation, so
compiler/process failures cannot reuse it. Python must run with assertions on.

The installed-contact consumer was independently configured outside the checkout
against `build/release/sdk-prefix/lib/cmake/rime`, linked only to public
`rime::fluids`, and called the new header/API with a warm/cold pair. This supplements
the repository's existing SDK consumer. Its configure/build/run log is retained.

The full suite started before the final subnormal-tolerance tightening; the final
full build and explicit fluids run passed after it, and the suite's fluids test
also completed successfully. Sanitizer, Clang archive, exact checker and fault
controls were regenerated after that tightening. No unrelated full-suite rerun
was used to reinterpret the unchanged block failure.

## Consultation and review

Tool-free independent proposals ran on `claude-fable-5-1` and `gpt-6-astra` over
devpass, after live price/credit checks, before implementation. The observed
spend was $0.32219. Prompts, answers and usage metadata are included; account
balances and credentials are excluded. The [research note](../../research/2026-10-18-thermal-contact.md)
describes adopted and rejected advice. Neither consultant reviewed the final
diff; the root agent performed the full diff self-review.

Persistent scratch:
`/home/next/reports/rime-thermal-exchange-2026-10-18/`.
