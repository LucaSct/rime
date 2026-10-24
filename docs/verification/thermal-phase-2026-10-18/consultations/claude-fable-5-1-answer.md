## Proposal: enthalpy-primary equilibrium reference

**Core choice.** Store specific enthalpy *h* (J/kg, relative to solid at T_m) as the single thermodynamic state variable. Temperature and liquid mass fraction are *derived*, never integrated. This removes the two-phase ambiguity at T_m (where T alone does not determine phase) and makes energy accounting exact by construction.

### Parameters (immutable per sample, validated at construction)
`T_m`, `cp_s`, `cp_l`, `L_f`, `m`, `[T_lo, T_hi]`. Refuse: non-finite, `m ≤ 0`, `cp ≤ 0`, `L_f ≤ 0`, `T_lo ≥ T_hi`, `T_m ∉ [T_lo, T_hi]`. Precompute `h_lo = cp_s(T_lo−T_m)`, `h_hi = L_f + cp_l(T_hi−T_m)`.

### State
```
h   // J/kg, finite, in [h_lo, h_hi]
```
Nothing else. Derived on demand:
- `h < 0`: solid, T = T_m + h/cp_s, x_l = 0
- `0 ≤ h ≤ L_f`: mixed, T = T_m, x_l = h/L_f
- `h > L_f`: liquid, T = T_m + (h−L_f)/cp_l, x_l = 1

Expose `Result{T, x_liquid, phase_tag, h}`; also `energy() = m·h` for accounting.

### Initialization contract
`fromTemperature(T, x_liquid_hint = nullopt)`:
- T outside interval, non-finite → refuse.
- T ≠ T_m with a hint ≠ consistent phase → refuse (contradiction, not silently coerced).
- T == T_m: **ambiguous**; require hint in [0,1]; absent hint → refuse (`AmbiguousPhase`). Rationale: a default of 0 or 1 hides a 333.5 kJ/kg energy choice. Callers must be explicit.
- `fromEnthalpy(h)` is the canonical path; `fromTemperature` only maps onto it.

### Update contract
`step(Q_J, P_W, dt_s) -> Expected<Result, Error>`:
1. Validate: all finite, `dt ≥ 0`. `dt == 0` with `P ≠ 0` is allowed (contributes nothing); `dt < 0` refused.
2. `dE = Q + P·dt`; `h' = h + dE/m` computed in a local.
3. If `h' ∉ [h_lo, h_hi]` → refuse `OutOfModelRange`, state untouched (atomic). Do not clamp: clamping destroys energy conservation and silently falsifies the reference.
4. Commit, return Result.

Zero-input invariance: `Q=0, P=0` ⇒ `h' == h` bit-exactly (no multiply-by-dt path adds rounding). Enthalpy-primary gives this for free; a T-primary scheme with phase branching generally does not.

### Latent-heat sensitivity control
Expose a *test-only* factory accepting `L_f` override. The falsifiable claim: time/energy to fully melt from x_l=0 at T_m scales linearly in L_f and is independent of cp. A bug conflating sensible and latent terms fails this.

### Mass vs volume fraction — be explicit
`x_liquid` is **mass** fraction. Volume fraction requires densities (ρ_ice ≈ 917, ρ_water ≈ 1000) and is *not* provided here; at x_l = 0.5, volume fraction liquid ≈ 0.478. Name the field `liquid_mass_fraction` to block misuse by rendering code.

### Equilibrium reference vs spatial simulation
This is a lumped, well-mixed point model: one h, one T, instantaneous internal equilibrium. It does **not** produce ice fronts, surface-first freezing, undercooling, or spatial gradients. It is the *ground truth per cell/parcel* that a future spatial solver's cells must conserve against; it is not a freezing simulation.

### Coupling traps
1. Spatial solver advecting T instead of h → latent energy destroyed at phase boundaries. Transport h (or m·h).
2. Existing water solver assuming constant density/volume → when expansion lands, volume must derive from x_l and densities, not from this module.
3. Clamping at range edges in a wrapper → silent energy leak. Propagate refusal.
4. Variable mass later (evaporation) → h must be re-based as total energy / new mass; don't just add Q.
5. Framerate-dependent dt with large P → range refusal every frame. Callers should sub-step; this module should not.

### Six decisive tests (defaults, m = 1 kg)
1. **Ambiguity refusal:** `fromTemperature(273.15)` with no hint → error; with hint 0.3 → h = 100050, T = 273.15.
2. **Melt energy:** from h=0, `step(Q=333500)` → x_l = 1.0, T = 273.15, h = 333500. `step(+1)` more → T = 273.15 + 1/4179 ≈ 273.1502393.
3. **Cooling across transition:** from T=293.15 liquid (h = 333500 + 83580 = 417080), `step(Q = −400000)` → h = 17080, T = 273.15, x_l ≈ 0.05121.
4. **Atomic range refusal:** from T=258.15 solid (h = −30780), `step(Q = −20000)` → h' = −50780 < h_lo = −41040 → refused; subsequent `result().h == −30780` exactly.
5. **Zero invariance & accounting:** 10⁶ steps of `(0, 0, 0.016)` → h bit-identical; then `step(0, 1000 W, 2 s)` → h increased by exactly 2000 and `energy()` delta = 2000.
6. **L_f sensitivity:** with L_f doubled (667000), full melt from h=0 needs Q=667000; with cp_l doubled but L_f default, still 333500. Isolates latent from sensible paths.
