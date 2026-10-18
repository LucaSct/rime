# Handoff — bounded thermal field — 2026-10-18

The user asked to continue water/smoke/fire/ice and the editor pipeline, choosing
temperature-driven freezing/melting first. This brick adds the bounded thermal
field consumer after sample and isolated-contact references. Nothing merged.

Branch: `feat/bounded-thermal-field`.
Parent: `feat/thermal-pair-exchange`, draft #338, now at
`55e5fb57f1eabaa79bd18a1d2bf678d1f3e17396` after its separate Debug warning fix.
Field implementation: `0f8df6d40ab78291375bfe3a6ed3be59fd96ff40`.
Evidence/handoff is the following commit. Prior handoffs remain historical.

## Implemented

`engine/fluids/include/rime/fluids/thermal_field.hpp` and `src/thermal_field.cpp`:
move-only `ThermalField`, immutable copied cell samples, canonical undirected
`ThermalLink` contacts and at most one constant signed `ThermalPowerSource` per
cell. Caller supplies spatial topology/geometry-derived conductance; no automatic
mesh/grid construction. Different sample masses/material configs are supported.

- Normalize links by `(min,max)` and sort; reject self/duplicates, bad indices,
  negative/nonfinite conductance, duplicate source cells and nonfinite power.
- Each positive `advance(dt)` chooses uniform bounded h=round(dt/N), source-first
  then canonical contacts per substep. Ordered first-order split; NOT global BE.
  Node relabeling and timestep subdivision can change results.
- Fixed preallocated cell/journal buffers, move-only so copying cannot lose
  reserve capacities. No allocations during tick. Caps: 256 cells, 1,024 links,
  256 sources, 1,024 substeps, 16,384 receipts. Defaults: 4,096 receipts and 65,536
  worst-case contact trials. Every contact budgets 128 trials, even zero flow.
- Receipt and worst-case work preflight, no automatic retries. Runtime result
  reports attempted operations, failing descriptor/substep and primitive status.
- Stage all operations. Commit cells, checked ordinal tick, last-step metadata
  and full journal together. Zero dt and failures retain all published state,
  including prior journal and its backing pointer. No floating elapsed-time sum.
- Journal every admitted operation, including explicit zero sources and unchanged
  links. Retain both primitive receipts and before/after stored energies.
- Exact delta + all update roundoffs = rounded source requests. Retained powers,
  dt,h,N additionally permit offline exact product/partition accounting to ideal
  P*dt. No hidden energy reservoir or scalar cumulative conservation claim.

No renderer, editor, mass transport, flowing water, geometry, expansion, collision
or authority is added. Existing cosmetic fire is not a thermal source by default.
ADR-0093, research note, module README and roadmap record scope and next work.

## Verification

`docs/verification/thermal-field-2026-10-18/README.md` and retained logs/hashes:

- Full RelWithDebInfo build pass. Fluids 46 cases / 25,061 assertions pass.
- New field ASan/UBSan 12 cases / 777 assertions pass, all three thermal runtime
  objects instrumented. New runtime/test TUs pass actual-flag Clang syntax.
- `tests/fluids/verify_thermal_field.py` exact Fraction diagnostic through actual
  GCC/Clang archives: 38 fields, 544 calls, 4,057 journal operations; identical
  output SHA-256 `227337ec3af0a27bcdaeb43a0106ed9c9c924af5522c73c67a5b8d80f3e50985`.
  Zero interposed C++ allocations during advance. All 1,097 source-product and
  96 duration-partition witnesses accounted for separately from update rounding.
- Scratch partial-publication, lost-receipt and tick-allocation mutants rejected.
  Python -O refused; compile/process failure cannot reuse old success metadata.
- Multi-hop freeze/melt, equal-T unequal-phase hold, disconnected/zero controls,
  source-free extrema, canonical replay, late-substep and final-contact rollback,
  budgets and moved field tested. Three-node time refinement is first order and
  detects relabeling sensitivity. Insulated rod N4/8/16 RMS errors:
  .0312724/.00808035/.00203663 K, refined fine-time .00200659 (~1.5% difference).
  This is a single-phase smooth spatial witness, not a phase-front proof.
- Full lavapipe CTest 89/90, exit8, 336.55s: only same three inherited block
  collapse/locality claims. No thresholds weakened. SDK smoke and separate
  out-of-tree installed public fluids field consumer pass (160 multi-hop ticks).
- Pinned format exact find set, license and whitespace pass. Full diff self-review
  completed. No Rust changes/Cargo runs or hardware performance benchmark.

## Parent correction and CI

Read actual #338 CI, not just check names. Linux run 39157706199 job 117764263470
fails inherited block claims. SDK job 117764263487 instead found a real contact
TEST compile warning: copying const std::pair in Debug under Werror. Corrected
loop to const reference in separate commit 55e5fb5, pushed to parent PR #338 and
advanced local parent ref to that exact commit. Child includes the same commit;
no rebase/cherry-pick needed. Contact tests 12/1048 and explicit Debug Clang
range-loop warning check pass. Parent rerun39161178783 SDK was queued at snapshot;
TSan passed; editor job 117774649879 fails same block claims. New field PR CI is
separate; no 3-OS/external green claimed. Never dismiss SDK warning as physics.

## Consultation / spend

One new raw tool-free Astra design consultation, backgrounded after live model
price/credit check. Response usage cost $0.06254. Shared gateway increased
$0.10313, of which $0.04059 is unattributed by this response; conservative task
spend records $0.10313. Do not publish private key/credit snapshots. Earlier
Fable contact advice was available; no new Fable call needed.

Adopted explicit topology, staged bounded journal, integer tick, operand retention,
late rollback and convergence controls. Simplified explicit tick-plan suggestion
to dt/configured substeps; journal zeros rather than skipping. Rejected dx^8 time
scaling for rod in favor of measured dx^4 plus further time refinement. This was
an independent proposal, not a final diff review. Root owns implementation/review.

## Next bricks

1. Reflected thermal field/source descriptions: explicit fixed cells/links first,
   validated mass, initial temperature AND fraction, material domain and budgets.
   Reconstruct runtime on Play, freeze Pause, clear/reset on Stop. Inspector
   edit/inverse/undo/save/reload and refusal diagnostics through real host. Save
   descriptions, not numerical buffers or unproven network identities.
2. Visible temperature/phase preview derived from validated state. Show actual
   freeze/melt, source-disabled control and aligned surface. No collision or
   thickness claim from a shader tint or liquid MASS fraction alone.
3. Independent M-FL1 solid displacement/body feedback. Existing basin has no
   vertical momentum; reaction records do not establish consumed conservation.
   Flowing-water freezing requires liquid/solid mass and transported energy.
4. Smoke intersections/lighting/temporal fidelity, destruction join/relevance
   stress; canonical ignition/extinguish, then fuel/thermal spread/damage authority.
5. Clicked editor asset proof, export, server/two clients/late join. M18 still
   open, parent sim.block p99 8.074ms >6; city convergence and hardware driver/clock
   qualification open. Nothing merged because these CPU tests pass.

## Environment

Actual global rules: `/home/next/.claude-switch/profiles/default/CLAUDE.md`;
`/home/next/.claude` does not exist. Read project CLAUDE/VISION/roadmaps and
brick-delivery skill. Shared workstation/runner: no hardware perf alongside
builds/tests/CI, no killing other jobs, reserve >=15GB (19GB at this brick).
Untracked older notes are unrelated; do not stage/delete them. Own branch per
brick, explicit staging, full diff self-review, pinned format exact find set,
REST PR API. User premium ceiling $7, live shared gateway caps are lower; check
availability/prices/credit before another narrow read-only consultation.

Scratch: `/home/next/reports/rime-thermal-field-2026-10-18/`: full outputs, probes,
bit corpora, binaries, ASan/Clang objects, mutants, SDK consumer, private billing
snapshots. Proof helpers must be copied to scratch before running. No own jobs
need to remain running after delivery; external CI may still be active.
