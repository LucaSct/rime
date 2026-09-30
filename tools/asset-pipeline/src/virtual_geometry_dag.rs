// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The M18.6 replacement-DAG cook: a Nanite-style cluster hierarchy (ADR-0043, ADR-0056).
//!
//! # The idea
//!
//! A virtualized mesh is drawn as a *cut* through a hierarchy of simplified versions of itself:
//! fine clusters near the camera, coarse clusters far away. The hard part is not simplification —
//! it is making every possible cut **watertight** without stitching at runtime. The trick, due to
//! Karis et al. (Nanite, 2021), is to simplify *groups* of clusters with the group's outer
//! boundary **locked**:
//!
//! 1. Partition the current level's clusters into groups of ~8 neighbours (by shared edges).
//! 2. Lock every vertex a group shares with another group, then simplify the group to ~50%.
//! 3. Re-split the simplified group into new clusters: the next level.
//! 4. Repeat on the new clusters until one cluster remains or no progress is made.
//!
//! Because a group's boundary is bit-identical before and after its simplification, swapping a
//! group's fine clusters for its coarse ones can never open a crack against a neighbour — whatever
//! LOD that neighbour is at. And because step 1 regroups the *new* clusters every level, the
//! boundary locked at level L is (mostly) interior at level L+1 and gets simplified there. That
//! re-grouping is what makes the hierarchy a DAG rather than a tree: one group's output clusters
//! land in several next-level groups. A tree (grouping only whole previous groups) would keep its
//! first seams at full resolution all the way to the root.
//!
//! # Mapping onto the cooked contract
//!
//! We call one step-2 simplification a *swap unit* `G`: its input clusters and its output clusters
//! `P(G)` are exchanged atomically. The cooked payload (engine/assets `VirtualGeometryAsset`) and
//! its selectors (`select_virtual_geometry()` and its GPU twin) walk a replacement graph top-down
//! from one coarse root and expect each group to be reached along exactly one path. So the cook
//! writes:
//!
//! * one payload **group per cluster** (the root group holds all top-level clusters);
//! * each output cluster of `G` as the parent of a disjoint, non-empty share of `G`'s input
//!   clusters (possible because simplification never yields more clusters than it consumed);
//! * one **page per swap unit**, holding its input clusters, depending on the pages that hold its
//!   outputs; the root page is permanently resident.
//!
//! Every output cluster of `G` carries the same error and the same children page, and — because
//! errors are monotone and the selector compares one projected threshold — either all of them
//! refine or none does. So the whole of `G` swaps at once even though each cluster is reached
//! along its own single path: the cut is a Nanite cut, with no duplicates for the tree-walking
//! selectors to trip over. The proofs in the tests check exactly that on real cooks.
//!
//! # Determinism
//!
//! Same input bytes, same output bytes. There is no hash-map iteration anywhere: orderings come
//! from sorted vectors, `BTreeMap`/`BTreeSet`, Morton codes with index tie-breaks, and a priority
//! queue whose keys end in vertex ids. Floating point is IEEE f32/f64 with a fixed operation order.

use std::cmp::Reverse;
use std::collections::{BTreeMap, BTreeSet, BinaryHeap};

use crate::cooked::ByteWriter;
use crate::mesh::Mesh;
use crate::virtual_geometry::{
    bounding_sphere_of_points, distance_f64, round_radius_up, validate_source_mesh, vertex_layout,
    write_vertex, Asset, Cluster, Group, LeafCookError, Page, MAX_TRIANGLES_PER_CLUSTER,
};

/// Clusters the greedy partitioner aims to put in one group. Eight clusters of 128 triangles
/// simplify to ~512 triangles, i.e. ~4 clusters: the cluster count halves per level.
pub const GROUP_TARGET_CLUSTERS: usize = 8;
/// Groups smaller than this are merged into a neighbour: a tiny group is mostly boundary, and its
/// locked boundary leaves almost nothing to simplify.
pub const GROUP_MIN_CLUSTERS: usize = 4;
/// Upper bound on a merged group (ADR-0056: 4–32 clusters per group).
pub const GROUP_MAX_CLUSTERS: usize = 32;
/// Safety cap on DAG depth. Halving per level reaches one cluster from 2^32 in 32 levels; the GPU
/// selector's ancestor walk (`kVirtualGeometryGpuSelectionMaxDepth` = 256) is far above this.
pub const MAX_LEVELS: usize = 32;
/// A level that removes fewer than this fraction of triangles *and* no clusters is "no progress":
/// it is discarded and the cook stops, with the reason recorded in [`DagStats::stop`].
pub const MIN_LEVEL_REDUCTION: f64 = 0.05;
/// A collapse may rotate a surviving triangle's normal by at most acos(0.1) ≈ 84°; beyond that it
/// is treated as a fold-over and rejected.
const MIN_NORMAL_COSINE: f64 = 0.1;

/// Why the level loop stopped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DagStop {
    /// The top level is a single cluster — the ideal root.
    SingleCluster,
    /// A level failed to reduce (see [`MIN_LEVEL_REDUCTION`]); it was discarded and the previous
    /// level became the root. Typical cause: everything left is locked (material/UV seams).
    NoProgress,
    /// [`MAX_LEVELS`] was reached.
    LevelCap,
}

/// One level of the DAG. Level 0 is the source mesh's leaf clusters.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DagLevel {
    pub clusters: u32,
    pub triangles: u32,
    /// Swap units (simplification groups) that consumed this level; 0 for the root level.
    pub groups: u32,
    /// Largest LOD error of a cluster at this level, metres.
    pub max_error_m: f32,
}

/// What the cook did, including every skip/reject path — a proof that cannot see what the cook
/// declined to do would still read as passing.
#[derive(Clone, Debug, PartialEq)]
pub struct DagStats {
    pub levels: Vec<DagLevel>,
    pub stop: DagStop,
    /// Levels simplified and then thrown away because they made no progress (0 or 1).
    pub no_progress_levels_discarded: u32,
    /// Swap units whose simplification removed no triangle at all (fully locked groups).
    pub groups_without_reduction: u32,
    /// Groups below [`GROUP_MIN_CLUSTERS`] merged into a neighbour by the partitioner.
    pub small_groups_merged: u32,
    /// Small groups that had no neighbour with room and stayed small.
    pub small_groups_kept: u32,
    pub collapses: u64,
    pub rejected_link: u64,
    pub rejected_flip: u64,
    pub rejected_attribute: u64,
    /// Vertices locked for simplification (summed over groups and levels): shared with another
    /// group, on a mesh border, on an attribute/material seam, or on a non-disc fan.
    pub locked_vertices: u64,
    /// Zero-area (repeated-position) triangles dropped from a group's simplified output.
    pub degenerate_triangles_dropped: u64,
}

impl DagStats {
    fn new() -> Self {
        Self {
            levels: Vec::new(),
            stop: DagStop::SingleCluster,
            no_progress_levels_discarded: 0,
            groups_without_reduction: 0,
            small_groups_merged: 0,
            small_groups_kept: 0,
            collapses: 0,
            rejected_link: 0,
            rejected_flip: 0,
            rejected_attribute: 0,
            locked_vertices: 0,
            degenerate_triangles_dropped: 0,
        }
    }
}

/// A cooked DAG and the record of how it was built.
#[derive(Clone, Debug, PartialEq)]
pub struct DagCook {
    pub asset: Asset,
    pub stats: DagStats,
}

/// A triangle by *source vertex* index (so it keeps its exact attributes) and material slot.
/// Simplification is half-edge collapse, which only ever re-targets a corner to another existing
/// source vertex: no level invents a position, which is what makes locked boundaries bit-exact.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Tri {
    v: [u32; 3],
    material: u32,
}

/// Source vertices welded by exact position. Topology (adjacency, boundaries, locking) is decided
/// on welded positions; attributes stay on the source vertices.
struct Welded {
    pos_of: Vec<u32>,
    positions: Vec<[f32; 3]>,
}

impl Welded {
    fn new(mesh: &Mesh) -> Self {
        let mut ids: BTreeMap<[u32; 3], u32> = BTreeMap::new();
        let mut positions = Vec::new();
        let mut pos_of = Vec::with_capacity(mesh.vertices.len());
        for vertex in &mesh.vertices {
            // `+ 0.0` folds -0.0 into +0.0 so the two zeros weld; the stored position keeps the
            // first vertex's exact bits.
            let key = vertex.position.map(|c| (c + 0.0).to_bits());
            let next = positions.len() as u32;
            let id = *ids.entry(key).or_insert_with(|| {
                positions.push(vertex.position);
                next
            });
            pos_of.push(id);
        }
        Self { pos_of, positions }
    }

    fn p(&self, source_vertex: u32) -> u32 {
        self.pos_of[source_vertex as usize]
    }

    fn pos(&self, source_vertex: u32) -> [f32; 3] {
        self.positions[self.p(source_vertex) as usize]
    }
}

/// A cluster during the build.
struct WorkCluster {
    tris: Vec<Tri>,
    /// The swap unit that consumed it (`None` for the root level).
    consumer: Option<usize>,
    lod_error_m: f32,
    lod_center: [f32; 3],
    lod_radius: f32,
}

/// One simplification group: `inputs` are replaced atomically by `outputs`.
struct SwapUnit {
    level: u32,
    inputs: Vec<usize>,
    outputs: Vec<usize>,
}

// ------------------------------------------------------------------------------------------------
// Small deterministic geometry helpers.

fn sub(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

fn cross(a: [f64; 3], b: [f64; 3]) -> [f64; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

fn dot(a: [f64; 3], b: [f64; 3]) -> f64 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

fn widen(p: [f32; 3]) -> [f64; 3] {
    p.map(f64::from)
}

fn triangle_normal(p: [[f64; 3]; 3]) -> [f64; 3] {
    cross(sub(p[1], p[0]), sub(p[2], p[0]))
}

/// Spread the low 10 bits of `v` so they occupy every third bit (the Morton/Z-order interleave).
fn spread_bits(mut v: u32) -> u32 {
    v &= 0x3ff;
    v = (v | (v << 16)) & 0x0300_00ff;
    v = (v | (v << 8)) & 0x0300_f00f;
    v = (v | (v << 4)) & 0x030c_30c3;
    v = (v | (v << 2)) & 0x0924_9249;
    v
}

/// Order points along a Z-order (Morton) curve over their own bounding box. Morton order keeps
/// spatially close points close in the sequence, so "take the next unassigned item" is a cheap,
/// deterministic "take something nearby".
fn morton_order(points: &[[f64; 3]]) -> Vec<usize> {
    let mut lo = [f64::INFINITY; 3];
    let mut hi = [f64::NEG_INFINITY; 3];
    for p in points {
        for axis in 0..3 {
            lo[axis] = lo[axis].min(p[axis]);
            hi[axis] = hi[axis].max(p[axis]);
        }
    }
    let mut keyed: Vec<(u32, usize)> = points
        .iter()
        .enumerate()
        .map(|(i, p)| {
            let mut code = 0u32;
            for axis in 0..3 {
                let extent = hi[axis] - lo[axis];
                let t = if extent > 0.0 {
                    (p[axis] - lo[axis]) / extent
                } else {
                    0.0
                };
                let q = (t * 1023.0).clamp(0.0, 1023.0) as u32;
                code |= spread_bits(q) << axis;
            }
            (code, i)
        })
        .collect();
    keyed.sort_unstable();
    keyed.into_iter().map(|(_, i)| i).collect()
}

fn centroid_of(tris: &[Tri], welded: &Welded) -> [f64; 3] {
    let mut lo = [f64::INFINITY; 3];
    let mut hi = [f64::NEG_INFINITY; 3];
    for tri in tris {
        for &v in &tri.v {
            let p = widen(welded.pos(v));
            for axis in 0..3 {
                lo[axis] = lo[axis].min(p[axis]);
                hi[axis] = hi[axis].max(p[axis]);
            }
        }
    }
    [
        (lo[0] + hi[0]) * 0.5,
        (lo[1] + hi[1]) * 0.5,
        (lo[2] + hi[2]) * 0.5,
    ]
}

fn sorted_edge(a: u32, b: u32) -> (u32, u32) {
    if a < b {
        (a, b)
    } else {
        (b, a)
    }
}

/// The smallest sphere (by this construction) that contains every given sphere: the centre is the
/// centre of the spheres' joint AABB, the radius the farthest reach from it, rounded up. This is
/// how a swap unit's LOD sphere comes to contain all of its children's — the containment half of
/// the monotonicity the runtime relies on.
fn enclose_spheres(spheres: &[([f32; 3], f32)]) -> ([f32; 3], f32) {
    let mut lo = [f64::INFINITY; 3];
    let mut hi = [f64::NEG_INFINITY; 3];
    for &(c, r) in spheres {
        for axis in 0..3 {
            lo[axis] = lo[axis].min(f64::from(c[axis]) - f64::from(r));
            hi[axis] = hi[axis].max(f64::from(c[axis]) + f64::from(r));
        }
    }
    let center = [
        ((lo[0] + hi[0]) * 0.5) as f32,
        ((lo[1] + hi[1]) * 0.5) as f32,
        ((lo[2] + hi[2]) * 0.5) as f32,
    ];
    let mut radius = 0.0f64;
    for &(c, r) in spheres {
        radius = radius.max(distance_f64(center, c) + f64::from(r));
    }
    (center, round_radius_up(radius))
}

// ------------------------------------------------------------------------------------------------
// Clustering: split a triangle set into single-material clusters of at most 128 triangles.

/// Region growing over shared-edge adjacency, per material. Each cluster starts at the first
/// unassigned triangle in Morton order and grows by the frontier triangle nearest the seed, so
/// clusters come out compact (small bounds, short boundaries). When the frontier runs dry the next
/// unassigned triangle in Morton order is taken instead, which keeps every cluster but the last of
/// a material exactly full. That fullness is load-bearing: it makes the number of clusters of each
/// material `ceil(triangles / 128)`, so a simplification (which never adds triangles) can never
/// produce more clusters than it consumed — see [`assign_parents`].
fn split_into_clusters(tris: &[Tri], welded: &Welded) -> Vec<Vec<Tri>> {
    let mut by_material: BTreeMap<u32, Vec<Tri>> = BTreeMap::new();
    for &tri in tris {
        by_material.entry(tri.material).or_default().push(tri);
    }
    let mut clusters = Vec::new();
    for tris in by_material.values() {
        let centroids: Vec<[f64; 3]> = tris
            .iter()
            .map(|tri| {
                let p = tri.v.map(|v| widen(welded.pos(v)));
                [
                    (p[0][0] + p[1][0] + p[2][0]) / 3.0,
                    (p[0][1] + p[1][1] + p[2][1]) / 3.0,
                    (p[0][2] + p[1][2] + p[2][2]) / 3.0,
                ]
            })
            .collect();
        let order = morton_order(&centroids);

        let mut edges: Vec<(u32, u32, u32)> = Vec::with_capacity(tris.len() * 3);
        for (t, tri) in tris.iter().enumerate() {
            for k in 0..3 {
                let (a, b) = sorted_edge(welded.p(tri.v[k]), welded.p(tri.v[(k + 1) % 3]));
                edges.push((a, b, t as u32));
            }
        }
        edges.sort_unstable();
        let mut neighbours: Vec<Vec<u32>> = vec![Vec::new(); tris.len()];
        let mut run = 0;
        while run < edges.len() {
            let mut end = run + 1;
            while end < edges.len() && edges[end].0 == edges[run].0 && edges[end].1 == edges[run].1
            {
                end += 1;
            }
            for i in run..end {
                for j in run..end {
                    if edges[i].2 != edges[j].2 {
                        neighbours[edges[i].2 as usize].push(edges[j].2);
                    }
                }
            }
            run = end;
        }
        for list in &mut neighbours {
            list.sort_unstable();
            list.dedup();
        }

        let mut assigned = vec![false; tris.len()];
        let mut cursor = 0;
        loop {
            while cursor < order.len() && assigned[order[cursor]] {
                cursor += 1;
            }
            if cursor == order.len() {
                break;
            }
            let seed = order[cursor];
            let center = centroids[seed];
            let mut members = vec![seed];
            assigned[seed] = true;
            let mut frontier: BTreeSet<(u64, u32)> = BTreeSet::new();
            let push = |frontier: &mut BTreeSet<(u64, u32)>, assigned: &[bool], t: usize| {
                for &n in &neighbours[t] {
                    if !assigned[n as usize] {
                        let d = sub(centroids[n as usize], center);
                        // Non-negative f64 bit patterns order like the values themselves.
                        frontier.insert((dot(d, d).to_bits(), n));
                    }
                }
            };
            push(&mut frontier, &assigned, seed);
            while members.len() < MAX_TRIANGLES_PER_CLUSTER {
                let mut next = None;
                while let Some((_, t)) = frontier.pop_first() {
                    if !assigned[t as usize] {
                        next = Some(t as usize);
                        break;
                    }
                }
                if next.is_none() {
                    while cursor < order.len() && assigned[order[cursor]] {
                        cursor += 1;
                    }
                    if cursor < order.len() {
                        next = Some(order[cursor]);
                    }
                }
                let Some(t) = next else { break };
                assigned[t] = true;
                members.push(t);
                push(&mut frontier, &assigned, t);
            }
            members.sort_unstable();
            clusters.push(members.into_iter().map(|t| tris[t]).collect());
        }
    }
    clusters
}

// ------------------------------------------------------------------------------------------------
// Grouping: partition a level's clusters into groups of adjacent clusters.

/// A deterministic greedy graph partition. The graph's nodes are clusters and its edge weights
/// the number of mesh edges two clusters share. Groups grow from a Morton-ordered seed by always
/// adding the frontier cluster that shares the MOST edges with the group so far — a cheap stand-in
/// for METIS's min-edge-cut objective. Weighting by shared-edge count is what makes the DAG move
/// its seams: last level's locked boundaries are still at full resolution (dense edges), while the
/// interiors were simplified (few, long edges), so the partitioner prefers to cut through the
/// simplified interiors and to join clusters across the old seams, which then get simplified.
fn partition_groups(
    current: &[usize],
    clusters: &[WorkCluster],
    welded: &Welded,
    stats: &mut DagStats,
) -> Vec<Vec<usize>> {
    let n = current.len();
    let mut edges: Vec<(u32, u32, u32)> = Vec::new();
    for (k, &c) in current.iter().enumerate() {
        for tri in &clusters[c].tris {
            for i in 0..3 {
                let (a, b) = sorted_edge(welded.p(tri.v[i]), welded.p(tri.v[(i + 1) % 3]));
                edges.push((a, b, k as u32));
            }
        }
    }
    edges.sort_unstable();
    edges.dedup();
    let mut weights: BTreeMap<(u32, u32), u32> = BTreeMap::new();
    let mut run = 0;
    while run < edges.len() {
        let mut end = run + 1;
        while end < edges.len() && edges[end].0 == edges[run].0 && edges[end].1 == edges[run].1 {
            end += 1;
        }
        for i in run..end {
            for j in (i + 1)..end {
                let (a, b) = sorted_edge(edges[i].2, edges[j].2);
                *weights.entry((a, b)).or_insert(0) += 1;
            }
        }
        run = end;
    }
    let mut adjacency: Vec<Vec<(u32, u32)>> = vec![Vec::new(); n];
    for (&(a, b), &w) in &weights {
        adjacency[a as usize].push((b, w));
        adjacency[b as usize].push((a, w));
    }
    let centroids: Vec<[f64; 3]> = current
        .iter()
        .map(|&c| centroid_of(&clusters[c].tris, welded))
        .collect();
    let order = morton_order(&centroids);

    const NONE: usize = usize::MAX;
    let mut group_of = vec![NONE; n];
    let mut groups: Vec<Vec<usize>> = Vec::new();
    for &seed in &order {
        if group_of[seed] != NONE {
            continue;
        }
        let gi = groups.len();
        let mut members = vec![seed];
        group_of[seed] = gi;
        let mut candidates: BTreeMap<u32, u32> = BTreeMap::new();
        for &(o, w) in &adjacency[seed] {
            if group_of[o as usize] == NONE {
                *candidates.entry(o).or_insert(0) += w;
            }
        }
        while members.len() < GROUP_TARGET_CLUSTERS {
            let mut best: Option<(u32, u64, u32)> = None; // (weight, distance bits, index)
            for (&o, &w) in &candidates {
                if group_of[o as usize] != NONE {
                    continue;
                }
                let d = sub(centroids[o as usize], centroids[seed]);
                let key = (w, dot(d, d).to_bits(), o);
                best = match best {
                    None => Some(key),
                    Some(b) => {
                        let better = key.0 > b.0 || (key.0 == b.0 && (key.1, key.2) < (b.1, b.2));
                        Some(if better { key } else { b })
                    }
                };
            }
            let Some((_, _, pick)) = best else { break };
            candidates.remove(&pick);
            group_of[pick as usize] = gi;
            members.push(pick as usize);
            for &(o, w) in &adjacency[pick as usize] {
                if group_of[o as usize] == NONE {
                    *candidates.entry(o).or_insert(0) += w;
                }
            }
        }
        groups.push(members);
    }

    // Merge groups that came out too small (their neighbours were taken first) into the adjacent
    // group they share the most edges with, if it has room; failing that, the previous group in
    // creation (≈ Morton) order.
    for gi in 0..groups.len() {
        let size = groups[gi].len();
        if size == 0 || size >= GROUP_MIN_CLUSTERS || groups.len() == 1 {
            continue;
        }
        let mut shared: BTreeMap<usize, u32> = BTreeMap::new();
        for &m in &groups[gi] {
            for &(o, w) in &adjacency[m] {
                let og = group_of[o as usize];
                if og != gi {
                    *shared.entry(og).or_insert(0) += w;
                }
            }
        }
        let mut target = None;
        let mut best_weight = 0;
        for (&og, &w) in &shared {
            if groups[og].len() + size <= GROUP_MAX_CLUSTERS && w > best_weight {
                best_weight = w;
                target = Some(og);
            }
        }
        if target.is_none() {
            target = (0..gi).rev().chain(gi + 1..groups.len()).find(|&og| {
                !groups[og].is_empty() && groups[og].len() + size <= GROUP_MAX_CLUSTERS
            });
        }
        match target {
            Some(og) => {
                let moved = std::mem::take(&mut groups[gi]);
                for &m in &moved {
                    group_of[m] = og;
                }
                groups[og].extend(moved);
                stats.small_groups_merged += 1;
            }
            None => stats.small_groups_kept += 1,
        }
    }
    groups
        .into_iter()
        .filter(|g| !g.is_empty())
        .map(|mut g| {
            g.sort_unstable();
            g.into_iter().map(|k| current[k]).collect()
        })
        .collect()
}

// ------------------------------------------------------------------------------------------------
// Simplification: quadric error metric edge collapse with a locked boundary.

/// A symmetric 4x4 quadric stored as its 10 unique coefficients:
/// `[a², ab, ac, ad, b², bc, bd, c², cd, d²]` for the plane `ax + by + cz + d = 0`.
type Quadric = [f64; 10];

fn plane_quadric(n: [f64; 3], d: f64) -> Quadric {
    let [a, b, c] = n;
    [
        a * a,
        a * b,
        a * c,
        a * d,
        b * b,
        b * c,
        b * d,
        c * c,
        c * d,
        d * d,
    ]
}

fn add_quadric(q: &mut Quadric, r: &Quadric) {
    for i in 0..10 {
        q[i] += r[i];
    }
}

/// `vᵀ Q v` for `v = (x, y, z, 1)`.
fn eval_quadric(q: &Quadric, p: [f64; 3]) -> f64 {
    let [x, y, z] = p;
    q[0] * x * x
        + 2.0 * q[1] * x * y
        + 2.0 * q[2] * x * z
        + 2.0 * q[3] * x
        + q[4] * y * y
        + 2.0 * q[5] * y * z
        + 2.0 * q[6] * y
        + q[7] * z * z
        + 2.0 * q[8] * z
        + q[9]
}

struct Simplified {
    tris: Vec<Tri>,
    /// sqrt of the largest quadric cost accepted, metres.
    error_m: f64,
}

struct LocalTri {
    p: [u32; 3],
    a: [u32; 3],
    material: u32,
    alive: bool,
}

/// Simplify one group to `target` triangles with the Garland–Heckbert quadric error metric.
///
/// **QEM in one paragraph.** Every triangle lies in a plane; the squared distance of a point `v`
/// to that plane is `vᵀ K v` for the 4x4 matrix `K = p pᵀ` built from the unit plane `p = (n, d)`.
/// Give every vertex the sum `Q` of its triangles' `K`s: `vᵀ Q v` is then the sum of squared
/// distances from `v` to all the planes that vertex started on. Collapsing edge `u→v` moves `u`
/// onto `v`, and its cost is `vᵀ (Q_u + Q_v) v` — how far `v` sits from every plane either vertex
/// was responsible for. Always collapsing the cheapest edge first, and summing quadrics as vertices
/// merge, keeps the surface near all the planes it was built from. Planes are unit-length and
/// unweighted, so `sqrt(cost)` is a length in metres (an upper bound on the distance to any one
/// accumulated plane), which is what the group's LOD error reports.
///
/// We use **half-edge** collapse (`u` moves onto the existing `v`) rather than the optimal
/// placement: no new position is ever created, so a vertex we refuse to move is bit-exact at every
/// level, attributes come unmodified from a real source vertex, and there is no 4x4 solve whose
/// rounding could differ between machines.
///
/// A vertex is **locked** (never moved; still a valid collapse *target*) when it is shared with
/// another group, on a mesh border or non-manifold edge, on a UV/normal seam, on a material
/// boundary, or its triangle fan is not a single disc.
fn simplify_group(
    input: &[Tri],
    shared: &[bool],
    welded: &Welded,
    target: usize,
    stats: &mut DagStats,
) -> Simplified {
    let mut ids: Vec<u32> = input
        .iter()
        .flat_map(|t| t.v.map(|v| welded.p(v)))
        .collect();
    ids.sort_unstable();
    ids.dedup();
    let local = |p: u32| ids.binary_search(&p).expect("position is in the group") as u32;
    let pos: Vec<[f64; 3]> = ids
        .iter()
        .map(|&p| widen(welded.positions[p as usize]))
        .collect();
    let n = ids.len();

    let mut tris: Vec<LocalTri> = Vec::with_capacity(input.len());
    for tri in input {
        let p = tri.v.map(|v| local(welded.p(v)));
        if p[0] == p[1] || p[1] == p[2] || p[0] == p[2] {
            stats.degenerate_triangles_dropped += 1;
            continue;
        }
        tris.push(LocalTri {
            p,
            a: tri.v,
            material: tri.material,
            alive: true,
        });
    }
    let mut live = tris.len();
    let mut fan: Vec<Vec<u32>> = vec![Vec::new(); n];
    for (t, tri) in tris.iter().enumerate() {
        for &p in &tri.p {
            fan[p as usize].push(t as u32);
        }
    }

    // --- Locking.
    let mut locked: Vec<bool> = ids.iter().map(|&p| shared[p as usize]).collect();
    let mut edges: Vec<(u32, u32)> = Vec::with_capacity(tris.len() * 3);
    for tri in &tris {
        for k in 0..3 {
            edges.push(sorted_edge(tri.p[k], tri.p[(k + 1) % 3]));
        }
    }
    edges.sort_unstable();
    let mut run = 0;
    while run < edges.len() {
        let mut end = run + 1;
        while end < edges.len() && edges[end] == edges[run] {
            end += 1;
        }
        if end - run != 2 {
            // A border (1 triangle) or non-manifold (3+) edge.
            locked[edges[run].0 as usize] = true;
            locked[edges[run].1 as usize] = true;
        }
        run = end;
    }
    for x in 0..n {
        if locked[x] || fan[x].is_empty() {
            // (An empty fan: the position only appears in dropped degenerate triangles.)
            locked[x] = true;
            continue;
        }
        let corner = |t: u32| {
            let tri = &tris[t as usize];
            let k = tri.p.iter().position(|&p| p == x as u32).unwrap();
            (
                tri.a[k],
                tri.p[(k + 1) % 3],
                tri.p[(k + 2) % 3],
                tri.material,
            )
        };
        let (a0, _, _, m0) = corner(fan[x][0]);
        let mut next: BTreeMap<u32, u32> = BTreeMap::new();
        let mut disc = true;
        for &t in &fan[x] {
            let (a, from, to, m) = corner(t);
            if a != a0 || m != m0 || next.insert(from, to).is_some() {
                disc = false;
            }
        }
        if disc {
            // Walk the ring: a single consistently oriented cycle through every triangle.
            let start = *next.keys().next().unwrap();
            let mut at = start;
            let mut steps = 0;
            loop {
                match next.get(&at) {
                    Some(&to) => at = to,
                    None => {
                        disc = false;
                        break;
                    }
                }
                steps += 1;
                if at == start || steps > fan[x].len() {
                    break;
                }
            }
            disc = disc && at == start && steps == fan[x].len();
        }
        if !disc {
            locked[x] = true;
        }
    }
    stats.locked_vertices += locked.iter().filter(|&&l| l).count() as u64;

    // --- Quadrics.
    let mut quadrics: Vec<Quadric> = vec![[0.0; 10]; n];
    for tri in &tris {
        let normal = triangle_normal(tri.p.map(|p| pos[p as usize]));
        let length = dot(normal, normal).sqrt();
        if length == 0.0 {
            continue;
        }
        let unit = normal.map(|c| c / length);
        let q = plane_quadric(unit, -dot(unit, pos[tri.p[0] as usize]));
        for &p in &tri.p {
            add_quadric(&mut quadrics[p as usize], &q);
        }
    }

    let neighbours = |tris: &[LocalTri], fan: &[Vec<u32>], x: u32| -> Vec<u32> {
        let mut out: Vec<u32> = Vec::new();
        for &t in &fan[x as usize] {
            let tri = &tris[t as usize];
            if tri.alive {
                out.extend(tri.p.iter().copied().filter(|&p| p != x));
            }
        }
        out.sort_unstable();
        out.dedup();
        out
    };

    // Min-heap of (cost bits, u, v, stamp_u, stamp_v). Costs are clamped non-negative, so their
    // f64 bit patterns order like the values; ties break on vertex ids — deterministic.
    type Entry = Reverse<(u64, u32, u32, u32, u32)>;
    let mut heap: BinaryHeap<Entry> = BinaryHeap::new();
    let mut stamp = vec![0u32; n];
    let mut removed = vec![false; n];
    let cost_of = |quadrics: &[Quadric], u: u32, v: u32| {
        let mut q = quadrics[u as usize];
        add_quadric(&mut q, &quadrics[v as usize]);
        eval_quadric(&q, pos[v as usize]).max(0.0)
    };
    let push =
        |heap: &mut BinaryHeap<Entry>, quadrics: &[Quadric], stamp: &[u32], u: u32, v: u32| {
            if !locked[u as usize] {
                let cost = cost_of(quadrics, u, v);
                heap.push(Reverse((
                    cost.to_bits(),
                    u,
                    v,
                    stamp[u as usize],
                    stamp[v as usize],
                )));
            }
        };
    for u in 0..n as u32 {
        for v in neighbours(&tris, &fan, u) {
            push(&mut heap, &quadrics, &stamp, u, v);
        }
    }

    let mut max_cost = 0.0f64;
    while live > target {
        let Some(Reverse((cost_bits, u, v, su, sv))) = heap.pop() else {
            break;
        };
        if removed[u as usize] || removed[v as usize] {
            continue;
        }
        let nu = neighbours(&tris, &fan, u);
        if nu.binary_search(&v).is_err() {
            continue;
        }
        if su != stamp[u as usize] || sv != stamp[v as usize] {
            push(&mut heap, &quadrics, &stamp, u, v);
            continue;
        }

        // Link condition: the only vertices adjacent to both u and v may be the apexes of the two
        // triangles on edge uv. Anything else would pinch the surface into a non-manifold edge.
        let fan_u: Vec<u32> = fan[u as usize]
            .iter()
            .copied()
            .filter(|&t| tris[t as usize].alive)
            .collect();
        let on_edge: Vec<u32> = fan_u
            .iter()
            .copied()
            .filter(|&t| tris[t as usize].p.contains(&v))
            .collect();
        if on_edge.len() != 2 {
            stats.rejected_link += 1;
            continue;
        }
        let mut apexes: Vec<u32> = on_edge
            .iter()
            .map(|&t| {
                *tris[t as usize]
                    .p
                    .iter()
                    .find(|&&p| p != u && p != v)
                    .unwrap()
            })
            .collect();
        apexes.sort_unstable();
        let nv = neighbours(&tris, &fan, v);
        let common: Vec<u32> = nu
            .iter()
            .copied()
            .filter(|p| nv.binary_search(p).is_ok())
            .collect();
        if apexes[0] == apexes[1] || common != apexes {
            stats.rejected_link += 1;
            continue;
        }
        // Nor may it produce a triangle that already exists around v — the link condition alone
        // lets a closed tetrahedron fold into a double-sided sheet and then vanish.
        let sorted_p = |mut p: [u32; 3]| {
            p.sort_unstable();
            p
        };
        let around_v: Vec<[u32; 3]> = fan[v as usize]
            .iter()
            .filter(|&&t| tris[t as usize].alive)
            .map(|&t| sorted_p(tris[t as usize].p))
            .collect();
        let duplicates = fan_u.iter().any(|&t| {
            let tri = &tris[t as usize];
            !tri.p.contains(&v)
                && around_v.contains(&sorted_p(tri.p.map(|p| if p == u { v } else { p })))
        });
        if duplicates {
            stats.rejected_link += 1;
            continue;
        }

        // Attributes: u's corners take v's source vertex from the two triangles on edge uv. Both
        // must agree, or uv crosses a seam at v and there is no single right attribute.
        let attr_at = |t: u32, x: u32| {
            let tri = &tris[t as usize];
            tri.a[tri.p.iter().position(|&p| p == x).unwrap()]
        };
        let av = attr_at(on_edge[0], v);
        if attr_at(on_edge[1], v) != av {
            stats.rejected_attribute += 1;
            continue;
        }

        // Fold-over: no surviving triangle may flip or rotate past MIN_NORMAL_COSINE.
        let mut folds = false;
        for &t in &fan_u {
            let tri = &tris[t as usize];
            if tri.p.contains(&v) {
                continue;
            }
            let before = tri.p.map(|p| pos[p as usize]);
            let after = tri.p.map(|p| pos[if p == u { v } else { p } as usize]);
            let n0 = triangle_normal(before);
            let n1 = triangle_normal(after);
            let scale = (dot(n0, n0) * dot(n1, n1)).sqrt();
            if scale == 0.0 || dot(n0, n1) <= MIN_NORMAL_COSINE * scale {
                folds = true;
                break;
            }
        }
        if folds {
            stats.rejected_flip += 1;
            continue;
        }

        // Collapse u → v.
        for &t in &fan_u {
            let tri = &mut tris[t as usize];
            if tri.p.contains(&v) {
                tri.alive = false;
                live -= 1;
            } else {
                let k = tri.p.iter().position(|&p| p == u).unwrap();
                tri.p[k] = v;
                tri.a[k] = av;
                fan[v as usize].push(t);
            }
        }
        fan[u as usize].clear();
        removed[u as usize] = true;
        let qu = quadrics[u as usize];
        add_quadric(&mut quadrics[v as usize], &qu);
        stamp[v as usize] += 1;
        max_cost = max_cost.max(f64::from_bits(cost_bits));
        stats.collapses += 1;

        // Re-offer every edge whose cost (around v) or validity (the 2-ring) may have changed,
        // including ones rejected earlier: a rejection is only final for the neighbourhood it saw.
        for w in neighbours(&tris, &fan, v) {
            push(&mut heap, &quadrics, &stamp, v, w);
            for x in neighbours(&tris, &fan, w) {
                push(&mut heap, &quadrics, &stamp, w, x);
            }
        }
    }

    Simplified {
        tris: tris
            .iter()
            .filter(|t| t.alive)
            .map(|t| Tri {
                v: t.a,
                material: t.material,
            })
            .collect(),
        error_m: max_cost.sqrt(),
    }
}

// ------------------------------------------------------------------------------------------------
// The level loop.

/// Give each of `G`'s input clusters exactly one parent among `G`'s output clusters, so that every
/// output has at least one child. Outputs first each claim their nearest free input, then every
/// remaining input joins its nearest output. Any such assignment is a valid replacement (all
/// outputs of `G` refine together); nearest-first just keeps a parent spatially near its children.
fn assign_parents(
    inputs: &[usize],
    outputs: &[usize],
    clusters: &[WorkCluster],
    welded: &Welded,
) -> Result<Vec<usize>, LeafCookError> {
    if outputs.len() > inputs.len() || outputs.is_empty() {
        return Err(LeafCookError::DagInvariant);
    }
    let ci: Vec<[f64; 3]> = inputs
        .iter()
        .map(|&c| centroid_of(&clusters[c].tris, welded))
        .collect();
    let co: Vec<[f64; 3]> = outputs
        .iter()
        .map(|&c| centroid_of(&clusters[c].tris, welded))
        .collect();
    let d2 = |a: [f64; 3], b: [f64; 3]| {
        let d = sub(a, b);
        dot(d, d)
    };
    const NONE: usize = usize::MAX;
    let mut parent = vec![NONE; inputs.len()];
    for (o, &c) in co.iter().enumerate() {
        let pick = (0..inputs.len())
            .filter(|&i| parent[i] == NONE)
            .min_by(|&a, &b| d2(ci[a], c).total_cmp(&d2(ci[b], c)).then(a.cmp(&b)))
            .ok_or(LeafCookError::DagInvariant)?;
        parent[pick] = o;
    }
    for i in 0..inputs.len() {
        if parent[i] == NONE {
            parent[i] = (0..outputs.len())
                .min_by(|&a, &b| {
                    d2(co[a], ci[i])
                        .total_cmp(&d2(co[b], ci[i]))
                        .then(a.cmp(&b))
                })
                .unwrap();
        }
    }
    Ok(parent)
}

fn tri_count(clusters: &[WorkCluster], ids: &[usize]) -> usize {
    ids.iter().map(|&c| clusters[c].tris.len()).sum()
}

impl Asset {
    /// Cook a rigid mesh into a multi-level replacement DAG (see the module docs). The output uses
    /// the same page-byte contract as [`Asset::from_mesh`]: per page, interleaved vertices in the
    /// source layout then little-endian u32 indices; indices are cluster-local.
    pub fn from_mesh_dag(source_mesh: u64, mesh: &Mesh) -> Result<DagCook, LeafCookError> {
        validate_source_mesh(mesh)?;
        let welded = Welded::new(mesh);
        let mut stats = DagStats::new();

        let mut source_tris = Vec::with_capacity(mesh.indices.len() / 3);
        for submesh in &mesh.submeshes {
            let range =
                submesh.first_index as usize..(submesh.first_index + submesh.index_count) as usize;
            for &v in mesh.indices[range].as_chunks::<3>().0 {
                source_tris.push(Tri {
                    v,
                    material: submesh.material_slot,
                });
            }
        }

        let sphere_of = |tris: &[Tri]| {
            bounding_sphere_of_points(tris.iter().flat_map(|t| t.v.map(|v| welded.pos(v))))
        };
        let mut clusters: Vec<WorkCluster> = split_into_clusters(&source_tris, &welded)
            .into_iter()
            .map(|tris| {
                let (lod_center, lod_radius) = sphere_of(&tris);
                WorkCluster {
                    tris,
                    consumer: None,
                    lod_error_m: 0.0,
                    lod_center,
                    lod_radius,
                }
            })
            .collect();
        let mut units: Vec<SwapUnit> = Vec::new();
        let mut current: Vec<usize> = (0..clusters.len()).collect();
        stats.levels.push(DagLevel {
            clusters: current.len() as u32,
            triangles: tri_count(&clusters, &current) as u32,
            groups: 0,
            max_error_m: 0.0,
        });

        let mut level = 0u32;
        loop {
            if current.len() <= 1 {
                stats.stop = DagStop::SingleCluster;
                break;
            }
            if level as usize >= MAX_LEVELS {
                stats.stop = DagStop::LevelCap;
                break;
            }
            let groups = partition_groups(&current, &clusters, &welded, &mut stats);

            // Lock every position that appears in more than one group: those are the boundaries
            // every possible cut stitches along, so they must be bit-identical in the input and
            // the output of every group's simplification.
            const NONE: u32 = u32::MAX;
            let mut owner = vec![NONE; welded.positions.len()];
            let mut shared = vec![false; welded.positions.len()];
            for (gi, group) in groups.iter().enumerate() {
                for &c in group {
                    for tri in &clusters[c].tris {
                        for &v in &tri.v {
                            let p = welded.p(v) as usize;
                            if owner[p] == NONE {
                                owner[p] = gi as u32;
                            } else if owner[p] != gi as u32 {
                                shared[p] = true;
                            }
                        }
                    }
                }
            }

            let mut level_stats = stats.clone();
            let mut simplified: Vec<(Vec<Vec<Tri>>, f64)> = Vec::with_capacity(groups.len());
            let mut out_tris = 0;
            let mut out_clusters = 0;
            for group in &groups {
                let input: Vec<Tri> = group
                    .iter()
                    .flat_map(|&c| clusters[c].tris.iter().copied())
                    .collect();
                let result =
                    simplify_group(&input, &shared, &welded, input.len() / 2, &mut level_stats);
                if result.tris.len() == input.len() {
                    level_stats.groups_without_reduction += 1;
                }
                out_tris += result.tris.len();
                let split = split_into_clusters(&result.tris, &welded);
                out_clusters += split.len();
                simplified.push((split, result.error_m));
            }
            let in_tris = tri_count(&clusters, &current);
            if out_tris as f64 > (1.0 - MIN_LEVEL_REDUCTION) * in_tris as f64
                && out_clusters >= current.len()
            {
                stats.no_progress_levels_discarded += 1;
                stats.stop = DagStop::NoProgress;
                break;
            }
            stats = level_stats;
            stats.levels.last_mut().unwrap().groups = groups.len() as u32;

            level += 1;
            let mut next = Vec::new();
            let mut max_error = 0.0f32;
            for (group, (split, raw_error)) in groups.into_iter().zip(simplified) {
                let unit = units.len();
                // MONOTONICITY, part 1: a swap unit's error is its own simplification error ON TOP
                // of the worst error it inherits. The inputs already deviate from the source by up
                // to their error; the outputs deviate from the inputs by `raw_error`; by the
                // triangle inequality the outputs deviate from the source by at most the sum. The
                // sum is also what guarantees parent ≥ child, which the runtime cut depends on.
                let inherited = group
                    .iter()
                    .map(|&c| clusters[c].lod_error_m)
                    .fold(0.0f32, f32::max);
                let error = (f64::from(inherited) + raw_error) as f32;
                // MONOTONICITY, part 2: the LOD sphere encloses every input's LOD sphere (and so,
                // by induction, every source triangle the unit stands for — half-edge collapse
                // never moves geometry outside the input's vertex set).
                let spheres: Vec<([f32; 3], f32)> = group
                    .iter()
                    .map(|&c| (clusters[c].lod_center, clusters[c].lod_radius))
                    .collect();
                let (center, radius) = enclose_spheres(&spheres);
                for &c in &group {
                    clusters[c].consumer = Some(unit);
                }
                let mut outputs = Vec::with_capacity(split.len());
                for tris in split {
                    outputs.push(clusters.len());
                    next.push(clusters.len());
                    clusters.push(WorkCluster {
                        tris,
                        consumer: None,
                        lod_error_m: error,
                        lod_center: center,
                        lod_radius: radius,
                    });
                }
                max_error = max_error.max(error);
                units.push(SwapUnit {
                    level,
                    inputs: group,
                    outputs,
                });
            }
            current = next;
            stats.levels.push(DagLevel {
                clusters: current.len() as u32,
                triangles: tri_count(&clusters, &current) as u32,
                groups: 0,
                max_error_m: max_error,
            });
        }

        let asset = assemble(source_mesh, mesh, &welded, &clusters, &units, &current)?;
        Ok(DagCook { asset, stats })
    }
}

/// Lay the DAG out in the cooked contract: pages (root first, then one per swap unit from the
/// coarsest level down), clusters page by page, one group per cluster plus the multi-cluster root.
fn assemble(
    source_mesh: u64,
    mesh: &Mesh,
    welded: &Welded,
    clusters: &[WorkCluster],
    units: &[SwapUnit],
    root: &[usize],
) -> Result<Asset, LeafCookError> {
    let (attribs, vertex_stride) = vertex_layout(mesh);

    // Page order: the root page, then swap units coarse → fine (stable by unit index).
    let mut unit_order: Vec<usize> = (0..units.len()).collect();
    unit_order.sort_by_key(|&u| (Reverse(units[u].level), u));
    let mut page_of_unit = vec![0u32; units.len()];
    for (i, &u) in unit_order.iter().enumerate() {
        page_of_unit[u] = i as u32 + 1;
    }
    let mut page_members: Vec<Vec<usize>> = vec![root.to_vec()];
    for &u in &unit_order {
        page_members.push(units[u].inputs.clone());
    }

    // Cluster and group numbering.
    const NONE: u32 = u32::MAX;
    let mut cluster_index = vec![NONE; clusters.len()];
    let mut order: Vec<usize> = Vec::with_capacity(clusters.len());
    for members in &page_members {
        for &c in members {
            cluster_index[c] = order.len() as u32;
            order.push(c);
        }
    }
    let root_count = root.len() as u32;
    // Group 0 is the root; every other cluster k (k >= root_count) is group k - root_count + 1.
    let group_of = |c: usize| -> u32 {
        let k = cluster_index[c];
        if k < root_count {
            0
        } else {
            k - root_count + 1
        }
    };

    // Replacement edges.
    let group_count = order.len() - root.len() + 1;
    let mut children: Vec<Vec<u32>> = vec![Vec::new(); group_count];
    for unit in units {
        if unit.outputs.iter().all(|&o| clusters[o].consumer.is_none()) {
            // A top-level unit: its outputs are part of the root group.
            for &i in &unit.inputs {
                children[0].push(group_of(i));
            }
        } else {
            let parents = assign_parents(&unit.inputs, &unit.outputs, clusters, welded)?;
            for (k, &i) in unit.inputs.iter().enumerate() {
                children[group_of(unit.outputs[parents[k]]) as usize].push(group_of(i));
            }
        }
    }
    for list in &mut children {
        list.sort_unstable();
    }

    // Page bytes.
    let mut page_bytes = ByteWriter::new();
    let mut pages = Vec::with_capacity(page_members.len());
    let mut page_dependencies = Vec::new();
    let mut cooked_clusters = vec![Cluster::default(); order.len()];
    for (page_index, members) in page_members.iter().enumerate() {
        let mut vertices = ByteWriter::new();
        let mut indices: Vec<u32> = Vec::new();
        for &c in members {
            let mut local: BTreeMap<u32, u32> = BTreeMap::new();
            let mut unique: Vec<u32> = Vec::new();
            let first_index = indices.len() as u32;
            let mut lo = [f32::INFINITY; 3];
            let mut hi = [f32::NEG_INFINITY; 3];
            for tri in &clusters[c].tris {
                for &v in &tri.v {
                    let next = unique.len() as u32;
                    let l = *local.entry(v).or_insert_with(|| {
                        unique.push(v);
                        next
                    });
                    indices.push(l);
                    let p = welded.pos(v);
                    for axis in 0..3 {
                        lo[axis] = lo[axis].min(p[axis]);
                        hi[axis] = hi[axis].max(p[axis]);
                    }
                }
            }
            let vertex_offset = vertices.len() as u32;
            for &v in &unique {
                write_vertex(&mut vertices, mesh, v as usize)?;
            }
            let k = cluster_index[c] as usize;
            cooked_clusters[k] = Cluster {
                bounds_min: lo,
                bounds_max: hi,
                lod_error_m: 0.0, // filled from the group below
                page: page_index as u32,
                vertex_offset,
                vertex_count: unique.len() as u32,
                first_index,
                index_count: indices.len() as u32 - first_index,
                material_slot: clusters[c].tris[0].material,
                replacement_group: group_of(c),
            };
        }
        let byte_offset = page_bytes.len() as u64;
        let vertex_bytes = vertices.into_vec();
        let size = vertex_bytes.len() as u64 + 4 * indices.len() as u64;
        if size > u64::from(u32::MAX) {
            return Err(LeafCookError::OversizedPayload);
        }
        page_bytes.bytes(&vertex_bytes);
        for index in indices {
            page_bytes.u32(index);
        }

        // A swap unit's page depends on the pages holding its outputs: its clusters can only be
        // drawn when their parents were reachable, and "ready" must imply "every ancestor page is
        // ready" for all outputs of a unit to refine together.
        let first_dependency = page_dependencies.len() as u32;
        if page_index > 0 {
            let unit = &units[unit_order[page_index - 1]];
            let mut deps: Vec<u32> = unit
                .outputs
                .iter()
                .map(|&o| match clusters[o].consumer {
                    Some(consumer) => page_of_unit[consumer],
                    None => 0,
                })
                .collect();
            deps.sort_unstable();
            deps.dedup();
            page_dependencies.extend(deps);
        }
        pages.push(Page {
            byte_offset,
            byte_size: size as u32,
            first_cluster: cluster_index[members[0]],
            cluster_count: members.len() as u32,
            first_dependency,
            dependency_count: page_dependencies.len() as u32 - first_dependency,
            permanently_resident: page_index == 0,
        });
    }

    // Groups.
    let mut groups = Vec::with_capacity(group_count);
    let mut child_groups = Vec::new();
    let root_spheres: Vec<([f32; 3], f32)> = root
        .iter()
        .map(|&c| (clusters[c].lod_center, clusters[c].lod_radius))
        .collect();
    let (root_center, root_radius) = if root.len() == 1 {
        root_spheres[0]
    } else {
        enclose_spheres(&root_spheres)
    };
    let root_error = root
        .iter()
        .map(|&c| clusters[c].lod_error_m)
        .fold(0.0f32, f32::max);
    for (g, kids) in children.iter().enumerate() {
        let (first_cluster, cluster_count, lod_error_m, lod_center, lod_radius) = if g == 0 {
            (0, root_count, root_error, root_center, root_radius)
        } else {
            let c = order[root.len() + g - 1];
            (
                root_count + g as u32 - 1,
                1,
                clusters[c].lod_error_m,
                clusters[c].lod_center,
                clusters[c].lod_radius,
            )
        };
        groups.push(Group {
            first_cluster,
            cluster_count,
            first_child: child_groups.len() as u32,
            child_count: kids.len() as u32,
            lod_error_m,
            permanently_resident: g == 0,
            lod_center,
            lod_radius,
        });
        child_groups.extend(kids.iter().copied());
    }
    for cluster in &mut cooked_clusters {
        cluster.lod_error_m = groups[cluster.replacement_group as usize].lod_error_m;
    }

    Ok(Asset {
        source_mesh,
        attribs,
        vertex_stride,
        pages,
        clusters: cooked_clusters,
        groups,
        child_groups,
        page_dependencies,
        page_bytes: page_bytes.into_vec(),
        coarse_group: 0,
    })
}

#[cfg(test)]
mod tests;
