# Orbit (turntable) camera — derivation notes (ICEM viewer, A2)

These notes derive the formulas implemented in `samples/03-icem-viewer/camera.hpp`. The code is terse
because the *why* lives here. Conventions follow [ADR-0004](../adr/0004-math-conventions.md): `float`,
column-major storage, the column-vector convention $v' = Mv$, a right-handed world, and Vulkan clip
space (depth $z\in[0,1]$, NDC $y$ down). GitHub renders the `$…$` / `$$…$$` math below.

The camera's job: let a user *inspect a part* — rotate around it, zoom, and slide it around — without
ever rolling it or losing its up-axis. That is a **turntable** (orbit) camera, the model every CAD/DCC
tool uses. We reject two alternatives up front: a *free-fly* camera (translates the eye freely — wrong
for "look at this object"), and an *arcball/trackball* (rotates via a virtual sphere — natural but it
**rolls** the model, leaving an engineering part tilted). The turntable keeps a chosen world axis
pointing up at all times.

---

## 1. State

The camera is four numbers plus a chosen up-axis:

- target $\mathbf t\in\mathbb R^3$ — the point we look at and orbit,
- distance $d>0$ — the orbit radius (eye-to-target),
- yaw $\varphi$ — azimuth, rotation about the up-axis,
- pitch $\theta$ — elevation above the horizon plane,
- world-up $\hat{\mathbf u}$ — the axis yaw spins about ($+\mathbf y$ by default; ICEM's parts are
  authored $z$-up, so the viewer sets $\hat{\mathbf u}=+\mathbf z$).

## 2. A basis from the up-axis

To support both $y$-up and $z$-up with one formula, build an orthonormal basis whose third axis is the
up-axis. Pick a **seed** world axis not parallel to $\hat{\mathbf u}$ (we use $+\mathbf y$, falling back
to $+\mathbf x$ when the up-axis is itself near $\pm\mathbf y$), then

$$\hat{\mathbf f}_0 = \frac{\text{seed}\times\hat{\mathbf u}}{\lVert\cdot\rVert},\qquad
  \hat{\mathbf r}_0 = \frac{\hat{\mathbf u}\times\hat{\mathbf f}_0}{\lVert\cdot\rVert}.$$

By construction $\{\hat{\mathbf r}_0,\hat{\mathbf f}_0,\hat{\mathbf u}\}$ is orthonormal and spans the
horizon plane with $\hat{\mathbf f}_0,\hat{\mathbf r}_0$. The seed only fixes *which* direction
$\varphi=0$ faces: with $\hat{\mathbf u}=+\mathbf y$ it gives $\hat{\mathbf f}_0=+\mathbf z$ (the
familiar "yaw 0 looks down $-\mathbf z$"); with $\hat{\mathbf u}=+\mathbf z$ it gives
$\hat{\mathbf f}_0=+\mathbf x$.

## 3. Eye position (spherical coordinates)

The eye sits on the sphere of radius $d$ about $\mathbf t$. In the basis above, with pitch measured
from the horizon plane and yaw within it,

$$\mathbf e \;=\; \mathbf t \;+\; d\Big(\cos\theta\,(\sin\varphi\,\hat{\mathbf r}_0 + \cos\varphi\,\hat{\mathbf f}_0) \;+\; \sin\theta\,\hat{\mathbf u}\Big).$$

The bracket is a unit vector (its horizon part has length $\cos\theta$, its up part $\sin\theta$, and
$\cos^2\theta+\sin^2\theta=1$), so the eye is exactly $d$ from the target for every $(\varphi,\theta)$.
Check: $\varphi=\theta=0\Rightarrow \mathbf e=\mathbf t+d\,\hat{\mathbf f}_0$ (front view);
$\varphi=\tfrac\pi2,\theta=0\Rightarrow \mathbf e=\mathbf t+d\,\hat{\mathbf r}_0$ (side view) — both
matched by the unit tests.

### 3.1 The pole singularity

The view matrix (§5) is `look_at(eye, target, up)`, which builds its basis from the view direction and
$\hat{\mathbf u}$ via a cross product. At $\theta=\pm\tfrac\pi2$ the view direction is $\mp\hat{\mathbf u}$,
parallel to up, so that cross product vanishes and the basis collapses (the image would spin
undefined-ly). We therefore **clamp** $\theta\in[-(\tfrac\pi2-\varepsilon),\,\tfrac\pi2-\varepsilon]$
with a small $\varepsilon$. This is the standard turntable guard; the cost is you cannot look *exactly*
straight down, which no one needs.

## 4. Input verbs

**Orbit.** Add deltas: $\varphi\mathrel{+}=\Delta\varphi$, $\theta\leftarrow\operatorname{clamp}(\theta+\Delta\theta)$.

**Zoom (dolly).** Multiplicative: $d\leftarrow d\cdot \rho^{\,s}$ with $\rho=0.9$ per notch $s$. A
multiplicative step covers the same *fraction* of the distance at every scale, so zooming feels uniform
whether the camera is near or far — an additive step would crawl when far and overshoot when near. $d$
is clamped to $[d_{\min},d_{\max}]$.

**Pan.** Slide the target in the screen plane. With the camera's right $\hat{\mathbf r}$ and up
$\hat{\mathbf v}$ (from the view basis, §5),

$$\mathbf t \mathrel{+}= s\,d\,(-\Delta x\,\hat{\mathbf r} + \Delta y\,\hat{\mathbf v}).$$

The factor $d$ makes a drag move the world by a constant fraction of the view regardless of zoom; the
$-\Delta x$ sign makes a rightward drag push the model right (the target moves left under the cursor).

## 5. View, projection, and the camera basis

$$V = \texttt{look\_at}(\mathbf e,\mathbf t,\hat{\mathbf u}),\qquad
  P = \texttt{perspective}(\text{fov}_y,\,\text{aspect},\,z_n,\,z_f),\qquad
  \text{clip} = P\,V\,\text{world}.$$

Both are the engine's existing right-handed, Vulkan-clip-space builders (ADR-0004), so the matrix
uploads to a uniform with no transpose. The camera basis used by pan and (later) the cross-section
plane is read straight off the geometry: forward $\hat{\mathbf f}=\widehat{\mathbf t-\mathbf e}$, right
$\hat{\mathbf r}=\widehat{\hat{\mathbf f}\times\hat{\mathbf u}}$, up $\hat{\mathbf v}=\hat{\mathbf r}\times\hat{\mathbf f}$.

## 6. Framing a bounding sphere

To "fit the part on screen", center the target on the part's bounding sphere (center $\mathbf c$, radius
$r$) and choose $d$ so the sphere fills the view. A cone of half-angle $\beta$ from the eye is tangent to
a sphere of radius $r$ at distance $d$ when

$$\sin\beta = \frac{r}{d}\quad\Longrightarrow\quad d = \frac{r}{\sin\beta}.$$

The vertical half-field is $\beta_y=\text{fov}_y/2$. The horizontal half-field follows from the aspect
ratio $a=\text{width}/\text{height}$ via the perspective projection, which scales $x$ by $1/a$ relative
to $y$: $\tan\beta_x = a\,\tan\beta_y$, i.e. $\beta_x=\arctan(a\tan\beta_y)$. To fit the sphere in *both*
directions, use the tighter cone $\beta=\min(\beta_y,\beta_x)$ (vertical when $a\ge 1$, horizontal when
$a<1$):

$$d = \frac{r}{\sin\big(\min(\beta_y,\beta_x)\big)}\cdot m,$$

with a margin $m\gtrsim 1$ for breathing room. The unit test confirms a silhouette point
$\mathbf c + r\,\hat{\mathbf v}$ then projects inside the NDC box.

---

### Alternatives considered

- **Arcball / trackball.** Maps cursor motion onto a virtual sphere and rotates by the resulting
  quaternion. Beautifully direct, but it introduces *roll*, which tilts an engineering part — undesirable
  for inspection. Revisit only if a free-orientation mode is wanted.
- **Storing the eye and orientation directly** (instead of $\varphi,\theta,d$ about a target). Simpler to
  render from, but orbit/zoom/pan and "frame the part" all become awkward, and the natural pole clamp is
  lost. The target-relative spherical state is the right model for an inspection camera.
