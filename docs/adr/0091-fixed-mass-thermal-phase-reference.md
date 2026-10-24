# ADR-0091: Fixed-mass water/ice energy reference

Status: proposed CPU reference, 2026-10-18; no transport or serialized-state contract.

## Context

The owner prioritized temperature-driven freezing/melting for M-FL2. Temperature
alone cannot determine the energy of a mixture at the melting point. Water's
existing flat-bed basin has neither transported heat nor a solid phase.

## Decision

Add an allocation-free `ThermalSample` to the core-only fluids module. Store total
energy for one immutable mass; derive temperature and liquid mass fraction from
a constant-property solid/latent/liquid relation. Require both initial values
explicitly, and reject contradictory phase/temperature descriptions. Defaults
and their approximation limits are documented in the
[research note](../research/2026-10-18-water-ice-phase.md).

Mutations are transactional: finite inputs, representable parameters, supported
binary64 arithmetic mode, exact prospective energy admission and decodable phase
all validate before committing energy and its derived cache. Zero input changes
nothing. Out-of-range energy is refused, never clipped. Return the actual energy
delta as a two-double expansion and an explicit update residual; no heat is
silently carried or reapplied. Power/time requests use a documented rounded
binary64 product, not an exact rational time integrator.

Keep exact rational arithmetic in a standalone verification script rather than
the runtime. This reference is a removable numerical brick with no ECS, renderer,
physics, editor or networking dependency and no new default workload.

## Consequences and verification

Tests cover heating/freezing/melting/cooling, phase endpoints, latent duration,
changed latent heat versus changed capacity, mass scaling, whole-step versus
partitioned heat, signed zero, atomic refusals, arithmetic mode and sub-ulp heat.
Adversarial controls expose rounded range overshoot and a collapsed interior
fraction. A Python exact-fraction diagnostic checks ledger identities through
the actual built archive across tiny and large masses and phase crossings.

This is a lumped equilibrium model, not a spatial freezing simulator. There is no
heat diffusion, advection, expansion, pressure/salinity/supercooling, boiling,
visible ice, collision or damage. Future basin coupling must account for liquid
and solid mass, transported energy, geometry and authority independently. Neither
this ADR nor a matching source tick establishes cross-device determinism.

Fable 5.1 and GPT-6 Astra supplied independent proposals. Their contributions,
disagreements, rejected advice and the $0.1965 observed spend are recorded in the
research note; their claims were checked locally. M18 and editor/export/network
gates remain open.
