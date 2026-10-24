## Proposed contract

Use **backward-Euler conduction**, with a temperature-limit cap. It is dissipative, stable for arbitrarily large finite `dt`, and does not require geometry or phase-fraction synchronization.

For distinct samples, determine the hotter sample \(H\) and colder sample \(C\) from their enthalpy-derived temperatures. If temperatures are equal, return `NoFlow`, even if phase fractions differ. Also return `NoFlow` for exactly zero `G` or `dt`, without evaluating their product.

Otherwise, for positive transfer \(q\), define

\[
D(q)=T_H(E_H-q)-T_C(E_C+q),\qquad
F(q)=q-G\,dt\,D(q).
\]

The unconstrained transfer is the unique root of \(F\): \(D\) is continuous and nonincreasing, so \(F\) is strictly increasing, including through latent-heat plateaus. Large timesteps approach thermal contact without crossing it.

Do not compute this equation using an ordinary potentially overflowing `G*dt`.

## Limits and bounded solve

Let \(q_{\rm lim}\) be the exact available hot-side cooling/cold-side heating allowance, whichever is smaller. Recommend an explicitly **limit-capped** law:

- If \(q_{\rm lim}=0\), return `BlockedByLimit`, not `NoFlow`.
- If \(F(q_{\rm lim})\le0\), target \(q_{\rm lim}\) and report `LimitCapped`.
- Otherwise target the root inside \([0,q_{\rm lim}]\).

Differing melting points need no special cases. Equality plateaus must not be traversed to equalize fractions: once \(D=0\), positive additional transfer cannot satisfy this law.

Use binary64 candidate transfers, bounded by exact prospective-energy admissibility. Binary-search their ordered nonnegative bit-pattern ranks; at most 64 subdivisions reach adjacent candidates, even across enormous exponent ranges. Never use midpoint arithmetic that can overflow or stagnate.

Distinguish the physical cap from the largest representable transfer. If the solution exceeds binary64 transfer range, refuse `Unrepresentable`; do not silently substitute `DBL_MAX`.

Evaluate signs and energy-bound comparisons with bounded, exponent-scaled interval/error-free arithmetic. Use a fixed refinement budget; if a required sign remains ambiguous, refuse `NumericallyIndeterminate`. No unbounded precision or runtime rational framework is necessary.

## Acceptance and transaction

Return a root bracket and its width as the numerical certificate. Accept a positive, no-crossing candidate only when the bracket meets a documented energy tolerance,

\[
q_{\rm high}-q_{\rm low}\le
\varepsilon_{\rm abs}+\varepsilon_{\rm rel}q_{\rm high}.
\]

Adjacent doubles alone are not convergence, particularly near underflow. Neither tolerance nor rounding may turn positive physical demand into `NoFlow`: if no positive acceptable candidate exists, refuse `Unrepresentable` or `AccuracyNotMet`. Do not use a temperature epsilon to identify equality.

Apply `-q` and `+q` to copies. Retain both receipts. Refuse if either operation refuses. Then validate both cached states, limits, heat directions, and absence of **realized** temperature-order reversal; requested-transfer safety alone is insufficient after rounding. Commit both through a nonthrowing commit path only after every check succeeds.

Reject aliasing explicitly. Validate finite, nonnegative `G` and `dt`; unsupported floating-point environments also refuse. Every refusal leaves both originals unchanged.

## Accounting

For receipts \(r_H,r_C\), retain the exact expansion

\[
R=r_H.\mathrm{roundoff}+r_C.\mathrm{roundoff}.
\]

Because the requests are exactly opposite,

\[
\Delta E_H+\Delta E_C+R=0
\]

as real values of doubles, with each stored-energy change represented by its receipt’s `applied_hi + applied_lo`. Never collapse this certificate into a rounded scalar or claim exact stored-energy conservation.

## Adversarial tests

Test latent-plateau endpoints; equal temperatures with unequal fractions; different melting points and limits; limit-blocked flow; huge products and underflowing products; subnormal transfers; asymmetric energy ulps; one-sided refusal; realized overshoot; adjacent-double brackets; exhausted sign refinement; aliasing; swapped arguments; rollback; and receipt identities using an independent exact-arithmetic test oracle.
