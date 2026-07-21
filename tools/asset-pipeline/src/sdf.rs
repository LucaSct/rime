// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Cooked mesh signed-distance fields (M10.4a, ADR-0032 §2 — **the cook half only**; the runtime
//! clipmap, its compose pass, and dirty-region tracking are m10.4b). Given a triangle mesh, this
//! builds a regular grid of signed distances: unsigned distance is the nearest point-to-triangle
//! distance over an owned BVH (no dependency — a cook-side, teaching-friendly structure, per the
//! m10.4 kickoff decision), and the SIGN uses **angle-weighted pseudonormals** (Bærentzen & Aanæs,
//! 2005) rather than a naive "dot with the nearest triangle's flat face normal", which is wrong
//! near shared edges/vertices (see `precompute_pseudonormals` and docs/math/sdf.md §3 for the
//! derivation and a worked counterexample). Destructibles cook one coarse SDF per convex PART
//! (`cook_destructible_part_sdf`) — parts are small, so a low resolution suffices, and per-part
//! granularity is what lets a partially-broken wall's distance field respond to individual pieces
//! (the m10.4 "decide at kickoff" ruling). The byte layout is in `docs/design/assets.md`; the C++
//! reader is `engine/assets/{include/rime/assets/sdf_asset.hpp, src/cooked_reader.cpp}`.

use std::path::Path;

use crate::cooked::{wrap_container, ByteWriter, ASSET_KIND_MESH_SDF, MESH_SDF_SCHEMA_HASH};
use crate::manifest::ManifestEntry;
use crate::math::{cross, normalize};
use crate::{CookOutput, PipelineError};

type V3 = [f32; 3];

// ── Tiny vector helpers (the pipeline keeps its own dependency-free math; VISION: teach from code,
// mirrors fracture.rs's own local dot/sub/add/scale/len rather than growing a shared math crate for
// half a dozen three-line functions) ──────────────────────────────────────────────────────────────
fn dot(a: V3, b: V3) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
fn sub(a: V3, b: V3) -> V3 {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
fn add(a: V3, b: V3) -> V3 {
    [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
}
fn scale(a: V3, s: f32) -> V3 {
    [a[0] * s, a[1] * s, a[2] * s]
}
fn dist_sq(a: V3, b: V3) -> f32 {
    dot(sub(a, b), sub(a, b))
}

/// The `encoding` header value for plain `f32` signed distances — v1's only encoding (see
/// `docs/design/assets.md`'s MeshSdf payload section for the compression-seam reasoning).
pub const SDF_ENCODING_FLOAT32: u32 = 0;

/// How coarsely to cook a mesh's SDF. `voxel_size` is fixed from `target_resolution` voxels along
/// the mesh's own LONGEST (unpadded) AABB axis — so it is a near-cubic voxel, not an independent
/// per-axis choice — and every axis's resolution is then derived from the PADDED extent and
/// clamped to `[min_resolution, max_resolution]`. Two presets ship: `for_mesh()` (a normal
/// renderable mesh) and `for_destructible_part()` (one convex fracture piece — deliberately a
/// LOWER `target_resolution`, since parts are small and there can be many of them per destructible;
/// this is what makes a per-part cook "coarse" — it is a deliberate config choice here, not an
/// automatic consequence of the part's smaller physical size). See docs/math/sdf.md §4 for the
/// resolution/feature-size derivation behind these numbers.
#[derive(Clone, Copy, Debug)]
pub struct SdfCookConfig {
    /// Target voxel count along the mesh's longest unpadded AABB axis; this alone fixes the
    /// (cubic) `voxel_size`.
    pub target_resolution: u32,
    /// Hard floor applied to every axis's derived voxel count (protects a thin/flat axis from
    /// rounding down to nothing).
    pub min_resolution: u32,
    /// Hard ceiling applied to every axis's derived voxel count (bounds memory/cook time for an
    /// extreme aspect-ratio AABB).
    pub max_resolution: u32,
    /// Extra voxels of margin added on every side of the mesh's own AABB before resolution is
    /// derived, so the field stays DEFINED (not clamped/extrapolated) a little outside the source
    /// surface — the m10.4b compose pass and any sphere-trace both want to sample past the
    /// boundary.
    pub padding_voxels: u32,
    /// The minimum number of voxels we want to see across the mesh's thinnest AABB extent before
    /// warning that a thin feature (a wall!) may not be reliably represented. See docs/math/sdf.md
    /// §4 for the Nyquist-based derivation.
    pub min_voxels_across_thinnest_extent: u32,
}

impl SdfCookConfig {
    /// A normal renderable mesh: fine enough to resolve reasonable detail, capped well below a
    /// memory-worrying size.
    pub fn for_mesh() -> Self {
        Self {
            target_resolution: 32,
            min_resolution: 4,
            max_resolution: 64,
            padding_voxels: 2,
            min_voxels_across_thinnest_extent: 3,
        }
    }

    /// One destructible PART: deliberately coarse (a low target resolution) — parts are small and a
    /// destructible can have dozens of them, so the per-part cook must stay cheap in count × size.
    pub fn for_destructible_part() -> Self {
        Self {
            target_resolution: 8,
            min_resolution: 4,
            max_resolution: 16,
            padding_voxels: 1,
            min_voxels_across_thinnest_extent: 3,
        }
    }
}

/// A cooked signed-distance volume, ready to wrap into an RMA1 file. See `engine/assets`'s
/// `MeshSdfAsset` (the exact in-memory mirror) for the grid-layout contract: `distances` is x
/// fastest, then y, then z, one sample per voxel CENTRE.
pub struct SdfVolume {
    pub local_aabb_min: V3,
    pub local_aabb_max: V3,
    pub grid_origin: V3,
    pub voxel_size: f32,
    pub resolution: [u32; 3],
    pub max_abs_distance: f32,
    pub distances: Vec<f32>,
}

impl SdfVolume {
    pub fn voxel_count(&self) -> usize {
        self.resolution[0] as usize * self.resolution[1] as usize * self.resolution[2] as usize
    }

    /// The world/local-space centre of voxel (i,j,k) — the point each stored distance is sampled
    /// at. Public so tests (and any future tooling) can re-derive exactly what a stored value means
    /// without duplicating the formula.
    pub fn voxel_centre(&self, i: u32, j: u32, k: u32) -> V3 {
        [
            self.grid_origin[0] + (i as f32 + 0.5) * self.voxel_size,
            self.grid_origin[1] + (j as f32 + 0.5) * self.voxel_size,
            self.grid_origin[2] + (k as f32 + 0.5) * self.voxel_size,
        ]
    }

    /// Encode into a complete RMA1 file, returning `(bytes, asset_id)`. The field order matches
    /// `detail::MeshSdfHeaderV1` in the engine's reader exactly (see docs/design/assets.md).
    pub fn cook(&self) -> (Vec<u8>, u64) {
        let mut p = ByteWriter::new();
        for c in self.local_aabb_min {
            p.f32(c);
        }
        for c in self.local_aabb_max {
            p.f32(c);
        }
        for c in self.grid_origin {
            p.f32(c);
        }
        p.f32(self.voxel_size);
        for r in self.resolution {
            p.u32(r);
        }
        p.u32(SDF_ENCODING_FLOAT32);
        p.f32(self.max_abs_distance);
        for &d in &self.distances {
            p.f32(d);
        }
        wrap_container(ASSET_KIND_MESH_SDF, MESH_SDF_SCHEMA_HASH, &p.into_vec())
    }
}

/// What `compute_sdf` produces: the volume, plus zero or more warnings about things the cooker
/// still produced an honest (not silently wrong) answer for — a mesh thinner than its own voxel
/// size, or a mesh that is not watertight/manifold. Non-fatal by design: a warning does not stop
/// the cook (there is no "correct" alternative to fall back to; garbage-in-garbage-out for a
/// malformed source mesh is documented behaviour, not a crash), but a caller (the CLI, or a future
/// cook-time lint pass) can surface it.
pub struct SdfCookReport {
    pub volume: SdfVolume,
    pub warnings: Vec<String>,
}

// ── The BVH (own, dependency-free, cook-side only) ──────────────────────────────────────────────

/// One BVH node: either an INTERNAL node (`count == 0`, `left`/`right` index other nodes) or a LEAF
/// (`count > 0`, `[start, start+count)` indexes `Bvh::order`). Kept as a flat `Vec` rather than a
/// pointer tree — cache-friendlier, and there is no ownership subtlety to get wrong (ADR-0024 §7
/// "teach from the code": a data-oriented layout over an idiomatic-but-indirect object tree).
#[derive(Clone, Copy)]
struct BvhNode {
    bmin: V3,
    bmax: V3,
    left: u32,
    right: u32,
    start: u32,
    count: u32,
}

/// Which feature of the WINNING triangle a query's closest point landed on — face interior, an
/// edge, or a vertex. This is the whole reason `closest_point_on_triangle` returns more than just a
/// distance: the correct SIGN test needs to know which pseudonormal (face/edge/vertex) applies
/// (docs/math/sdf.md §3). Vertex/edge indices are LOCAL (0, 1, 2 within the winning triangle); the
/// caller maps them to the mesh's global vertex indices via that triangle's own index triple.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Feature {
    Vertex(u8),
    Edge(u8, u8),
    Face,
}

/// A leaf holds at most this many triangles before splitting further — small enough to keep
/// traversal shallow, large enough that leaves aren't dominated by node-visit overhead. Not
/// performance-critical at the mesh sizes this cooker targets (cook time is dominated by the O(grid
/// size) sampling loop, not the O(log n) BVH query it drives); chosen for simplicity.
const BVH_LEAF_SIZE: usize = 4;

struct Bvh {
    nodes: Vec<BvhNode>,
    order: Vec<u32>, // a permutation of triangle indices; leaves index a contiguous range of this
    root: u32,
}

impl Bvh {
    /// Build a BVH over `triangles` (index triples into `vertices`). `triangles` must be non-empty
    /// — `compute_sdf` guarantees this before ever constructing a `Bvh`, so this is a documented
    /// precondition rather than a public failure mode.
    fn build(vertices: &[V3], triangles: &[[u32; 3]]) -> Self {
        assert!(
            !triangles.is_empty(),
            "Bvh::build requires at least one triangle"
        );
        let n = triangles.len();
        let mut tmin = vec![[0.0f32; 3]; n];
        let mut tmax = vec![[0.0f32; 3]; n];
        let mut centroid = vec![[0.0f32; 3]; n];
        for (i, tri) in triangles.iter().enumerate() {
            let a = vertices[tri[0] as usize];
            let b = vertices[tri[1] as usize];
            let c = vertices[tri[2] as usize];
            let mut lo = a;
            let mut hi = a;
            for p in [b, c] {
                for k in 0..3 {
                    lo[k] = lo[k].min(p[k]);
                    hi[k] = hi[k].max(p[k]);
                }
            }
            tmin[i] = lo;
            tmax[i] = hi;
            centroid[i] = scale(add(add(a, b), c), 1.0 / 3.0);
        }

        let mut order: Vec<u32> = (0..n as u32).collect();
        let mut nodes = Vec::new();
        let root = Self::build_range(&mut nodes, &mut order, 0, n, &centroid, &tmin, &tmax);
        Bvh { nodes, order, root }
    }

    /// Recursively build the node covering `order[start..end]`, splitting along the LONGEST axis of
    /// the range's CENTROID bounds and partitioning at the median — a simple, deterministic
    /// top-down build (a proper SAH cost model is a later optimization if profiling ever asks for
    /// one; correctness of the traversal below does not depend on split quality, only on every leaf
    /// eventually being visited or soundly pruned).
    fn build_range(
        nodes: &mut Vec<BvhNode>,
        order: &mut [u32],
        start: usize,
        end: usize,
        centroid: &[V3],
        tmin: &[V3],
        tmax: &[V3],
    ) -> u32 {
        let mut bmin = [f32::INFINITY; 3];
        let mut bmax = [f32::NEG_INFINITY; 3];
        for &idx in &order[start..end] {
            let i = idx as usize;
            for k in 0..3 {
                bmin[k] = bmin[k].min(tmin[i][k]);
                bmax[k] = bmax[k].max(tmax[i][k]);
            }
        }

        let count = end - start;
        if count <= BVH_LEAF_SIZE {
            nodes.push(BvhNode {
                bmin,
                bmax,
                left: 0,
                right: 0,
                start: start as u32,
                count: count as u32,
            });
            return (nodes.len() - 1) as u32;
        }

        let mut cmin = [f32::INFINITY; 3];
        let mut cmax = [f32::NEG_INFINITY; 3];
        for &idx in &order[start..end] {
            let c = centroid[idx as usize];
            for k in 0..3 {
                cmin[k] = cmin[k].min(c[k]);
                cmax[k] = cmax[k].max(c[k]);
            }
        }
        let extent = [cmax[0] - cmin[0], cmax[1] - cmin[1], cmax[2] - cmin[2]];
        let axis = if extent[0] >= extent[1] && extent[0] >= extent[2] {
            0
        } else if extent[1] >= extent[2] {
            1
        } else {
            2
        };

        // Sort this range by centroid coordinate along `axis`; tie-broken by triangle index so the
        // split is fully deterministic even if the sort were ever swapped for an unstable one
        // (Rust's sort_by is already stable, so ties currently just keep arriving order — the
        // explicit tiebreak documents that intent rather than leaning on an implementation detail).
        order[start..end].sort_by(|&ia, &ib| {
            let ca = centroid[ia as usize][axis];
            let cb = centroid[ib as usize][axis];
            ca.partial_cmp(&cb)
                .unwrap_or(std::cmp::Ordering::Equal)
                .then(ia.cmp(&ib))
        });

        let mid = start + count / 2;
        let left = Self::build_range(nodes, order, start, mid, centroid, tmin, tmax);
        let right = Self::build_range(nodes, order, mid, end, centroid, tmin, tmax);
        nodes.push(BvhNode {
            bmin,
            bmax,
            left,
            right,
            start: 0,
            count: 0,
        });
        (nodes.len() - 1) as u32
    }

    /// Find the globally nearest point on any triangle to `p`: returns `(closest point, squared
    /// distance, winning triangle index, local feature within it)`.
    fn query_nearest(
        &self,
        vertices: &[V3],
        triangles: &[[u32; 3]],
        p: V3,
    ) -> (V3, f32, u32, Feature) {
        let mut best_point = [0.0f32; 3];
        let mut best_dist_sq = f32::INFINITY;
        let mut best_tri = 0u32;
        let mut best_feature = Feature::Face;
        self.visit(
            self.root,
            vertices,
            triangles,
            p,
            &mut best_point,
            &mut best_dist_sq,
            &mut best_tri,
            &mut best_feature,
        );
        (best_point, best_dist_sq, best_tri, best_feature)
    }

    #[allow(clippy::too_many_arguments)]
    fn visit(
        &self,
        node_idx: u32,
        vertices: &[V3],
        triangles: &[[u32; 3]],
        p: V3,
        best_point: &mut V3,
        best_dist_sq: &mut f32,
        best_tri: &mut u32,
        best_feature: &mut Feature,
    ) {
        let node = &self.nodes[node_idx as usize];
        // Branch-and-bound: a box's nearest possible point is at least this far from `p`. If that
        // lower bound already exceeds the best distance found so far, nothing in this subtree can
        // improve it — prune without descending. This is what keeps the query near O(log n) instead
        // of O(triangle count).
        if aabb_dist_sq(p, node.bmin, node.bmax) >= *best_dist_sq {
            return;
        }

        if node.count > 0 {
            for &ti in &self.order[node.start as usize..(node.start + node.count) as usize] {
                let tri = triangles[ti as usize];
                let a = vertices[tri[0] as usize];
                let b = vertices[tri[1] as usize];
                let c = vertices[tri[2] as usize];
                let (pt, d2, feature) = closest_point_on_triangle(p, a, b, c);
                if d2 < *best_dist_sq {
                    *best_dist_sq = d2;
                    *best_point = pt;
                    *best_tri = ti;
                    *best_feature = feature;
                }
            }
            return;
        }

        // Visit the nearer child first: pruning kicks in sooner (the farther child is much more
        // likely to be skipped once the nearer one has tightened best_dist_sq), though correctness
        // does not depend on the order — the box test above still guards every call.
        let (l, r) = (node.left, node.right);
        let dl = aabb_dist_sq(p, self.nodes[l as usize].bmin, self.nodes[l as usize].bmax);
        let dr = aabb_dist_sq(p, self.nodes[r as usize].bmin, self.nodes[r as usize].bmax);
        let (first, second) = if dl <= dr { (l, r) } else { (r, l) };
        self.visit(
            first,
            vertices,
            triangles,
            p,
            best_point,
            best_dist_sq,
            best_tri,
            best_feature,
        );
        self.visit(
            second,
            vertices,
            triangles,
            p,
            best_point,
            best_dist_sq,
            best_tri,
            best_feature,
        );
    }
}

/// Squared distance from `p` to the nearest point of the axis-aligned box `[bmin, bmax]` (0 if `p`
/// is inside) — the BVH traversal's pruning bound.
fn aabb_dist_sq(p: V3, bmin: V3, bmax: V3) -> f32 {
    let mut d2 = 0.0f32;
    for k in 0..3 {
        let c = p[k].clamp(bmin[k], bmax[k]);
        let diff = p[k] - c;
        d2 += diff * diff;
    }
    d2
}

/// Closest point on triangle `(a, b, c)` to `p`, classified by which Voronoi region of the triangle
/// (in barycentric space) it fell in — a vertex, an edge, or the face interior. This is the
/// standard algorithm (Ericson, *Real-Time Collision Detection* §5.1.5): walk the six half-plane
/// tests that partition the plane into the triangle's seven barycentric regions (3 vertices, 3
/// edges, 1 face), computed entirely from dot products against edges `ab`/`ac` — no trigonometry,
/// no division except inside the two regions that need it. Returns squared distance (callers that
/// only need magnitude skip a sqrt; the sign test needs the feature classification more than the
/// raw number). See docs/math/sdf.md §2 for the barycentric-region derivation.
fn closest_point_on_triangle(p: V3, a: V3, b: V3, c: V3) -> (V3, f32, Feature) {
    let ab = sub(b, a);
    let ac = sub(c, a);
    let ap = sub(p, a);
    let d1 = dot(ab, ap);
    let d2 = dot(ac, ap);
    if d1 <= 0.0 && d2 <= 0.0 {
        return (a, dist_sq(p, a), Feature::Vertex(0)); // barycentric (1,0,0)
    }

    let bp = sub(p, b);
    let d3 = dot(ab, bp);
    let d4 = dot(ac, bp);
    if d3 >= 0.0 && d4 <= d3 {
        return (b, dist_sq(p, b), Feature::Vertex(1)); // barycentric (0,1,0)
    }

    let vc = d1 * d4 - d3 * d2;
    if vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0 {
        let v = d1 / (d1 - d3);
        let pt = add(a, scale(ab, v));
        return (pt, dist_sq(p, pt), Feature::Edge(0, 1)); // on edge AB
    }

    let cp = sub(p, c);
    let d5 = dot(ab, cp);
    let d6 = dot(ac, cp);
    if d6 >= 0.0 && d5 <= d6 {
        return (c, dist_sq(p, c), Feature::Vertex(2)); // barycentric (0,0,1)
    }

    let vb = d5 * d2 - d1 * d6;
    if vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0 {
        let w = d2 / (d2 - d6);
        let pt = add(a, scale(ac, w));
        return (pt, dist_sq(p, pt), Feature::Edge(0, 2)); // on edge AC
    }

    let va = d3 * d6 - d5 * d4;
    if va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0 {
        let w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        let pt = add(b, scale(sub(c, b), w));
        return (pt, dist_sq(p, pt), Feature::Edge(1, 2)); // on edge BC
    }

    // Interior: the barycentric weights fall out of the same six dot products.
    let denom = 1.0 / (va + vb + vc);
    let v = vb * denom;
    let w = vc * denom;
    let pt = add(add(a, scale(ab, v)), scale(ac, w));
    (pt, dist_sq(p, pt), Feature::Face)
}

// ── Sign: angle-weighted pseudonormals (Bærentzen & Aanæs) ──────────────────────────────────────

/// Precomputed per-feature "generalized normals" the sign test looks up once the nearest feature is
/// known. See docs/math/sdf.md §3 for the full derivation and why this is the fix for the naive
/// "dot with the nearest triangle's face normal" bug.
struct Pseudonormals {
    face_normal: Vec<V3>,
    vertex_normal: Vec<V3>,
    /// Keyed by the undirected (global vertex index) pair, `a < b`.
    edge_normal: std::collections::BTreeMap<(u32, u32), V3>,
    /// Edges NOT shared by exactly two triangles — a boundary (open mesh) or a non-manifold
    /// junction. Zero for a closed, watertight, manifold mesh.
    non_manifold_edges: usize,
}

/// The interior angle the triangle `(p, prev, next)` subtends AT vertex `p` — the weight the
/// angle-weighted pseudonormal (Bærentzen & Aanæs, 2005) gives this face's contribution to `p`'s
/// vertex normal. Weighting by the SUBTENDED ANGLE (not face area) is what makes the vertex
/// pseudonormal well-defined independent of how finely a surface happens to be triangulated —
/// area-weighting would let an unrelated large/small neighbouring face skew a vertex's normal
/// regardless of how much of the vertex's surroundings it actually occupies.
fn angle_at(p: V3, prev: V3, next: V3) -> f32 {
    let u = normalize(sub(prev, p));
    let v = normalize(sub(next, p));
    dot(u, v).clamp(-1.0, 1.0).acos()
}

/// Build every face/vertex/edge pseudonormal the sign test needs, in one pass over the triangles.
fn precompute_pseudonormals(vertices: &[V3], triangles: &[[u32; 3]]) -> Pseudonormals {
    let mut face_normal = Vec::with_capacity(triangles.len());
    let mut vertex_accum = vec![[0.0f32; 3]; vertices.len()];
    let mut edge_accum: std::collections::BTreeMap<(u32, u32), (V3, u32)> =
        std::collections::BTreeMap::new();

    for tri in triangles {
        let (ia, ib, ic) = (tri[0], tri[1], tri[2]);
        let a = vertices[ia as usize];
        let b = vertices[ib as usize];
        let c = vertices[ic as usize];
        let n = normalize(cross(sub(b, a), sub(c, a)));
        face_normal.push(n);

        let ang_a = angle_at(a, b, c);
        let ang_b = angle_at(b, c, a);
        let ang_c = angle_at(c, a, b);
        for (k, &nk) in n.iter().enumerate() {
            vertex_accum[ia as usize][k] += ang_a * nk;
            vertex_accum[ib as usize][k] += ang_b * nk;
            vertex_accum[ic as usize][k] += ang_c * nk;
        }

        for &(u, v) in &[(ia, ib), (ib, ic), (ic, ia)] {
            let key = if u < v { (u, v) } else { (v, u) };
            let entry = edge_accum.entry(key).or_insert(([0.0f32; 3], 0u32));
            for (k, &nk) in n.iter().enumerate() {
                entry.0[k] += nk;
            }
            entry.1 += 1;
        }
    }

    let vertex_normal: Vec<V3> = vertex_accum.iter().map(|&v| normalize(v)).collect();
    let mut non_manifold_edges = 0usize;
    let mut edge_normal = std::collections::BTreeMap::new();
    for (&key, &(sum, count)) in &edge_accum {
        if count != 2 {
            non_manifold_edges += 1;
        }
        edge_normal.insert(key, normalize(sum));
    }

    Pseudonormals {
        face_normal,
        vertex_normal,
        edge_normal,
        non_manifold_edges,
    }
}

/// Map a (triangle, local feature) pair to its global pseudonormal.
fn pseudonormal_for(
    pn: &Pseudonormals,
    triangles: &[[u32; 3]],
    tri_idx: u32,
    feature: Feature,
) -> V3 {
    let tri = triangles[tri_idx as usize];
    match feature {
        Feature::Face => pn.face_normal[tri_idx as usize],
        Feature::Vertex(local) => pn.vertex_normal[tri[local as usize] as usize],
        Feature::Edge(l0, l1) => {
            let u = tri[l0 as usize];
            let v = tri[l1 as usize];
            let key = if u < v { (u, v) } else { (v, u) };
            pn.edge_normal[&key]
        }
    }
}

/// The signed distance from `p` to the mesh: unsigned distance from the BVH's nearest-triangle
/// query, signed by dotting `(p - closest_point)` against the CORRECT pseudonormal for whichever
/// feature (face/edge/vertex) the closest point landed on.
fn signed_distance(
    bvh: &Bvh,
    vertices: &[V3],
    triangles: &[[u32; 3]],
    pn: &Pseudonormals,
    p: V3,
) -> f32 {
    let (closest, d2, tri_idx, feature) = bvh.query_nearest(vertices, triangles, p);
    let n = pseudonormal_for(pn, triangles, tri_idx, feature);
    let to_p = sub(p, closest);
    let sign = if dot(to_p, n) >= 0.0 { 1.0f32 } else { -1.0f32 };
    sign * d2.sqrt()
}

// ── The cook entry point ─────────────────────────────────────────────────────────────────────────

/// Build a signed-distance volume for `triangles` (index triples into `vertices`), per `cfg`. Errors
/// only if there is no geometry to cook. Never panics on a malformed (non-watertight,
/// self-intersecting) mesh — it produces the honest best-effort answer the pseudonormal method
/// gives, and reports why in `SdfCookReport::warnings` rather than silently returning garbage
/// (docs/math/sdf.md §3/§5).
pub fn compute_sdf(
    vertices: &[V3],
    triangles: &[[u32; 3]],
    cfg: &SdfCookConfig,
) -> Result<SdfCookReport, PipelineError> {
    if vertices.is_empty() || triangles.is_empty() {
        return Err(PipelineError::Unsupported(
            "SDF cook needs at least one triangle".to_string(),
        ));
    }

    let mut lo = [f32::INFINITY; 3];
    let mut hi = [f32::NEG_INFINITY; 3];
    for v in vertices {
        for k in 0..3 {
            lo[k] = lo[k].min(v[k]);
            hi[k] = hi[k].max(v[k]);
        }
    }
    let extent = [hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]];

    // voxel_size is fixed from the LONGEST unpadded axis, so a voxel is a cube, not an
    // independently-scaled box per axis. A degenerate (point-like or perfectly flat) input floors
    // to a tiny epsilon rather than dividing by zero.
    let longest = extent.iter().cloned().fold(0.0f32, f32::max).max(1.0e-5);
    let voxel_size = longest / cfg.target_resolution as f32;

    // The thin-feature check (docs/math/sdf.md §4): a coarse, HONEST proxy using the tightest
    // AXIS-ALIGNED extent as a stand-in for "wall thickness" — it catches an axis-aligned thin
    // slab (the common destructible-wall case) but not a thin fin at an angle to every axis, which
    // is documented as a known limitation rather than silently claimed to be covered.
    let thinnest = extent.iter().cloned().fold(f32::INFINITY, f32::min);
    let mut warnings = Vec::new();
    let min_feature_span = cfg.min_voxels_across_thinnest_extent as f32 * voxel_size;
    if thinnest < min_feature_span {
        warnings.push(format!(
            "mesh's thinnest AABB extent ({thinnest:.5}) spans only {:.2} voxels at voxel_size \
             {voxel_size:.5} (want >= {}); thin features (walls!) may not be reliably represented \
             — see docs/math/sdf.md §4",
            thinnest / voxel_size,
            cfg.min_voxels_across_thinnest_extent
        ));
    }

    // Per-axis resolution from the PADDED extent, clamped to the configured floor/ceiling, with the
    // grid re-centred on the mesh's own AABB centre so padding/clamping grows or shrinks the volume
    // symmetrically instead of drifting it off the source geometry.
    let pad_world = cfg.padding_voxels as f32 * voxel_size;
    let mut resolution = [0u32; 3];
    let mut grid_origin = [0.0f32; 3];
    for k in 0..3 {
        let padded_extent = extent[k] + 2.0 * pad_world;
        let res = ((padded_extent / voxel_size).ceil().max(1.0) as u32)
            .clamp(cfg.min_resolution, cfg.max_resolution);
        resolution[k] = res;
        let centre = 0.5 * (lo[k] + hi[k]);
        grid_origin[k] = centre - 0.5 * res as f32 * voxel_size;
    }

    let bvh = Bvh::build(vertices, triangles);
    let pn = precompute_pseudonormals(vertices, triangles);
    if pn.non_manifold_edges > 0 {
        warnings.push(format!(
            "{} edge(s) are not shared by exactly two triangles — the mesh is not watertight; \
             sign near those boundaries may be unreliable — see docs/math/sdf.md §3/§5",
            pn.non_manifold_edges
        ));
    }

    let (rx, ry, rz) = (resolution[0], resolution[1], resolution[2]);
    let mut distances = vec![0.0f32; rx as usize * ry as usize * rz as usize];
    let mut max_abs_distance = 0.0f32;
    for kz in 0..rz {
        let z = grid_origin[2] + (kz as f32 + 0.5) * voxel_size;
        for jy in 0..ry {
            let y = grid_origin[1] + (jy as f32 + 0.5) * voxel_size;
            for ix in 0..rx {
                let x = grid_origin[0] + (ix as f32 + 0.5) * voxel_size;
                let d = signed_distance(&bvh, vertices, triangles, &pn, [x, y, z]);
                let idx = ix as usize + rx as usize * (jy as usize + ry as usize * kz as usize);
                distances[idx] = d;
                max_abs_distance = max_abs_distance.max(d.abs());
            }
        }
    }

    Ok(SdfCookReport {
        volume: SdfVolume {
            local_aabb_min: lo,
            local_aabb_max: hi,
            grid_origin,
            voxel_size,
            resolution,
            max_abs_distance,
            distances,
        },
        warnings,
    })
}

/// Fan-triangulate a convex hull's CSR face list — exactly the shape `fracture::Part` /
/// `DestructibleAsset` store (`face_counts` + `face_indices`) — into a plain triangle list. Every
/// face of a convex hull is itself convex, so fanning around each face's first vertex is EXACT (no
/// clipping, no approximation): the SDF cooker only ever speaks triangles, and this is the one step
/// that lets it consume a destructible part's hull directly.
pub fn triangulate_convex_faces(face_counts: &[u32], face_indices: &[u32]) -> Vec<[u32; 3]> {
    let mut triangles = Vec::new();
    let mut cursor = 0usize;
    for &count in face_counts {
        let count = count as usize;
        let face = &face_indices[cursor..cursor + count];
        for k in 1..count - 1 {
            triangles.push([face[0], face[k], face[k + 1]]);
        }
        cursor += count;
    }
    triangles
}

/// Cook a coarse SDF for one destructible PART (ADR-0032 §2's per-part decision, m10.4 "decide at
/// kickoff": per-part vs per-destructible granularity — per-part wins because it is what lets a
/// partially-broken wall's distance field respond to individual surviving pieces). Fan-triangulates
/// the part's own COM-centred CSR hull and runs the same `compute_sdf` a whole mesh uses, just with
/// the coarser `SdfCookConfig::for_destructible_part` preset. Composing per-part volumes into the
/// runtime clipmap is m10.4b's job — this is only the cook.
pub fn cook_destructible_part_sdf(
    part: &crate::fracture::Part,
    cfg: &SdfCookConfig,
) -> Result<SdfCookReport, PipelineError> {
    let triangles = triangulate_convex_faces(&part.face_counts, &part.face_indices);
    compute_sdf(&part.vertices, &triangles, cfg)
}

/// Cook a triangle mesh's SDF and write it as `<name>.rsdf` into `out_dir`, returning it plus its
/// one manifest entry (mirrors `cook_fracture`'s shape). Deliberately NOT wired into `cook_path`'s
/// per-extension dispatch: like a Destructible, an SDF is cooked via its own explicit call (the CLI's
/// `sdf` subcommand, or this function directly) rather than auto-discovered from a directory walk —
/// nothing downstream consumes an SDF yet (the runtime clipmap is m10.4b), so auto-cooking one for
/// every imported mesh today would only add cook time with no consumer.
pub fn cook_mesh_sdf(
    vertices: &[V3],
    triangles: &[[u32; 3]],
    cfg: &SdfCookConfig,
    name: &str,
    out_dir: &Path,
) -> Result<CookOutput, PipelineError> {
    let report = compute_sdf(vertices, triangles, cfg)?;
    for warning in &report.warnings {
        eprintln!("rime sdf: {warning}");
    }
    let (bytes, id) = report.volume.cook();
    let cooked_file = format!("{name}.rsdf");
    std::fs::create_dir_all(out_dir)?;
    std::fs::write(out_dir.join(&cooked_file), &bytes)?;
    Ok(CookOutput {
        cooked_files: vec![out_dir.join(&cooked_file)],
        manifest: vec![ManifestEntry {
            source_path: format!("sdf:{name}"),
            kind: "mesh_sdf",
            id,
            cooked_file,
        }],
        ..Default::default()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    // ── Test geometry: a box and a UV sphere, generated in-test (no fixture needed for these —
    // the checked-in cross-language fixture, cube.rsdf, is a separate, smaller proof; see
    // tools/asset-pipeline/tests/cook_fixture.rs) ───────────────────────────────────────────────

    /// An axis-aligned box, half-extents `h`, centred at the origin: 8 vertices, 12 triangles
    /// (2 per face), consistently outward-wound. Mirrors the corner/winding convention of
    /// `samples/03-icem-viewer/stl.hpp`'s `make_unit_cube()` (the source of the committed
    /// `cube.stl`/`cube.rsdf` fixtures) generalized to arbitrary half-extents.
    fn box_mesh(h: V3) -> (Vec<V3>, Vec<[u32; 3]>) {
        let c: [V3; 8] = [
            [-h[0], -h[1], -h[2]],
            [h[0], -h[1], -h[2]],
            [h[0], h[1], -h[2]],
            [-h[0], h[1], -h[2]],
            [-h[0], -h[1], h[2]],
            [h[0], -h[1], h[2]],
            [h[0], h[1], h[2]],
            [-h[0], h[1], h[2]],
        ];
        let faces: [[u32; 4]; 6] = [
            [0, 3, 2, 1], // -z
            [4, 5, 6, 7], // +z
            [0, 1, 5, 4], // -y
            [3, 7, 6, 2], // +y
            [0, 4, 7, 3], // -x
            [1, 2, 6, 5], // +x
        ];
        let mut triangles = Vec::with_capacity(12);
        for f in &faces {
            triangles.push([f[0], f[1], f[2]]);
            triangles.push([f[0], f[2], f[3]]);
        }
        (c.to_vec(), triangles)
    }

    /// The analytic signed distance from `p` to a box of half-extents `h` centred at the origin
    /// (Inigo Quilez's standard box SDF: the "outside" term is the length of the positive part of
    /// `|p|-h`, the "inside" term is the least-negative axis of `|p|-h` clamped to <= 0 — see
    /// docs/math/sdf.md §5 for the derivation and why it is the ground truth these tests check
    /// against).
    fn analytic_box_distance(p: V3, h: V3) -> f32 {
        let q = [p[0].abs() - h[0], p[1].abs() - h[1], p[2].abs() - h[2]];
        let outside = [q[0].max(0.0), q[1].max(0.0), q[2].max(0.0)];
        let outside_len =
            (outside[0] * outside[0] + outside[1] * outside[1] + outside[2] * outside[2]).sqrt();
        let inside = q[0].max(q[1]).max(q[2]).min(0.0);
        outside_len + inside
    }

    /// A UV sphere of radius `r`: `rings` latitude bands x `segments` longitude bands, triangulated
    /// with a fan at each pole. Fine enough tessellation keeps the chord-vs-arc faceting error small
    /// relative to the grid's own voxel tolerance (checked empirically by the tests below).
    fn uv_sphere_mesh(r: f32, rings: u32, segments: u32) -> (Vec<V3>, Vec<[u32; 3]>) {
        assert!(rings >= 2 && segments >= 3);
        // ONE vertex at each pole (not `segments` coincident copies — that would make every
        // pole-adjacent edge ambiguous between "shared by 2 faces" and "shared by many
        // coincident-but-distinct edges", which is a meshing bug, not a real non-manifold seam) and
        // `segments` vertices per interior latitude ring (ring = 1..=rings-1).
        let mut vertices = vec![[0.0f32, r, 0.0]]; // index 0: north pole
        let ring_start = |ring: u32| 1 + (ring - 1) * segments; // first index of interior ring `ring`
        for ring in 1..rings {
            let phi = std::f32::consts::PI * ring as f32 / rings as f32; // (0, PI), poles excluded
            let y = r * phi.cos();
            let ring_r = r * phi.sin();
            for seg in 0..segments {
                let theta = 2.0 * std::f32::consts::PI * seg as f32 / segments as f32;
                vertices.push([ring_r * theta.cos(), y, ring_r * theta.sin()]);
            }
        }
        let south_pole = vertices.len() as u32;
        vertices.push([0.0, -r, 0.0]);

        let mut triangles = Vec::new();
        // North cap: fan from the pole to the first interior ring. Winding (pole, b, a) — verified
        // by hand (docs/math/sdf.md's derivation companion) to point outward at the north pole;
        // the opposite order would wind this cap inward while every other band winds outward.
        for seg in 0..segments {
            let a = ring_start(1) + seg;
            let b = ring_start(1) + (seg + 1) % segments;
            triangles.push([0, b, a]);
        }
        // Middle bands between consecutive interior rings.
        for ring in 1..(rings - 1) {
            for seg in 0..segments {
                let a = ring_start(ring) + seg;
                let b = ring_start(ring) + (seg + 1) % segments;
                let c = ring_start(ring + 1) + seg;
                let d = ring_start(ring + 1) + (seg + 1) % segments;
                triangles.push([a, b, c]);
                triangles.push([b, d, c]);
            }
        }
        // South cap: fan from the last interior ring to the pole. Winding (pole, a, b) — the
        // mirror-image order from the north cap, since the pole sits on the opposite side.
        for seg in 0..segments {
            let a = ring_start(rings - 1) + seg;
            let b = ring_start(rings - 1) + (seg + 1) % segments;
            triangles.push([south_pole, a, b]);
        }
        (vertices, triangles)
    }

    #[test]
    fn box_sdf_matches_analytic_distance_at_every_voxel_centre() {
        let h = [1.0f32, 1.0, 1.0];
        let (verts, tris) = box_mesh(h);
        let report = compute_sdf(&verts, &tris, &SdfCookConfig::for_mesh()).unwrap();
        assert!(
            report.warnings.is_empty(),
            "a cube should need no warnings: {:?}",
            report.warnings
        );
        let vol = &report.volume;
        let tol = 1.5 * vol.voxel_size; // discretization tolerance, not a fudge for a wrong answer

        let mut checked_inside = 0;
        let mut checked_outside = 0;
        for kz in 0..vol.resolution[2] {
            for jy in 0..vol.resolution[1] {
                for ix in 0..vol.resolution[0] {
                    let p = vol.voxel_centre(ix, jy, kz);
                    let got = vol.distances[vol_index(vol, ix, jy, kz)];
                    let want = analytic_box_distance(p, h);
                    assert!(
                        (got - want).abs() <= tol,
                        "voxel ({ix},{jy},{kz}) at {p:?}: got {got}, analytic {want}, tol {tol}"
                    );
                    if want < -tol {
                        checked_inside += 1;
                    } else if want > tol {
                        checked_outside += 1;
                    }
                }
            }
        }
        assert!(
            checked_inside > 0 && checked_outside > 0,
            "the sweep must exercise both sides"
        );
    }

    #[test]
    fn sphere_sdf_matches_analytic_distance_at_every_voxel_centre() {
        let r = 1.0f32;
        let (verts, tris) = uv_sphere_mesh(r, 24, 24);
        let report = compute_sdf(&verts, &tris, &SdfCookConfig::for_mesh()).unwrap();
        assert!(
            report.warnings.is_empty(),
            "a sphere should need no warnings: {:?}",
            report.warnings
        );
        let vol = &report.volume;
        // A UV-sphere's chords sit slightly inside the true sphere (the faceting error), on top of
        // the grid's own discretization error — both are small at this tessellation, but the
        // tolerance has to cover both, not just the grid.
        let tol = 2.0 * vol.voxel_size + 0.02 * r;

        let mut checked_inside = 0;
        let mut checked_outside = 0;
        for kz in 0..vol.resolution[2] {
            for jy in 0..vol.resolution[1] {
                for ix in 0..vol.resolution[0] {
                    let p = vol.voxel_centre(ix, jy, kz);
                    let got = vol.distances[vol_index(vol, ix, jy, kz)];
                    let want = (p[0] * p[0] + p[1] * p[1] + p[2] * p[2]).sqrt() - r;
                    assert!(
                        (got - want).abs() <= tol,
                        "voxel ({ix},{jy},{kz}) at {p:?}: got {got}, analytic {want}, tol {tol}"
                    );
                    if want < -tol {
                        checked_inside += 1;
                    } else if want > tol {
                        checked_outside += 1;
                    }
                }
            }
        }
        assert!(
            checked_inside > 0 && checked_outside > 0,
            "the sweep must exercise both sides"
        );
    }

    /// Helper mirroring `MeshSdfAsset::index` (x fastest, then y, then z) so the tests read the
    /// same layout the cooked bytes carry.
    fn vol_index(vol: &SdfVolume, i: u32, j: u32, k: u32) -> usize {
        i as usize
            + vol.resolution[0] as usize * (j as usize + vol.resolution[1] as usize * k as usize)
    }

    #[test]
    fn sign_is_negative_deep_inside_and_positive_well_outside_a_box() {
        let h = [1.0f32, 0.75, 0.5];
        let (verts, tris) = box_mesh(h);
        let report = compute_sdf(&verts, &tris, &SdfCookConfig::for_mesh()).unwrap();
        let vol = &report.volume;
        for kz in 0..vol.resolution[2] {
            for jy in 0..vol.resolution[1] {
                for ix in 0..vol.resolution[0] {
                    let p = vol.voxel_centre(ix, jy, kz);
                    let want = analytic_box_distance(p, h);
                    let got = vol.distances[vol_index(vol, ix, jy, kz)];
                    // Only assert the SIGN where the analytic answer is comfortably away from the
                    // surface (the discretization band right at the boundary is expected to be
                    // ambiguous — that's what the tolerance-based test above already covers).
                    let margin = 2.0 * vol.voxel_size;
                    if want < -margin {
                        assert!(
                            got < 0.0,
                            "voxel at {p:?} should read negative (inside), got {got}"
                        );
                    } else if want > margin {
                        assert!(
                            got > 0.0,
                            "voxel at {p:?} should read positive (outside), got {got}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn angle_at_a_right_angle_corner_is_a_quarter_turn() {
        // p=(0,0,0), prev=(1,0,0), next=(0,1,0): the two edges from p are perpendicular.
        let a = angle_at([0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]);
        assert!((a - std::f32::consts::FRAC_PI_2).abs() < 1.0e-5);
    }

    #[test]
    fn edge_pseudonormal_is_the_normalized_sum_of_its_two_adjacent_face_normals() {
        // Two triangles sharing edge (0,1): a flat quad in the XY plane (both face normals +Z), so
        // the edge pseudonormal must come out exactly +Z, not some other direction.
        let verts = vec![
            [0.0, 0.0, 0.0],
            [1.0, 0.0, 0.0],
            [1.0, 1.0, 0.0],
            [0.0, 1.0, 0.0],
        ];
        let tris = vec![[0u32, 1, 2], [0, 2, 3]];
        let pn = precompute_pseudonormals(&verts, &tris);
        assert_eq!(
            pn.non_manifold_edges, 4,
            "an open quad's 4 boundary edges are each 1-triangle"
        );
        let shared = pn.edge_normal[&(0, 2)]; // the diagonal both triangles share
        assert!(
            (shared[2] - 1.0).abs() < 1.0e-5
                && shared[0].abs() < 1.0e-5
                && shared[1].abs() < 1.0e-5
        );
    }

    #[test]
    fn a_single_open_triangle_reports_non_manifold_edges_and_still_returns_a_finite_answer() {
        // One lone triangle: every one of its 3 edges is a boundary (1 adjacent face, not 2). The
        // cooker must degrade HONESTLY (a warning, a finite number) rather than crash or silently
        // fabricate a watertight answer (docs/math/sdf.md §5).
        let verts = vec![[0.0, 0.0, 0.0], [1.0, 0.0, 0.0], [0.0, 1.0, 0.0]];
        let tris = vec![[0u32, 1, 2]];
        let report = compute_sdf(&verts, &tris, &SdfCookConfig::for_mesh()).unwrap();
        assert!(report.warnings.iter().any(|w| w.contains("not watertight")));
        assert!(report.volume.distances.iter().all(|d| d.is_finite()));
    }

    #[test]
    fn a_thin_wall_triggers_the_thin_feature_warning() {
        // A 2m x 1.5m x 5cm slab (half-extents 1, 0.75, 0.025): thinner, along Z, than the mesh
        // preset's 3-voxel minimum at this longest-axis resolution.
        let h = [1.0f32, 0.75, 0.025];
        let (verts, tris) = box_mesh(h);
        let report = compute_sdf(&verts, &tris, &SdfCookConfig::for_mesh()).unwrap();
        assert!(
            report.warnings.iter().any(|w| w.contains("thin features")),
            "expected a thin-feature warning, got {:?}",
            report.warnings
        );
    }

    #[test]
    fn cook_is_byte_stable() {
        let (verts, tris) = box_mesh([1.0, 1.0, 1.0]);
        let a = compute_sdf(&verts, &tris, &SdfCookConfig::for_mesh())
            .unwrap()
            .volume
            .cook();
        let b = compute_sdf(&verts, &tris, &SdfCookConfig::for_mesh())
            .unwrap()
            .volume
            .cook();
        assert_eq!(
            a.0, b.0,
            "cooking the same mesh twice must be byte-identical"
        );
        assert_eq!(a.1, b.1);
    }

    #[test]
    fn destructible_part_preset_is_coarser_than_the_mesh_preset() {
        let (verts, tris) = box_mesh([0.1, 0.1, 0.1]); // a small "part"-scale box
        let mesh_report = compute_sdf(&verts, &tris, &SdfCookConfig::for_mesh()).unwrap();
        let part_report =
            compute_sdf(&verts, &tris, &SdfCookConfig::for_destructible_part()).unwrap();
        assert!(
            part_report.volume.voxel_count() < mesh_report.volume.voxel_count(),
            "the destructible-part preset must cook a coarser (fewer-voxel) volume"
        );
    }

    #[test]
    fn triangulate_convex_faces_fans_a_quad_into_two_triangles() {
        // A single 4-vertex face (indices 0,1,2,3): should fan into (0,1,2) and (0,2,3).
        let tris = triangulate_convex_faces(&[4], &[0, 1, 2, 3]);
        assert_eq!(tris, vec![[0, 1, 2], [0, 2, 3]]);
    }

    /// The adversarial case the naive "dot with the nearest triangle's flat face normal" gets
    /// wrong: a REGULAR TETRAHEDRON (a closed, convex, watertight solid — 4 vertices, 4 faces, each
    /// vertex shared by exactly 3 faces), queried at a point whose nearest feature is a shared
    /// VERTEX. docs/math/sdf.md §3 derives this example in full (the exact dot products both
    /// methods compute); in short: querying straight "up" in Z from vertex V0=(1,1,1) lands a point
    /// that is genuinely OUTSIDE the solid (confirmed independently via the face-plane half-space
    /// test), for which the angle-weighted vertex pseudonormal gives the correct positive sign,
    /// while naively using just ONE of the three faces touching V0 (the one "opposite" V3) gives a
    /// NEGATIVE (wrong) sign — because that lone face's plane, extended infinitely, happens to have
    /// the query point on its interior side even though the point is exterior to the actual solid.
    #[test]
    fn sign_near_a_shared_vertex_uses_the_angle_weighted_pseudonormal_not_one_face_alone() {
        let v0: V3 = [1.0, 1.0, 1.0];
        let v1: V3 = [1.0, -1.0, -1.0];
        let v2: V3 = [-1.0, 1.0, -1.0];
        let v3: V3 = [-1.0, -1.0, 1.0];
        let verts = vec![v0, v1, v2, v3];
        // Outward-wound faces (verified by hand: each cross(edge1,edge2) points away from the
        // origin-centred solid's centroid — see docs/math/sdf.md §3 for the per-face check).
        let tris: Vec<[u32; 3]> = vec![
            [1, 3, 2], // opposite V0
            [0, 2, 3], // opposite V1
            [0, 3, 1], // opposite V2
            [0, 1, 2], // opposite V3
        ];

        let bvh = Bvh::build(&verts, &tris);
        let pn = precompute_pseudonormals(&verts, &tris);
        assert_eq!(
            pn.non_manifold_edges, 0,
            "a tetrahedron is closed: every edge has 2 faces"
        );

        // Straight "up" in Z from V0 — NOT along the fully symmetric radial direction through the
        // origin, which is what exposes the disagreement (see the doc for why the symmetric
        // direction does NOT expose it: all three adjacent faces happen to agree there).
        let q: V3 = [v0[0], v0[1], v0[2] + 1.0];

        // Ground truth, independent of this crate's own sign logic: q is outside the tetrahedron
        // iff it is on the positive side of at least one face's plane (a point inside a convex
        // polytope is on the non-positive side of EVERY face). Using the face "opposite V1"
        // (vertices v0,v2,v3, normal pointing away from v1) as the witness:
        let n_opposite_v1 = normalize(cross(sub(v2, v0), sub(v3, v0)));
        let ground_truth_outside = dot(sub(q, v0), n_opposite_v1) > 0.0;
        assert!(
            ground_truth_outside,
            "test precondition: q must be genuinely outside"
        );

        let (closest, d2, tri_idx, feature) = bvh.query_nearest(&verts, &tris, q);
        // Precondition check: q's nearest feature really is the vertex V0 (not a face/edge) — if a
        // future change to this test's geometry breaks that, fail loudly here rather than silently
        // exercising a different (less interesting) code path.
        assert!(
            (closest[0] - v0[0]).abs() < 1e-4
                && (closest[1] - v0[1]).abs() < 1e-4
                && (closest[2] - v0[2]).abs() < 1e-4,
            "test precondition: q's nearest point must be V0 itself, got {closest:?}"
        );
        assert!(
            matches!(feature, Feature::Vertex(_)),
            "test precondition: nearest feature must be a vertex, got {feature:?}"
        );

        let correct_normal = pseudonormal_for(&pn, &tris, tri_idx, feature);
        let correct_sign = dot(sub(q, closest), correct_normal) >= 0.0;
        assert_eq!(
            correct_sign, ground_truth_outside,
            "the angle-weighted pseudonormal must agree with ground truth"
        );

        // The naive comparison: using JUST the "opposite V3" face's own flat normal (one of the
        // three faces genuinely touching V0) instead of the properly-averaged vertex pseudonormal.
        let opposite_v3_normal = normalize(cross(sub(v1, v0), sub(v2, v0)));
        let naive_sign = dot(sub(q, closest), opposite_v3_normal) >= 0.0;
        assert_ne!(
            naive_sign, ground_truth_outside,
            "this case is only interesting if the naive single-face approach gets it WRONG"
        );

        // And the full pipeline (compute_sdf's own signed_distance) agrees with the correct method,
        // not the naive one, end to end.
        let d = signed_distance(&bvh, &verts, &tris, &pn, q);
        assert!(
            d > 0.0,
            "the full pipeline must report q as outside (positive): got {d}, dist {}",
            d2.sqrt()
        );
    }
}
