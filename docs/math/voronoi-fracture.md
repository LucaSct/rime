# Voronoi fracture — partitioning a convex source into convex parts

Companion to the fracture cook (`tools/asset-pipeline/src/fracture.rs`, M8.1) and
[ADR-0029](../adr/0029-destruction-model.md). The cook turns a convex source — v1 is an axis-aligned
box: a wall, a column, a slab — into the convex **parts** of a destructible, plus the **bond** graph
and **anchors**. The partition is a **seeded Voronoi diagram** clipped to the box. This note derives
why that gives convex parts for free, how a cell's geometry is recovered, and where the mass numbers
come from (they reuse [`polyhedral-mass-properties.md`](polyhedral-mass-properties.md)).

The whole method is *half-spaces in, convex polytopes out* — no convex-hull construction is needed to
*generate* the cells (quickhull, ADR-0027's deferral, is the robustness backstop for a future
mesh-sourced fracture, not this box-sourced one). Everything is deterministic in the `(seed, config)`
pair (a SplitMix64 PRNG seeds the sites), the reproducibility the M11 replay contract needs.

## 1. Sites and cells

Seed `n` **sites** `p₀ … p_{n−1}` uniformly in the box. The Voronoi **cell** of site `pᵢ` is every
point closer to `pᵢ` than to any other site:

$$ C_i = \{\, x : |x - p_i|^2 \le |x - p_j|^2 \ \ \forall j \ne i \,\} \ \cap\ \text{box}. $$

Each part is one cell. A dense set of sites gives many small parts; a sparse set, few large ones. The
site count is the target part count.

## 2. The bisector is a half-space (so a cell is convex)

Expand the defining inequality for one pair `(i, j)`:

$$ |x - p_i|^2 \le |x - p_j|^2 \iff -2\,x\!\cdot\!p_i + |p_i|^2 \le -2\,x\!\cdot\!p_j + |p_j|^2 $$
$$ \iff \ x \cdot (p_j - p_i) \ \le\ \tfrac{1}{2}\big(|p_j|^2 - |p_i|^2\big). $$

The quadratic `|x|²` terms cancel — so the locus "closer to `pᵢ` than to `pⱼ`" is a **half-space**
`{ x : x·n ≤ d }` with normal `n = p_j − p_i` and offset `d = ½(|p_j|² − |p_i|²)`. Geometrically it is
the perpendicular bisector plane of the segment `pᵢpⱼ`, keeping the `pᵢ` side (check: `p_i·n − d =
½(p_i − p_j)·(p_j − p_i) = −½|p_j − p_i|² < 0`, so `pᵢ` satisfies it).

A cell is therefore the intersection of the six box faces (themselves half-spaces `±x_k ≤ h_k`) with
one bisector per other site — an **intersection of half-spaces**, which is convex by definition (the
intersection of convex sets is convex). No repair, no hull step: convexity is structural, exactly the
property [`register_hull`](../adr/0027-convex-hull-shapes.md) demands of every part.

## 3. From half-spaces to a mesh: vertex enumeration

A convex polytope is the intersection of its bounding half-spaces, but the runtime wants its **surface**
— vertices and faces. Recover them directly:

- **Vertices.** A vertex of the polytope is where three of its face planes meet. So for every triple of
  planes `(a, b, c)`, solve the 3×3 system `[nₐ; n_b; n_c] x = [dₐ; d_b; d_c]` (Cramer's rule via the
  adjugate: `x = (dₐ(n_b×n_c) + d_b(n_c×nₐ) + d_c(nₐ×n_b)) / det`, with `det = nₐ·(n_b×n_c)`; skip
  near-parallel triples where `det ≈ 0`). Keep `x` only if it satisfies **every** half-space
  (`x·n_k ≤ d_k + ε` for all `k`) — that is what makes it an actual corner of *this* cell, not of some
  larger unclipped polytope. De-duplicate coincident points (three planes through one corner report it
  three times).
- **Faces.** For each plane, gather the vertices lying on it (`|x·n_k − d_k| < ε`). Three or more of
  them form that plane's face; fewer means the plane only grazed the cell at an edge or point and
  contributes nothing.

For `P` planes this is `O(P³)` triples each tested against `O(P)` planes. A box-clipped cell has
`P = 6 + (n−1)` planes — a handful — so this brute force is comfortably fast at the part counts M8
fractures (tens); a spatial prune is a later optimization if a much finer partition ever needs it.

## 4. Ordering a face: a 2D convex loop

The vertices on one plane are an unordered set; a face needs them as a **loop, wound outward**. Project
them into the plane (an in-plane basis `(u, w)` with `w = n × u`), take the angle
`θ = atan2(x·w, x·u)` about the face centroid, and sort. Because the cell is convex, its every face is
a convex polygon, so the angular sort *is* the boundary order — no general 2D hull needed. Sorting with
`w = n × u` yields a counter-clockwise loop seen from the `+n` (outside) side, which is the outward
winding `register_hull` validates; consecutive duplicates and collinear midpoints are dropped so a face
reports its true vertex count. Faces are capped at 16 vertices (the hull-face cap); modest part counts
never approach it.

## 5. Volume, centre of mass, and the re-centring

A part's **volume** and **centre of mass** come straight from
[`polyhedral-mass-properties.md`](polyhedral-mass-properties.md) §§1–2: fan-triangulate each outward
face and sum signed tetrahedra with their apex at the origin,

$$ V = \sum_{\triangle} \tfrac{1}{6}\, a\cdot(b\times c), \qquad
   \bar{x} = \frac{1}{V}\sum_{\triangle} \tfrac{1}{6}\,\big(a\cdot(b\times c)\big)\,\frac{a+b+c}{4}, $$

using the same divergence-theorem identity and the same winding the hull store integrates with — so the
cook's volume and the value `hull_info()` reports after registration agree (the oracle test checks it).
Each part's vertices are then stored **re-centred on its COM**, and the COM is stored separately as the
part's placement: this keeps the engine-wide "a body's position *is* its COM" invariant, and means
registering the part re-centres it by ≈ 0. Uniform density is the v1 model, so a part's mass fraction is
its volume fraction — no per-part material yet (ADR-0029 §3).

## 6. Bonds and anchors

Two cells that **share a face** are Voronoi-adjacent: cell `i` has a face lying on the bisector toward
site `j` exactly when they are neighbours. That face's area — the summed cross-products of its
fan-triangulation, `½|Σ (v_k − v_0) × (v_{k+1} − v_0)|` — is the **bond strength** (a wider weld resists
harder). Bonds are emitted once per pair with `a < b` in ascending order (determinism). An **anchor** is
a part with a vertex on the source's attachment plane (a wall's base): those parts are pinned, and a
runtime connectivity solve treats a part as supported iff a chain of live bonds reaches an anchor.

## 7. Verification pins (what the tests check)

- **Volume is conserved.** The parts' volumes sum to the source box's volume (the partition tiles it
  with no gaps or overlaps) — `Σ Vᵢ ≈ w·h·d` to < 0.5%.
- **Every part is a valid convex hull.** Positive volume, ≥ 4 vertices, ≥ 4 faces, every face 3..16
  vertices with a consistent CSR index count — and, the real gate, every part **registers** into a
  `PhysicsWorld` (`register_hull` accepts it) and the whole set as one `register_compound`.
- **The graph is connected from the anchors.** Union-find over the bonds reaches every part from an
  anchored one — a wall is one solid until damage removes bonds.
- **Determinism.** The same `(seed, config)` cooks byte-identical output; a different seed gives a
  different partition of the same conserved volume.
