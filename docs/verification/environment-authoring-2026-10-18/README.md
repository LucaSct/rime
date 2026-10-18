# Environment source authoring verification, 2026-10-18

Implementation: `a1f26763ac980753e0798f887f5ef14befe0b5e8`, parent `f9371de` (`perf/plain-contact-bounds`).
The exact source and host/client binary digests are in [provenance.json](provenance.json).

The reusable adapter and editor host build in Release/RelWithDebInfo. Full
software-Vulkan CTest completed **89/90** in 382.23 s (exit 8). The sole failure is
`the_block_headless`: the same three inherited locality/collapse claims. It still
applies all 5,051 damage ops and peers agree after 263 quiet ticks. No thresholds,
physics implementation or preexisting assertions changed in this brick.

* New adapter suite: six cases, **442 assertions**; also passes ASan/UBSan. The
  sanitizer pass instruments the new adapter/test translation units and the
  underlying VFX dust/plume/fire files, linked to ordinary optimized archives for
  the other modules. This is not a whole-engine sanitizer rebuild or a new TSan run.
* Four changed C++ translation units pass Clang syntax checks using actual compile
  database flags (adapter, tests, editor host, scene renderer).
* Transient light software-Vulkan proof: seven assertions; visible illumination,
  exact return to the disabled image with jitter/TAA off, copy ownership, invalid
  rows and cap accounting. The complete render suite also passes in CTest.
* Rust editor GUI-feature suite: **120/120**. Workspace all-target Clippy and editor
  GUI-feature Clippy pass with `-D warnings`. Pinned C++ format, Rust format, SPDX
  headers and diff whitespace checks pass.
* Installed SDK consumer configures, links and runs three headless ticks and a
  cooked-mesh load. New module target/header export succeeds with the package.
* Live Rust client against `rime-engine`: schema/default/edit/inverse/redo checks
  for both source types, engine-owned save of five entities, then fresh-process
  reload with **14 components, zero skipped**.
* Live viewport: creates a NEW source entity, adds/edits both components, awaits
  each of twelve Steps, then Stop. Host ledger: **12 ticks, 12 lit ticks, eight
  particles spawned, all eight cleared**, zero invalid/source-cap/emission-budget/
  pool drops. It also passes the existing frame decode, pick and lens/gizmo checks.

Representative commands (from repository root):

```sh
cmake --build build/release -j 4
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json RIME_REQUIRE_VULKAN=1 ctest --test-dir build/release --output-on-failure -j 2
tools/target/debug/editor --smoke --effect-sources --engine build/release/bin/rime-engine --save /path/to/authored-effects.rscene
tools/target/debug/editor --smoke --engine build/release/bin/rime-engine --scene /path/to/authored-effects.rscene
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json tools/target/debug/editor --smoke --effect-sources --frames 3 --engine build/release/bin/rime-engine
CMAKE_BUILD_PARALLEL_LEVEL=3 scripts/sdk-smoke.sh --preset release
```

This proves the scripted authoring/wire and opt-in presentation path. It does not
claim a clicked environmental GUI proof, an exported multiplayer environmental
scene, qualified native hardware capacity, or completed smoke/fire simulation.
No native performance run was made for this brick; M18 remains open. Water still
uses its CPU basin without two-way displacement or visible evolving surface; ice
is a newly prioritized energy/phase plan, not implemented freezing/melting.

Review: complete diff self-review, including moving-fire fractional debt,
new-entity transform derivation, no persisted derived lights and initial protocol
message ordering. No independent premium consultation or paid model spend.
