# Signed distance fields — the cook, the runtime clipmap, and how coarse is too coarse

Companion to the SDF cooker (`tools/asset-pipeline/src/sdf.rs`, M10.4a) and the runtime clipmap
(`engine/render/{include/rime/render/lighting/sdf_clipmap.hpp, src/lighting/sdf_clipmap.cpp,
shaders/sdf_compose.comp}`, M10.4b) — [ADR-0032](../adr/0032-lighting-v2.md) §2 and §10. §§1–5
below are **the cook**: building a per-mesh (or per-destructible-*part*) sampled signed-distance
volume offline. §§6–10 are **the runtime clipmap**: composing many cooked volumes into one
traceable field around the camera, incrementally, as the world moves and breaks. The byte layout
the cook writes and `engine/assets` reads is in [`docs/design/assets.md`](../design/assets.md)'s
"mesh-SDF payload" section; the clipmap touches the RHI and a device (the cook half never does).

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
- **The runtime clipmap, its compose pass, and destruction-driven dirty-region re-stamping are
  m10.4b** (§§6–10 below) — this cook produces the volumes that composition consumes.
- **Per-part destructible SDFs are cooked (`cook_destructible_part_sdf`, fan-triangulating a part's
  convex-hull CSR faces) but wiring "cook a destructible ⇒ automatically cook all its part SDFs" into
  the fracture cook's own output remains deliberately deferred** — the runtime clipmap (§6) can
  compose a per-part field the moment one is cooked and registered (`SdfClipmap::update_instance`
  takes any `MeshSdfAsset`, mesh or part alike), but nothing yet calls `cook_destructible_part_sdf`
  automatically when a destructible is cooked, nor registers the results with a clipmap when a wall
  breaks — that ECS/asset-loading wiring (which entity's part owns which cooked SDF) is m10.5's job,
  alongside the DDGI probes that are the field's first real consumer. The cook capability, the
  compose capability, and the C1/C2 dirty-tracking seams are all built and tested *today*; only the
  glue that discovers "this destructible's parts have cooked SDFs, register them" is not.

## 6. The runtime clipmap — composing many cooked fields into one traceable volume

A single cooked field (§1–5) only covers one mesh's local space. The GI probes (m10.5) need to
sphere-trace through the WHOLE scene near the camera — every wall, floor, and piece of debris,
composed into one field, in world space, updated as things move and break. A **clipmap** is the
standard answer to "I need a lot of world coverage but only fine detail near the viewer, and I
can't afford either infinite resolution or infinite memory": nest a handful of same-resolution
volumes at successively coarser voxel sizes, each centred (approximately — see §8) on the camera,
so memory is spent where it is most useful — dense right around the viewer, sparse far away —
rather than uniformly across a scene that is mostly not being looked at closely. This is the same
idea a mipmap chain applies to *texture resolution*; a clipmap applies it to *world space*.

Rime's clipmap (ADR-0032 §10, pre-decided) is **3 levels, 64³ voxels each, stored as R16Snorm**
(§7). Level $i$'s voxel size is $4^i$ times level 0's:

$$ v_0 = 0.125\text{ m}, \quad v_i = 4^i \, v_0 \ \Rightarrow \ v_0, v_1, v_2 = 0.125, 0.5, 2.0 \text{ m}. $$

A level's world-space coverage is $64 \, v_i$, giving $8\text{ m} / 32\text{ m} / 128\text{ m}$ for
levels 0/1/2. Level 0's voxel size matches the m10.4a destructible-**part** cook preset's own
target resolution (8 voxels across a part's longest axis) closely enough that a typical fractured
chunk is represented by several voxels across its thinnest dimension at the finest clipmap level —
the level a probe near a wall actually samples from. The $\times 4$ step (rather than, say,
$\times 2$) is chosen so that **3 levels comfortably span "a room" to "a city block"** without
needing a 4th: doubling would need one more level to reach the same 128 m outer radius, for a
technique whose entire cost model (§9) already scales with instance count × levels touched.
128 m is deliberately generous for the "a destructible urban block" headline scene (VISION.md §5);
a scene meaningfully larger than that in one direction is honestly outside what level 2 alone can
resolve, and is future work (either a 4th level or a coarser fallback) if a real scene ever
needs it — recorded here rather than silently assumed away.

## 7. The narrow-band encoding — why R16Snorm loses nothing this use needed

A cooked field (§1–5) stores exact `f32` distances because the cook is not memory-constrained. The
*composed* clipmap is: three 64³ volumes is only 3 MiB at 2 bytes/voxel (R16Snorm) but 6 MiB at 4
(f32) — and the field is only ever consumed one way here: a sphere-trace reads "how far can I step
without risk," for which a **bounded, quantized** answer is exactly as useful as an exact one,
*provided the bound is chosen honestly*.

Each level defines a **band** — the largest distance magnitude worth storing exactly:

$$ \text{band}_i = 4 \, v_i \quad (\Rightarrow 0.5\text{ m} / 2.0\text{ m} / 8.0\text{ m}\text{ for levels }0,1,2). $$

A voxel stores $\mathrm{clamp}(d / \text{band}, -1, 1)$; the GPU's R16Snorm decode
(`texel / 32767`, clamped) hands back that same $[-1,1]$ ratio, so reconstruction is
$d_{\text{reconstructed}} = \text{snorm} \times \text{band}$. Two honest consequences fall out of
this immediately:

- **Anything farther than `band` away is indistinguishable from anything farther still.** A voxel
  reading exactly $+1$ means "at least `band` away, direction unknown" — not "exactly `band` away."
  A sphere-trace (§10) that reads a saturated value must therefore step by exactly `band` (never
  more, since the truth could be exactly `band`; never *claiming* to know more, since it can't) —
  a **conservative, safe** step, just not the largest one a perfect oracle could take. This is
  exactly why the ray-marching literature calls this a *narrow-band* representation: precision is
  spent only near the surface, where it is needed, and the field degrades gracefully (never
  wrongly) farther out.
- **Quantization error is a fixed fraction of the band, not of the true distance.** The worst-case
  rounding error is $\text{band} / 32767$ — for level 0's $0.5\text{ m}$ band that is $\approx
  15\ \mu\text{m}$, utterly negligible next to the $0.125\text{ m}$ voxel size itself (§9's
  tolerance budget is dominated by voxel/trilinear error, not this).

Each cooked instance's OWN field (§1–5) is quantized the same way for upload — normalized by its
own `max_abs_distance` (a *different* number from any clipmap level's band, since an instance's
extent has nothing to do with which clipmap level currently contains it) — so a small object and a
128 m-wide level share one format and one hardware sampling path (`sampler3D`, trilinear, free
interpolation between the cook's voxel centres) without a second RHI format ever entering the
picture. See ADR-0032 §10's ledger entry for why this needed a genuine RHI top-up
(`shaderStorageImageExtendedFormats` — Vulkan's mandatory storage-image format list does not
include narrow/normalized single-channel formats without it) and `tests/rhi/volume_storage_test.cpp`
for the adjacent, separately-verified claim (a 3-D image + `Storage` + compute `imageStore`
composes at all on lavapipe) the clipmap's own compose pass depends on just as much.

## 8. Texel-snapping — the same anti-shimmer trick as the cascades, one level at a time

A clipmap level is "centred on the camera," but centring it EXACTLY (recomputing its origin from
the camera's continuously-moving position every frame) would mean every voxel's world position
drifts by a sub-voxel amount every frame too — and since the field is a quantized, discretely
sampled thing, a drifting sample grid reads as shimmer, exactly the translation-shimmer problem
`compute_cascades` (`engine/render/src/lighting/shadows.cpp`, docs/math/shadow-mapping.md §3)
already solved for cascaded shadow maps. The fix is the same idea, applied per level: snap the
level's origin — the world-space corner of voxel $(0,0,0)$ — to a whole multiple of **that level's
own** voxel size,

$$ \text{origin}_i = \left\lfloor \frac{\text{camera} - \tfrac12 \cdot 64 v_i}{v_i} \right\rfloor v_i, $$

so voxel centres sit on a fixed world-space lattice that never moves except in whole-voxel jumps.
Three consequences, all load-bearing for the dirty-tracking story in §9:

- **Sub-voxel camera motion causes the SAME floor result** — the snapped origin is bit-identical
  frame to frame, so nothing needs to recompose. This is the "zero recomposition" half of the
  brick's snap-stability test.
- **Each level snaps INDEPENDENTLY.** Because $v_0 < v_1 < v_2$, a camera move can cross a level-0
  voxel boundary while staying within the same level-1 (and level-2) voxel — the finer level
  recomposes, the coarser ones do not. This is a genuinely useful property (moving normally around
  a scene mostly perturbs only the near, cheap-to-recompose level), not just an implementation
  detail — see the `sdf_clipmap_test.cpp` "texel-snapping" proof for a hand-verified worked
  example.
- **A big move (past a level's own 64-voxel extent) necessarily changes that level's origin, and
  the v1 policy is: any origin change forces that level's ENTIRE volume to recompose** — not just
  the newly-exposed band at the leading edge, the way a scrolling/toroidal-addressed clipmap would.
  This is a deliberate simplification: correctness (a moved level always ends up fully, correctly
  populated) over the scroll optimization real-time GI implementations often use to avoid paying
  for the whole volume on every boundary crossing. It costs more while the camera is moving
  continuously through fine geometry (level 0's 0.125 m voxels mean almost any walking speed
  crosses one every frame or two) — measured, not hidden, and the natural follow-up if a profile
  ever asks for it.

## 9. Why min-blend composes — and what it costs

Composing $N$ instances' fields into one clipmap level is, at every voxel, "the distance to
whichever instance's surface is nearest" — the pointwise minimum of every instance's own (signed)
distance at that point (§ ADR-0032's decision 2: "a global field built from many local ones is
just the pointwise minimum of unsigned distances… no boolean/CSG machinery needed"). `min` is
commutative ($\min(a,b) = \min(b,a)$) and associative
($\min(\min(a,b),c) = \min(a,\min(b,c))$), so folding instances into a voxel one at a time, in
ANY order, across ANY number of separate dispatches, over ANY number of frames, gives the
identical result a from-scratch recompose of every live instance would. This single algebraic fact
is *why* the whole brick's incremental-recomposition story is sound: re-stamping only the
instances that changed, whenever invalidation happens to drain them, still converges to "the
closest surface any live instance puts here" — never a stale answer, never an order-dependent one.

**The cost.** The RHI has no bindless texture array (a deliberate M10.4b non-goal — inventing one
is its own brick), so each instance's cooked field is a separate `sampler3D` binding, and Rime's
descriptor model binds resources per dispatch (ADR-0020) — there is no way to hand a single
compute invocation "N textures, pick one by index" without one. The consequence, stated plainly
rather than hidden behind an abstraction: **composing $N$ instances into a dirty region costs $N$
compute dispatches**, one per (level, dirty-region, overlapping-instance) triple, plus one "clear
to +band" dispatch per (level, dirty-region) pair that runs first (so a re-stamp never inherits a
neighbour's stale minimum — see `sdf_compose.comp`'s header). Each dispatch's own *voxel* count is
bounded to (dirty region) ∩ (that instance's own cooked-grid extent in world space), not the whole
level, so the GPU-side cost of a small, localized change is small; the dispatch-COUNT cost is what
scales with how many separate instances happen to overlap a large simultaneous change (a big
camera jump forcing a full-level recompose, §8, is the worst case: every instance touching that
level, once). `SdfClipmapStats` (`stamps`/`clears`/`dirty_regions`/`levels_recomposed`) exists
specifically so this cost is a number a test (or a future profiling HUD) can read, not a guess.

**Dirty-region correctness — an EXACT bound, not merely a conservative one.** An instance's
"world bounds," for the purpose of deciding what to recompose when it moves, appears/disappears
or is explicitly invalidated, is the transform of its OWN cooked grid's extent
(`MeshSdfAsset::grid_origin`/`resolution`/`voxel_size` — the padded box the cook actually sampled,
not the tighter source-mesh AABB). This is exact, not conservative, *because* the compose shader
never writes a voxel whose local-space position falls outside that same box (it would otherwise
have to trust a sampler's `ClampToEdge` extrapolation at the edge, which is honest-but-meaningless
data — see `sdf_compose.comp`'s own comment on why it skips instead). An instance therefore
cannot have influenced any voxel outside its own grid extent, so recomposing exactly that box on
removal/move/invalidate is provably sufficient — no extra safety padding by a level's band or
anything else is needed, unlike, say, `LocalShadowMap`'s frustum-overlap test, which genuinely does
pad conservatively because a shadow frustum's influence is not so crisply bounded.

One further simplification, honestly conservative rather than exact: when SEVERAL unrelated
regions are dirty in the same level in the same frame, the CPU side clears+re-stamps their
BOUNDING BOX as one region, not each individually — guaranteeing a level's one clear dispatch can
never land between two of that frame's own stamps and erase one (see `SdfClipmap::add`'s comment).
The cost is occasionally recomposing a few voxels between two unrelated changes that, in
isolation, did not need it; paid only when multiple such changes coincide in one frame, which is
the uncommon case the "destroy one object, everything else is untouched" proof is not exercising.

## 10. Sphere tracing through the composed field

Sphere tracing (Hart, 1996) is the whole reason an SDF is worth tracing at all rather than a plain
occupancy/voxel grid: at any point $\mathbf p$ along a ray, the field value $f(\mathbf p)$ is a
lower bound on how far the ray can advance without risking passing through a surface — advance by
$f(\mathbf p)$, re-sample, repeat, until the value drops below a small stopping epsilon (a hit) or
the ray's budget runs out (a miss). Marching through the CLIPMAP specifically means, at each step,
picking the finest level whose current (snapped, §8) volume contains the sample point and
trilinearly sampling THAT level's texture (`sdf_sample` in
`tests/render/shaders/sdf_probe_trace.comp` — the reference m10.5 lifts verbatim; there is no
shader-include mechanism in this engine yet, see `sdf_compose.comp`'s own note, so this is written
out where it is first exercised rather than in a shared file that does not exist). Falling through
to a coarser level only when a point is outside the finer one's volume is a hard select at the
level boundary (no cross-level blend) — simple, and the boundary itself only matters many metres
out, where a probe's own footprint is already coarse. A step taken while the sample reads a
SATURATED value (§7) is a `band`-sized conservative step, not a wrong one — several such steps
happen while far from any surface, and the march converges quickly once it enters a level's
unsaturated (informative) range.

**The proof's tolerance.** `tests/render/sdf_clipmap_test.cpp`'s analytic-agreement test compares a
sphere-traced hit distance against the one-line analytic formula for a sphere/box centred at the
origin, within `2 × (finest level's voxel size) + 0.05 × (its band)`. The $2v$ term is the same
discretization/trilinear-interpolation margin the m10.4a cook's own analytic tests use
(`tools/asset-pipeline/src/sdf.rs`); the small band-relative term absorbs the march's own stopping
epsilon and any residual roughness right at a saturation boundary. Measured results sit well inside
this budget (a few millimetres of error against a several-centimetre tolerance at level 0's
0.125 m voxel size) — the margin is honest headroom, not a number chosen to make a shaky result
pass.

## Verification pins — the cook (what tools/asset-pipeline/src/sdf.rs's tests check)

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

## Verification pins — the runtime clipmap (what tests/render/sdf_clipmap_test.cpp checks)

- **Analytic agreement.** A composed sphere and box sphere-trace to their one-line analytic hit
  distance within the §10 tolerance — proof the compose (§6–7) and sample (§10) math is right, not
  merely that it runs.
- **The re-stamp proof (the headline).** A ray hits a box. Remove the instance and `invalidate()`
  its region (the C2 destruction hook). Recompose. The same ray now passes through
  (`sdf_sphere_trace` returns $-1$) — "break a wall, the light gets through" as an assertion, not a
  screenshot.
- **Dirty economy (ADR-0032 §11).** 16 non-overlapping instances fully recompose once (48 stamps:
  16 instances × 3 levels, 3 clears: one per level), then settle to EXACTLY zero stamps/clears on
  an unchanged camera with nothing invalidated. Destroying one of the 16 recomposes only its own
  (empty, once it's gone) neighbourhood — zero stamps, a bounded few clears — never touching the
  other 15.
- **Snap stability.** A sub-voxel camera nudge leaves every level's snapped origin bit-identical
  (§8) and declares zero passes. A move past level 0's own voxel (but, from a hand-verified
  starting point, not past level 1's or level 2's) recomposes level 0 alone — each level really
  does snap to its own grid independently, not as a group. A move past a level's whole extent
  forces (at least) that level fully.
- **The RHI spike (`tests/rhi/volume_storage_test.cpp`).** A compute dispatch `imageStore`s into
  every voxel of a genuine 3-D (`depth > 1`) storage image and reads every voxel back exactly —
  isolating "does this combination even work" from the clipmap's own R16Snorm-specific proof
  above, and the fix it found (`copy_texture_to_buffer` truncating a volume readback to its z=0
  slice) along with it.
