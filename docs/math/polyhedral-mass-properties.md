# Polyhedral mass properties — volume, centre of mass, inertia, principal axes

*The math behind `register_hull` (M7.11, ADR-0027): how `engine/physics/src/hull.hpp` turns an
authored convex polyhedron into the mass, centre of mass, and inertia the solver needs — exactly,
from closed forms, with no sampling.*

A rigid body's response to forces is fully described by three quantities of its mass
distribution: total mass `m`, centre of mass (COM), and the **inertia tensor** `I` — the 3×3
symmetric matrix in `τ = I·ω̇` that says how hard the body is to *spin* about each axis. The
primitives have textbook formulas (`rigid-body-dynamics.md`); a convex hull is an arbitrary
polyhedron, so we must actually integrate over its volume. The whole game is turning volume
integrals into a sum of per-triangle closed forms.

## 1. Signed tetrahedra: any polyhedron is a sum of tets through the origin

Fan-triangulate every face and, for each surface triangle `(a, b, c)` (wound outward), form the
tetrahedron `(0, a, b, c)` with the **origin** as the fourth vertex. Its *signed* volume is

```
V_tet = det[a b c] / 6 = a · (b × c) / 6,
```

positive when the triangle's outward side faces away from the origin, negative when it faces
toward it. Summing over all surface triangles, the regions outside the solid are counted once
positively and once negatively and **cancel exactly**, leaving the enclosed volume — wherever the
origin sits, even outside the body. This is the divergence theorem in discrete form: the volume
integral of `∇·(x/3) = 1` becomes a sum of flux terms over the faces, one per triangle.

The same cancellation works for *any* integrand that we can integrate over one tetrahedron in
closed form, because each tet's contribution scales with its signed volume. So we need exactly
one tool: integrals of polynomials over a tetrahedron.

## 2. Closed forms over one tetrahedron

Parametrize the tet `(0, a, b, c)` by barycentric coordinates: `x = u·a + v·b + w·c` with
`u, v, w ≥ 0`, `u + v + w ≤ 1`. The Jacobian of this map is `det[a b c] = 6·V_tet`, so any
integral becomes a *standard-simplex* integral times `6V`:

```
∫_tet f(x) dV = 6V · ∫∫∫_{u+v+w≤1} f(u·a + v·b + w·c) du dv dw.
```

The simplex integrals of the monomials are classical (integrate iteratedly):

| integrand | ∫ over the standard 3-simplex |
| --- | --- |
| `1`         | `1/6`  |
| `u`         | `1/24` |
| `u²`        | `1/60` |
| `u·v` (u≠v) | `1/120`|

**First moment** (→ centroid). By linearity and the `u` row:

```
∫_tet x dV = 6V · (a + b + c) · 1/24 = V · (a + b + c) / 4,
```

which is just "the centroid of a tet is the average of its four vertices" (`(0+a+b+c)/4`) times
its volume.

**Second moment.** The engine accumulates not the inertia tensor directly but the **covariance
matrix** `C = ∫ x xᵀ dV` — it is the form with the tidiest closed form and the tidiest shift rule
(§4). Expanding `x xᵀ = Σᵢⱼ eᵢeⱼ · vᵢvⱼᵀ` over the tet's parametrization and applying the table:

```
∫_tet x xᵀ dV = 6V · [ (a aᵀ + b bᵀ + c cᵀ)/60  +  (mixed pairs)/120 ]
             = V/20 · (a aᵀ + b bᵀ + c cᵀ + s sᵀ),      s = a + b + c,
```

using `s sᵀ = Σᵢ vᵢvᵢᵀ + Σᵢ≠ⱼ vᵢvⱼᵀ` to absorb the twelve mixed terms into one rank-1 product.
Six unique entries (the matrix is symmetric), a handful of multiplies per triangle — that is the
inner loop of `integrate_polyhedron`.

## 3. Covariance → inertia

The inertia tensor about the origin is, entry by entry, `I_xx = ∫(y² + z²) dm`,
`I_xy = −∫ x·y dm`, etc. Every one of those is a combination of covariance entries:

```
I = tr(C)·𝟙 − C.
```

(Check the diagonal: `tr(C) − C_xx = ∫(x²+y²+z²) − ∫x² = ∫(y²+z²)` ✓; the off-diagonals are just
negated covariances ✓.) So we integrate the *covariance* once and read the inertia off at the
end — and gain the shift rule below for free.

## 4. Re-centring: the parallel-axis theorem as one rank-1 update

Everything above was measured about the *authored origin*, which is wherever the fracture tool
happened to put it. Physics wants everything about the **COM** `d = (∫x dV)/V`. In covariance
form the shift is a single subtraction:

```
∫ (x − d)(x − d)ᵀ dV = C − M·d dᵀ          (M = ∫dV, the mass at unit density)
```

(expand, and use `∫x dV = M·d`). This *is* the parallel-axis theorem — apply `I = tr(C)·𝟙 − C`
to both sides and the classic `I_com = I_origin − M(|d|²𝟙 − d dᵀ)` falls out.

`register_hull` also **re-centres the stored vertices** on `d` (ADR-0027): the engine-wide
invariant "a body's `position` IS its centre of mass" — which every lever arm in the solver,
`apply_impulse`, and the integrator assumes — then holds for hulls by construction. The shift is
reported back through `HullInfo::centroid` so callers can align render geometry.

## 5. Principal axes: diagonalize once, stay diagonal forever

For the symmetric primitives the inertia tensor is diagonal in the body frame, and the solver
exploits that: it stores three inverse moments and applies `I⁻¹ = R·diag⁻¹·Rᵀ` with two
quaternion rotates (`apply_inv_inertia`). A general hull's tensor has off-diagonals — but every
real symmetric matrix is diagonal in *some* orthonormal basis (the spectral theorem). Those are
the **principal axes**: the directions a body can spin about without wobbling, with the
eigenvalues as the principal moments.

So registration solves the 3×3 symmetric eigenproblem once, by **cyclic Jacobi rotations**: pick
the off-diagonal pair `(p,q)`, apply the Givens rotation that zeroes `A[p][q]` exactly
(`tan 2θ = 2A_pq/(A_qq − A_pp)`, stable half-angle form), sweep the three pairs in a fixed order,
repeat. Each rotation may re-grow previously zeroed entries but the off-diagonal norm shrinks
monotonically and convergence is quadratic — a fixed 12 sweeps is far past `float` precision for
a 3×3, and a *fixed* count matches the determinism house style (no convergence early-outs,
ADR-0026). The accumulated eigenvector matrix is a product of rotations, so it is a proper
rotation (det +1) by construction — it converts to a quaternion with no handedness fix-up, and an
already-diagonal input (an axis-aligned box hull) yields exactly the identity.

The solver then stores, per body: the inverse principal moments (diagonal, as always) plus the
**principal→body quaternion** (identity for primitives), composing `q_body · q_principal` at
apply time. One extra quaternion product per inertia application buys a full tensor without
widening any storage or math to 3×3.

## 6. Mass vs density

All integrals run at unit density, so "mass" during integration is the volume. For a uniform
solid, inertia scales *linearly* with mass at fixed geometry: the store keeps
`inertia_per_mass = eigenvalues(C_com/V → I)` (the moments of a 1 kg body), and `create_body`
multiplies by the body's real mass. COM and principal axes are density-independent.

## 7. Verification pins (the closed forms the tests check against)

- **Box** (half-extents `h`): `V = 8·h_x·h_y·h_z`, `I/m = ((h_y² + h_z²)/3, …)`, principal =
  identity. A box-shaped hull must reproduce the `shape.hpp` primitive exactly
  (`tests/physics/hull_test.cpp`).
- **Regular tetrahedron**, vertices `(1,1,1), (1,−1,−1), (−1,1,−1), (−1,−1,1)` (edge `2√2`):
  `V = 8/3`, and by symmetry the tensor is isotropic with `I/m = a²/20 = 0.4`.
- **Rotated box**: authoring the vertices pre-rotated by `R` must yield moments equal to the
  box's (in some order) with principal ≈ `R` (up to axis permutation/sign) — verified
  *behaviourally*: the same off-COM impulse produces the same world-space `Δω` as the primitive
  box posed at `R`, whatever eigenbasis Jacobi picked.
