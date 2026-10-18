# Handoff — thermal contact — 2026-10-18

The user asked to continue water/smoke/fire/ice and editor work, prioritizing
water freezing/melting from temperature. This brick adds isolated conductive
heat exchange on top of the completed fixed-mass thermal phase reference.

Branch: `feat/thermal-pair-exchange`.
Parent: `feat/thermal-phase-oracle` at
`c1416037b8e59f4583910bbfef08e99712f71908` (draft #337).
Implementation: `9d50cb843226b93180b9de98457b27056afe3f09`.
Evidence/handoff is the following commit on this branch. Nothing merged.

## Implemented

- `engine/fluids/include/rime/fluids/thermal_contact.hpp`,
  `engine/fluids/src/thermal_contact.cpp`: allocation-free `exchange_heat(a,b,G,dt)`.
- Caller supplies distinct immutable-mass samples, W/K conductance and elapsed
  seconds. Different configs/melting points are supported. Zero G/dt and exactly
  equal temperatures return `NoExchange`, retaining unequal phase fractions.
- Backward-Euler isolated contact, binary64 normalized residual `q/Q0` versus
  `gap(q)/gap(0)`, `Q0=round(round(G*dt)*gap(0))`. Temperature caches are rounded;
  the result certifies a sign bracket, not an exact-real constitutive root.
- At most 128 midpoint trials; require relative transfer width <= `2^-36` and
  apply the positive lower endpoint. Actual temperature order cannot reverse.
  The strict width comparison handles a subnormal tolerance rounded upward.
- Trial copies update through public `ThermalSample::apply_heat`, then both
  originals commit. Any refusal leaves both unchanged and returns empty receipts.
  Temperature limits refuse instead of clipping the contact law.
- Explicit `InvalidInput` (including aliasing), `TemperatureLimit`,
  `Unrepresentable`, `UnsupportedEnvironment`, `IterationLimit` statuses. Some
  finite extreme scales deliberately refuse. Positive ineffective heat is never
  called physical zero. An unrepresentable trial refuses immediately.
- Both receipts request exactly opposite heat. The two applied expansions plus
  two residuals sum exactly to zero as mathematical values of six doubles.
  Stored total energy alone need not be conserved; no hidden reservoir exists.

This creates no default workload. No grid, contact geometry, fluid transport,
expansion, collision, rendered ice, editor or network authority is added.

## Verification

See `docs/verification/thermal-contact-2026-10-18/README.md`, source/artifact
hashes and retained logs. Full diff self-review completed before code commit.

- Full RelWithDebInfo build passes; fluids 34 cases / 24,284 assertions pass.
- New contact ASan+UBSan: 12 cases / 1,048 assertions pass, with both contact and
  phase runtime objects instrumented. New TUs pass actual-flag Clang syntax.
- Exact Python Fraction diagnostic `tests/fluids/verify_thermal_contact.py`:
  1,225 cases on GCC and Clang runtime archives, identical output hash
  `2e735109d82bbf4f2baa0d2fe36126f4642ee0e2aff95b629cb6d2fc2c4b45b6`.
  1,042 exchanged / 170 valid zero contacts / 13 atomic refusals. Of accepted
  pairs, 954 have nonzero stored-total residual exactly balanced by receipts.
- Scratch-only missing-second-commit and lost-residual mutants are rejected.
  Python -O explicitly refuses. Diagnostic cannot publish a stale success JSON
  after compiler/process failure.
- Tests include analytic sensible/latent cases, large accepted phase crossings,
  shared/different plateaus, swapped order, mass scaling, first-order refinement,
  zero exchange, doubled latent/conductance controls, and tiny/exotic refusals.
- Full lavapipe CTest: 89/90, exit 8, 288.20 seconds. Only inherited
  `the_block_headless` three locality/collapse claims fail; none were weakened.
  The run started before final subnormal-tolerance tightening; final build,
  targeted tests, sanitizer and exact/fault controls were checked afterward.
- Normal SDK smoke and separate out-of-tree installed `rime::fluids` contact
  consumer both pass. No Rust changes; no hardware performance benchmark.
- Actual parent #337 CI ASan/editor failure logs confirm the same block claims:
  run 39154495346, jobs 117754779529/117754779497. Parent TSan/SDK/format passed;
  hosted draft jobs skipped. Do not call this a completed three-OS proof.

## Consultation

User authorized independent Fable/devpass and Astra proposals. Live availability,
prices and credit caps checked first; both raw calls were tool-free and
backgrounded. Fable $0.24645 + Astra $0.07574 = **$0.32219**, matched observed gateway
usage increase. Requests/answers/usage retained, credentials/account balances not.

Fable recommended equilibrium-capped forward Euler; Astra backward Euler with
limit clipping and wider numerical machinery. Chose backward Euler's time law,
Fable's refusal of validity clipping, and Astra's two-residual/realized-order
checks. Declined Fable's extra-unknown claim and silent convergence suggestion,
Astra's physical validity cap and extended interval/ranked search. These were
independent proposals, not final code reviews. Root owns all edits and review.

## Next bricks

1. Bounded spatial thermal field: explicit contact topology and canonical order,
   fixed capacities/caps, whole-tick transactionality, bounded heat sources and
   energy accounting, spatial/refinement witnesses. Sequential pair updates are
   order dependent; do not claim N-body accuracy merely by looping this API.
2. Reflected initial thermal state and heat-source descriptions, validation,
   inspector undo/save/reload, Play reconstruction and Stop reset. Keep state
   derived from descriptions, not serialized solver buffers. Preview temperatures
   and phase through validated state before claiming visible ice.
3. Separate M-FL1 solid displacement/body feedback. Basin has no vertical
   momentum: opposite reaction records are not a consumed conservative coupling.
   Flowing-water freezing needs liquid/solid mass and transported-energy accounting.
4. Smoke soft intersections/lighting/temporal response and destruction
   relevance/join stress. Existing `vfx_scene` smoke/fire authoring stays available;
   its fire is cosmetic and provides no thermal/fuel/spread/damage authority.
5. Clicked editor asset proof, export and multiplayer/late-join authoring pipeline.

M18 still fails (parent sim.block p99 8.074 ms against 6). City convergence and
hardware driver/clock qualification remain open. Never merge this stack based
on the CPU contact tests. No unsafe collision shortcut was reintroduced.

## Working environment

`/home/next/.claude` does not exist. Actual global rules:
`/home/next/.claude-switch/profiles/default/CLAUDE.md`.
Read project CLAUDE/VISION/roadmaps and brick-delivery skill before changes.
Shared runner/workstation: no performance tests alongside builds/tests/CI, no
killing other jobs, preserve >=15 GB free (18 GB at this brick). Untracked old
notes are unrelated; don't stage/delete them. Own branch per brick, explicit
staging, full-diff self-review, pinned clang-format exact find set, REST PR API.
The $7 user ceiling remains; live gateway weekly cap is lower, so recheck before
more premium calls. Use narrow independent proposals and verify locally.

Scratch: `/home/next/reports/rime-thermal-exchange-2026-10-18/`.
Contains full build/test logs, probes, input/output corpus, binaries, ASan objects,
mutants, installed consumer and private credit snapshots. The docs proof helpers
must be copied to a scratch directory before running; do not generate objects
inside docs. No tools/jobs need to remain running after delivery.
