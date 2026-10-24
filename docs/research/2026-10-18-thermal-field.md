# Bounded thermal graph — 2026-10-18

The field is a removable CPU consumer of the existing sample and contact
references. Caller-supplied links represent spatial adjacency and conductance;
there is no automatic grid or geometric contact generation yet.

## Method and scope

The heat equation can be expressed as transient diffusion and discretized over
finite volumes. NIST's [FiPy diffusion example](https://pages.nist.gov/fipy/en/latest/generated/examples.diffusion.explicit.mesh1D.html)
demonstrates this formulation and comparison to an analytic solution. It is a
method reference, not an imported dependency or evidence for Rime's method.
Our field uses ordered pair updates, not FiPy's solver or its boundary conditions.

For each substep, apply constant signed sources in cell order, then undirected
contacts in canonical lexicographic order. Each contact solves its own bounded
backward-Euler problem using the staged state. Composing these first-order steps
is a first-order graph reference; it is not global backward Euler. Canonical
input descriptions produce the same order locally, but relabeling nodes changes
the order. Source-first splitting also means a source can hit a domain limit
before conduction would have removed the heat. Such a tick refuses, rather than
guessing an alternative schedule.

Two preallocated cell/journal buffers make whole-tick rollback allocation-free.
Every admitted operation is recorded, including zero sources and unchanged
contacts. Descriptors and powers remain immutable. Positive ticks replace the
published journal; zero steps and failures retain it. Callers needing history
must consume or copy each journal before the next successful positive tick.

Exact source accounting has three separate layers. The stored-energy delta plus
all primitive update residuals equals the rounded `P*h` requests. Adding exact
product differences `P*h - rounded(P*h)` gives the ideal products for the actual
substeps. Adding `P*(dt-N*h)` gives ideal `P*dt` for the requested duration.
The latter two are verified offline from retained operands; the runtime does not
claim that the primitive receipt contains them or silently accumulate residuals.

## Refinement witnesses

For a three-node, equal-capacity chain, initialize temperatures `[300,288,276] K`,
capacity 4 J/K per node and conductance 1 W/K per edge. The exact graph solution
is `288 + 12 exp(-t/4) * [1,0,-1]`. Halving time steps from 1/16 to 1/32 seconds
reduces RMS error by approximately two; reversed node labels expose the changed
split order. Tests explicitly require both behaviors.

The spatial witness is a unit-length insulated rod with density 1, area 1,
specific heat 4 and conductivity 1 in consistent SI units. For N uniform cells,
`dx=1/N`, mass is `dx` and each interior conductance is `1/dx`. Both ends have no
external contact. Its initial temperature is the cell average of
`288 + 8 cos(pi*x)`. Direct differentiation gives the continuum solution
`288 + 8 exp(-pi²*t/4) cos(pi*x)` and zero end gradients. Cell averaging multiplies
the cosine by `sin(pi*dx/2)/(pi*dx/2)`. This derivation is our analytic oracle.
The entire case remains liquid; it does not test a moving phase front.

At t=0.05 s, with h=0.1*dx^4, observed RMS temperature errors are:

| Cells | RMS error, K |
| --- | --- |
| 4 | 0.0312724 |
| 8 | 0.00808035 |
| 16 | 0.00203663 |
| 16, h halved | 0.00200659 |

Successive mesh error ratios are about 3.87 and 3.97. Further halving h changes
the fine result's error by about 1.5%, below the explicit 10% contamination bound.
The test uses fixed parameter choices and observed convergence, not a theorem for
all fields, stiffnesses or phase configurations. Large stable pair steps alone
are not sufficient for spatial or temporal accuracy.

## Independent consultation

Before implementing the API, `gpt-6-astra` supplied its own tool-free proposal over
devpass; it did not see or review the final diff. Availability/pricing and actual
credit caps were checked live first. The narrow background call's response usage
reports $0.06254. The shared gateway's before/after increase was $0.10313; the
remaining $0.04059 cannot be attributed from this response. Conservatively record
the full observed increase against this task's spend ceiling rather than claiming
that the account delta matched our one call.
Fable's preceding contact consultation was already available; no new Fable call
was needed for this bounded consumer.

Adopted: immutable topology/sources, bounded staged state, unaggregated primitive
receipts, retained source operands, integer tick ordinal, actual extrema checks,
late rollback, exact journal telescoping and explicit order-dependence tests.
Its suggested explicit tick-plan API was simplified to a validated dt with a
configured substep bound. We deliberately journal zero descriptors rather than
skip them, so admission and operation identities remain explicit.

Astra suggested h proportional to dx^8 for the spatial witness. We declined that
cost and measured dx^4 with a further time-refinement control instead. The table
above supports that narrower experiment; it is not agreement between models.
No generic sparse nonlinear solver, runtime rationals or third-party code was added.

The [verification report](../verification/thermal-field-2026-10-18/README.md)
retains test/diagnostic results, hashes, fault controls and actual parent CI
failures, including the separately fixed Debug test warning.
