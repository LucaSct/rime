<!-- SPDX-License-Identifier: Apache-2.0 -->
# Collapse phases and play corrections — 2026-10-17

See [the measured scaling plan](../design/collapse-scaling.md) for the phase table,
independent Astra consultations, exact-code candidates and long-term representation
boundaries. [Raw capture](collapse-scaling-2026-10-17/run/) retains per-frame passes,
per-physics-step phases and per-island times; [generated source](collapse-scaling-2026-10-17/generated/)
and source/link hashes identify the executable. The diagnostic used three charges,
60 warmup and 360 measured ticks on RTX 3060 hardware. Optional instrumentation
adds overhead; clocks/load/CPU placement were uncontrolled. It is not a gate pass.

There are exactly 360 whole-step rows and 360 rows for every existing phase in
each world, including the client. Measurements show contacts taking 66–69% of
summed step duration. Separate broadphase/key-sort, compound preparation/culling
and exact narrowphase instrumentation is the next brick. No solver change is
justified by body count alone. Per-step island profiles include each world's
own work; summing overlapping worker times cannot produce wall-frame time.
The population and peer-hash mismatch pattern match the earlier three-building
capture, so neither settled convergence nor a replication fix is asserted.

## Weapon feedback corrections

Holding aim after an accepted sniper shot now retains the sight picture. The
pending cycle remains busy, preventing another shot or reload bypass. Releasing
RMB (or the held Q alternative) starts one recoil/bolt cycle and one case at
extraction; hip fire starts immediately. Pure tests cover long holds, duplicate
fire flags, invalid/large dt and selection cancellation. The rendered smoke
checks the held optic pose and visible pupil through a hold longer than the bolt
cycle, then verifies extraction on release.

The first reload framing proposal stretched wrists by up to 134 mm and was rejected.
The final pose raises and cants the gun enough to expose the action hand instead
of moving it forward beyond reach. All three real-rig reload sweeps and the
sniper bolt have zero invalid/clamped frames. At three magazine-action checkpoints,
the muzzle and support wrist remain inside the viewport; the wrist is within the
specified central/lower band. These are actual projected world-space wrist targets,
not a pixel-difference claim that could pass with the hand offscreen. See the
[SCAR](collapse-scaling-2026-10-17/verification/framing-v5.scar-reload.png) and
[sniper](collapse-scaling-2026-10-17/verification/framing-v5.sniper-reload.png) captures.
Gun meshes still have rigid magazines/bolt geometry; this remains procedural
presentation, not a separately authored magazine-removal animation. Asset authors
and licenses: [viewmodel notices](../../samples/99-the-block/assets/viewmodel/ASSET-LICENSES.md).

## Checks and the ASan failure

The combined city/play plus initial emission/buoyancy integration tree passes
all **88 lavapipe CTests**, including the new fluids suite, existing render/RHI,
actual sniper/reload paths and environment effects. Targeted gameplay/VFX/fluids
suites also pass; new/changed numerical test sources pass Clang syntax checks.
CI's complete engine/tests format set and SPDX headers pass. [Logs](collapse-scaling-2026-10-17/verification/)
include the combined-tree test result; the effects seams will be delivered separately.
Physics/core source is identical to main, so the CPU-only seams use the same
physics implementation when delivered independently.

The previous city head `e631c08` failed ASan CI in `the_block_environment_smoke`:
`core::inverse` asserted on the tiny instance determinant of the resized tracer
and new spent cases. This was a checked-build regression despite Release tests
passing. Mesh dimensions are now baked into the case vertices and a 1 cm base
sphere, keeping actual world sizes and fixed pool ownership while avoiding tiny
instance scales. A Release sample relinked with the existing assertion-enabled
Debug matrix object passes the hardware environment smoke; the [link/object
provenance](collapse-scaling-2026-10-17/verification/checked-inverse-provenance.json)
explicitly labels this as a checked-inverse diagnostic, **not an ASan build**.
The production smoke also checks the transform determinant condition in Release.
Fresh ASan CI is still required; the earlier failure is not called a flake.
