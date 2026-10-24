<!-- SPDX-License-Identifier: Apache-2.0 -->
# Rime handoff — 2026-10-17 environment preview

The owner requested a wrap and handoff. Resume from this file rather than the
October 15 handoff. No new feature work should be inferred from unfinished gates.

## Starting point and owner intent

Root checkout: `/home/next/projects/rime`, branch `integration/environment-preview`.
The implementation is an integration preview joining the city/player work and
the environment seams. It is not a claim that all milestones are complete.

The owner wants: believable visible reloads, sniper focus retained while RMB is
held (bolt/recoil after release), slower believable shooting/case ejection, richer
city art, and collapse performance that scales to more parts. They also requested
separate milestones for general smoke, destruction smoke, water/buoyancy, and
fire light/smoke, with reusable seams and research including actual public code.
Most recent request: continue, finish up and write this handoff.

## Rules and machine

- Global rulebook: `/home/next/.claude-switch/profiles/default/CLAUDE.md`.
  Bare `~/.claude` deliberately does not exist. Read repo instructions too.
- User authorizes cheaper coding delegates; bounded work used `gpt-6-sol` medium
  in isolated worktrees, no nested agents. Root reviewed and tested their edits.
  Current-turn worktrees were removed after their clean commits were integrated;
  branch refs remain. Other sessions' worktrees and untracked notes were preserved.
- Hard questions may use read-only Astra/Fable consultations. This task's Astra
  review is done, via subscription-authenticated Codex CLI. No paid devpass call.
  User ceiling is $7/task, about $10/week premium; check live pricing/availability
  before any new paid request, never assume an old availability report is current.
- This desktop is a shared self-hosted CI runner. Root owns heavy builds/tests;
  never overlap benchmarks with builds, tests, another benchmark or the demo.
  Do not kill other users' processes or cancel their CI. Disk reserve is 15 GB.
- User explicitly authorized unloading Ollama. It was gracefully unloaded through
  `/api/generate` with `keep_alive:0`; it subsequently loaded again from another
  process. Check `/api/ps` and `nvidia-smi` before interpreting a hardware result.
  The loaded Qwen3.6 35B model consumes about 10 GB of GPU memory and 22 GB RAM.
  Do not disable the service or kill unrelated processes.
- `.claude/skills/brick-delivery/SKILL.md` applies: small commits, actual tests,
  pinned clang-format 20.1.8, SPDX, Clang, hardware `docs/perf/` evidence, REST API
  GitHub calls. Never merge a draft merely because an older SHA passed CI.
- Many old untracked `.brick-*`, `.report-*`, HANDOFF files belong to other work.
  Do not `git add .`, reset the checkout, or delete those notes.

## Delivery state

- #330 merged at `2281ba8`.
- #331: https://github.com/LucaSct/rime/pull/331 — draft, city/player/performance
  branch `integration/city-support`, last checked head `e8804af`. Seven actual
  self-hosted jobs green in run `39059362387`; hosted Windows/macOS were skipped
  on the draft. This is not three-OS coverage and does not cover this new preview.
- #332: https://github.com/LucaSct/rime/pull/332 — draft CPU environment seams,
  branch `feat/environment-seams`, head `7ca43d8`. Seven self-hosted jobs green
  in run `39059457675`; same limits apply.
- This preview includes #331 plus cherry-picked #332 and the new leaf commits.
  Do not replay seams twice or squash it into #331 without reviewing the scope.
  No merge was performed during this wrap. Check live PR state before resuming.
- Final source commit, backup/push status and validation results are recorded in
  the wrap results below. User notes remain untracked by design.

## Implemented now

1. **M-FX1 general smoke:** bounded CPU `PlumeEmitter`, explicit caller dt,
   fractional/deferred emission accounting, no hidden clock. `FxParticlePass`
   gains an opt-in sorted straight-alpha mode, input validation/drop accounting
   and a frames-in-flight upload ring. Default additive behavior is preserved.
   `SceneRenderer::set_smoke_particles` copies a bounded input and adds `fx-smoke`
   after SSR, before scope/first-person/TAA. One submission per pass per frame.
   It is billboard smoke, not a volume/density/pressure solver. No self-shadowing,
   soft intersections or proper smoke motion vectors; incident RGB is approximate.
2. **M-FX2 destruction smoke:** sample-owned `PlayEffects` consumes immutable
   destruction events, deduplicates per tick by semantic source IDs, supports
   zero-impulse detachments and caps bursts/shared particle capacity. Engine VFX
   does not depend on destruction. Old ticks are refused; same-tick deliveries
   do not advance particle age twice. Live transport/relevance stress remains.
3. **M-FL1 water/buoyancy:** flat-bed conservative finite-volume CPU `Basin`,
   Rusanov fluxes, reflective boundary states, explicit CFL/substep budget and
   atomic rollback on refusal. Dry cells return no sample, preventing phantom
   lift below the bed. Tests include 10,000 mass-conservation steps and wet/dry,
   refusal, sampling/tick and wall behavior. Existing spherical-cap buoyancy and
   drag exchange are reusable seams. No moving visible pond, bathymetry, spray,
   two-way displacement or GPU water is delivered.
4. **M-FX3 fire presentation:** monotonic keyed source snapshots drive bounded
   plume emission and one ordinary point-light contribution; extinguish/reset
   paths are tested. This is scripted cosmetic fire, with no combustion, fuel,
   heat transport, spreading or damage authority.
5. **Playable wiring:** `the_block --play --effects-preview` enables the three
   effects consumers together in a 256-particle pool. Ordinary `--play` leaves
   them off. The static pond is still authored reflective art. `--effects-smoke`
   is a new CTest with a frozen simulation, presentation toggle, drift control,
   visibility witness, drop counts and unchanged physics hashes.
6. **Reloads:** all three cooked weapons now have separately rendered, measured
   magazine components, preserved materials, a stock-centered visible cant,
   ordered approach/grasp/extract/reseat/return phases, and coupled support-wrist
   contact. SCAR/M4/MSR magazine counts: 1152/664/918 triangles. Full 60 Hz sweeps
   and rendered phase checkpoints cover all three. MSR selection explicitly
   excludes the grip that a first bounding-box guess selected. Geometry hashes
   guard authored source changes. Fingers are still fixed; the same magazine is
   removed/reinserted, not a fresh magazine retrieved from a pouch. Real ammo
   state/weapon profiles are a separate future brick. Held-sniper-focus behavior
   remains covered by the existing live transport proof.

Milestones and exit criteria:
`docs/design/environment-effects-milestones.md`. Primary papers, individual
developer implementations and license findings:
`docs/research/2026-10-17-water-buoyancy.md`,
`docs/research/2026-10-17-smoke-fire.md`, and
`docs/research/2026-10-17-reload-bf6-and-gpu-collision.md`.

## Collision investigation: useful result, no safe speedup yet

Default-off `ContactProfile` work counters and disjoint phase timings now expose
plain/compound/terrain paths, pair query/canonicalization, child posing/midphase,
exact/speculative calls, produced contacts and cache work. Default production
behavior is unchanged; equality tests compare diagnostic on/off hashes/manifolds.
The generated profiler's `--contact-profile` requires `--physics-phases`.

New 360-step-per-world, three-building-collapse capture attributes 77.3%/77.6%
of contact buckets to **plain exact collision**. Compound posing is 1.3%/1.4%,
compound midphase 0.3%. This overturns Astra's initial compound-cache-first
priority for this workload. Contact generation was previously 66–69% of physics.
These instrumented buckets are not unprofiled frame performance; server/client
steps overlap and must not be added as if sequential.

**Rejected experiment:** tight world-AABB filtering skipped about a third of
plain calls but changed contacts and 209/720 hashes. Commits `2b7af70`, `556166e`
are retained in history but REVERTED by `8783a5e`, `31ac589`. Do not resurrect
them based on the unit tests or claim their timing difference is a safe speedup.

A shadow variant kept every call and matched all 720 baseline hashes. Some
hull/hull calls produce one zero-depth contact although world AABBs have positive
separation: first client Y gap 18.45 mm, first server Z gap 5.41 mm, further cases
up to 23.40 mm. Four first-pair hull snapshots plus poses/topology/normals survive
in `docs/perf/environment-preview-2026-10-17/ghost-contact-hulls.json`.
Likely investigation area: GJK/EPA in
`engine/physics/include/rime/physics/narrowphase.hpp`, including the EPA-witness
fallback when face clipping produces no vertices. Underlying cause is NOT yet
isolated. Do not assert this explains old angular impulses or replication desync.

Compare raw rows by `(world, frame, step)`, not append order. The retained CSVs,
analysis, provenance, shadow source/commands and independent review live under
`docs/perf/environment-preview-2026-10-17/`. The shadow helper targets the old
experiment API: reconstruct its historical commits in an isolated worktree,
not against the reverted production header. No third-party media is committed.

**Next collision brick:** make a minimal separated-hull fixture from these poses,
determine correct contact semantics, then establish an unchanged-contact/hash
oracle for any conservative filter. Avoid an arbitrary larger margin until the
scene happens to pass. Only benchmark after fixed-workload correctness passes.

**GPU answer:** feasible, not currently implemented. Study PhysX's real CUDA
rigid-body pipeline and its buffer/fallback constraints, plus Jolt's CPU child
tree. First Rime experiment should be a Vulkan shadow candidate bitset with
outward-rounded conservative bounds, CPU authority/oracle and fallback for any
late/stale/overflowed result. Measure upload+dispatch+readback+synchronization
with graphics running. Do not move authoritative floating-point narrowphase to
the GPU as an unmeasured quick fix or assume cross-device determinism.

## Video and art findings

Downloaded and inspected all 38 seconds at 1 Hz of the owner's BF6 video
`https://www.youtube.com/watch?v=rKHv9CBno0g`. It is DANNYonPC's **2 secret reload
animations in Battlefield 6 (PW7A2 + P18) Easter egg**, not a normal MSR reload.
Magazine juggling/exaggerated pistol gestures are not ordinary technique. Also
inspected a real soldier reload photo and a held BF6 rifle reload still.

Useful observations: visible magazine/hand contact, opposed fingers, independent
weapon parts/materials, glove seams/padding, modular room structure with local
wear/decals/clutter, readable exposure behind the viewmodel. Trim sheets, material
layers and baked normals are plausible inferences, not recovered asset facts.
Video pixels cannot establish triangle counts, source texture sizes or shaders.
The clip gives no meaningful outdoor water/collapse/smoke evidence. Links and
observation/inference boundaries are in the reload/BF6 research document.

## Still open, in priority order

1. Isolated GJK/EPA correctness fixture, then a proven safe scaling optimization.
   No collapse speedup is claimed by this preview.
2. #331's burst replication divergence: deterministic per-step cap is a workaround,
   not a fix; prior captures still show late mismatch. Keep its merge gate open.
3. Qualified 1080p hardware report and representative multi-collapse smoke cost,
   with no builds/CI/demo/Ollama contention. Diagnostic 640x360 headless times do
   not satisfy native-FPS or city/network delivery gates.
4. Fresh cross-OS, format/export and ASan/UBSan/TSan CI for the actual preview SHA.
   Earlier green checks belong to #331/#332's older commits.
5. Finish each documented milestone independently: visible heightfield and bounded
   two-way water coupling; transport/relevance smoke lifecycle; smoke depth/lighting;
   controllable ignite/extinguish presentation before combustion/spread.
6. Art: proper finger grasp/fresh magazine exchange, authored building facade
   detail/height variety and better bulb housing/material response. Earlier city
   improvements exist; this wrap did not attempt to equal BF6 asset quality.
7. Previously recorded defects remain: double emission near `damage.cpp:473`,
   guessed lamp/barrier thresholds, and missing new-building SDF GI proxies.

## Restart commands and local artifacts

```sh
cmake --build build/release -j4
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json RIME_REQUIRE_VULKAN=1 \
  ctest --test-dir build/release --output-on-failure -j4
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json RIME_REQUIRE_VULKAN=1 \
  build/release/bin/the_block --viewmodel-smoke --ppm /home/next/reports/reload-check
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json RIME_REQUIRE_VULKAN=1 \
  build/release/bin/the_block --effects-smoke --ppm /home/next/reports/effects-check
scripts/perf.sh --sample the-block --commit
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/nvidia_icd.json RIME_REQUIRE_VULKAN=1 \
  build/release/bin/the_block --play --effects-preview
```

Run those sequentially. Native demo controls include RMB/Q aim, R reload,
F detonate, weapon selection and Escape exit; read the printed startup controls.
The desktop launcher/log/PID and third-party research frames are under
`/home/next/reports/rime-reload-environment-2026-10-17/`. CPU process observations
found separate CI workloads; high whole-desktop CPU is not necessarily the game.

## Wrap results

- Production integration commit: `6a68aed` (the final build and hardware proofs).
- Research/evidence/handoff commit: `8a8aa2c`; later wrap commits only record results.
- Release full build passed after both unsafe optimization reverts.
- Hardware viewmodel proof passed all SCAR/M4/MSR reload and held-focus checks;
  66/66/84 magazine-contact frames with zero measured gap, no clamped IK targets.
  Inspected phase-1 image sheet: low action framing and dominant MSR geometry
  remain art limitations, alongside fixed fingers. Do not call this final polish.
- Hardware effects proof passed: changed 4,052 pixels, drift 0, live/requested/
  spawned 72, capacity/render drops 0, unchanged physics hashes. GPU `fx-smoke`
  24-frame diagnostic p50 0.004096 ms, p99/max 0.382976 ms at 640x360.
- Pinned exact engine/tests format gate, SPDX check and `git diff --check` passed.
  New translation units and the modified physics/render/sample units had passed
  Clang syntax checks earlier in this integration; removed experimental tests do
  not ship. Full CTest, qualified perf attempt and backup status follow below.
- All coding agents completed; no subagents or agent builds remain running.

- **Full CTest passed: 89/89**, software Vulkan, 286.97 seconds. Long cases:
  viewmodel 283.97 s, environment 201.71 s, effects 30.90 s (parallel functional
  checks, not performance measurements). Full log is committed with the evidence.
- **Standard hardware performance gate FAILED**, source stamp `b376d15`, Release,
  1920x1080, 600 measured frames. Sim.block p99 7.397 ms exceeds 6.000 ms budget;
  old baseline driver 615.71.09 differs from current 615.78.08, so regression
  comparison was refused; graphics clocks varied 210–1935 MHz (93.9% spread), so
  automatic filing was refused. No override. See `delivery-perf.log` and
  `unqualified-1080p-b376d15.json` under the committed evidence directory. This is
  not a valid baseline, an established regression, or a passing delivery gate.
- The final push backs up `integration/environment-preview` on origin. Verify
  `git rev-parse HEAD` against `git ls-remote origin refs/heads/integration/environment-preview`
  before resuming. No additional preview PR is opened during this wrap; #331/#332
  remain drafts. The push queues fresh self-hosted CI; results are not awaited
  under the wrap request and no fresh green/sanitizer result is claimed.
- The desktop launcher uses `--play --effects-preview`. Inspect `demo-pid`,
  `demo.log` and `demo-exit` in the report directory for current process state.
  Starting CI after the final push may again compete for desktop CPU; do not
  interpret that concurrent demo as a clean performance capture.
