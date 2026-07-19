# Transform gizmos — derivation notes (M9.6b)

These notes derive the geometry behind the editor's **transform gizmos**: how a cursor pixel becomes
a world-space ray, how that ray is constrained to a translate/scale **axis** or a rotate **plane**,
how a drag becomes a signed distance or angle, and why the handles are drawn at a **screen-constant**
size. They are the companion to two pieces of code:

- `tools/editor/src/gizmo.rs` — the editor-side math (pure functions + the `DragSession`), and
- `engine/render/src/gizmo_renderer.cpp` — the engine-side handle rendering.

Both are terse because the *why* lives here. Conventions follow
[ADR-0004](../adr/0004-math-conventions.md): a right-handed world, **column-vector** math ($v' = Mv$),
column-major storage, and **Vulkan clip space** — normalized-device $x,y \in [-1,1]$ but depth
$z \in [0,1]$, with $y$ pointing **down**. GitHub renders the `$…$` / `$$…$$` math below.

The split between editor and engine is the ratified architecture (ADR-0016/0031): the **engine
renders** the handles and, each frame, ships the editor the exact lens it rendered with (the
`ViewportCamera` message — $M_{vp}$, its inverse, and the eye); the **editor does the math** here and
edits the entity through the ordinary `SetComponent` path, so a gizmo drag is undoable exactly like an
inspector edit. The editor therefore never has to invert a matrix — the one operation it cannot do
cheaply or agree on bit-for-bit — because the engine, which *built* the projection, sends the inverse.

---

## 1. The lens: a pixel is a ray

The frame the editor shows was rendered through a single $4\times4$ matrix

$$M_{vp} = P \, V,$$

the **view-projection**: $V$ (world → view, the inverse of the camera's world placement) followed by
$P$ (the perspective projection, `core::perspective`). A world point $\mathbf p$ lands on screen by

$$\mathbf c = M_{vp}\,\begin{pmatrix}\mathbf p\\ 1\end{pmatrix},\qquad
\text{NDC} = \frac{\mathbf c_{xyz}}{c_w},\qquad
\begin{aligned}
x_\text{px} &= (\text{NDC}_x \cdot \tfrac12 + \tfrac12)\,W,\\
y_\text{px} &= (\text{NDC}_y \cdot \tfrac12 + \tfrac12)\,H,
\end{aligned}$$

with no $y$-flip in the pixel step because `perspective` already baked Vulkan's $y$-down into $P$'s
second row (this is `project_point`). The divide by $c_w$ is the perspective foreshortening; a point
at or behind the eye plane has $c_w \le 0$ and no honest screen position, so `project_point` returns
`None` there and hover tests skip it.

**Unprojection** is the inverse question: which world points could a pixel have come from? Invert the
whole map. Turn the pixel back into NDC,

$$\text{NDC}_x = \frac{2\,x_\text{px}}{W} - 1,\qquad \text{NDC}_y = \frac{2\,y_\text{px}}{H} - 1,$$

and push two homogeneous points — one on the **near** plane ($z=0$), one on the **far** plane ($z=1$,
the Vulkan depth range) — back through $M_{vp}^{-1}$:

$$\mathbf q_\text{near} = \operatorname{dehom}\!\big(M_{vp}^{-1}\,(\text{NDC}_x,\ \text{NDC}_y,\ 0,\ 1)^\top\big),\qquad
\mathbf q_\text{far} = \operatorname{dehom}\!\big(M_{vp}^{-1}\,(\text{NDC}_x,\ \text{NDC}_y,\ 1,\ 1)^\top\big),$$

where $\operatorname{dehom}(\mathbf a) = \mathbf a_{xyz}/a_w$. The two world points share the pixel's
line of sight, so the ray is

$$\mathbf r(s) = \mathbf o + s\,\hat{\mathbf d},\qquad \mathbf o = \mathbf e\ (\text{the eye}),\qquad
\hat{\mathbf d} = \widehat{\mathbf q_\text{far} - \mathbf q_\text{near}}.$$

For a perspective camera every such line passes through the eye, so we take the eye as the origin
(the engine ships it) and the near→far difference as the direction (this is `ray_from_pixel`). The
`$\;\hat{}\;$` is normalization; the closest-point formulae below assume $\hat{\mathbf d}$ is unit.

This is the whole reason the `ViewportCamera` message carries $M_{vp}^{-1}$ **and** insists it be the
engine's own: if the editor recomputed the inverse from a re-derived $M_{vp}$, a one-ULP disagreement
in $\tan(\text{fov}/2)$ between two platforms' `libm` would put the ray a fraction of a pixel off the
handle the user is looking at. Ship the inverse, and the pixels and the math cannot disagree.

## 2. Translate & scale: closest point on the axis

A translate or scale drag is constrained to one **world axis** through the entity: the line

$$\mathbf a(t) = \mathbf c_0 + t\,\hat{\mathbf u},$$

with $\mathbf c_0$ the entity's world center and $\hat{\mathbf u}$ the unit axis direction
($\pm x/y/z$; the handles are world-aligned in v1). The cursor ray $\mathbf r(s) = \mathbf o + s\,
\hat{\mathbf d}$ almost never *intersects* that line in 3-D — two skew lines rarely meet — so "where on
the axis is the cursor?" is answered by the **closest point** of the axis to the ray. That is the
classic two-line closest-approach problem.

Minimize the squared gap $f(t,s) = \lVert \mathbf a(t) - \mathbf r(s)\rVert^2$. Writing
$\mathbf w = \mathbf c_0 - \mathbf o$ and using unit directions ($\hat{\mathbf u}\cdot\hat{\mathbf u} =
\hat{\mathbf d}\cdot\hat{\mathbf d} = 1$), set the two partials to zero:

$$\begin{aligned}
\tfrac12\,\partial_t f &= t - b\,s - d = 0,\\
\tfrac12\,\partial_s f &= b\,t - s - e = 0,
\end{aligned}\qquad
b = \hat{\mathbf u}\cdot\hat{\mathbf d},\quad d = \hat{\mathbf u}\cdot\mathbf w,\quad e = \hat{\mathbf d}\cdot\mathbf w.$$

Eliminate $s$ and solve for the axis parameter:

$$\boxed{\,t = \frac{b\,e - d}{1 - b^2}\,}$$

(this is `closest_t_on_axis`). The sign of $\mathbf w = \mathbf c_0 - \mathbf o$ matters — flip it and
$t$ negates; the code and this note both use *center minus origin*.

**Translate.** Anchor $t_0$ at the grab, and each frame move the entity by the *change* along the
axis, snapped:

$$\mathbf t_\text{new} = \mathbf t_\text{start} + \operatorname{snap}(t - t_0)\,\hat{\mathbf u}.$$

Measuring a **delta from the grab** (not an absolute) is what makes the handle stick to the cursor
without teleporting on the first frame — the object moves exactly as far as the cursor dragged it
along the rail.

**Scale.** Use the *ratio* of distances instead, so dragging the handle to twice its grab distance
doubles that axis' scale:

$$s^{(k)}_\text{new} = \operatorname{snap}\!\Big(s^{(k)}_\text{start}\cdot \tfrac{t}{t_0}\Big),\qquad k \in \{x,y,z\}\ \text{the grabbed axis}.$$

The grab is refused when $t_0 \approx 0$ (grabbing the entity's own center), because the ratio
$t/t_0$ would explode.

**The degeneracy.** The determinant of the $2\times2$ system is $1 - b^2 = 1 - (\hat{\mathbf u}\cdot
\hat{\mathbf d})^2 = \sin^2\theta$, where $\theta$ is the angle between the ray and the axis. As the
ray lines up with the axis (you sight straight down it) $\theta \to 0$, the system loses rank, and the
closest point races to infinity — a tiny cursor move would fling the object across the world. So
`closest_t_on_axis` returns `None` once $1 - b^2 < \varepsilon$; a **grab** in that state is refused
(the drag never starts) and a **move** into it *holds* the last good transform rather than jump. Both
are the correct UX: a computation that has gone ill-conditioned must not masquerade as a large edit.

## 3. Rotate: intersect the plane, measure the angle

A rotate drag turns the entity about one world axis $\hat{\mathbf u}$, dragging along the **ring** that
lies in the plane through $\mathbf c_0$ with normal $\hat{\mathbf u}$. First intersect the cursor ray
with that plane. A point $\mathbf x$ is on the plane when $(\mathbf x - \mathbf c_0)\cdot\hat{\mathbf u}
= 0$; substitute the ray and solve for its parameter:

$$s^\* = \frac{(\mathbf c_0 - \mathbf o)\cdot\hat{\mathbf u}}{\hat{\mathbf d}\cdot\hat{\mathbf u}},\qquad
\mathbf x = \mathbf o + s^\*\,\hat{\mathbf d}$$

(this is `ray_plane_intersect`). When $\hat{\mathbf d}\cdot\hat{\mathbf u}\approx 0$ the ray is parallel
to the plane — the ring is edge-on — and there is no usable hit, so we refuse; likewise a hit behind
the ray ($s^\* < 0$) is rejected.

The drag angle is the **signed angle** swept in the plane, from the grab vector $\mathbf v_0 =
\mathbf x_0 - \mathbf c_0$ to the current $\mathbf v = \mathbf x - \mathbf c_0$, measured about
$\hat{\mathbf u}$:

$$\Delta\phi = \operatorname{atan2}\!\big((\mathbf v_0\times\mathbf v)\cdot\hat{\mathbf u},\ \ \mathbf v_0\cdot\mathbf v\big).$$

Using `atan2` of the (out-of-plane) cross term over the dot is what makes the angle **quadrant-correct**
over the full $(-\pi,\pi]$ and correctly *signed* by the right-hand rule — a plain $\arccos$ of the
normalized dot would lose the sign and fold at $\pi$ (this is `signed_angle_on_plane`). Snap $\Delta\phi$
to the angular grid, then apply it as a world-axis rotation *pre-multiplied* onto the start
orientation (world rotation composes on the left, matching the engine's `parent * child`):

$$q_\text{new} = \operatorname{normalize}\!\big(q_{\hat{\mathbf u}}(\Delta\phi)\cdot q_\text{start}\big),\qquad
q_{\hat{\mathbf u}}(\phi) = \big(\sin\tfrac\phi2\,\hat{\mathbf u},\ \cos\tfrac\phi2\big).$$

The renormalization is the usual quaternion-drift hygiene: a long drag composes many small rotations,
and floating-point products slowly leave the unit sphere.

## 4. Screen-constant handle size

A gizmo you must walk toward to grab is a broken gizmo, so the handles are drawn at a size that covers
a fixed **fraction of the viewport** regardless of the entity's distance. At view depth $d$ from the
eye, the viewport's half-height spans $d\tan(\text{fov}_y/2)$ world units (the definition of the
vertical field of view). So a handle of world length

$$\ell = d\,\tan\!\tfrac{\text{fov}_y}{2}\cdot f,\qquad d = \lVert \mathbf c_0 - \mathbf e\rVert,$$

always spans the fraction $f$ of the half-height — `kGizmoScreenFraction` in the engine, mirrored as
`GIZMO_SCREEN_FRACTION` in the editor (change one, change both, or the clickable area drifts off the
drawn pixels). The engine bakes $\ell$ into the handle's model matrix (`gizmo_renderer.cpp`); the
editor sizes its 2-D hover targets with the same $\ell$ (`gizmo_world_size`).

The editor needs $\tan(\text{fov}_y/2)$ but the wire deliberately does **not** carry the fov as its own
field. It is recovered from $M_{vp}$ itself: `perspective` writes $P_{11} = -1/\tan(\text{fov}_y/2)$
(the negative is Vulkan's $y$-flip), and $V$'s rotation rows are orthonormal, so **row 1** of
$M_{vp} = PV$ has Euclidean length exactly $1/\tan(\text{fov}_y/2)$. Hence

$$\tan\tfrac{\text{fov}_y}{2} = \frac{1}{\lVert (M_{vp})_{1,\ast}\rVert}$$

(this is `tan_half_fov`) — one fewer thing to keep in sync on the wire, derived from the matrix that is
already the single source of truth for the lens.

## 5. One drag, one undo step

Every intermediate frame of a drag sends a live `SetComponent` (and optimistically patches the
editor's mirror, so the inspector and the streamed frame track the drag), but the whole gesture folds
into **one** undo `Edit` at release: `forward = Set(final bytes)`, `inverse = Set(grab bytes)`. The
`DragSession` captures the grab-time component bytes verbatim as that bit-exact inverse and computes
every update from the *anchor*, never incrementally — which is what makes a drag replayable in a unit
test and immune to event-rate jitter. This mirrors the m9.4 inspector's gesture rule (a slider drag is
one undo step), and it is why the gizmo needs no new edit message: a drag is just `SetComponent`s the
user can undo in a single stroke.

See also [quaternions-transforms.md](quaternions-transforms.md) for the TRS/quaternion algebra the
rotate path leans on, [vectors-matrices.md](vectors-matrices.md) for the projection matrix, and
[orbit-camera.md](orbit-camera.md) for the camera whose lens all of this unprojects.
