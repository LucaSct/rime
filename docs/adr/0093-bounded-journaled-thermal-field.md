# ADR-0093: Bounded thermal graph with transactional ticks

Status: proposed CPU reference, 2026-10-18; no default frame workload.

## Context

ADR-0091 derives temperature and liquid mass fraction from fixed-mass energy;
ADR-0092 supplies an isolated conductive contact. A spatial consumer needs
explicit topology, external sources, work bounds and whole-tick accounting before
it can drive editor preview. Looping isolated contacts does not establish the
accuracy of an N-body solver or global backward Euler.

## Decision

Add a move-only `fluids::ThermalField` owning copied immutable descriptions and
fixed-mass samples. Caller supplies cell indices and geometry-derived links.
Normalize undirected links and sort by `(first,second)`; reject self/duplicate
links and invalid indices/conductances. Sources are constant signed powers, at
most one per cell, sorted by cell index. Zero descriptors remain explicit and
consume budgets; input permutation/orientation cannot change the operation order.
Cell relabeling can change it and is not a replicated identity contract.

Each positive `advance(dt)` chooses bounded uniform substeps from a configured
maximum duration, applies sources first and then contacts in canonical order.
This is a first-order ordered split using backward-Euler pair steps, not a
simultaneous implicit field solve. Whole-step versus subdivided calls can differ.
Temperature-domain limits refuse rather than act as thermostats or sinks.

Creation preallocates two cell buffers and two complete operation journals.
Disable copying so a vector copy cannot discard reserved capacities and introduce
later tick allocations. Preflight receipt counts and worst-case pair trial work
with checked bounds. Default budgets are 4,096 receipts and 65,536 pair trials;
hard caps are 256 cells, 1,024 links, 256 sources, 1,024 substeps and 16,384 receipts.
Every contact budgets 128 trials even if zero conductance or equal temperature.
Some otherwise valid descriptions cannot advance within the selected budgets.

Stage the entire tick and publish cells, integer tick ordinal, last-step metadata
and journal together only after success. No automatic retry/subdivision on
primitive refusal. Return the attempted operation count, failure substep and
descriptor, and underlying source/contact status. Refusals and zero dt preserve
all published state and the previous journal. Unsupported FP mode refuses even
zero dt. Tick is a checked ordinal, not a floating elapsed-time accumulator.

Each journal row retains primitive receipts and both before/after stored energies.
Keep them unaggregated. The field's exact stored-energy delta plus every update
roundoff equals the sum of rounded source requests; internal requests cancel.
Sources use rounded `P*h`, where `h=round(dt/N)`. Retained powers, `dt`, `h` and
`N` allow offline exact product and partition accounting all the way back to
ideal `P*dt`. Runtime receipts alone do not include those two rounding effects.
There is no hidden reservoir or cumulative scalar energy ledger.

## Consequences and verification

Tests exercise canonical replay, source-first order, zero/disconnected controls,
distinct phase reservoirs, multi-hop freezing/melting, late rollback including
the old journal, work/substep/receipt limits, move semantics and FP-mode refusal.
Source-free extrema are checked across substeps. Three-cell temporal refinement
and node relabeling expose first-order error and order dependence explicitly.

An insulated single-phase rod uses finite-volume mass/conductance and cosine cell
averages. Mesh refinement reduces RMS temperature error by about 4x; a further
time refinement controls splitting contamination. This is not a spatial phase-front
proof. The exact offline journal diagnostic interposes allocation calls and
checks telescoping, sources, multiplication/partition losses and rollback through
actual GCC/Clang archives. Deliberate partial-publication, missing-receipt and
tick-allocation mutants must fail.

Astra supplied an independent design proposal for $0.06254. The
[research note](../research/2026-10-18-thermal-field.md) records adopted advice and
the rejected timestep-scaling suggestion. Root owns the implementation and diff
review. No new code from third parties is imported.

There is no automatic mesh/grid construction, mass advection, water/ice geometry,
expansion, collision, rendering or network authority. Reflected temperature/source
authoring and Play/Stop reconstruction are next; flowing-water freezing still
needs separate liquid/solid mass and energy transport. Existing M18, clicked
editor, city convergence and shipment gates remain open.
