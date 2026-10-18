# Handoff — water/ice thermal reference, 2026-10-18

Branch `feat/thermal-phase-oracle`, base `feat/environment-authoring` / draft #336
(`ab201bd32cd4bd86cfe03600922d5c2012002602`). Implementation `d15783dd6d9b005b1df0277758e0c69eb4b2a4cd`; a following documentation commit retains
this handoff and the verification artifacts. Nothing merged.

## Latest owner request

Continue environmental bricks and editor pipeline. Ice priority remains
**water freezing/melting from temperature first**. The owner additionally asked
for Fable over devpass and GPT-6 Astra consultations as useful for complex modules.
Both were consulted as independent read-only proposals; see below.

## Implemented

`engine/fluids/phase_change.hpp/.cpp`: allocation-free `ThermalSample`, immutable
mass/material/temperature limits, stored total enthalpy relative to solid at Tm,
and a derived temperature/liquid MASS fraction cache. Construction requires both
temperature and fraction; contradictory phase inputs refuse. Explicit heat and
power/time exchanges warm/cool, melt/freeze and cross both phase endpoints.
No frame consumer, GPU workload, ECS component or scene-format change.

Default approximate material: Tm 273.15 K, solid/liquid cp 2052/4179 J/(kg K),
latent 333500 J/kg, model bounds 253.15..313.15 K. Sources and model limits are
in `docs/research/2026-10-18-water-ice-phase.md`; provisional ADR-0091 and roadmap
progress are committed. The old fluids README's stale "no basin" claim is fixed.

Mutations validate before committing energy and cache. Prospective range admission
uses an ordered FastTwoSum residual, so exact out-of-domain heat cannot round
onto a bound and pass. An interior mixture cannot silently decode to zero phase.
A two-double applied-energy delta plus update residual accounts for input exactly
as a real-number identity between returned doubles. The caller must preserve the
low parts; there is no hidden cumulative ledger or automatic residual reapplication.
Power requests use the rounded P*dt product, explicitly excluding product rounding.

Round-to-nearest binary64 with gradual underflow is required. Creation/mutation
probe the mode; unsupported modes refuse, while state getters use the validated
cache. Invalid, nonfinite, ineffective sub-ulp or otherwise unrepresentable inputs
leave the sample unchanged. Zero heat/duration/power preserves state bits.

## Verification

See `docs/verification/thermal-phase-2026-10-18/README.md`, exact digests and logs.
Full optimized build; full software-Vulkan CTest **89/90**, same inherited three
block locality/collapse assertions, **356.03 s**, exit 8. No threshold changes.
New phase ASan/UBSan: **11 cases / 5701 assertions**. Both new TUs Clang-check.
Exact Python Fraction corpus: **2838 transactions**, 1325 accepted exact-balance,
1075 atomic heat refusals, 438 expected invalid/unrepresentable initializations.
GCC and Clang runtime objects produce identical outputs locally. Both weakened
guard mutants fail the checker. Python -O refuses; stale successful reports are
removed before a rerun. This is a standalone diagnostic, not a Python runtime
requirement or a cross-platform deterministic-physics claim. Installed SDK passes.

Parent #336 CI was read: Linux/ASan fail those same three block claims, editor
round-trip fails the same claims in its demo consumer; format/TSan/SDK pass.
Filtered failure logs retained. No new native performance run or local TSan run.
M18, city convergence and shipment/editor visual gates remain open.

## Consultations and judgement

Live model availability/pricing and credits were checked before/after. Both raw
chat requests used the devpass gateway, no tools/files, low effort, max 8000
output tokens. Fable 5.1: $0.1102; GPT-6 Astra: $0.0863; observed spend **$0.1965**.
Credentials and raw personal credit responses stay outside the repo.

Fable contributed explicit phase initialization, sensible/latent separation and
latent-versus-capacity controls. Its "enthalpy makes accounting exact" assertion
is false in floating point; substeps also cannot fix an unsupported final heat
budget. Astra contributed strict phase representation and exact-boundary/ledger
concerns, both reproduced against the prototype. Its full runtime rational system
was declined; small error-free expansions and an external exact checker suffice
for this fixed-mass reference. Both preferred specific enthalpy; total energy
avoids Q/m and reconstructed-delta rounding for this immutable mass. No future
transport/serialized format is committed to that choice. Full diff self-reviewed.

## Next useful bricks

1. Conservative heat exchange between fixed-mass samples with equal-and-opposite
   energy accounting, stable approach to equilibrium, an exchange-disabled control
   and transactional refusal. Equal temperatures with different liquid fractions
   do NOT alone imply heat flow; equilibrium need not equalize phase fractions.
   This leads to a bounded thermal field and temperature/heat-source authoring.
2. The independent M-FL1 next brick remains solid displacement/body feedback into
   the basin, with mass/coupling witnesses. No vertical momentum exists in the
   shallow-water grid, so don't claim full momentum conservation from a record.
3. Coupling requires liquid/solid MASS and transported energy accounting before
   flowing water can freeze. Ice expansion, area/thickness, collision and breakage
   are later contracts; liquid mass fraction is not a volume/thickness field.
4. Editor thermal/basin controls, visible evolving surface/phase, clicked proof,
   export/server/two-client lifecycle and measured hardware cost remain open.

Local scratch: `/home/next/reports/rime-thermal-phase-2026-10-18/`. Keep 15 GB disk
reserve; last about 19 GB free. Do not benchmark alongside builds/tests or disturb
other workloads. Leave preexisting untracked notes alone. Recheck live credit
limits before another consult; dated/approximate allowance notes are not balances.
