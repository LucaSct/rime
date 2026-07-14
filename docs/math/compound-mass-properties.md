# Compound mass properties — composing COM and inertia over child shapes

*The math behind `register_compound` (M7.12, ADR-0028): how `engine/physics/src/compound.hpp`
turns a list of posed convex children into the one mass, centre of mass, and inertia tensor their
rigid union carries — exactly, by composition, with no integration beyond what each child already
did for itself.*

A compound is several shapes welded into ONE rigid body. Rigidity is what makes the math easy:
mass properties are integrals over the occupied volume, and an integral over a union of disjoint
parts is the sum of the integrals over the parts. Every child already knows its own closed-form
answers (primitives from `rigid-body-dynamics.md`, hulls from `polyhedral-mass-properties.md`), so
the compound's answers are *sums of transformed child answers*. Three transforms appear, and each
is a one-liner of classical mechanics.

Throughout, child `i` has volume `V_i`, mass fraction `f_i` (see §0), its own COM at position
`c_i` in the compound's authored frame, and orientation `R_i` (a rotation matrix; the code carries
it as a quaternion). One quiet precondition does real work: **every child's stored form is already
centred on its own COM** — primitives by symmetry, hulls because *their* registration re-centred
them (ADR-0027) — so a child's COM in the compound frame is simply its authored position `c_i`,
and its own inertia is already the about-its-own-COM tensor. No per-child offset bookkeeping ever
appears.

## 0. Mass fractions: uniform density

v1 assumes one material across the children — exactly the destruction case, where the children are
the fracture cells of one wall. With uniform density `ρ`, child masses are `m_i = ρ·V_i`, and for
a body of total mass `M` each child carries the fraction

```
f_i = V_i / Σ_j V_j          (so m_i = f_i · M).
```

Volumes are closed forms: sphere `4/3·π·r³`, box `8·h_x·h_y·h_z`, capsule `π·r²·2h` — the v1
**cylinder** volume, deliberately matching the capsule's v1 cylinder *inertia* (shape.hpp): one
approximation, told once, upgraded together — and a hull's volume was integrated at its own
registration. Because everything below is per unit total mass (inertia scales linearly with mass
at fixed geometry), `M` never appears: a body just multiplies the stored per-unit-mass results by
its own mass, the same economy as hulls.

## 1. Centre of mass: the mass-weighted average

The COM of a union is the mass-weighted average of the parts' COMs — directly from the definition
`COM = (1/M)·∫ x dm` split over the parts:

```
COM = Σ_i f_i · c_i .
```

The stored child poses are then **re-centred**: `c_i ← c_i − COM`, so the compound's local origin
IS its centre of mass and the engine-wide invariant "a body's `position` is its COM" (every solver
lever arm, `apply_impulse`, the integrator) holds for compounds by construction. The shift is
reported as `CompoundInfo::centroid` so visuals can be aligned — the exact contract
`HullInfo::centroid` set.

## 2. Inertia: rotate each child's tensor, then shift it — the parallel-axis theorem

The compound's inertia about its COM is the sum of each child's inertia about that same point.
Each child's own data is a *diagonal* `J_i = diag(j_i)` — its per-unit-mass principal moments —
valid about the *child's* COM, in the *child's* principal frame. Two classical transforms carry it
to the compound's COM and frame:

**(a) Rotation.** A tensor expressed in a frame rotated by `R` becomes `R·J·Rᵀ`. Writing `R`'s
columns `r_m = R·e_m` makes the computation transparent:

```
R·diag(j)·Rᵀ = Σ_m j_m · (r_m · r_mᵀ),
```

each principal moment contributing a rank-1 outer product along its rotated axis
(`rotate_diagonal_inertia` in compound.hpp — six unique entries of a symmetric matrix, no general
matrix multiply). For a primitive child `R_i` is just its authored orientation; for a hull child
it composes the hull's own principal rotation first (`R_i = R_pose · R_principal`), because the
hull's diagonal lives in the hull's *principal* frame, not its local frame.

**(b) Translation — the parallel-axis theorem.** Moving the reference point of an inertia tensor
from a body's own COM to a point at offset `d` adds, per unit mass,

```
ΔI = |d|²·E − d·dᵀ          (E = identity).
```

Derivation in one line: about the new point, `I' = ∫ (|x−(−d)|²E − (x+d)(x+d)ᵀ) dm` expands into
the about-COM integral plus the `d` terms plus cross terms `∫x dm`, and the cross terms vanish
*because the integral is taken about the COM* — that is the theorem, and also why step (a) must
happen about the child's COM first. Diagonal entries gain the familiar `m·(d_y² + d_z²)` (distance
from the axis, squared); off-diagonals gain `−m·d_x·d_y`.

Summing with the mass fractions gives the compound's per-unit-mass tensor about its COM:

```
I/M = Σ_i f_i · ( R_i·diag(j_i)·R_iᵀ + |d_i|²·E − d_i·d_iᵀ ),      d_i = c_i − COM .
```

A worked sanity check the tests pin exactly (`compound_test.cpp`): two boxes of half-extents
`{0.25, 0.2, 0.3}` at `x = ±0.25` ARE one box of half-extents `{0.5, 0.2, 0.3}`. About y:
each child's own `(0.25² + 0.3²)/3 ≈ 0.0508` plus its shift `d² = 0.0625` sums to `0.1133` —
exactly the big box's `(0.5² + 0.3²)/3`. The theorem reproduces the monolithic answer to the last
digit, which is the whole point: composition is not an approximation.

## 3. Principal axes: the M7.11 machinery, reused

The composed `I/M` is a full symmetric 3×3 (children at off-axis offsets introduce products of
inertia even when every child is axis-aligned). The solver stores inertia as a diagonal plus a
principal-axis rotation (ADR-0027's representation decision), so registration diagonalizes once —
the same fixed-sweep cyclic Jacobi and Shepperd quaternion extraction as hulls
(`hull.hpp: jacobi_diagonalize / quat_from_columns`, derivations in
`polyhedral-mass-properties.md` §5). Fixed sweep count, first-wins ties, det +1 by construction:
identical registrations yield bit-identical moments and axes (ADR-0026). An already-diagonal
composition (a symmetric arrangement like the two-box bar above) passes through with the identity
rotation, bit for bit.

Bodies then instantiate the compound exactly as they do a hull: `inertia_diagonal =
inertia_per_mass · mass`, principal quaternion into the body pool's principal column, and the
solver's `R·diag·Rᵀ` inertia application needs no new code at all.
