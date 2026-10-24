<!-- SPDX-License-Identifier: Apache-2.0 -->
# Rime continuation — 2026-10-18 contact scaling

Resume here after the October 17 environment-preview and October 18 separated-
hull handoffs. The user requested continued coding. Two further bricks are
implemented, measured and kept as a draft stack; no merge was performed.

## Branches and implementation

- `integration/environment-preview` remains `e559cb5`, unmerged.
- `fix/gjk-flat-tetrahedron`, PR #333, remains `35435d2`.
- `test/contact-workload-oracle`: `e5b6e3e`, based on #333. New reusable CPU-only
  `contact_bench`, lossless runtime-input recorder, defensive corpus loader,
  exact/speculative bit oracle and guarded pinned-CPU ABBA measurement.
- `perf/plain-contact-bounds`: implementation `498b260` plus evidence/docs,
  based on the oracle. This is the current checkout. Verify current origin heads
  and actual PR check SHAs before relying on delivery state.

`contact_bounds.hpp` caches tight/padded bounds once per body slot **per contact
build**, resetting validity even on inspection/empty calls. It uses the existing
1 mm compound margin plus an outward ULP, retains unsupported/unresolved shapes,
and never skips the speculative CCD fallback. No new runtime toggle, changed
solver order, dropped bodies, GPU path or replication shortcut was introduced.
The reference-only compiler macro is `RIME_PHYSICS_UNFILTERED_CONTACTS`.

Proposed decision: ADR-0089. New profile fields count eligible tests, rejects,
posed bodies and exclusive bound time. The external collector includes them.

## Evidence and results

`docs/perf/contact-oracle-2026-10-18/` contains the full compressed frozen corpus:
900 total world contact builds, 44 selected builds (stride 20), 3,318,610 queries,
166,374 selected/written rows, 302 hull dictionary entries, zero drops. Exact
reference replay matches all 48,879 contacts. Tight bounds still discard 11
reference contacts: do not revive that shortcut. The fixed margin with outward
rounding skips 58,008 calls with zero contact differences. A one-bit normal
mutation is detected and refuses timing; malformed/truncated/capped corpora fail.

`docs/perf/plain-contact-bounds-2026-10-18/` contains the final runtime evidence:

- **900/900 world hashes unchanged** against the unfiltered final-source build;
  that reference also matches the oracle parent's 900 hashes. All startup and
  warmup steps are included. Recording itself also matched 900/900 on/off.
- 3,365,643 → 2,205,878 exact calls, **1,159,765 removed**; all 992,631 measured
  manifolds retained. Bound builds 516,931 over 720 measured world steps.
- Controlled frozen CPU ABBA, pinned CPU 0, 21 repeats/arm: reference p50
  110.989/111.267 ms, cached 87.664/87.865 ms — **about 21% less corpus CPU time**.
  Quiet and clock guards passed with identical input/work/contact identities.
  Includes the actual cache; not an FPS claim. Earlier per-pair bounds saved ~6%.
- New runtime cases pass 964 assertions. Seven related cases, with the modified
  world object instrumented, pass 1,175 assertions under ASan/UBSan and Clang TSan.
  Unchanged dependencies use the ordinary build; not a full-engine sanitizer claim.
- Full optimized build passes; full CTest **88/89**, 317.77 s. Same three block
  locality/collapse claims fail; all physics/wall/network/viewmodel/effects/
  environment tests pass. No assertion or tick window was changed.
- Final Clang unit checks, exact pinned engine/tests format, SPDX and diff checks
  pass. Root self-review ran; no new independent consultation or premium spend.

Standard clean-`498b260` 1080p/600-frame hardware attempt **FAILED**. Observed frame
p99 ~16.25 ms, sim.block p99 **8.074 >6.000 ms**. Driver mismatch (615.78.08 vs
615.71.09) refuses comparison; 40.8% GPU-clock spread refuses filing. No override
or rebaseline. The nested report is explicitly unqualified. An earlier CPU
attempt refused busy-host preflight; a later quiet window allowed the valid ABBA.
Ollama had no model loaded. Do not compare unqualified native numbers as a speedup.

## Open work

1. Simulation still misses 6 ms. Use the frozen oracle to profile/optimize the
   remaining plain exact work and solver cost without changing the workload.
   Keep any contact/hash discrepancy fatal before timing an optimization.
2. City burst replication divergence persists unchanged: 145 mismatched frames,
   33 in the final 60. The deterministic per-tick cap is still a workaround.
3. Qualified hardware clocks/current-driver comparison and fresh exact-head CI
   remain delivery gates. #333's Linux/ASan fail only the same block thresholds;
   its Editor job fails them in the authoring round trip. Logs now available and
   inspected; earlier missing-log diagnosis is superseded. Format/SDK/TSan pass
   on that parent, not this new stack. Hosted jobs skipped on drafts are not coverage.
4. Keep #331/#332/preview and this stack unmerged. Retain the separate smoke,
   water, fire, animation and art limitations from the October 17 handoff.

Global rules: `/home/next/.claude-switch/profiles/default/CLAUDE.md` (bare `.claude`
does not exist). Shared workstation: never overlap benchmarks with builds/tests/
other benchmarks or demo workloads; stop only our own measurement if contention
appears. The guarded runner now also checks aggregate foreign CPU use. Keep the
15 GB disk reserve (about 20 GB observed). Existing user notes and worktrees
were preserved; no agent was spawned. Local scratch is in
`/home/next/reports/rime-contact-oracle-2026-10-18/` and
`/home/next/reports/rime-plain-bounds-2026-10-18/`.
