# GJK, EPA, and contact manifolds — exact convex collision

Companion to `engine/physics` (M7, ADR-0026). This note derives the geometry the M7.3 narrowphase
runs on: the Minkowski-difference reformulation that turns collision into a question about the
origin, the **GJK** distance/overlap test, the **EPA** penetration query, and how a single
penetration direction becomes a multi-point contact manifold. The *systems* reasoning — when each
route is chosen, the manifold cache, determinism — is in the design note
[`docs/design/physics.md`](../design/physics.md); contact **response** (the impulse solver) gets its
own note at M7.4.

## 1. The Minkowski difference

Let $A, B \subset \mathbb{R}^3$ be convex. Define their **Minkowski difference**

$$ A \ominus B = \{\, \mathbf{a} - \mathbf{b} : \mathbf{a} \in A,\ \mathbf{b} \in B \,\}. $$

It is itself convex, and it collapses both collision questions to the position of a *single point*
— the origin — relative to one convex set $M = A \ominus B$:

- **Overlap.** $A \cap B \neq \varnothing \iff \mathbf{0} \in M$. (Some $\mathbf{a}=\mathbf{b}$ iff
  their difference is $\mathbf{0}$.)
- **Distance.** When disjoint, $\operatorname{dist}(A,B) = \operatorname{dist}(\mathbf{0}, M)$, and
  the nearest point of $M$ to the origin encodes the nearest points of $A$ and $B$.
- **Penetration.** When overlapping, the shallowest separation is
  $\operatorname{dist}\!\big(\mathbf{0}, \partial M\big)$ — the shortest translation that carries
  the origin *out* of $M$.

GJK settles the first two; EPA settles the third. Neither ever *builds* $M$ (it can have millions
of faces) — they sample it through one query.

## 2. Support functions

The **support function** of a convex set $S$ in direction $\mathbf{d}$ returns its farthest point
along $\mathbf{d}$:

$$ s_S(\mathbf{d}) = \arg\max_{\mathbf{p} \in S}\ \mathbf{p} \cdot \mathbf{d}. $$

The identity that makes the whole method practical lets us sample $M$ without enumerating it:

$$ s_{A \ominus B}(\mathbf{d}) = s_A(\mathbf{d}) - s_B(-\mathbf{d}). $$

To go farthest along $\mathbf{d}$ in $A \ominus B$, go farthest along $\mathbf{d}$ in $A$ and along
$-\mathbf{d}$ in $B$. It is the *only* thing GJK and EPA ask of a shape — which is why the same two
algorithms will run convex hulls (M7.9) unchanged. Our primitives, in their local frame (a posed
shape rotates $\mathbf{d}$ into local space, answers, and rotates the answer back):

- **Sphere**, radius $r$: $\;s(\mathbf{d}) = r\,\hat{\mathbf{d}}$.
- **Box**, half-extents $\mathbf{h}$: $\;s(\mathbf{d})_i = \operatorname{sign}(d_i)\,h_i$ — an
  independent argmax per axis, because a box is a product of intervals.
- **Capsule**, core segment $\pm\,\text{hh}$ along local $Y$, radius $r$: a capsule is a segment
  Minkowski-*summed* with a ball, and supports of a sum add, so
  $\;s(\mathbf{d}) = \big(0,\ \operatorname{sign}(d_y)\,\text{hh},\ 0\big) + r\,\hat{\mathbf{d}}$.

For every Minkowski vertex $\mathbf{w} = s_A(\mathbf{d}) - s_B(-\mathbf{d})$ we store the two
witnesses $s_A(\mathbf{d})$ and $s_B(-\mathbf{d})$ separately. Barycentric weights over $M$'s
features then reconstruct the closest points **on $A$ and on $B$**, not merely in the difference.

## 3. GJK — is the origin in $M$, and if not, how far?

GJK keeps a **simplex**: 1–4 vertices of $M$ whose convex hull is its current best guess at the
feature of $M$ nearest the origin. Each iteration:

1. Find the point of the current simplex closest to the origin, and **reduce** the simplex to the
   minimal sub-feature (vertex / edge / triangle) that actually supports that point.
2. Search along $\mathbf{d} = -\mathbf{c}$, where $\mathbf{c}$ is that closest point.
3. Sample $\mathbf{w} = s_M(\mathbf{d})$. If $\mathbf{w}$ gets no closer to the origin than
   $\mathbf{c}$ does, the origin lies outside $M$ → **separated**; $\|\mathbf{c}\|$ is the distance.
4. Otherwise add $\mathbf{w}$; if the simplex becomes a tetrahedron enclosing the origin →
   **overlapping**, and it seeds EPA.

### 3.1 The distance subalgorithm

Step 1 is the heart: the closest point to the origin on a simplex, by Voronoi-region case analysis
(Ericson, *Real-Time Collision Detection* §5.1). For a triangle $\mathbf{a}\mathbf{b}\mathbf{c}$
there are seven regions — three vertices, three edges, the face interior — and edge dot products
such as $d_1 = (\mathbf{b}-\mathbf{a})\cdot(-\mathbf{a})$ decide which one the origin projects into.
Each region yields exact barycentric weights $\lambda_i$; the reduced feature keeps only the
vertices with nonzero weight. For a tetrahedron the origin is inside iff it sits on the interior
side of all four faces (each oriented by its opposite vertex); otherwise GJK recurses into the
nearest *outside* face. The weights do double duty:

$$ \mathbf{c} = \sum_i \lambda_i\, \mathbf{w}_i, \qquad
   \mathbf{p}_A = \sum_i \lambda_i\, \mathbf{a}_i, \qquad
   \mathbf{p}_B = \sum_i \lambda_i\, \mathbf{b}_i. $$

### 3.2 Termination

The support plane through $\mathbf{w}$ perpendicular to $\mathbf{c}$ *bounds* $M$: if $\mathbf{w}$
does not lie strictly closer to the origin than $\mathbf{c}$, no point of $M$ does. In squared
terms, GJK stops "separated" when

$$ \|\mathbf{c}\|^2 - \mathbf{c}\cdot\mathbf{w} \;\le\; \varepsilon_{\text{rel}}\,\|\mathbf{c}\|^2 . $$

When the origin is *inside* $M$ this test can never fire (every support past the origin keeps the
left side $\ge \|\mathbf{c}\|^2$), so overlap is detected by containment instead. Two more guards
keep floating point honest: a **duplicate-vertex** check (re-sampling a vertex means noise, not
geometry, is driving the loop) and a **monotonicity** check (exact GJK strictly decreases
$\|\mathbf{c}\|$; if it stops improving, accept the current upper bound). Epsilons are absolute at
metre scale — the engine's working range.

## 4. EPA — penetration depth and direction

When GJK reports overlap, the origin is inside $M$ and the penetration vector is the shortest
translation to $\partial M$. **EPA** grows a polytope *inside* $M$ until one face reaches the
boundary:

1. **Seed** a tetrahedron from GJK's terminal simplex. GJK can stop with fewer than four vertices,
   or a flat four, so we build a genuine 3-D start by **affine rank**: accept seed vertices while
   each strictly raises the rank (a distinct point, then one off the line, then one off the plane);
   when the seed runs dry, probe fixed axis directions in a fixed order. Determinism demands the
   same order every time.
2. **Orient** each face outward using the seed centroid, which is interior and *stays* interior as
   the polytope only ever grows — immune to the origin lying exactly on a face (the touching case).
3. **Expand.** Take the face nearest the origin, with unit normal $\mathbf{n}$ and plane distance
   $d$. Sample $\mathbf{w} = s_M(\mathbf{n})$ and measure how far $M$ extends past the face,
   $g = \mathbf{n}\cdot\mathbf{w} - d$. If $g \le \varepsilon_{\text{grow}}$ the face lies on
   $\partial M$: report $(\mathbf{n}, d)$. Otherwise delete every face **visible** from $\mathbf{w}$
   ($\mathbf{n}_f \cdot (\mathbf{w} - \mathbf{v}_f) > 0$), collect the **horizon** — the loop of
   edges bounding the hole, found because interior edges of the deleted region cancel in opposite
   directions — and stitch a fan of new faces from $\mathbf{w}$ to that loop. This is exactly the
   horizon step of incremental convex-hull construction.
4. **Read off** the witnesses: express the origin's projection onto the winning face in barycentric
   coordinates of its three Minkowski vertices, then apply those weights to the stored per-shape
   supports — the same trick as GJK, giving the deepest-overlap points on $A$ and $B$.

The posture on numerics is explicit rather than hidden: sliver faces are skipped, there is a
vertex/iteration budget, and on any dead end (a broken horizon, a flat difference) EPA returns the
best face found so far. A pair whose difference is *numerically* flat — which, because every EPA
pair involves a 3-D shape, means genuinely degenerate — is dropped for the tick, a conservative,
documented miss.

## 5. From one direction to a manifold

EPA yields a normal, a depth, and a single witness point — enough to un-overlap two spheres, but a
box set flat on a box would balance on that one point and rock. A resting contact needs a **patch**.
For polyhedra the pipeline **clips**:

- The **reference face** is the face most aligned with the contact normal (ties break toward body
  $a$, so the choice — and every feature id derived from it — cannot flip frame to frame on
  floating-point noise). The **incident face** is the other shape's face most anti-parallel to the
  normal.
- Clip the incident polygon against the reference face's side planes by **Sutherland–Hodgman**:
  walking the polygon edge by edge, keep endpoints on the inside of a half-space and insert the
  crossing point at parameter

  $$ t = \frac{d_{\text{cur}}}{\,d_{\text{cur}} - d_{\text{nxt}}\,}, $$

  where $d$ is the signed plane distance of each endpoint (the denominator's operands differ in
  sign at a crossing, so it is never zero).
- Keep the survivors below the reference plane; if more than four remain, **reduce** greedily and
  deterministically — the deepest point, then the farthest from it, then the two that add the most
  triangle-area spread — so the solver anchors the largest stable patch.

Each surviving point is stamped with a **feature id**: a hash-combine of the codes of the features
that made it — a box corner or face index, a capsule end, or, for a clip-born vertex, the pair
(incident edge, reference side-plane). Because the fixed windings and tie-breaks make the whole
pipeline a pure function of contact topology, the same physical contact hashes to the same id every
frame; the design note explains how the manifold cache turns that into warm starting.

## 6. Fast paths — the shrunk-shape trick

A sphere is a point $\oplus$ ball$(r)$ and a capsule is a segment $\oplus$ ball$(r)$, so any pair of
them (or either against a box) skips GJK/EPA entirely. Collide the *cores* — a point or a segment —
with closed forms, then inflate by the radii: with core distance $d$ along the core-to-core
direction $\hat{\mathbf{n}}$, the shapes touch iff $d < r_A + r_B$, with

$$ \text{depth} = (r_A + r_B) - d, \qquad \mathbf{n} = \hat{\mathbf{n}}. $$

The one subtlety is the closest points between two **segments** $[\mathbf p_1,\mathbf q_1]$ and
$[\mathbf p_2,\mathbf q_2]$: minimize $\lVert (\mathbf p_1 + s\,\mathbf d_1) - (\mathbf p_2 +
t\,\mathbf d_2) \rVert^2$ over the unit square $(s,t) \in [0,1]^2$, clamping region by region
(Ericson §5.1.9); the near-parallel case is detected via $\lVert \mathbf d_1 \times \mathbf d_2
\rVert^2$ and handled by projecting the overlapping span and emitting **two** points, so a capsule
lying flat cannot seesaw on a single arbitrary one.

## 7. Tolerances

All constants are absolute at metre scale (the engine's working range, bodies $\sim$0.05–10 m),
because the algorithms compare squared lengths and dot products directly. They are **calibration
points, not truths** — chosen to stop iterating once float's $\sim$7 significant digits are spent
— and are revisited against the M7.10 debris-scale stress harness:

| Constant | Meaning | Value |
|---|---|---|
| GJK `kRelEps` | relative squared-distance improvement to keep iterating | $10^{-4}$ |
| GJK `kTouchEps2` | squared distance below which the origin is "on" the simplex | $10^{-10}$ |
| EPA `kGrowthEps` | face growth (m) below which a face is on the boundary | $10^{-4}$ |
| EPA `kPlaneEps` | plane-side slop for face visibility | $10^{-5}$ |
| clip `kFaceTieEps` | reference-face tie bias (prefer $a$; keeps ids stable) | $10^{-4}$ |

## References

- G. van den Bergen, *Collision Detection in Interactive 3D Environments* — GJK with witnesses.
- G. Snethen, "Xenocollide" / E. Catto, Box2D & GDC talks — EPA, clipping, warm starting.
- C. Ericson, *Real-Time Collision Detection*, §5.1 — the closest-point subroutines used verbatim.
