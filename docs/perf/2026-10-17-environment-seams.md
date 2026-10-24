<!-- SPDX-License-Identifier: Apache-2.0 -->
# Initial smoke and buoyancy seams — 2026-10-17

Scope and research: [four milestone plan](../design/environment-effects-milestones.md),
[ADR-0088](../adr/0088-environment-effects-as-separate-milestones.md).
These are CPU interfaces/reference calculations. No new frame workload, water
solver, visible plume or fire simulator is enabled by this change.

## Observable emission

The existing ParticleField/EmitParams/families are retained. Requested, spawned,
capacity-dropped, invalid, naturally retired and explicitly cleared counts expose
work that previously disappeared silently. Ordinary valid emissions retain their
float count calculation and random-stream order; the exceptional huge-count case
avoids an out-of-range float-to-uint32 conversion. Invalid calls leave the scatter
stream untouched. Tests cover mixing families, full/zero capacity, invalid values,
retirement/clear accounting and reset without changing live data or RNG.

This is the General Smoke milestone's accounting seam, not its rendering exit.
Destruction smoke and fire will reuse the same emission inputs in their consumers.

## Buoyancy exchange

The removable core-only rime::fluids module evaluates exact spherical-cap volume
against a supplied locally horizontal surface. It returns rho*g*V lift and optional
approximate quadratic relative-flow drag as body impulses, with source tick and
stable caller-owned identity. Finite validation rejects unrepresentable results.
The opposite water impulse is recorded; no grid consumes it yet, so no two-way
wave or fluid conservation claim follows.

The integration proof applies these impulses through PhysicsWorld's public API.
A sphere with mass matching half its displaced water remains afloat for 120 steps;
the dry control falls. Endpoint volume, force units, drag direction and invalid
inputs have separate checks. The module does not depend on physics or rendering.

## Verification and reproducibility

An independent optimized build of this branch configured with Vulkan OFF,
samples OFF and C API OFF builds the core, physics, VFX and fluids targets.
Both rime_vfx_tests and rime_fluids_tests pass: [configure/build/test logs](environment-seams-2026-10-17/).
The generated SDK export declares rime::fluids; full installed-consumer CI remains
required. No new third-party dependency or source import was added.

The combined city/play plus seams verification tree also passed 88/88 lavapipe
CTests. Its physics/core sources match main exactly, and the independently built
seam sources match the [recorded source hashes](environment-seams-2026-10-17/source-hashes.json).
Clang syntax checks for the new numerical TUs pass; the complete engine/tests
format and SPDX checks pass. The combined-tree log is integration evidence, not
an assertion that the future smoke/water milestones are complete.

No speedup or frame-budget pass is claimed. Future enabled smoke rendering,
heightfield stepping, GPU capacity and two-way coupling each require their own
hardware docs/perf report and authority proof. The city branch's standard 1080p
attempt again refused background CPU load; it was not overridden or repurposed
as this new module's numerical proof.
