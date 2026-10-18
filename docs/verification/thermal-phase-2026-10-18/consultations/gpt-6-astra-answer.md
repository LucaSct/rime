## Proposed bounded oracle

Use **specific enthalpy \(h\), J/kg**, as the authoritative binary64 state, referenced to solid water at melting temperature. Mass and parameters are immutable. Never evolve temperature or phase fraction independently.

For \(T_m,c_s,c_l,L\):
\[
(T,x)=
\begin{cases}
(T_m+h/c_s,\;0),&h<0\\
(T_m,\;h/L),&0\le h\le L\\
(T_m+(h-L)/c_l,\;1),&h>L.
\end{cases}
\]
Here \(x\) is liquid **mass** fraction. Thus \(h=0\) and \(h=L\) are distinct, exact phase endpoints.

Require finite \(m,c_s,c_l,L>0\), positive finite temperatures, and \(T_{\min}<T_m<T_{\max}\). Accept the supplied defaults. Compare state against:
\[
h_{\min}=c_s(T_{\min}-T_m),\qquad
h_{\max}=L+c_l(T_{\max}-T_m).
\]

## Numerical and transaction semantics

For this small reference, I propose **exact dyadic/rational intermediates**, implemented locally with bounded integer storage, followed by explicitly specified round-to-nearest, ties-to-even conversions. Binary64 inputs represent exact dyadic values. This avoids accidental overflow/underflow in products, differences, \(Q/m\), and decoding. Integer-capacity exhaustion is an explicit refusal—not silent approximation.

For heat input \(Q\), calculate the exact target \(h+Q/m\), then round once to binary64. For power, require finite \(P\), finite \(dt\ge0\), and use the **exact product** \(Q=Pdt\), without an intermediate binary64 conversion.

Before committing:

- Reject invalid inputs, exact targets outside the declared interval, nonfinite rounded states, or rounded states outside the interval.
- Decode using exact intermediates and correctly rounded outputs.
- Require an interior enthalpy \(0<h<L\) to produce \(0<x<1\); otherwise refuse as unrepresentable. Never silently turn an interior mixture into an endpoint.
- If nonzero \(Q\) rounds back to the old \(h\), refuse as **heat below state resolution**. This includes underflow to an unchanged zero.
- Commit state and ledger together only after every check succeeds.

Valid zero heat—including either signed zero, zero power, or zero duration—is a bitwise state-and-ledger no-op. Still validate all arguments first. Canonicalize stored enthalpy zero to \(+0\) at initialization. Require gradual underflow; reject unsupported execution environments, including fast-math semantics.

Large steps cross both endpoints directly; no phase-stepping loop is needed. Extreme finite inputs are not automatically valid: conversion, representability, and storage-capacity limits remain explicit domain boundaries.

## Energy ledger

For each accepted transaction record exactly:
\[
\Delta U=m(h_{\mathrm{new}}-h_{\mathrm{old}}),\qquad
R=Q-\Delta U.
\]

Maintain exact cumulative accepted input \(A\) and rounding residual \(R_\Sigma\):
\[
A=m(h-h_0)+R_\Sigma.
\]
Use bounded exact dyadic storage; exhaustion refuses atomically. Residual is **not subsequently reapplied**. Rejected input contributes nothing. This distinguishes requested energy from representable stored energy.

## Eight adversarial tests

1. NaN, infinities, nonpositive parameters, negative duration: refusal with unchanged state/ledger.
2. Both signed zeros: bitwise invariance.
3. Exact endpoints and adjacent representable enthalpies: correct branches and strict interior fractions.
4. One-step heating and cooling across both endpoints: direct-map agreement.
5. Sub-ulp heat and halfway ties: specified refusal or ties-to-even result.
6. Extreme masses/parameters, subnormal values, overflowing intermediate products: exact-reference agreement or documented refusal.
7. Heat/cool cycles: verify ledger exactly; **do not require bitwise reversibility**, because rounding loses information.
8. Change only \(L\): melting plateau energy must change by \(m\Delta L\), with sensible-heat slopes unchanged.

Use exact checks for decisions and ledger identities; correctly rounded outputs must match bitwise. Avoid blanket epsilons.

This first oracle must own numerical safety, endpoints, accounting, and atomicity. Real-water accuracy outside the declared model, spatial gradients, pressure effects, transport, mechanics, and coupling conservation require explicit exclusions or a later coupled solver.
