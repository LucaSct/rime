**Proposal: equilibrium-capped explicit exchange, breakpoint-bounded, refuse-don't-clamp.**

**Transfer law (v1).** For samples A (hotter, T_A>T_B) and B, with G≥0, dt≥0:

Q = min(G·dt·(T_A−T_B), Q*)

where Q* is the smallest energy such that T_A(E_A−Q*) = T_B(E_B+Q*). Apply −Q to a copy of A, +Q to a copy of B; commit both only if both apply_heat succeed. Because each sample's T(E) is piecewise-linear nondecreasing with ≤2 breakpoints (plateau start/end), f(Q)=T_A(E_A−Q)−T_B(E_B+Q) is piecewise-linear nonincreasing with ≤4 breakpoints. Q* is found by enumerating breakpoints in ascending Q and solving one linear equation on the first segment where f crosses ≤0. No Newton, no tolerance loop, no runtime rationals: bounded ≤5 segment evaluations. If f is flat-zero on an interval (only possible when both samples share Tm and are both mid-plateau), T_A=T_B already and Q=0 — this is exactly where equal fractions are *not* forced.

**Why capped forward Euler, not backward Euler root.** Both are first-order; both are unconditionally non-overshooting for two bodies. Backward Euler needs the same breakpoint machinery plus a second unknown (the G·dt factor inside f), and for dt→∞ it only approaches equilibrium asymptotically, so "large dt" tests can't be checked against a closed-form end state. The cap gives the exact two-body equilibrium in one step, is monotone in dt, and is auditable: every output lies in [T_B,T_A]. Honest cost: for intermediate dt it overestimates the rate relative to the exact exponential decay (reaches equilibrium when G·dt ≥ ~C_eff/2 rather than never). Neither scheme extends to N-body without ordering effects; defer that.

**Refusal contract.**
- G=0, dt=0, or T_A=T_B (including shared-plateau): success, zero receipts, both states bitwise unchanged.
- Negative/non-finite G or dt, or `&a==&b` (aliasing): refuse `InvalidArgument`, nothing touched.
- Either copy's apply_heat returns `TemperatureLimit` (differing configs: B's upper limit may be below A's current T, or A's lower limit above T_B): refuse `TemperatureLimit`, both unchanged. Do *not* silently clamp to limits — limits are config validity domains, not physics; caller shortens dt.
- Either copy returns `Unrepresentable` (Q nonzero but ineffective): refuse `Unrepresentable`. Do not apply to one side only.
- Round Q* toward zero by one ulp (`nextafter`) before use so rounding of T cannot produce a sign inversion from the cap itself.

**Receipt.** `{Q_requested, receiptA, receiptB, residual}` where residual = (A.hi+A.lo)+(B.hi+B.lo) computed with TwoSum so it is exact as a real value. State explicitly: stored E_A+E_B after commit differs from before by exactly `residual`; no exact-conservation claim.

**Six tests / negative controls.**
1. *Zero invariants:* G=0, dt=0, equal T (and shared-Tm both mid-plateau with fractions 0.3/0.9) → bitwise unchanged, receipts zero, residual 0.
2. *Analytic large-dt plateau:* 1 kg water 300 K + 1 kg ice 260 K, dt=1e30. Water releases 4179·26.85=112,206 J; ice needs 26,984 J to reach Tm; 85,222 J melts 0.2555 kg. Assert both at Tm, fractions 1.0 and 0.2555±1e-12, T_A'≥T_B'.
3. *Linear regime:* small dt such that Q ≪ Q*; assert Q = G·dt·ΔT exactly, A.hi+A.lo = −(B.hi+B.lo) + residual with residual ≤ 1 ulp of Q, and post-ΔT shrinks.
4. *Differing melting points:* A config Tm=280 K, 1 kg liquid at 285 K; B config Tm=270 K, 1 kg solid at 265 K; dt huge. Breakpoints at 280 (A freezes) and 270 (B melts). Assert T_A'=T_B' within 2 ulp, and bracket-by-bracket energies match hand calculation.
5. *Limit refusal + rollback:* B upper limit 290 K, A at 310 K, dt huge → `TemperatureLimit`; assert both states bitwise identical to pre-call (catches partial commit).
6. *Negative controls:* aliasing refuses; G=−1, dt=NaN refuse; Q=1e-300 effective-less → `Unrepresentable`, unchanged; and a constructed case (masses 1:3, odd energies) where residual ≠ 0 — test *fails* if someone later asserts exact stored conservation.

**Pitfalls.** Computing breakpoints in temperature space then converting back to energy loses exactness — do everything in energy space. T_A>T_B by 1 ulp with G·dt huge gives Q*≈0 and `Unrepresentable`; callers must treat that as "converged," not error-fatal — document it. Sequential pairwise exchange is order-dependent; not conservative beyond receipts. The cap is not time-accurate; don't tune G against it. Gradual underflow means tiny Q*·(1−ulp) can be denormal; apply_heat already handles it, but test near 1e-308.

**Limitations.** Two-body only; first-order; no contact geometry; no exact conservation; no sub-step shared-plateau fraction redistribution.
