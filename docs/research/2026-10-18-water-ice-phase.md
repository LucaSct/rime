# Water/ice phase reference, 2026-10-18

The owner selected temperature-driven freezing and melting as ice's first result.
This brick establishes a fixed-mass equilibrium calorimetry reference. It does
not yet simulate an ice front, conduction, flowing water or visible geometry.

## Physical references and chosen approximation

COMSOL's public [ice-to-water example](https://doc.comsol.com/6.4/doc/com.comsol.help.models.heat.phase_change/phase_change.html)
lists solid/liquid heat capacities of 2052/4179 J/(kg K), at −8/27 °C, and fusion
energy 333.5 kJ/kg. These are the frozen coefficients for the first reference,
with melting at 273.15 K and an explicitly limited 253.15–313.15 K interval.
That interval is an engine model limit, not a claimed accuracy interval for
constant properties. The example also compares a latent-heat-disabled model.

The [phase-material documentation](https://doc.comsol.com/6.4/doc/com.comsol.help.heat/heat_ug_ht_features.09.036.html)
describes smoothed transitions and warns that density, volume and transport must
agree to conserve mass. Rime instead chooses a sharp equilibrium plateau for
this small analytic reference; a later spatial solver may need different
discretization. Liquid **mass** fraction is not liquid volume fraction.

For historical context, [Osborne's National Bureau of Standards paper](https://nvlpubs.nist.gov/nistpubs/jres/23/jresv23n6p643_A1b.pdf)
is dated December **1939**, despite a much later web indexing date. It reports
333.5 international joules per gram. It is not used here as a precision modern-SI
property database. [UCL's thermal notes](https://www.ucl.ac.uk/~zcapc57/Thermal_04.pdf)
give another classroom calorimetry reference with rounded coefficients.

## Independently chosen state and numerical contract

Store total energy `E` relative to all-solid material at melting temperature,
with immutable mass `m` and coefficients. This is a total enthalpy reference at
fixed pressure, not gravitational or kinetic energy:

```text
E < 0:       T = Tm + E/(m*cs),          liquid_mass_fraction = 0
0 <= E <= mL: T = Tm,                    liquid_mass_fraction = E/(mL)
E > mL:      T = Tm + (E-mL)/(m*cl),    liquid_mass_fraction = 1
```

Initialization requires both temperature and phase fraction. Contradictory
inputs refuse. Heat steps cross both phase endpoints directly. The configured
binary64 energy bounds refuse the whole prospective transaction; no accepted
energy is clipped. A zero input preserves the sample's bits.

An ordered FastTwoSum expansion captures addition/subtraction errors. See
[Shewchuk's research and code](https://www.cs.cmu.edu/~quake/robust.html) and the
inspectable sum helpers in [CPython's math module](https://github.com/python/cpython/blob/main/Modules/mathmodule.c).
These are algorithm/code study references; no third-party implementation is
imported. Rime's small helper is independently written, with a source comment
explaining the magnitude ordering and rounding preconditions.

The ledger returns a rounded applied amount, its low component and the update
residual. Their exact real-number sum equals the requested heat. There is no
runtime rational-number library or hidden reapplication of residual heat. A
separate Python `Fraction` diagnostic checks those identities exactly against
the built library. Power uses the rounded binary64 `P*dt`; multiplication
rounding is explicitly outside that transaction ledger.

Round-to-nearest binary64 and gradual underflow are required. Creation and
mutation probe the arithmetic mode; unsupported modes, overflow, ineffective
nonzero heat and unrepresentable interior phase fractions refuse. Derived
temperature/fraction is cached only after validation, so a getter needs no new
arithmetic. Temperature decoding may correct one final rounding bit at its
configured bounds; the underlying energy admission remains strict.

## Consultation findings, independently checked

The owner explicitly requested Fable/devpass and Astra as needed. Both ran as
read-only questions through the devpass gateway, with no tools or repository
access, low reasoning effort and capped output. Live pricing was $10/M input
and $50/M output tokens for both. Live credit checks preceded and followed the
calls; both fit within the user's approved task and premium allowance.
Reported and observed plan spend: **$0.1965 total**.

* **Fable 5.1 ($0.1102):** proposed specific enthalpy, explicit phase at melting,
  strict range refusal, and latent-heat/capacity sensitivity controls. Adopted
  the explicit construction and controls. Rejected its claim that using enthalpy
  alone makes floating-point accounting exact, and its suggestion that substeps
  cure a final out-of-range heat budget. Substeps cannot fix an unsupported final
  temperature. Its mass/volume distinction is retained.
* **GPT-6 Astra ($0.0863):** proposed exact dyadic/rational runtime arithmetic,
  explicit ledger residuals, strict interior phase representation and adversarial
  boundary checks. Adopted the safety requirements. Declined the runtime rational
  framework for this bounded first reference; a two-double expansion is enough
  for addition/transaction accounting, and exact fractions belong in the verifier.
  Do not generalize that choice to a future transport solver.

Both preferred specific enthalpy. Rime retains mathematically equivalent total
energy for this immutable mass to avoid a `Q/m` rounding stage and an `m*delta_h`
reconstruction in each heat transaction. No serialized format, mass-transfer
contract or spatial solver is committed to this representation yet.

Two numerical suggestions exposed real prototype defects: a requested target
just outside a bound could round onto it, and a positive interior energy could
decode to zero liquid fraction. New tests first failed against the prototype,
then passed with strict admission/representation guards. These are locally
verified findings, not reliance on model agreement.
