# ADR-0092: Transactional two-sample thermal contact

Status: proposed bounded CPU reference, 2026-10-18; no new default workload.

## Context

ADR-0091 supplies a fixed-mass water/ice energy/phase relation. The next thermal
brick must move heat between two samples without crossing thermal equilibrium,
committing only one side, or claiming that opposite requests alone conserve
stored binary64 energy. Equal-temperature mixtures can have different fractions.

## Decision

Add the allocation-free `fluids::exchange_heat` consumer. Caller supplies distinct
samples, conductance in W/K and nonnegative elapsed time. Backward Euler evaluates
the heat rate at the end of the step; solve the scalar positive-transfer equation
using immutable originals and copied trials. Different melting temperatures and
capacities are allowed. Neither temperature equality nor contact synchronizes
phase fractions. Zero conductance/time or exactly equal temperatures succeeds
unchanged after input/environment validation.

Use a bounded binary64 reference rather than a general nonlinear-solver framework.
The implemented residual compares `q/Q0` to `gap(q)/gap(0)`, with
`Q0 = round(round(G*dt)*gap(0))` and validated rounded temperature caches. Require
finite nonzero products. At most 128 midpoint trials narrow the sign bracket to
relative transfer width `2^-36`; commit its positive lower endpoint only after
both copied updates and actual temperature ordering validate. Rounded caches make
the function a staircase: the certificate is a sign bracket, not proof of an exact
zero or an exact-real constitutive solution. Underflow, overflow, an ineffective
or undecodable trial, unsupported arithmetic and iteration exhaustion refuse.
Temperature-domain limits refuse the entire contact rather than cap the law.
No claim is made that all finite inputs can advance.

Return both original `HeatExchange` receipts plus the positive magnitude bracket
and trial count. Their exact six-double identity is
`deltaA_hi + deltaA_lo + deltaB_hi + deltaB_lo + residualA + residualB = 0`.
Do not combine residuals into a rounded scalar, silently store them for later, or
claim exact conservation of stored energy alone. Every refusal returns empty
receipts and leaves both originals unchanged. Aliasing is invalid.

## Alternatives and consequences

Fable proposed equilibrium-capped forward Euler; Astra proposed backward Euler
with a material-limit cap and more elaborate scaled/ranked search. We chose
backward Euler for its temporal behavior, rejected limit clipping because these
limits bound model validity, and kept an explicit scale/iteration refusal contract
instead of a wider numerical framework. The independent proposals, disagreement,
local evidence and observed $0.32219 consultation cost are recorded in the
[research note](../research/2026-10-18-thermal-contact.md).

Tests exercise analytic sensible exchange, freezing/melting, whole phase
crossings, shared/different plateaus, zero controls, latent/conductance sensitivity,
first-order refinement, mass scaling and two-sided failure. Exact-fraction checks
run through actual GCC/Clang archives; deliberate partial-commit and lost-residual
mutants must fail. Python is an offline diagnostic dependency only.

This reference adds no geometry, diffusion grid, flow, expansion, renderer, editor,
networking or authority. Sequential pair updates are order dependent; an N-body
field needs its own ordering, bounds, accounting and timestep proof. M18 and the
existing editor/export/network gates remain open. An opt-in spatial field and
temperature/heat-source authoring are subsequent consumers.
