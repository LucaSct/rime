# Two-sample water/ice heat exchange, 2026-10-18

The goal is one isolated fixed-mass conductive contact on top of ADR-0091, before
spatial thermal fields or editor authoring. No third-party source was imported.

## Physical and numerical contract

A lumped thermal model assigns a single temperature to each sample. The contact's
heat rate is proportional to its temperature difference, with opposite signs at
the two ports; the supplied conductance includes geometry. This follows the
[COMSOL lumped-system two-port description](https://doc.comsol.com/6.4/doc/com.comsol.help.heat/heat_ug_theory.07.036.html).
It is an approximation for well-mixed samples, not resolved internal conduction.

Backward Euler evaluates the rate using the new state. It is first-order in time
and L-stable, as described in the [MOOSE time-integrator documentation](https://mooseframework.inl.gov/source/timeintegrators/ImplicitEuler.html).
For exact arithmetic, the isolated pair's scalar law is
`q = G dt [Th(Eh-q) - Tc(Ec+q)]`. The bracketed difference is nonincreasing with
transfer, including latent plateaus; subtracting it from `q/(G dt)` is increasing.
That monotonicity is our derivation for a scalar bracket, not a claim that a pair
step solves a general heat equation. Exact equal temperatures cause zero flow
even when liquid mass fractions differ.

The implemented numerical problem explicitly uses rounded products and rounded
cached temperatures. `Q0 = round(round(G dt) * initial_gap)`; compare `q/Q0` with
`gap(q)/initial_gap`. Both products must be finite and nonzero for positive demand.
The search takes at most 128 trials and accepts the lower positive sign endpoint
when bracket width is at most `upper * 2^-36`. Actual committed temperatures
cannot reverse their initial order. Because caches are rounded, a sign change can
be a jump, rather than an exact zero. The bracket certifies this numerical
problem, not a precision-independent constitutive solution.

Either side's range/numerical failure can refuse the whole call. Range trials can
bound search, but cannot certify a positive residual. If the final upper boundary
is still a temperature-domain refusal, the call refuses rather than clips heat.
An unrepresentable trial refuses immediately. Extreme exponent spans can exhaust
the fixed search budget. Near equilibrium, positive demand below an energy ulp
refuses explicitly; it is not relabeled physical zero flow.

Both samples are copied and validated before either original is assigned. The
returned receipts request `-q` and `+q`. Each exact stored-energy subtraction has
two components, and each receipt retains its update residual. All six returned
components sum exactly to zero as real values of doubles. The two residuals are
not silently accumulated or rounded into one scalar. A diagnostic witness with
masses 1 and 1,000,000 kg, a binary-exact synthetic material and G=2 W/K has a
nonzero stored-total change. Thus equal requests alone do not prove conservation.

## Independent consultations and disagreements

Both calls were raw, tool-free devpass chat, backgrounded with narrow prompts,
before implementation. Neither consultant saw the final diff or independently
reviewed its code. The root agent owns implementation and verification.

Availability/prices and credit caps were checked live first. Both canonical models
were available at $10/M input and $50/M output. Fable 5.1 cost $0.24645; GPT-6
Astra cost $0.07574: total $0.32219, below the approved task ceiling. Prompts,
answers and usage metadata are retained in the verification folder; credentials
and private account balances are excluded.

Fable proposed a breakpoint-bounded, equilibrium-capped explicit step. It usefully
emphasized first-contact plateau handling, no clipping of validity limits,
transactional refusal and phase-independent temperature equality. We declined its
time law: an explicit cap reaches equilibrium prematurely at intermediate steps.
Its suggestion that backward Euler needs a second unknown is unnecessary: q alone
determines both energies. Its suggestion to treat ineffective nonzero demand as
convergence is also not our API: it remains an explicit numerical refusal.

Astra proposed a monotone backward-Euler root, realized-state ordering checks,
two residuals, sign/bracket accounting, and atomic two-copy commit. Those informed
this contract. We declined its physical temperature-limit cap, because an authored
validity interval is not an energy sink. Its ranked-bit search and scaled interval
arithmetic could support a wider domain but are deferred; this reference reports
explicit finite-product and bounded-iteration refusals instead. Agreement between
models was treated as a hypothesis, not measurement.

## Local verification and limits

Portable tests compare a binary-exact sensible case to its closed-form
backward-Euler result; unequal masses, swapped argument order, mass scaling,
different melting points and large accepted steps must behave consistently.
Freezing/melting tests include a zero-contact control, doubled latent heat
(half the changed fraction at the same heat), and doubled conductance (more heat).
Step refinement approaches the analytic continuous single-phase exponential at
first order, distinguishing temporal accuracy from equilibrium stability.

An offline Python Fraction diagnostic marshals bit patterns through the actual
compiled archive, then checks exact stored deltas, each receipt, the six-component
pair identity, accepted lower/upper sign certificates, bounds, order and atomic
refusals. Deliberate partial-commit and lost-residual runtime mutants are rejected.
GCC and Clang runtime builds are compared independently. The dated
[verification report](../verification/thermal-contact-2026-10-18/README.md)
records exact counts, hashes, sanitizer results and the inherited full-suite failure.

No spatial freezing, heat advection, water/solid mass exchange, thickness,
collision, expansion or visual ice is implemented here. Sequential isolated pair
updates would be order dependent in a field; no network determinism or enabled
frame-budget claim follows from these CPU tests.
