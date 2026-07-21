# Signed distance fields — the cook, the sign problem, and how coarse is too coarse

Companion to the SDF cooker (`tools/asset-pipeline/src/sdf.rs`, M10.4a) and
[ADR-0032](../adr/0032-lighting-v2.md) §2. This is **the cook half only**: it builds a per-mesh (or
per-destructible-*part*) sampled signed-distance volume offline. The runtime clipmap that composes
many of these into one traceable field around the camera, the compute pass that stamps them in, and
the re-stamping that responds to destruction are **m10.4b**, a separate brick — nothing here touches
the RHI or a device. The byte layout the cook writes and `engine/assets` reads is in
[`docs/design/assets.md`](../design/assets.md)'s "mesh-SDF payload" section.

## 1. What a signed distance field is, and why it is the trace medium

A signed distance field is a scalar function $f : \mathbb{R}^3 \to \mathbb{R}$ where $|f(\mathbf p)|$
is the distance from $\mathbf p$ to the nearest point on some surface, and the *sign* says which side
$\mathbf p$ is on — negative inside the solid, positive outside. Two properties make this the right
medium for M10's GI probes to sphere-trace through (ADR-0032, decision 2):

- **It bounds how far a ray can safely step.** If $f(\mathbf p) = d$, no part of the surface can be
  closer than $d$, so a ray from $\mathbf p$ may advance by $d$ without risk of stepping through thin
  geometry — the whole idea behind *sphere tracing* (march by the field value, not a fixed step).
  Nothing about a mesh's raw triangle list gives you that guarantee for free.
- **It composes.** A global field built from many local ones is just the pointwise minimum of unsigned
  distances (with the sign of whichever surface is nearest) — no boolean/CSG machinery needed. That is
  exactly the shape of the runtime clipmap m10.4b builds: many cooked, per-instance volumes stamped
  (transformed, min-blended) into one grid around the camera, and a destroyed wall's SDF simply
  disappears from the min the next time its region re-stamps.

We *sample* the field on a regular grid (never keep it analytic/procedural) because the source is an
arbitrary triangle mesh with no closed form — the whole cook is "evaluate the true nearest-surface
function densely enough, once, offline, so the runtime only ever does a cheap trilinear lookup."

## 2. Unsigned distance: closest point on a triangle

For one triangle $(\mathbf a,\mathbf b,\mathbf c)$ and a query point $\mathbf p$, the closest point on
the triangle is not always the foot of the perpendicular onto its plane — that foot can fall outside
the triangle, in which case the true closest point is on an edge or at a vertex. The plane's *own*
barycentric coordinates partition it into exactly seven regions — one per vertex, one per edge, one
for the interior — and which region $\mathbf p$'s projection lands in is decided entirely by six dot
products (Ericson, *Real-Time Collision Detection* §5.1.5):

$$
d_1=\overrightarrow{ab}\!\cdot\!\overrightarrow{ap},\ \ d_2=\overrightarrow{ac}\!\cdot\!\overrightarrow{ap},\ \ 
d_3=\overrightarrow{ab}\!\cdot\!\overrightarrow{bp},\ \ d_4=\overrightarrow{ac}\!\cdot\!\overrightarrow{bp},\ \ 
d_5=\overrightarrow{ab}\!\cdot\!\overrightarrow{cp},\ \ d_6=\overrightarrow{ac}\!\cdot\!\overrightarrow{cp}.
$$

- $d_1\le 0 \wedge d_2\le 0 \Rightarrow$ closest point is $\mathbf a$ (barycentric $(1,0,0)$): $\mathbf p$
  is "behind" both edges leaving $\mathbf a$, so nothing on the triangle is closer than the vertex
  itself. The symmetric tests at $\mathbf b$ ($d_3\ge0,\,d_4\le d_3$) and $\mathbf c$
  ($d_6\ge0,\,d_5\le d_6$) are the same idea rotated.
- Edge $\mathbf{ab}$: $v_c = d_1 d_4 - d_3 d_2 \le 0 \wedge d_1\ge0 \wedge d_3\le0 \Rightarrow$ the
  closest point is $\mathbf a + t(\mathbf b - \mathbf a)$ with $t = d_1/(d_1-d_3)$ — a 1-D projection
  clamped onto the segment. Edges $\mathbf{ac}$ and $\mathbf{bc}$ are the same construction with the
  analogous $v_b,\,v_a$.
- Otherwise the projection lands in the face interior: $v = v_b/(v_a+v_b+v_c)$,
  $w = v_c/(v_a+v_b+v_c)$ are the barycentric weights, and the closest point is
  $\mathbf a + v\,\overrightarrow{ab} + w\,\overrightarrow{ac}$.

This one function returns both the distance **and** which feature (vertex/edge/face) the closest
point landed on — the classification §3 needs. It is implemented directly in
`sdf.rs::closest_point_on_triangle`.

### The BVH: making "nearest over every triangle" cheap

Evaluating every voxel against every triangle is $O(\text{voxels}\times\text{triangles})$ — fine for a
handful of triangles, not for anything bigger. The cooker builds its **own** bounding-volume
hierarchy over the mesh (a deliberate choice, ADR-0032's m10.4 kickoff: cook-side, teaching-friendly,
no new dependency): a binary tree of axis-aligned boxes, split top-down along the longest axis of the
current range's *centroids* and partitioned at the median. A query descends the tree with
**branch-and-bound pruning**: a box's nearest possible point to $\mathbf p$ is
$\sqrt{\sum_k \big(p_k - \mathrm{clamp}(p_k,\,b^{\min}_k,\,b^{\max}_k)\big)^2}$; if that lower bound
already exceeds the best distance found so far, the whole subtree is skipped without visiting a single
triangle inside it. This is the standard reason a spatial hierarchy turns an $O(n)$ nearest-point query
into something close to $O(\log n)$ in practice.

## 3. The sign problem — and why the naive fix is wrong

Distance alone is unsigned. The sign needs a **direction to compare against**: given the closest point
$\mathbf q$ on the surface, $\mathbf p$ is outside if $(\mathbf p - \mathbf q)\cdot \mathbf n \ge 0$
for the "right" surface normal $\mathbf n$ at $\mathbf q$.

**The naive approach** — use the flat face normal of *whichever triangle happened to be nearest* — is
correct when $\mathbf q$ is interior to that triangle's face (there is only one normal to consider).
It is **not** correct in general when $\mathbf q$ lands on an edge or vertex shared by several
triangles, because a single adjacent face's normal is only a valid stand-in for the surface's local
outward direction *within that face's own region* — using it for a query whose nearest feature sits on
the *boundary* between several faces' regions can disagree with an equally "nearest" neighbouring
face, and for some query directions it disagrees with the truth.

**A concrete, hand-verified counterexample** (exercised by
`sdf.rs::tests::sign_near_a_shared_vertex_uses_the_angle_weighted_pseudonormal_not_one_face_alone`):
take a regular tetrahedron with vertices

$$ V_0=(1,1,1),\quad V_1=(1,-1,-1),\quad V_2=(-1,1,-1),\quad V_3=(-1,-1,1), $$

outward-wound faces $T_0=(V_1,V_3,V_2)$ (opposite $V_0$), $T_1=(V_0,V_2,V_3)$ (opposite $V_1$),
$T_2=(V_0,V_3,V_1)$ (opposite $V_2$), $T_3=(V_0,V_1,V_2)$ (opposite $V_3$) — a closed, watertight,
consistently-wound solid. Query $\mathbf q = V_0 + (0,0,1) = (1,1,2)$, straight "up" in $z$ from the
vertex $V_0$ (**not** along the fully symmetric radial direction through the centroid — that direction
turns out *not* to expose the bug, because all three faces touching $V_0$ happen to agree there).

$\mathbf q$'s nearest feature is $V_0$ itself (every edge/face distance from $\mathbf q$ works out
larger). Ground truth, independent of any pseudonormal machinery: a point is outside a convex
polytope iff it is on the positive side of **at least one** face's plane. Using face $T_1$'s plane
(normal $\mathbf n_{T_1} = \widehat{(V_2{-}V_0)\times(V_3{-}V_0)} = \tfrac{1}{\sqrt3}(-1,1,1)$) as the
witness:

$$ (\mathbf q - V_0)\cdot \mathbf n_{T_1} = (0,0,1)\cdot\tfrac{1}{\sqrt3}(-1,1,1) = \tfrac{1}{\sqrt3} > 0 $$

— $\mathbf q$ is genuinely **outside**. Now compare the two sign methods at $V_0$:

- **Angle-weighted pseudonormal** (§ next): by the tetrahedron's symmetry every face subtends the same
  angle at $V_0$, so the three touching faces' normals average, unweighted, to
  $\tfrac13\big((-1,1,1)+(1,-1,1)+(1,1,-1)\big)/\sqrt3 = \tfrac{1}{\sqrt3}(1,1,1)$ — pointing exactly
  radially outward through $V_0$, the geometrically obvious "right" answer at a symmetric vertex.
  $(\mathbf q-V_0)\cdot\tfrac{1}{\sqrt3}(1,1,1) = \tfrac{1}{\sqrt3} > 0$ — **correctly outside**.
- **Naively, one adjacent face alone** — say $T_3$ (normal $\mathbf n_{T_3} = \tfrac{1}{\sqrt3}(1,1,-1)$,
  one of the three faces genuinely touching $V_0$):
  $(\mathbf q-V_0)\cdot\mathbf n_{T_3} = (0,0,1)\cdot\tfrac{1}{\sqrt3}(1,1,-1) = -\tfrac{1}{\sqrt3} < 0$
  — **wrongly inside**, purely because $T_3$'s own plane, extended infinitely, happens to have $\mathbf
  q$ on its interior side even though $\mathbf q$ is exterior to the actual solid.

Same vertex, same query, opposite answers depending on *which* of the three touching faces a naive
implementation happened to reach for. That arbitrariness — not "edges are always wrong," but "a
single face's normal is not a well-defined thing at a shared feature" — is the actual bug.

### The fix: angle-weighted pseudonormals (Bærentzen & Aanæs, 2005)

The correct generalization gives every vertex and every edge its **own** normal, built once, so the
sign test never has to guess which face "owns" a shared feature:

- **Face normal** — the ordinary flat normal, used when the closest point is face-interior.
- **Edge pseudonormal** — the normalized **sum** of the two faces sharing that edge. A well-formed
  (watertight, manifold) edge has exactly two; the two agree at the boundary between the two faces'
  own regions by construction, so the field is continuous across it.
- **Vertex pseudonormal** — the normalized sum of every incident face's normal, each weighted by the
  **interior angle that face subtends at the vertex** (not by face area). Weighting by the subtended
  angle is what makes this well-defined *independent of triangulation density*: it is a genuine
  measure of "how much of the vertex's surroundings does this face occupy" — an angle is intrinsic to
  the corner, whereas an unrelated neighbouring face's *area* has nothing to do with how much solid
  angle it takes up at this particular vertex. This is exactly the tetrahedron computation above: each
  of the three faces subtends the same angle at $V_0$ (the tetrahedron's faces are equilateral), so the
  weights are equal and the average is the simple mean — but for an irregular mesh the angle weights
  are what keep the result meaningful.

Both are precomputed once per mesh (`sdf.rs::precompute_pseudonormals`, one pass accumulating into a
per-vertex array and a per-undirected-edge map), then the sign test looks up whichever one applies to
the winning triangle's classified feature (§2) — no extra distance computation, just a table lookup.

### Watertightness is a well-formedness precondition, not an assumption we get to skip checking

The edge pseudonormal's "exactly two adjacent faces" premise is a property of a **closed, manifold**
mesh. The cooker counts, for every edge, how many triangles claim it while it accumulates
pseudonormals — free, since it is already visiting every triangle once — and **warns** (not errors)
whenever an edge's count isn't exactly two. A boundary edge (an open mesh) falls back to that one
face's own normal for anything whose nearest feature lands there; a non-manifold edge (three or more
triangles) averages whichever ones exist. Neither crashes, and neither is silently claimed correct —
see §5 for what "degrade honestly" means here in practice, including a real, non-obvious case this
cook actually hit.

## 4. Resolution: how many voxels, and how thin is too thin

`voxel_size` is fixed from a single number, `target_resolution`, applied to the mesh's own **longest**
unpadded AABB axis:

$$ h = \frac{\max(\text{extent}_x,\,\text{extent}_y,\,\text{extent}_z)}{\text{target\_resolution}}. $$

Every voxel is therefore a **cube** (one $h$ for all three axes), not an independently-scaled box —
sphere-tracing and trilinear sampling both assume isotropic step sizes. Each axis's voxel *count* is
then derived from the AABB **padded** by `padding_voxels·h` on every side (so the field stays defined
a little past the surface, which both the m10.4b compose pass and a sphere-trace's first few steps
want) and clamped to `[min_resolution, max_resolution]`. Two presets exist: a normal mesh
(`target_resolution = 32`) and a destructible **part** (`target_resolution = 8`, deliberately coarser
— parts are small and numerous, so this is a config choice, not a consequence of a part's smaller
physical size; see `docs/design/assets.md`'s payload section for why per-part granularity is the
destructible cooking unit).

**How thin a feature can this resolution actually represent?** A voxel grid is a sampled signal, and
the sampling theorem's floor is well known: a feature narrower than **two** voxels cannot be
distinguished from noise at all (its two surfaces alias onto each other). For a *signed* distance
field specifically, the practical floor is a little more conservative than the bare Nyquist limit: at
two voxels across, the two opposing surfaces' zero-crossings sit on adjacent voxel centres with almost
no room for a clean sign transition between them, so trilinear interpolation across the slab can blur
or even invert the local gradient. The cooker's default policy asks for **three** voxels across a
feature's thinnest span as a safety margin over the two-voxel hard floor — not a derived constant, an
engineering choice recorded here so it can be revisited with real destructible-wall data.

The check itself is a coarse, **honestly-labelled** proxy: the mesh's tightest *axis-aligned* AABB
extent stands in for "wall thickness." It correctly flags the common destructible-wall case (a slab
thin along one world axis) but does **not** catch a thin feature at an angle to every axis (a diagonal
fin) — a real limitation, not a silent gap; see §5. When the thinnest extent spans fewer than the
configured minimum voxels, the cook emits a warning naming the actual voxel count and this document,
rather than silently cooking a wall that disappears between two samples.

## 5. Limits, and what comes next

**Non-watertight / self-intersecting input degrades, it does not crash or silently lie.** §3 already
covers the watertightness warning; self-intersection (two parts of the same surface crossing each
other) is **not** detected at all in v1 — doing so robustly is its own $O(n\log n)$ triangle–triangle
intersection problem, out of scope here — so a self-intersecting mesh can produce a locally wrong sign
near the intersection with no warning. Both cases still return a finite, structurally valid volume
(every value checked finite before being trusted downstream); "wrong near a flaw in the input" is a
different, much better failure mode than "undefined behaviour" or "silently garbage everywhere."

**A genuinely surprising instance of the above, found while building this brick's cross-language
fixture:** a **flat-shaded / hard-edge** mesh — one where a vertex *position* is duplicated once per
adjacent face so each copy can carry that face's own normal (the common way to render sharp edges) —
looks non-watertight to this cooker, even though it is a perfectly closed solid in actual 3-D space.
The reason: pseudonormal sharing is keyed on the mesh's own **vertex indices**, and a hard-edge mesh
gives the two triangles meeting at a "shared" edge *different* vertex indices for the same point (one
per face, so each can carry a different normal) — so the edge-accumulation in §3 sees each of those
edges claimed by only one triangle, not two, and warns. The committed `cube.rsdf` fixture (cooked from
the existing hard-edge `cube.stl`) hits exactly this: the cook reports 24 non-manifold edges for a
mesh that is topologically a perfectly closed cube. The sign is still correct everywhere the fixture's
own tests sample (deliberately face-interior points, away from any edge), because a cube's edges are
all *convex*, where — as the tetrahedron example in §3 shows for a *shared vertex* case, but does not
universally guarantee for every configuration — a single adjacent face's normal is often still an
adequate stand-in; but this is exactly the kind of near-miss the honest-warning design exists to
surface rather than paper over. **This is a real, worth-fixing-later limitation**: a position-based
vertex weld (grouping by *coincident point*, not by *index*) as a preprocessing pass would let a
hard-edge mesh's topology be recognized correctly. It is out of this brick's scope (it is a nontrivial
feature — tolerance choice, performance — in its own right) and is recorded here rather than fixed
silently underneath a green test suite.

**Format seams left for later bricks, on purpose:**

- **The cooked payload is exact `f32`.** The cook is not the memory-constrained side of this pipeline —
  the runtime clipmap is (ADR-0032 §10: three cascades, budgeted, `R16Snorm`) — so v1 keeps this format
  simple and exact rather than prematurely compressing data a later, different-purpose format will
  reprocess anyway. `max_abs_distance` is recorded (free, alongside the sampling loop's own running
  max) specifically as the scale factor that *later* compressed encoding will need.
- **The runtime clipmap, its compose pass, and destruction-driven dirty-region re-stamping are m10.4b**
  — this brick produces the cooked volumes that composition will consume; it does not build the
  compositor.
- **Per-part destructible SDFs are cooked (`cook_destructible_part_sdf`, fan-triangulating a part's
  convex-hull CSR faces) but wiring "cook a destructible ⇒ automatically cook all its part SDFs" into
  the fracture cook's own output is deliberately deferred** — nothing downstream consumes a per-part
  SDF yet (that is m10.4b's compose pass), so automatically paying that cook cost today would have no
  consumer. The capability is built and tested; the automatic wiring is not.

## Verification pins (what the tests check)

- **Analytic correctness.** A box mesh's cooked field matches the standard box SDF,
  $d(\mathbf p)=\lVert\max(|\mathbf p|-\mathbf h,\,0)\rVert + \min\!\big(\max((|\mathbf p|-\mathbf
  h)_x,(|\mathbf p|-\mathbf h)_y,(|\mathbf p|-\mathbf h)_z),\,0\big)$ (Quilez's standard formula — the
  "outside" term is the length of the positive part of $|\mathbf p|-\mathbf h$, the "inside" term is
  the least-negative axis, clamped to $\le 0$), and a UV-sphere mesh's matches $\lVert\mathbf
  p\rVert - r$ — both checked **at every voxel centre**, inside and outside, within a
  discretization-scale tolerance.
- **Sign correctness.** Deep-inside samples read negative, well-outside samples read positive, on both
  shapes; the tetrahedron case in §3 is the specific, hand-verified proof that the naive single-face
  method disagrees with ground truth at a shared vertex while the angle-weighted pseudonormal agrees.
- **Determinism.** Cooking the same mesh twice is byte-identical (`sdf.rs::tests::cook_is_byte_stable`,
  and the fixture's own `cube_sdf_cook_is_byte_stable`).
- **Reader robustness.** Every field of the fixed header is validated (finite, positive where it must
  be, a known encoding, resolution within a sanity ceiling) before any allocation is sized from it, a
  stored sample's magnitude is cross-checked against the header's own `max_abs_distance`, and
  truncation at every byte length is rejected cleanly — `tests/assets/cooked_mesh_sdf_test.cpp`'s
  negative battery, the same discipline as every other cooked kind.
- **Cross-language fixture.** `cube.rsdf` (cooked from the existing `cube.stl`) round-trips: the Rust
  cooker still produces these exact bytes, and the C++ reader decodes them and samples them to the
  same analytic box values documented above.
