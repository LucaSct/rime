# Vectors & Matrices — derivation notes (M1.3)

These notes derive, from first principles, the formulas implemented in
`engine/core/include/rime/core/math/`. They are the companion to the code: the code is
terse because the *why* lives here. Conventions are fixed in
[ADR-0004](../adr/0004-math-conventions.md); the one-line summary is **`float`,
column-major storage, the column-vector convention `v' = M v`, a right-handed world, and
Vulkan clip space (depth `z ∈ [0, 1]`, NDC y pointing down).**

GitHub renders the `$…$` / `$$…$$` math below.

---

## 1. Vectors

### 1.1 Dot product

For $\mathbf a, \mathbf b \in \mathbb R^n$,

$$\mathbf a \cdot \mathbf b = \sum_i a_i b_i = \lVert \mathbf a\rVert\,\lVert \mathbf b\rVert\cos\theta .$$

The two expressions agree by the law of cosines applied to the triangle with sides
$\mathbf a$, $\mathbf b$, $\mathbf a - \mathbf b$:
$\lVert\mathbf a-\mathbf b\rVert^2 = \lVert\mathbf a\rVert^2 + \lVert\mathbf b\rVert^2 - 2\lVert\mathbf a\rVert\lVert\mathbf b\rVert\cos\theta$,
and expanding the left side coordinate-wise leaves $\mathbf a\cdot\mathbf b = \lVert\mathbf a\rVert\lVert\mathbf b\rVert\cos\theta$.

Consequences we use constantly: $\mathbf a\cdot\mathbf a = \lVert\mathbf a\rVert^2$
(hence `length` $=\sqrt{\mathbf v\cdot\mathbf v}$, and `length_squared` when we only need to
compare magnitudes and want to skip the square root); and $\mathbf a\cdot\mathbf b = 0 \iff$
the vectors are orthogonal. The scalar projection of $\mathbf a$ onto a unit vector
$\hat{\mathbf u}$ is $\mathbf a\cdot\hat{\mathbf u}$ — this is exactly how the view matrix
(§4) reads off a point's coordinate along each camera axis.

### 1.2 Cross product

In $\mathbb R^3$,

$$\mathbf a \times \mathbf b = (a_y b_z - a_z b_y,\; a_z b_x - a_x b_z,\; a_x b_y - a_y b_x).$$

Its geometry: $\mathbf a\times\mathbf b$ is **perpendicular to both** inputs
($(\mathbf a\times\mathbf b)\cdot\mathbf a = (\mathbf a\times\mathbf b)\cdot\mathbf b = 0$,
which you can verify by direct substitution), it has magnitude
$\lVert\mathbf a\times\mathbf b\rVert = \lVert\mathbf a\rVert\lVert\mathbf b\rVert\sin\theta$
(the area of the parallelogram they span), and its direction follows the **right-hand rule**.
That last fact is what pins down our handedness: in a right-handed basis,

$$\hat{\mathbf x}\times\hat{\mathbf y}=\hat{\mathbf z},\quad
  \hat{\mathbf y}\times\hat{\mathbf z}=\hat{\mathbf x},\quad
  \hat{\mathbf z}\times\hat{\mathbf x}=\hat{\mathbf y}.$$

The product is **anti-commutative** ($\mathbf a\times\mathbf b = -\,\mathbf b\times\mathbf a$),
so order matters everywhere we build a basis from it (§4). All four identities are checked
in `math_test.cpp`.

### 1.3 Normalization

$\hat{\mathbf v} = \mathbf v / \lVert\mathbf v\rVert$ has unit length and the same direction.
The code guards the degenerate case: if $\lVert\mathbf v\rVert \le \varepsilon$ we return the
zero vector instead of dividing by (near-)zero. `kEpsilon` $= 10^{-6}$ is an absolute
tolerance tuned for unit-scale data; for very large magnitudes a relative/ULP comparison
would be more honest (noted in `scalar.hpp`).

---

## 2. Homogeneous coordinates

A 3-D affine map is $\mathbf x \mapsto A\mathbf x + \mathbf t$ — a linear part plus a
translation. Translation is **not** linear (it moves the origin), so it cannot be written as
a $3\times3$ matrix. The fix is to lift $\mathbb R^3$ into $\mathbb R^4$ by appending a
fourth coordinate $w$ and writing the affine map as a single $4\times4$ matrix:

$$\begin{pmatrix}\mathbf x'\\ w'\end{pmatrix}
= \begin{pmatrix} A & \mathbf t\\ \mathbf 0^{\top} & 1\end{pmatrix}
  \begin{pmatrix}\mathbf x\\ w\end{pmatrix}.$$

The convention that makes this useful:

- **$w = 1$ marks a point.** The bottom row reproduces $w' = 1$, and the top rows give
  $A\mathbf x + \mathbf t$ — translation included. → `transform_point`.
- **$w = 0$ marks a direction.** The $\mathbf t$ column is multiplied by $0$, so directions
  get the linear part $A\mathbf x$ **only** — never translated. → `transform_vector`.

This single distinction (one number, $w$) is why normals, velocities, and ray directions
stay correct under a translating transform while positions move. The test
`"translation moves points but not directions"` is exactly this invariant.

(The $w$-divide also powers perspective in §5: there the bottom row is *not* $(\,\mathbf 0\;1)$,
so $w'$ becomes a function of depth and the subsequent divide produces foreshortening.)

---

## 3. Matrices

### 3.1 Storage and the multiplication convention

We store **column-major**: element $(r, c)$ lives at flat index $c\cdot N + r$, so each matrix
*column* is contiguous in memory. We use **column vectors**, $\mathbf v' = M\mathbf v$, i.e.

$$v'_r = \sum_c M_{rc}\,v_c .$$

Two reasons (ADR-0004): this is byte-for-byte GLSL/Vulkan's layout, so a `Mat4` uploads to a
uniform buffer **with no transpose**; and the columns of $M$ are the images of the basis
vectors $M\hat{\mathbf e}_i$, which makes "the columns *are* the transformed axes" literally
true — handy when reading a transform by eye.

Matrix product, $(AB)_{rc} = \sum_k A_{rk} B_{kc}$, then composes **right-to-left**:

$$(AB)\mathbf v = A(B\mathbf v),$$

so $B$ acts first. Hence `model = T * R * S` scales, then rotates, then translates — the
order you say out loud, read right to left. The test `"multiplication composes right-to-left"`
demonstrates that $T\!S$ and $S\!T$ differ.

### 3.2 Elementary affine builders

- **Translation** `mat4_translation(t)`: identity with $\mathbf t$ in the 4th column. Acts as
  $A=I$, offset $\mathbf t$.
- **Scaling** `mat4_scaling(s)`: diagonal $(s_x, s_y, s_z, 1)$.
- The upper-left $3\times3$ is the **linear part** $A$. `mat3_from_mat4` extracts it — used for
  transforming directions and (after an inverse-transpose, a later brick) normals.

### 3.3 Determinant

The determinant is the signed volume-scaling factor of the linear part: $|\det M|$ is how much
the map scales volumes, and $\operatorname{sign}(\det M)$ tells whether it preserves
orientation ($+$) or mirrors it ($-$). We compute it by **Laplace (cofactor) expansion** along
the first row,

$$\det M = \sum_{c} (-1)^{c}\, M_{0c}\, \mu_{0c},$$

where the **minor** $\mu_{0c}$ is the determinant of the $3\times3$ submatrix left after
deleting row $0$ and column $c$ (and each $3\times3$ is the standard rule-of-Sarrus expansion,
`det3` in `mat.cpp`). Sanity checks (in the tests): $\det I = 1$; a scaling has
$\det = s_x s_y s_z$; a translation or rotation has $\det = 1$ (they preserve volume).

### 3.4 Inverse

The inverse undoes the transform: $M M^{-1} = M^{-1} M = I$. The general formula is

$$M^{-1} = \frac{1}{\det M}\,\operatorname{adj}(M),\qquad
  \operatorname{adj}(M) = C^{\top},\quad C_{ij} = (-1)^{i+j}\mu_{ij},$$

i.e. the transpose of the cofactor matrix divided by the determinant. `mat.cpp` writes out the
sixteen cofactors explicitly (the classic MESA formulation; because our column-major flat
layout matches OpenGL's, the indices `m[0..15]` line up directly), and reuses the first column
of cofactors to form $\det M$ — so the determinant comes "for free" from the work already done.
A singular matrix ($|\det M| \le \varepsilon$) has no inverse: we `RIME_ASSERT` in debug and
fall back to identity. The test verifies $M M^{-1} = I$, $(M^{-1})^{-1} = M$, and a point
round-trip.

> **Performance note.** Most engine matrices are *affine* with the block form
> $\bigl[\begin{smallmatrix}A & \mathbf t\\ \mathbf 0 & 1\end{smallmatrix}\bigr]$, whose inverse
> is the much cheaper
> $\bigl[\begin{smallmatrix}A^{-1} & -A^{-1}\mathbf t\\ \mathbf 0 & 1\end{smallmatrix}\bigr]$
> (and $A^{-1}=A^{\top}$ when $A$ is a pure rotation). We keep the general cofactor inverse for
> now because it is correct for *any* matrix and nothing here is hot yet (CLAUDE.md: measure
> first). The fast affine path is a future optimization behind the same signature.

---

## 4. The view matrix — `look_at`

The view matrix maps **world space → camera (view) space**, placing the camera at the origin.
Given `eye`, `center`, and an approximate `up`, build an orthonormal camera basis:

$$\mathbf f = \widehat{\text{center}-\text{eye}},\quad
  \mathbf r = \widehat{\mathbf f\times\text{up}},\quad
  \mathbf u = \mathbf r\times\mathbf f .$$

$\mathbf f$ is the viewing direction; $\mathbf r$ ("right") is forced perpendicular to it by the
cross product; recomputing $\mathbf u$ from $\mathbf r\times\mathbf f$ makes the basis exactly
orthonormal even if the supplied `up` was not perpendicular to $\mathbf f$.

In a right-handed view space the camera looks down **$-\mathbf z$**, so the view-space axes,
expressed in world coordinates, are $\mathbf x_v=\mathbf r$, $\mathbf y_v=\mathbf u$,
$\mathbf z_v=-\mathbf f$. The matrix $B=[\,\mathbf r\;\;\mathbf u\;\;-\mathbf f\,]$ is
view→world; since $B$ is orthonormal, world→view is $B^{\top}$. Combining the rotation with the
translation that sends `eye` to the origin gives

$$V=\begin{pmatrix}
\mathbf r^{\top} & -\,\mathbf r\cdot\text{eye}\\
\mathbf u^{\top} & -\,\mathbf u\cdot\text{eye}\\
-\mathbf f^{\top} & \;\;\mathbf f\cdot\text{eye}\\
\mathbf 0^{\top} & 1
\end{pmatrix}.$$

Each upper-left row is a camera axis; each translation entry is *minus the projection of the eye
onto that axis* (§1.1) — re-expressing the world origin in camera coordinates. The tests confirm
`eye` maps to the view-space origin, the target lands at $(0,0,-d)$ in front of the camera, and
$\det V = 1$ (a rigid motion).

---

## 5. Projections

Projections map view space into **clip space**; the GPU then performs the perspective divide by
$w$ to reach normalized device coordinates (NDC). We target **Vulkan NDC**: $x,y\in[-1,1]$ with
**y pointing down**, and depth $z\in[0,1]$.

### 5.1 Perspective

Let the vertical field of view be $\theta_y$ and the aspect ratio $a = \text{width}/\text{height}$.
Define the focal length $f = \cot(\theta_y/2) = 1/\tan(\theta_y/2)$. A point at view-space
$(x,y,z)$ with $z<0$ (in front of the camera) foreshortens by its depth $-z$:

$$x_{\text{ndc}} = \frac{f}{a}\,\frac{x}{-z},\qquad
  y_{\text{ndc}} = -\,f\,\frac{y}{-z}.$$

We arrange the perspective divide to happen automatically by setting $w' = -z$ — i.e. the
**bottom row is $(0,0,-1,0)$** (this is where homogeneous coordinates earn their keep, §2). The
leading minus on $y$ is the **Vulkan y-flip**, baked in here so no other code worries about it.

For depth we want an affine-in-clip map $z' = A z + B$ (with $w'=-z$) such that the near plane
$z=-z_n$ lands at $z_{\text{ndc}}=0$ and the far plane $z=-z_f$ at $z_{\text{ndc}}=1$:

$$\frac{-A z_n + B}{z_n}=0,\qquad \frac{-A z_f + B}{z_f}=1
\;\;\Longrightarrow\;\;
A=\frac{z_f}{z_n-z_f},\quad B=\frac{z_f z_n}{z_n-z_f}.$$

Assembling (column-major, $v'=Mv$):

$$P=\begin{pmatrix}
\frac{f}{a} & 0 & 0 & 0\\
0 & -f & 0 & 0\\
0 & 0 & \frac{z_f}{z_n-z_f} & \frac{z_f z_n}{z_n-z_f}\\
0 & 0 & -1 & 0
\end{pmatrix}.$$

This is `perspective(fovy, aspect, z_near, z_far)`. The non-linear $z\mapsto z_{\text{ndc}}$
packs precision near the camera — the reason a reversed-Z depth buffer is a common later
refinement. The tests verify both planes map to $\{0,1\}$ and that $w'=$ depth.

### 5.2 Orthographic

No foreshortening: parallel lines stay parallel, $w'=1$. We map the box
$[l,r]\times[b,t]\times[-z_n,-z_f]$ to Vulkan NDC by an independent affine map per axis:

$$x_{\text{ndc}}=\frac{2x}{r-l}-\frac{r+l}{r-l},\quad
  y_{\text{ndc}}=\frac{-2y}{t-b}+\frac{t+b}{t-b},\quad
  z_{\text{ndc}}=\frac{-z}{z_f-z_n}-\frac{z_n}{z_f-z_n}.$$

The $x$ and $z$ rows are ordinary "remap interval to target interval" maps; the $y$ row carries
the extra minus sign for Vulkan's y-down NDC. As a matrix:

$$O=\begin{pmatrix}
\frac{2}{r-l} & 0 & 0 & -\frac{r+l}{r-l}\\
0 & \frac{-2}{t-b} & 0 & \frac{t+b}{t-b}\\
0 & 0 & \frac{-1}{z_f-z_n} & \frac{-z_n}{z_f-z_n}\\
0 & 0 & 0 & 1
\end{pmatrix}.$$

This is `ortho(left, right, bottom, top, z_near, z_far)`; the tests check the near/far planes
and a corner.

---

## 6. The SIMD seam (why this is scalar today)

Every operation above is written as plain scalar arithmetic over a SIMD-*friendly* layout:
`Vec4` and `Mat4` are 16-byte aligned and stored contiguously, so each `Vec4` fits one SSE/AVX
register and arrays of them stay aligned. `math/simd.hpp` is the single place a future
SSE/AVX/NEON backend plugs in. We deliberately **do not** hand-write intrinsics yet: there is
no renderer or ECS to benchmark against, and CLAUDE.md's rule is *measure before optimizing*.
The real math hot loops arrive with ECS transforms (M4) and PBR (M5); when they do, the
`Vec`/`Mat` operators gain intrinsic specializations behind that seam with **no change to their
public signatures**. Until then, clarity wins — which is the entire point of this document.
