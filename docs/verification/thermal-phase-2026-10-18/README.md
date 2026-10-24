# Thermal phase reference verification, 2026-10-18

Implementation `d15783dd6d9b005b1df0277758e0c69eb4b2a4cd`, based on environment-authoring `ab201bd32cd4bd86cfe03600922d5c2012002602` (draft #336).
Source/binary hashes and the scope of each proof are in [provenance.json](provenance.json).

* Complete optimized build succeeds. Full software-Vulkan CTest: **89/90**,
  **356.03 s**, exit 8. The sole failure is the inherited `the_block_headless`
  three locality/collapse claims; no physics or preexisting threshold changed.
* Phase reference ASan/UBSan: **11 cases / 5,701 assertions**, all pass. Both the
  new runtime translation unit and the standalone phase test translation unit
  are instrumented; this is not a whole-engine sanitizer rebuild. Both new C++
  TUs also pass Clang syntax checks using real compile-database flags.
* The actual GCC-built library passes **2,838** exact-fraction probe transactions:
  **1,325 accepted with exact energy balance**, **1,075 heat refusals with unchanged
  energy**, **438 correctly refused initializations**. The inputs include the
  literal lower temperature bound and `Tm-20`, which differ by one binary64 bit,
  very small/large masses, phase endpoints, sub-ulp heat and rounded range overshoot.
* An independently Clang-compiled runtime object passes the same corpus with
  **bit-identical probe output on this machine**. Both output digests are
  `be273948341566f29856d3a6c9f2b9f533acc8258625eddc7e0bad7631ed2726`.
  This does not claim cross-device or network determinism.
* Two temporary weakened runtime copies are rejected by the final exact checker:
  remove the exact boundary residual guard, or allow an interior energy to decode
  to zero liquid fraction. Their patches, source/checker hashes and refusal logs
  are in [mutants](mutants/results.json). Production code was never switched to
  those variants. The earlier prototype's two failing numerical controls are
  retained as historical evidence too; the current mutation proof is reproducible.
* The Python verifier refuses `-O`/`PYTHONOPTIMIZE`, removes a stale success report
  before running and writes a new report only after all checks pass. Compile and
  process exit statuses are checked; a stale executable cannot provide the result.
* Installed SDK consumer configures, links, ticks and loads a cooked mesh. Pinned
  clang-format, SPDX headers and diff-whitespace checks pass. No Rust code changed.

Independent proposals: **Fable 5.1** and **GPT-6 Astra** over the devpass gateway,
without tools or repository access. Both returned HTTP 200 / finish `stop`.
Fable cost $0.1102; Astra cost $0.0863; observed plan spend agrees at **$0.1965**.
The [research note](../../research/2026-10-18-water-ice-phase.md) records useful
findings and explicit disagreements. No runtime rational framework was adopted;
exact fractions are a verification dependency only. Credential/credit responses
are not retained in the repository.

Parent #336 CI was inspected rather than inferred: Linux and ASan fail the same
three block claims; editor authoring round-trip fails the same claims in its demo
consumer. Format, TSan and SDK checks pass. Filtered parent-job failure logs are
retained; they exclude echoed shell commands that merely contain `::error`.

Representative commands:

```sh
cmake --build build/release -j 3
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json RIME_REQUIRE_VULKAN=1 ctest --test-dir build/release --output-on-failure -j 2
build/release/bin/rime_fluids_tests --test-case='thermal reference:*' --no-colors
python3 tests/fluids/verify_phase_change.py --library build/release/lib/librime_fluids.a --work-dir /path/to/scratch --compiler clang++
CMAKE_BUILD_PARALLEL_LEVEL=3 scripts/sdk-smoke.sh --preset release
```

No spatial heat exchange, flowing-water phase coupling, expansion, visible ice,
editor thermal controls or authority integration is claimed. No native benchmark
was run for this unconsumed CPU reference. M18 and the inherited CI gates remain
open. Nothing merged.
