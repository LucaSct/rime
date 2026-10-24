## Next brick: a bounded, journaled graph integrator

Yes: **canonical sequential pair backward Euler, with source-first substeps**, is a useful first-order graph reference. It is an ordered operator split—not global backward Euler, order-independent diffusion, or a precision guarantee.

### State, API, and clock

Construct a `ThermalField` with:
- Fixed cells containing the existing samples; immutable masses/configurations.
- A canonical sorted list of undirected contacts `(min_id,max_id,G)`. Reject self-edges, duplicates, invalid indices, and invalid conductances; permit explicit zero contacts.
- Immutable constant signed source powers, at most one per cell, sorted by cell ID.
- Immutable capacity limits and preallocated staging/journal storage.

Expose `try_tick(plan) -> status + committed_journal_view`. A plan specifies an integer substep count and one finite nonnegative binary64 substep duration. Bound both explicitly. Return distinct domain, range, FP-mode, work-budget, receipt-budget, and pair-convergence failures, including operation location.

Use a checked integer committed-tick ordinal. Record accepted durations, rather than repeatedly adding floating-point time. Do not imply their rounded sum is exact elapsed time. A zero-duration tick is a successful no-op: no clock, energy, or journal changes.

### Stepping contract

For each substep, apply sources in cell order, then contacts in canonical order, using updated staged states. Invoke the existing primitives unchanged.

Preflight worst-case work and receipt counts using checked arithmetic, including up to 128 pair trials per contact invocation. No tick allocations. Keep field state, clock, and published journal untouched until complete success; a late failure discards all staged effects. Diagnostic failure reporting is separate from the committed ledger.

No automatic timestep retries. Explicit retries must identify a new plan: subdivision changes splitting, rounding, receipts, and potentially success. Never promise identical results under different limits; promise identical successful results when limits merely admit the *same operations*.

Zero-power sources perform no operation. A nonzero power whose product underflows to zero is not a zero source.

### Energy and numerical claims

Define source requests as rounded `Q = power * h`, with checked multiplication. Distinguish:
1. Ideal real product of the two input doubles.
2. Rounded request.
3. Actual stored-energy change.

Record a validated exact product residual expansion, or equivalent retained operands with offline exact verification. Reject unsupported cases rather than hiding source multiplication loss.

Retain every primitive receipt unaggregated, with operation identity and before/after energies. Verify exactly:
`requested = stored_delta_hi + stored_delta_lo + update_roundoff`.
Opposite contact requests cancel; contact update roundoffs generally do not. The stored hi/lo sum alone is not the field delta.

Pairwise no-reversal alone does **not** prove an N-body maximum principle. Establish that every realized pair endpoint remains within its pre-operation temperature interval, including rounding and plateau cases. Composition then preserves source-free extrema. Sources invalidate that bound; temperatures alone cannot detect latent-energy defects.

### Decisive verification

- Multi-hop freezing/melting across several contacts: check latent fractions, propagation, and invariants.
- Disconnected components and zero-contact/zero-source controls: bitwise unchanged where appropriate.
- Exact offline journal telescope; inject failure at the final operation and verify complete rollback.
- Temporal refinement against a much-finer run: demonstrate first-order behavior, order sensitivity, and no claim of global BE equivalence.
- Insulated 1D cosine rod, entirely within one constant-heat-capacity phase. Use cell-average initial cosine data, `mass=rho*A*dx`, `G=k*A/dx`, and no boundary contacts. Compare against exact decaying cosine **cell averages** at common final times.

For spatial refinement, choose dimensionally normalized `h ∝ dx^8`, on modest affordable meshes. This deliberately suppresses splitting/time error relative to second-order spatial error; confirm by halving `h` again. Prebudget the increasingly expensive runs, and stop before roundoff or pair-solver tolerances dominate. Report observed second-order convergence, not an unconditional theorem about the implemented toleranced solver.
