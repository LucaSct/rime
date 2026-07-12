# Rigid-body dynamics — integration, inertia, and why semi-implicit Euler

Companion to `engine/physics` (M7, ADR-0026). This note derives exactly the math M7.1 uses:
how a rigid body's state is advanced one timestep, how its mass distribution (the inertia
tensor) is computed for our primitive shapes, and why we integrate the way we do. Collision
response (the sequential-impulse solver) gets its own note at M7.4.

## 1. State and the equations of motion

A rigid body's state is a position **x** (of its centre of mass), an orientation quaternion
**q**, a linear velocity **v**, and an angular velocity **ω** (world space). Newton–Euler:

$$ \dot{\mathbf{x}} = \mathbf{v}, \qquad \dot{\mathbf{v}} = \mathbf{g} + \tfrac{1}{m}\mathbf{F} $$
$$ \dot{\mathbf{q}} = \tfrac{1}{2}\,\boldsymbol{\omega}_q \otimes \mathbf{q}, \qquad
   \dot{\boldsymbol{\omega}} = I^{-1}\left(\boldsymbol{\tau} - \boldsymbol{\omega}\times I\boldsymbol{\omega}\right) $$

where **g** is gravitational acceleration, **F**/**τ** are accumulated force/torque, $m$ is
mass, and $I$ is the world-space inertia tensor. Gravity enters as an *acceleration* (it is
mass-independent — Galileo), which is why the code adds `g` straight to the velocity and never
divides it by mass.

**M7.1 scope:** no contacts yet, so **F** = 0 and **τ** = 0. The gyroscopic term
$\boldsymbol{\omega}\times I\boldsymbol{\omega}$ is dropped in v1 (its explicit integration is
famously unstable, and torque-free debris does not need the tennis-racket theorem — an implicit
gyro solve is a backlog item). So angular velocity only decays by damping, and the interesting
part of M7.1 is integrating **x**, **v**, and **q**.

## 2. Why semi-implicit (symplectic) Euler

Explicit (forward) Euler advances both quantities from *old* values:

$$ \mathbf{x}_{n+1} = \mathbf{x}_n + \mathbf{v}_n\,\Delta t, \qquad
   \mathbf{v}_{n+1} = \mathbf{v}_n + \mathbf{a}\,\Delta t. $$

It systematically *injects* energy: for a spring–mass oscillator its trajectory spirals
outward, and a stack of bodies buzzes and never settles. Semi-implicit (a.k.a. symplectic)
Euler makes one change — update velocity **first**, then advance position with the **new**
velocity:

$$ \mathbf{v}_{n+1} = \mathbf{v}_n + \mathbf{a}\,\Delta t, \qquad
   \mathbf{x}_{n+1} = \mathbf{x}_n + \mathbf{v}_{n+1}\,\Delta t. $$

This is *symplectic*: it conserves a nearby "shadow" energy, so oscillators stay bounded and
resting bodies stay at rest. That energy-stability is precisely what a physics engine needs
(debris that settles and sleeps, not debris that jitters forever), and it costs nothing extra.
It is the near-universal choice in game physics for this reason.

### The closed form the test checks

For constant acceleration $a$ (free fall, $a=-g$) starting from rest, semi-implicit Euler gives
$\mathbf{v}_n = -g\,n\,\Delta t$ (which equals the analytic $-g\,t$ **exactly** at $t=n\Delta t$)
and, summing the per-step drops $\mathbf{x}_n = -g\,\Delta t^2 \sum_{k=1}^{n} k$,

$$ y_n = -\,g\,\Delta t^{2}\,\frac{n(n+1)}{2}. $$

That is the discrete curve `integration_test.cpp` asserts against — it differs from the
continuous $-\tfrac12 g t^2$ by exactly $\tfrac12 g \Delta t\, t$ (an $O(\Delta t)$ bias, ≈2 cm
at $t=1\text{s},\,\Delta t=1/240$). We check the exact discrete form tightly and the continuous
form loosely, so the test proves the *integrator*, not a fudge factor.

## 3. Integrating the orientation quaternion

The quaternion kinematic equation is $\dot{\mathbf{q}} = \tfrac12\,\boldsymbol{\omega}_q\otimes\mathbf{q}$,
where $\boldsymbol{\omega}_q = (\omega_x,\omega_y,\omega_z,0)$ is angular velocity written as a
*pure* quaternion and $\otimes$ is the Hamilton product. Intuition: multiplying by
$\tfrac12\boldsymbol{\omega}_q$ rotates **q** infinitesimally about the world axis
$\boldsymbol{\omega}$; the $\tfrac12$ is the quaternion half-angle showing up again (a unit
quaternion encodes $\cos\tfrac\theta2,\ \hat{\mathbf n}\sin\tfrac\theta2$).

A first-order step is therefore

$$ \mathbf{q}_{n+1} = \operatorname{normalize}\!\left(\mathbf{q}_n +
   \tfrac12\,\boldsymbol{\omega}_q\otimes\mathbf{q}_n\,\Delta t\right). $$

The addition nudges **q** off the unit 3-sphere, so we renormalize every step (cheap, and one
of the reasons quaternions beat rotation matrices, which need a full re-orthonormalization). For
the large per-step rotations a fast spinner can accumulate, an exact exponential map
$\mathbf{q}_{n+1} = \exp(\tfrac12\boldsymbol{\omega}\Delta t)\otimes\mathbf{q}_n$ is more
accurate; v1 uses the linearized form (simpler, and the error is a renormalized direction, not
energy) and leaves the exponential map as a documented refinement.

## 4. Inertia tensors of the primitive shapes

The inertia tensor $I=\int_V \rho\,(\lVert\mathbf r\rVert^2\mathbf 1 - \mathbf r\mathbf r^{\!\top})\,dV$
measures resistance to angular acceleration. For our primitives in their local, principal frame
it is diagonal, so three numbers $(I_x,I_y,I_z)$ suffice (`MassProperties::inertia_diagonal`).
For total mass $m$ (uniform density):

- **Solid sphere**, radius $r$: $\;I_x=I_y=I_z=\tfrac{2}{5}m r^{2}$.
- **Solid box**, half-extents $(h_x,h_y,h_z)$ (full sides $2h$):
  $\;I_x=\tfrac{1}{12}m\big((2h_y)^2+(2h_z)^2\big)=\tfrac{1}{3}m(h_y^2+h_z^2)$, and cyclically
  for $I_y,I_z$.
- **Capsule ≈ solid cylinder (v1)**, axis = local $y$, radius $r$, half-height $hh$:
  $\;I_y=\tfrac12 m r^2$ (about the axis), $\;I_x=I_z=\tfrac{1}{12}m\big(3r^2+(2hh)^2\big)$.
  The hemispherical caps add a small parallel-axis term folded in at M7.9; v1 documents the
  approximation rather than hiding it.

The solver (M7.4) uses the **inverse** inertia; static and kinematic bodies get zero inverse
mass and inertia — the "infinite mass" convention, so an impulse never moves them. General
convex hulls (M7.9) compute a full tensor from the polyhedral mass integrals and diagonalize it
to a principal-axis frame; the three-number storage here already anticipates that.

## 5. Determinism

Every operation above is a fixed sequence of IEEE-754 float ops with no reordering, no RNG, and
no unordered iteration — so the same inputs and the same binary produce bit-identical state,
which `integration_test.cpp` asserts run-to-run (and M7.5 will assert across thread counts once
the step is parallel). This is the property M11 replays destruction against; it is a design
constraint from the very first brick, not an afterthought. Cross-*platform* determinism is a
non-goal (ADR-0026).
