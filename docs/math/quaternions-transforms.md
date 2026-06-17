# Quaternions & Transforms — derivation notes (M1.4)

Companion to `engine/core/include/rime/core/math/quat.hpp` and `transform.hpp`. These notes
derive every formula the code implements. Conventions: unit quaternions, **Hamilton** product,
right-handed, **active** rotations, composition right-to-left — fixed in
[ADR-0005](../adr/0005-rotation-representation.md), atop the linear-algebra conventions of
[ADR-0004](../adr/0004-math-conventions.md). GitHub renders the `$…$` math.

---

## 0. Why quaternions

A rotation has only **3** degrees of freedom, yet the obvious encodings are each flawed: a
$3\times3$ matrix spends 9 numbers and *drifts* off the orthonormal manifold under repeated
multiplication (round-off accumulates; you must re-orthonormalize); Euler angles use 3 numbers
but suffer **gimbal lock** and have no clean interpolation. **Unit quaternions** use 4 numbers
on the unit 3-sphere $S^3$: composition is one multiply, staying on the manifold needs only a
*renormalize* (one `sqrt`), and there is a natural shortest-arc interpolation (slerp). That is
why orientation is *stored* as a quaternion and only *baked* to a matrix to be applied.

---

## 1. Quaternion algebra

A quaternion is $q = w + x\,\mathbf i + y\,\mathbf j + z\,\mathbf k$ with the defining relations
(Hamilton, 1843):

$$\mathbf i^2=\mathbf j^2=\mathbf k^2=\mathbf{ijk}=-1
\;\Longrightarrow\;
\mathbf{ij}=\mathbf k=-\mathbf{ji},\quad
\mathbf{jk}=\mathbf i=-\mathbf{kj},\quad
\mathbf{ki}=\mathbf j=-\mathbf{ik}.$$

Write $q=(w,\mathbf u)$ with scalar part $w$ and vector part $\mathbf u=(x,y,z)$. Expanding the
product $q_1 q_2$ with the distributive law and the relations above, then collecting the
scalar and vector terms, gives the compact form

$$q_1 q_2 = \bigl(w_1 w_2 - \mathbf u_1\!\cdot\!\mathbf u_2,\;\;
                  w_1 \mathbf u_2 + w_2 \mathbf u_1 + \mathbf u_1\!\times\!\mathbf u_2\bigr).$$

The cross product term is what makes quaternion multiplication **non-commutative** — exactly the
property 3-D rotations need (rotating about $x$ then $y$ ≠ $y$ then $x$). This is `operator*` in
`quat.hpp`; written out per component it is the four lines in the code. Like matrices it composes
**right-to-left**: in $q_1 q_2$, $q_2$ acts first (proved in §3).

The **conjugate** negates the vector part, $q^* = (w, -\mathbf u)$, and the **norm** is
$\lVert q\rVert^2 = q\,q^* = w^2 + \mathbf u\cdot\mathbf u$. For a **unit** quaternion
($\lVert q\rVert = 1$) the inverse is therefore just the conjugate, $q^{-1} = q^*$ — cheap and
exact, the reason we keep rotations normalized.

---

## 2. Unit quaternions rotate vectors — and why the *half* angle

Embed a vector $\mathbf v$ as the pure quaternion $v=(0,\mathbf v)$. The rotation is the
**sandwich**

$$v' = q\,v\,q^{*}.$$

First, the result is always pure (its scalar part is $0$), so $v'$ is again a vector — you can
check the scalar part of $q v q^*$ cancels. Expanding the double product (using the
scalar/vector product form of §1 twice) yields a closed form with **no quaternions left**:

$$\mathbf v' = (w^2 - \mathbf u\!\cdot\!\mathbf u)\,\mathbf v
             + 2(\mathbf u\!\cdot\!\mathbf v)\,\mathbf u
             + 2w\,(\mathbf u\!\times\!\mathbf v). \tag{2.1}$$

Now write the unit quaternion with an explicit angle, $q=(\cos\tfrac\theta2,\;\sin\tfrac\theta2\,\hat{\mathbf n})$,
so $w=\cos\tfrac\theta2$ and $\mathbf u=\sin\tfrac\theta2\,\hat{\mathbf n}$. Take a vector
$\mathbf v\perp\hat{\mathbf n}$ (so $\mathbf u\cdot\mathbf v=0$). Substituting into (2.1):

$$\mathbf v' = \underbrace{(\cos^2\tfrac\theta2-\sin^2\tfrac\theta2)}_{\cos\theta}\,\mathbf v
             + \underbrace{2\cos\tfrac\theta2\sin\tfrac\theta2}_{\sin\theta}\,(\hat{\mathbf n}\times\mathbf v)
           = \cos\theta\,\mathbf v + \sin\theta\,(\hat{\mathbf n}\times\mathbf v),$$

which is precisely **Rodrigues' formula** for rotating $\mathbf v$ about $\hat{\mathbf n}$ by
angle $\theta$. (The component of $\mathbf v$ along $\hat{\mathbf n}$ is left unchanged — work it
through and the parallel part is fixed.) So a quaternion built from $\theta/2$ produces a
rotation of $\theta$: the **half-angle** is intrinsic to the two-sided sandwich. This is also why
$q$ and $-q$ give the *same* rotation (replace $\theta/2$ by $\theta/2+\pi$): the map
$S^3 \to SO(3)$ is a **double cover**. Hence `quat_from_axis_angle` uses $\theta/2$, and
`same_rotation` compares $|q_1\cdot q_2|$ against $1$ rather than the raw components.

### 2.1 The fast `rotate()` form

Building the literal $q v q^*$ is two quaternion products. The code instead uses the
algebraically identical (for unit $q$)

$$\mathbf t = 2\,(\mathbf u\times\mathbf v),\qquad
  \mathbf v' = \mathbf v + w\,\mathbf t + \mathbf u\times\mathbf t.$$

Expanding $\mathbf u\times\mathbf t = 2\,\mathbf u\times(\mathbf u\times\mathbf v)
= 2\bigl((\mathbf u\!\cdot\!\mathbf v)\mathbf u-(\mathbf u\!\cdot\!\mathbf u)\mathbf v\bigr)$
(the BAC–CAB identity) recovers (2.1) exactly, while costing only two cross products and a couple
of adds. This is `rotate(q, v)`.

---

## 3. Composition is right-to-left

Apply $q_2$ then $q_1$ to a vector:
$q_1\bigl(q_2\,v\,q_2^{*}\bigr)q_1^{*} = (q_1 q_2)\,v\,(q_1 q_2)^{*}$,
using $(q_1 q_2)^{*} = q_2^{*} q_1^{*}$. So the single quaternion $q_1 q_2$ performs "$q_2$ first,
then $q_1$" — the **same right-to-left order as matrix multiplication** (ADR-0004 §3.1). A
rotation chain reads identically whether you store it as quaternions or matrices, which is the
whole point of matching the convention. (`quat_test.cpp`: `rotate(a*b, v) == rotate(a, rotate(b, v))`.)

---

## 4. Quaternion → rotation matrix

Equation (2.1) is linear in $\mathbf v$, so it *is* a matrix. Reading off the coefficients of
$\mathbf v=(1,0,0),(0,1,0),(0,0,1)$ for a unit $q=(x,y,z,w)$ gives the standard rotation matrix

$$R=\begin{pmatrix}
1-2(y^2+z^2) & 2(xy-wz)     & 2(xz+wy)\\
2(xy+wz)     & 1-2(x^2+z^2) & 2(yz-wx)\\
2(xz-wy)     & 2(yz+wx)     & 1-2(x^2+y^2)
\end{pmatrix},$$

which is `to_mat3`; `to_mat4` drops it into the upper-left block of an identity $4\times4$ (no
translation/scale). Because $R$ is orthonormal with $\det R = +1$, the tests check
`to_mat3(q) * v == rotate(q, v)` and `det(to_mat4(q)) == 1`.

### 4.1 Euler convenience

`quat_from_euler(x, y, z)` uses the **extrinsic X→Y→Z** convention: rotate about world $x$,
then $y$, then $z$, i.e. $q = q_z\,q_y\,q_x$ (rightmost — $x$ — applied first, per §3). It is
built directly from half-angle sines/cosines rather than three products. Euler input is offered
only as an authoring convenience (ADR-0005); the engine's internal form is always the quaternion.

---

## 5. Slerp — the shortest-arc interpolation

Two unit quaternions are points on $S^3$ separated by angle $\Omega$, where
$\cos\Omega = q_0\cdot q_1$. The natural interpolation is the **constant-speed great-circle arc**
between them. Seek $r(t)=a(t)\,q_0 + b(t)\,q_1$ that stays unit-length and makes angle $t\Omega$
with $q_0$ and $(1-t)\Omega$ with $q_1$. Dotting that requirement with $q_0$ and with $q_1$:

$$a + b\cos\Omega = \cos(t\Omega),\qquad a\cos\Omega + b = \cos\bigl((1-t)\Omega\bigr).$$

Solving the $2\times2$ system gives $a=\dfrac{\sin((1-t)\Omega)}{\sin\Omega}$,
$b=\dfrac{\sin(t\Omega)}{\sin\Omega}$, hence

$$\operatorname{slerp}(q_0,q_1;t)=\frac{\sin((1-t)\Omega)}{\sin\Omega}\,q_0
                                  +\frac{\sin(t\Omega)}{\sin\Omega}\,q_1.$$

Two practical guards, both in `slerp`:

1. **Shortest path.** Since $q_1$ and $-q_1$ are the same rotation, if $q_0\cdot q_1<0$ we flip
   $q_1$, so we never interpolate the "long way" (> 180°) around.
2. **Near-parallel fallback.** As $\Omega\to0$, $\sin\Omega\to0$ and the weights are ill-conditioned;
   there we return a normalized linear interpolation $\widehat{q_0+t(q_1-q_0)}$, which is
   numerically safe and visually identical at small angles.

The tests verify the endpoints, unit length along the path, and that the midpoint between the
identity and a 90° turn is a 45° turn.

---

## 6. Transforms (TRS)

A `Transform` stores translation $\mathbf t$, rotation $q$, and (possibly non-uniform) scale
$\mathbf s$. A point maps as

$$\mathbf p' = \mathbf t + R_q\,(\mathbf s \odot \mathbf p), \tag{6.1}$$

where $\odot$ is the component-wise (Hadamard) product and $R_q$ is the rotation of $q$:
**scale, then rotate, then translate**. Baking gives `to_matrix` $= T\,R\,S$ (the matrices of
§3/ADR-0004), and because matrix multiply also composes right-to-left, the baked matrix applies
the operations in the same order as (6.1) — so `transform_point(tf, p) == transform_point(to_matrix(tf), p)`
exactly (a tested invariant). A direction uses the same map without $\mathbf t$.

### 6.1 Composition

Substituting the child map into the parent map, $\mathbf p\mapsto P(C(\mathbf p))$ with
$X(\mathbf p)=\mathbf t_X + R_X(\mathbf s_X\odot\mathbf p)$, and matching it back to the TRS form:

$$\mathbf s_{PC} = \mathbf s_P \odot \mathbf s_C,\quad
  q_{PC} = q_P\,q_C,\quad
  \mathbf t_{PC} = \mathbf t_P + R_{q_P}\!\left(\mathbf s_P \odot \mathbf t_C\right)
                 = P(\mathbf t_C).$$

This is `operator*` (parent × child) — the scene-graph "world = parent_world × local". It is
**exact when scale is uniform**. Under *non-uniform* scale combined with rotation the true
composite contains **shear**, which a single TRS cannot represent; the result is then a close
approximation. When that matters, bake to `Mat4` and multiply — exact for any case.

### 6.2 Inverse

Invert (6.1): from $\mathbf p' = \mathbf t + R(\mathbf s\odot\mathbf p)$,

$$\mathbf p = \tfrac1{\mathbf s}\odot\Bigl(R^{-1}(\mathbf p' - \mathbf t)\Bigr)
            = \tfrac1{\mathbf s}\odot\bigl(R^{-1}\mathbf p'\bigr)
            - \tfrac1{\mathbf s}\odot\bigl(R^{-1}\mathbf t\bigr),$$

with $R^{-1}=R_{q^*}$ (conjugate). Identifying a TRS gives
$\mathbf s^{-1}=1/\mathbf s$, $q^{-1}=q^{*}$, and
$\mathbf t^{-1}=\mathbf s^{-1}\odot\bigl(R_{q^{*}}(-\mathbf t)\bigr)$ — `inverse(tf)`. Again this
is **exact for uniform scale** (where the scalar $1/s$ commutes with $R^{-1}$); for non-uniform
scale the exact inverse is a *scale-after-rotation*, not a TRS, so use the `Mat4` inverse. The
tests cover both: the TRS inverse round-trips a point under uniform scale, and the matrix path
round-trips under non-uniform scale.

---

## 7. Status

All of the above is scalar code over the same 16-byte-aligned, SIMD-friendly layout as the rest
of the math (`Quat` mirrors `Vec4`). Vectorization waits for a measured hot path (M4 transforms,
M6 animation) behind the `math/simd.hpp` seam — see ADR-0004 §6. Until then the code stays in
one-to-one correspondence with the derivations above, which is the point.
