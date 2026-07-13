# Sequential impulses — contact response, friction, restitution, and why NGS

Companion to `engine/physics` (M7, ADR-0026). This note derives exactly the math the M7.4 solver
runs (`src/solver.hpp`): the contact constraint and its effective mass, the sequential-impulse
(projected Gauss–Seidel) iteration with Catto's accumulate-and-clamp, warm starting, the Coulomb
friction pyramid, restitution as a velocity bias, and the non-linear Gauss–Seidel **position**
pass — including the argument for why it is NGS and *not* Baumgarte stabilization. The inputs are
the contact manifolds of [`gjk-epa.md`](gjk-epa.md); the *systems* reasoning (pipeline order, the
cache, determinism plumbing) is in [`docs/design/physics.md`](../design/physics.md).

## 1. Contact as a velocity constraint

A contact point gives us (from the narrowphase): a point $\mathbf{p}$, a unit normal
$\hat{\mathbf{n}}$ pointing from body $a$ toward body $b$, and lever arms
$\mathbf{r}_a = \mathbf{p}-\mathbf{x}_a$, $\mathbf{r}_b = \mathbf{p}-\mathbf{x}_b$ from each
centre of mass. The material points touching there move with

$$ \mathbf{v}_{p,a} = \mathbf{v}_a + \boldsymbol{\omega}_a \times \mathbf{r}_a, \qquad
   \mathbf{v}_{p,b} = \mathbf{v}_b + \boldsymbol{\omega}_b \times \mathbf{r}_b, $$

and the **relative normal velocity** is
$v_n = (\mathbf{v}_{p,b} - \mathbf{v}_{p,a}) \cdot \hat{\mathbf{n}}$ — positive when separating
(the direction the normal points), negative when approaching.

Non-penetration is a one-sided (inequality) constraint, and we enforce it at the **velocity**
level: for a touching contact, demand $v_n \ge 0$ after the solve. Why velocities and not forces
or positions? In a discrete step, the only quantity that can stop an approach *within the step*
is an **impulse** — an instantaneous change of momentum. Forces need time to act; positions are
handled by their own pass (§8). And a contact can only ever *push*: writing $\lambda$ for the
scalar normal impulse, the physics is the complementarity condition

$$ \lambda \ge 0, \qquad v_n \ge 0, \qquad \lambda \, v_n = 0 $$

— either the bodies separate on their own ($\lambda = 0$) or the impulse holds them exactly at
$v_n = 0$ ($\lambda > 0$). Never both, never a pull.

## 2. One impulse: the effective mass

Apply an impulse $\mathbf{P} = \lambda\,\hat{\mathbf{n}}$ at the point — $+\mathbf{P}$ to $b$,
$-\mathbf{P}$ to $a$ (Newton's third law; total momentum is conserved by construction):

$$ \Delta\mathbf{v}_b = \tfrac{\lambda}{m_b}\hat{\mathbf{n}}, \quad
   \Delta\boldsymbol{\omega}_b = \lambda\, I_b^{-1}(\mathbf{r}_b \times \hat{\mathbf{n}}), \qquad
   \Delta\mathbf{v}_a = -\tfrac{\lambda}{m_a}\hat{\mathbf{n}}, \quad
   \Delta\boldsymbol{\omega}_a = -\lambda\, I_a^{-1}(\mathbf{r}_a \times \hat{\mathbf{n}}). $$

Substituting into $v_n$ shows the response is linear in $\lambda$: $\;v_n' = v_n + k_n \lambda$
with

$$ k_n \;=\; \frac{1}{m_a} + \frac{1}{m_b}
   \;+\; \Big[\big(I_a^{-1}(\mathbf{r}_a\times\hat{\mathbf{n}})\big)\times\mathbf{r}_a
   \;+\; \big(I_b^{-1}(\mathbf{r}_b\times\hat{\mathbf{n}})\big)\times\mathbf{r}_b\Big]
   \cdot \hat{\mathbf{n}}. $$

$k_n$ is the **inverse effective mass** of the contact along $\hat{\mathbf{n}}$ (in constraint
language, $J M^{-1} J^{\top}$ for the contact Jacobian): how much relative normal velocity one
unit of impulse buys, including what leaks into rotation at these lever arms. A head-on hit
through both centres has $\mathbf{r}\times\hat{\mathbf{n}} = 0$ and reduces to the freshman
two-body formula; a glancing hit rotates more and stops less. Driving $v_n$ to a target $b$ is
then a single division:

$$ \lambda = -\frac{v_n - b}{k_n} \;=\; -m_{\text{eff}} (v_n - b),
   \qquad m_{\text{eff}} = 1/k_n. $$

Static and kinematic bodies enter with $1/m = 0$ and $I^{-1} = 0$ (the infinite-mass convention,
`rigid-body-dynamics.md` §4): they contribute nothing to $k_n$ and receive no velocity change —
immovable, for free, with no special cases.

Two implementation notes. The stored inverse inertia is the **body-space diagonal** (our
primitives' principal axes), so the world-space product is $I^{-1}_{\text{world}} = R\,
\mathrm{diag}(i)\,R^{\top}$ — applied without ever building the matrix by rotating into body
space, scaling per axis, and rotating back (a unit quaternion's inverse is its conjugate). And
positions in the pool *are* the centres of mass for the v1 centred primitives, so
$\mathbf{r} = \mathbf{p} - \mathbf{x}$ needs no COM offset.

## 3. Many contacts: projected Gauss–Seidel

With many simultaneous contacts the conditions of §1 over all points form a **linear
complementarity problem** (LCP). Solving it directly (Lemke, Dantzig) is cubic-ish and destroys
sparsity; games solve it iteratively. **Sequential impulses** is projected Gauss–Seidel in
impulse space: sweep every contact point *in a fixed order*, solve each one-dimensional problem
(§2) against the bodies' *current* velocities, clamp (project) its impulse, apply it immediately,
and repeat the sweep a fixed number of times.

"Apply it immediately" is the Gauss–Seidel part and it is a feature, not sloppiness: the next
point in the sweep sees the correction the previous one made, which is exactly how support
propagates through a stack — the ground pushes the bottom box, whose contact then pushes the box
above it, within a single sweep. Each sweep contracts the remaining error roughly geometrically.

Rime runs **8 velocity sweeps and 2 position sweeps** (ADR-0026), *fixed* — no convergence
early-out — because an early-out makes the float-op sequence depend on intermediate values and
would break bit-identical determinism (§9). Under-convergence shows up as softness (a heavy
stack breathing), not instability; the fixed budget plus warm starting (§5) is what makes eight
sweeps enough.

## 4. Accumulate and clamp

The projection $\lambda \ge 0$ hides a classic trap. Each visit to a point computes a *change*
$\Delta\lambda$; the obvious code clamps that change to be non-negative. Wrong — and the bug is
subtle enough to name (it is Catto's central observation): a legitimate solve often needs a
**negative** change, to take back an over-push made by an earlier iteration or by the warm
start. What must stay non-negative is the **running total**, not the step:

$$ \lambda_{\text{acc}}' = \max(\lambda_{\text{acc}} + \Delta\lambda,\ 0),
   \qquad \text{apply } (\lambda_{\text{acc}}' - \lambda_{\text{acc}})\,\hat{\mathbf{n}}. $$

Because every applied delta is the difference of consecutive accumulator values, the *net*
impulse a tick applies telescopes to exactly the final accumulator — the projection lands on the
solution, not on the path taken to reach it. Clamping deltas instead freezes over-pushes in
(bodies stick to ceilings, boxes hop off the ground after warm starting); the accumulated
formulation is what makes warm starting (§5) safe at all.

## 5. Warm starting

PGS converges geometrically *from wherever it starts*. A resting contact needs essentially the
same impulses every tick (they cancel one tick of gravity, §7's test measures exactly this), and
eight sweeps from $\lambda = 0$ never quite rebuild them — the stack sags a little, every tick,
forever: visible buzz. So each tick starts from last tick's answer:

- the narrowphase tags every contact point with a stable **feature id**
  ([`gjk-epa.md`](gjk-epa.md) §8) and the persistent manifold cache matches points across frames
  by that id;
- at prepare time the accumulators start at the cached $\lambda$ values, and the full cached
  impulse is applied to the bodies up front (the "warm start");
- the iterations then only correct the *change* since last tick — near zero at rest;
- after the solve, the converged accumulators are written back and committed to the cache.

The commit **must happen after the solve** — a cache of pre-solve manifolds would carry the same
stale impulses forward forever and the solver would start cold every tick (world.cpp's stage
ordering exists for this). Stale warm starts are self-correcting through §4: if the cached
impulse is now too big, the accumulated solve walks it back, and what was applied nets out to
the walked-back total.

## 6. Coulomb friction as a two-axis pyramid

Coulomb's law bounds the tangential force by the normal force: $|\boldsymbol{\lambda}_t| \le \mu
\lambda_n$ — a **cone** in impulse space. The exact cone couples the two tangent directions (a
2-D projection per point per iteration); Rime linearizes it into the inscribed **pyramid**: two
fixed orthonormal tangent directions $\hat{\mathbf{t}}_1, \hat{\mathbf{t}}_2 \perp
\hat{\mathbf{n}}$, each solved like a normal constraint (target $v_t = 0$: friction opposes
sliding) with its own accumulator clamped to a *box*:

$$ \lambda_{t_i} \in [-\mu\,\lambda_n,\ +\mu\,\lambda_n], $$

using the **current** accumulated normal impulse as the bound (friction can hold no harder than
the contact presses). The price of the pyramid: sliding exactly along a diagonal can recruit up
to $\sqrt{2}\,\mu$ — accepted everywhere in games for never solving a coupled projection in the
hot loop. Friction solves after the normals within each sweep, so the bound uses this sweep's
fresh $\lambda_n$.

The tangent basis must be **deterministic and frame-stable**, or the cached tangent impulse
changes meaning between ticks: it is built from the world axis least aligned with
$\hat{\mathbf{n}}$ (fixed X→Y→Z scan, strict less-than), $\hat{\mathbf{t}}_1 =
\widehat{\hat{\mathbf{n}} \times \mathbf{e}}$, $\hat{\mathbf{t}}_2 = \hat{\mathbf{n}} \times
\hat{\mathbf{t}}_1$ — a pure function of the normal. Only $\hat{\mathbf{t}}_1$'s accumulator is
persisted in the cache (one slot in `ContactPoint`); $\hat{\mathbf{t}}_2$ re-converges from zero
each tick, cheap because a persistent slide direction lands in $\hat{\mathbf{t}}_1$.

Per-pair materials combine as $\mu = \sqrt{\mu_a \mu_b}$ (geometric mean: zero if either surface
is frictionless, and never exceeding the rougher surface) and $e = \max(e_a, e_b)$ (the bouncier
material wins — a rubber ball bounces on concrete).

## 7. Restitution as a velocity bias

Newton's restitution hypothesis: the separation speed after impact is $e$ times the approach
speed, along the normal. In impulse form it is just a nonzero target for the normal solve. At
prepare time (after gravity has been integrated, before any impulse), measure $v_n^{\text{pre}}$
per point and set the bias

$$ b = \begin{cases} -e\, v_n^{\text{pre}} & v_n^{\text{pre}} < -v_{\text{threshold}} \\
   0 & \text{otherwise,} \end{cases} $$

so the solve of §2 drives $v_n \to b \ge 0$. The **threshold** ($1\ \text{m/s}$; the classic
gate) is not an optimization but a stability requirement: gravity is integrated *before* the
solve, so a resting body re-approaches its support at $g\,\Delta t$ ($\approx 0.16\ \text{m/s}$
at 60 Hz) every single tick. Reflect that and resting contact buzzes forever; below the
threshold, the target is plain $v_n = 0$ and the body simply rests. Energy across a bounce is
conserved to first order at $e = 1$ (a dropped ball returns to nearly its drop height — the
proof asserts a window, since discrete stepping samples the impact up to one tick late).

## 8. Position recovery: NGS, not Baumgarte

The velocity solve stops bodies from approaching *further*, but it never removes overlap that
already exists — from the discrete detection (a falling body is inside the floor by up to
$v\,\Delta t$ when first noticed), or from spawns. Two standard cures:

**Baumgarte stabilization** folds the position error into the velocity constraint's bias,
$b \mathrel{+}= \tfrac{\beta}{\Delta t}\,d$ (penetration depth $d$): one solver, no extra pass —
but the correction velocity is *real*. The body leaves the contact with kinetic energy it never
earned: debris pops out of rubble, stacks hum, nothing settles — and "settles" is load-bearing
here, because sleeping (M7.5) and a lighting pipeline that re-stamps only what moved (M10) both
key off bodies coming to genuine rest. ADR-0026 rejects Baumgarte by name.

**NGS (non-linear Gauss–Seidel, the split-impulse family)** — Rime's choice — runs a *second*
Gauss–Seidel pass, after positions integrate, that corrects **poses only**. Per point, with the
same effective-mass machinery as §2 evaluated at the current poses:

$$ C = \min\big(0,\ s + s_{\text{slop}}\big), \qquad
   \lambda_{\text{pos}} = -\frac{\beta\,C}{k_n}\ \ (\text{clamped to } \le c_{\max}), $$

where $s$ is the (re-measured, signed) separation. The pseudo-impulse
$\lambda_{\text{pos}}\hat{\mathbf{n}}$ is applied in *displacement* units:
$\mathbf{x} \mathrel{\pm}= \lambda_{\text{pos}}\,\hat{\mathbf{n}}/m$ and the orientation gets the
first-order nudge $\mathbf{q} \leftarrow \operatorname{normalize}(\mathbf{q} + \tfrac12
\boldsymbol{\theta}_q \otimes \mathbf{q})$ with $\boldsymbol{\theta} = \pm I^{-1}(\mathbf{r}
\times \lambda_{\text{pos}}\hat{\mathbf{n}})$ — the positional twin of the quaternion integrator
(`rigid-body-dynamics.md` §3). **The velocity arrays are never read or written**, so the pass
cannot add kinetic energy — that single sentence is the entire NGS-vs-Baumgarte argument, and
the test pins it: two boxes spawned deeply overlapped at rest must separate with speeds staying
at zero.

The tuning, each with a reason:

- **slop** $s_{\text{slop}} = 5\ \text{mm}$: recover to *slightly overlapping*, not to exact
  touch. Full recovery makes a resting contact oscillate between "touching" (manifold exists)
  and "separated" (manifold gone, body falls back in) on alternating ticks; the slop keeps the
  manifold and its warm-start cache alive at rest.
- **rate** $\beta = 0.2$ per iteration: a full correction overshoots because each point's move
  disturbs its neighbours' measurements (that is Gauss–Seidel); 20% per iteration converges
  monotonically tick over tick.
- **cap** $c_{\max} = 0.2\ \text{m}$ per point per iteration: bounds the teleport a pathological
  spawn-inside-a-wall can cause in one tick.

"Non-linear" is the last piece: each iteration **re-poses** the contact anchors (stored in each
body's local frame at prepare time — the two *surface* points, at $\pm d/2$ along the normal
from the manifold midpoint) using the bodies' *current* transforms, and re-measures $s$ from
them — chasing the true geometry as it moves, instead of solving a linearization frozen at tick
start. The normal itself rides body $a$'s rotation (v1 simplification; at worst one tick stale,
absorbed by slop + rate and re-aimed by the next tick's fresh manifold).

## 9. Determinism

Everything above is a fixed sequence of IEEE-754 operations: constraints are prepared and swept
in the manifolds' canonical (broadphase) order; iteration counts are fixed; the friction basis
is a pure function of the normal; warm starting matches by feature-id equality (never
hash-iteration order); no fast-math anywhere in the build (ADR-0026, CI-enforced). Same binary +
same inputs ⇒ bit-identical impulses, velocities, and poses — asserted by hashing body state
across runs, through full contact scenes. Because every per-contact routine is a pure function
of the two bodies and the constraint, M7.5 can partition contacts by island and keep this
bit-identical across any thread count — the milestone's determinism thesis.
