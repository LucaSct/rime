# rime::fluids — CPU water and thermal references

This removable, CPU-only module contains spherical-probe buoyancy calculations,
a bounded flat-bed shallow-water `Basin`, and a fixed-mass water/ice thermal
reference. None automatically couples bodies, thermal state and water transport,
or renders a surface. The caller supplies the authoritative sample tick and
stable body key, evaluates probes in canonical key order, and applies each
returned `body_impulse` through physics on the appropriate thread before stepping.
The opposite `water_reaction_impulse` is an accounting record; no two-way wave
or momentum-feedback claim follows from recording it.

The submerged sphere-cap volume is analytic. Hydrostatic lift is `density * gravity * submerged_volume`. Optional quadratic drag uses relative water/body velocity and a projected area scaled by the submerged volume fraction. That area is an approximation suitable for a bounded pontoon, not a surface pressure integral. Probes should not overlap or their displaced volumes are counted twice. Dry probes produce a zero exchange. Invalid inputs and outputs outside finite `float` range are rejected.

The module links only `rime::core` and exports `rime::fluids`. The test executable links `rime::physics` to prove that a consumer can apply the public impulse. Neither the module nor this test needs a GPU. A gameplay authority design must still decide how surface samples and resulting impulses agree across peers; a tick label alone does not establish determinism.

`phase_change.hpp` adds `ThermalSample::create(mass, {temperature, liquid_fraction})`
with immutable model parameters. Positive heat melts/warms; negative heat freezes/
cools. At 273.15 K, every liquid mass fraction from zero to one has the same
temperature and different energy, so construction requires both values. The
default constants and −20 to +40 °C model limits are approximations documented
in the [research note](../../docs/research/2026-10-18-water-ice-phase.md).

`apply_heat` and `apply_power` return a `HeatExchange`. On acceptance,
`applied_joules + applied_error_joules` represents the exact change between
stored energy doubles; adding `roundoff_joules` gives the requested heat as an
exact real-number identity. Ordinary floating-point summation can lose a low
component, so a caller must not silently drop it and claim an exact cumulative
ledger. Refusals apply no heat and leave the sample unchanged. Nonzero heat too
small to move energy, undecodable interior phase, temperature-limit overshoot,
nonfinite arithmetic or an unsupported floating-point mode refuses explicitly.
Power requests use the rounded `P*dt` product. No residual is automatically
buffered. This model needs neither a timestep limit nor substeps to cross phases.

The sample has no heat diffusion, water transport, density expansion, geometry,
collision or authority. Changing mass is deliberately absent. It supplies a
calorimetry witness for later coupling, not visible or spatial freezing today.

The optional offline exact-arithmetic diagnostic compiles a bit-pattern probe
against an ordinary GCC/Clang-built fluids archive:

```sh
python3 tests/fluids/verify_phase_change.py --library build/release/lib/librime_fluids.a --work-dir /path/to/scratch --compiler clang++
```

Python fractions are only used by this diagnostic, never by the runtime. Portable
C++ phase tests remain part of the normal fluids CTest target on all CI platforms.

`thermal_contact.hpp` adds `exchange_heat(a, b, conductance_w_k, dt_seconds)`.
This isolated two-sample contact uses backward-Euler conduction, including latent
plateaus and different material configs. It returns `Exchanged`, `NoExchange`, or
an explicit refusal. Both samples commit together, after validation of copied
states. Accepted transfers preserve temperature ordering; disabled contacts and
equal temperatures leave both states bitwise unchanged, even with different
liquid fractions. Conductance includes the caller's contact geometry and has
units W/K; it is not conductivity in W/(m K).

The result retains one `HeatExchange` receipt per argument. Opposite requests
cancel exactly, and the two applied expansions plus the two residuals sum to
zero as real values of the returned doubles. Stored total energy alone can drift
by those residuals; ordinary summation or discarding them is not conservation.
There is no hidden accumulated heat or mass transfer.

The numerical contract is deliberately bounded: binary64 rounded products and
cached temperatures define the residual, at most 128 bisection trials bracket
its sign change to relative heat width `2^-36`, and the non-overshooting lower
endpoint is committed. Overflow/underflow of nonzero demand, an ineffective or
undecodable trial, unsupported arithmetic, exhausted iteration budget, or a
root beyond either configured temperature limit refuses both samples. Limits
never clip the transfer. This is a first-order isolated-contact reference, not
a spatial/N-body integrator or a guarantee that every finite input is supported.

```sh
python3 tests/fluids/verify_thermal_contact.py --library build/release/lib/librime_fluids.a --work-dir /path/to/contact-scratch
```

The diagnostic checks exact two-sided energy accounting, range/order, both sign
certificates, zero controls and atomic failure against the actual compiled
archive. See [ADR-0092](../../docs/adr/0092-transactional-thermal-contact.md) and
the [contact research note](../../docs/research/2026-10-18-thermal-contact.md).

`thermal_field.hpp` adds a bounded fixed-mass graph with explicit cell indices,
conductive links and constant signed power sources. `ThermalField::create`
copies and canonicalizes descriptions, rejecting invalid or duplicate links and
multiple sources for one cell. `advance(dt)` applies sources then contacts in
sorted order over bounded uniform substeps. This is an ordered first-order
reference, not a global implicit solve. Timestep subdivision and node relabeling
can change results; geometry, flow and authority remain caller responsibilities.

Creation reserves all tick buffers. Fields are move-only. Receipt and worst-case
contact-trial budgets are checked before work; any primitive failure rolls back
the entire tick, including the previously published journal. The result reports
the failing operation and underlying status. Zero dt preserves state, ordinal
tick and journal. Positive successful calls publish a complete operation journal
and `last_step()` metadata. Borrowed views are replaced on success; consume or
copy the journal before advancing again if history is needed.

The journal retains before/after energies and each primitive receipt, without
rounding them into a cumulative scalar. Their exact telescope gives field energy
change plus update residuals equal to rounded source requests. Immutable powers
and recorded dt/substep duration/count additionally retain all operands needed
to check multiplication and partition rounding against ideal P*dt offline.

```sh
python3 tests/fluids/verify_thermal_field.py --library build/release/lib/librime_fluids.a --work-dir /path/to/field-scratch
```

This exact diagnostic also interposes allocations during `advance()`. Portable
tests exercise multi-hop freezing/melting, no-contact controls, rollback and
temporal/spatial refinement. [ADR-0093](../../docs/adr/0093-bounded-journaled-thermal-field.md)
documents caps, accuracy and the remaining field/editor integration work.
