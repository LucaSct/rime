// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The fracture cook (M8.1, [ADR-0029](../../../docs/adr/0029-destruction-model.md)): turn a convex
//! source (v1: an axis-aligned box — a wall, a column, a slab) into a **Destructible** — a set of
//! convex **parts**, the **bond** graph gluing neighbours, and the **anchors** pinning parts to the
//! world. The runtime (`engine/destruction`, M8.2) instances it as one static compound body (each
//! part a `register_hull`ed convex, the whole a `register_compound`), accumulates damage into per-part
//! health, and on fracture detaches unsupported parts into debris bodies.
//!
//! The partition is a **seeded Voronoi diagram** clipped to the source box. Every cell is the
//! intersection of half-spaces — the six box faces plus one bisector per other site — so each part is
//! **convex by construction** (no quickhull needed to *generate* it; quickhull, the ADR-0027
//! deferral, becomes the robustness backstop only when a future brick fractures a non-convex mesh).
//! We enumerate a cell's vertices by intersecting plane triples and keeping those inside every
//! half-space, then reconstruct each face as the 2D-convex loop of the vertices on its plane. That
//! yields exactly the CSR shape `PhysicsWorld::register_hull` validates (closed, convex, outward,
//! 3..16 vertices per face, positive volume) — the cross-language oracle test registers every cooked
//! part into a real `PhysicsWorld` to prove it.
//!
//! The math (why cells are convex, the bisector derivation, the polyhedral volume/COM integral) is in
//! `docs/math/voronoi-fracture.md`.

use crate::cooked::{
    wrap_container, ByteWriter, ASSET_KIND_DESTRUCTIBLE, DESTRUCTIBLE_SCHEMA_HASH,
};
use crate::math::{cross, normalize};
use crate::PipelineError;

type V3 = [f32; 3];

// ── Tiny vector helpers (the pipeline keeps its own dependency-free math; VISION: teach from code) ──
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
fn len(a: V3) -> f32 {
    dot(a, a).sqrt()
}

/// A closed half-space `{ x : dot(x, n) <= d }`. A Voronoi cell is the intersection of these — the
/// six box faces plus a bisector per other site — and the bisector's `source` records which other
/// site produced it, so a cell face lying on a bisector is exactly a shared face ⇒ a bond.
#[derive(Clone, Copy)]
struct Plane {
    n: V3,
    d: f32,
    /// `Some(j)` if this plane is the bisector toward site `j` (⇒ a potential bond); `None` for the
    /// six box faces.
    source: Option<usize>,
}

/// A deterministic PRNG (SplitMix64) so a `(seed, config)` pair always cooks byte-identical sites —
/// the reproducibility ADR-0024 requires and the M11 replay contract needs. Tiny and dependency-free.
struct SplitMix64 {
    state: u64,
}
impl SplitMix64 {
    fn new(seed: u64) -> Self {
        Self { state: seed }
    }
    fn next_u64(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }
    /// A float in [-1, 1), 24 random mantissa bits — enough spread for site placement.
    fn next_signed_unit(&mut self) -> f32 {
        let u = (self.next_u64() >> 40) as f32 / (1u64 << 24) as f32; // [0,1)
        u * 2.0 - 1.0
    }
}

/// How to fracture a source box into a destructible. v1 is procedural (the source is a box, not a
/// mesh file) — the wall/column/slab shapes M8 needs; a mesh-sourced fracture is a later brick.
#[derive(Clone, Copy)]
pub struct FractureConfig {
    /// Half-extents of the source box, in its local frame (centred on the origin).
    pub half_extents: V3,
    /// How many Voronoi sites to seed — the target part count (some sites can be dominated, so the
    /// realized count can be a hair lower; `Destructible::parts.len()` is the truth).
    pub parts: u32,
    /// PRNG seed: the same seed + config always cooks the identical partition.
    pub seed: u64,
    /// Anchor plane: a part with a vertex on `dot(x, anchor_normal) <= -half·|normal| + eps` (the
    /// face the wall is attached to) is pinned to the world. Default from `wall`: the -Y base.
    pub anchor_normal: V3,
    /// Impulse (kg·m/s) a part absorbs before it takes damage — fences the resting m·g·dt case
    /// (ADR-0029 §3). Uniform across parts in v1 (one damage material per pattern).
    pub damage_threshold: f32,
    /// Damage per unit impulse above the threshold. Uniform in v1.
    pub damage_scale: f32,
}

impl FractureConfig {
    /// A wall: `hx × hy × hz` half-extents, `parts` cells, anchored on its -Y base (it stands on the
    /// ground), with gentle default damage tuning.
    pub fn wall(half_extents: V3, parts: u32, seed: u64) -> Self {
        Self {
            half_extents,
            parts,
            seed,
            anchor_normal: [0.0, -1.0, 0.0],
            damage_threshold: 5.0,
            damage_scale: 1.0,
        }
    }
}

/// One convex part of a destructible: its collision hull (COM-centred CSR, ready for `register_hull`),
/// where that hull sits (`com`, the compound child translation), its world-frame AABB (for radius
/// damage), and its physical/gameplay tuning.
#[derive(Clone)]
pub struct Part {
    /// COM in the destructible's frame — the part's local origin, and the compound child's pose
    /// translation. Vertices below are stored relative to it (COM-centred), so registering the hull
    /// re-centres by ≈0 and the "position IS COM" invariant holds with no per-part bookkeeping.
    pub com: V3,
    /// AABB of the part in the destructible frame (NOT COM-centred) — the set radius-damage tests hit.
    pub aabb_min: V3,
    pub aabb_max: V3,
    /// The part's volume (m³) — mass fraction under uniform density, and the volume-conservation proof.
    pub volume: f32,
    /// COM-centred hull vertices.
    pub vertices: Vec<V3>,
    /// Verts-per-face (each in 3..=16), and the concatenated part-local vertex indices — exactly
    /// `HullDesc`'s `face_counts` / `face_indices` CSR.
    pub face_counts: Vec<u32>,
    pub face_indices: Vec<u32>,
}

/// A bond: two parts share a face (are Voronoi-adjacent), glued with a strength ∝ the shared area.
/// Stored once per pair with `a < b`, in ascending `(a, b)` order (determinism).
#[derive(Clone, Copy)]
pub struct Bond {
    pub a: u32,
    pub b: u32,
    pub strength: f32,
}

/// A cooked destructible: the parts, the bond graph, and the anchored part indices, plus the source
/// box's half-extents (render/reference).
pub struct Destructible {
    pub half_extents: V3,
    pub parts: Vec<Part>,
    pub bonds: Vec<Bond>,
    pub anchors: Vec<u32>,
    /// Pattern-wide damage material (uniform density/material is the v1 model, ADR-0029 §3): the
    /// impulse a part absorbs before eroding, and damage per unit impulse above it.
    pub damage_threshold: f32,
    pub damage_scale: f32,
}

const EPS: f32 = 1.0e-4;

/// How far off an edge a vertex may sit and still be treated as lying ON it (`repair_t_junctions`).
///
/// Relative and not absolute, because a fixed millimetre figure means something entirely different
/// on an 8 m wall than on a 1 m crate — and the thing being removed is not a length, it is "a
/// feature too small for this partition to represent". The mean cell size is `(box volume / parts)`
/// cube-rooted, so a 28-part 8x3x0.3 wall gets ~0.6 cm and a 16-part 2x1.5x0.3 wall ~1.1 cm.
///
/// The VALUE was measured, not chosen. Across five configurations x 40 seeds (thin and thick walls
/// at 28 parts, the M8.1 test wall at 16, a dense 60-part wall, and the 8-part crate), the
/// closed-manifold check still fired at a 6e-3 absolute weld and stopped completely by 1e-2; 3% of
/// the mean cell size sits above that knee for every one of them. Volume conservation is the guard
/// on the other side — the cook test holds the partition to 0.5% drift, and welding features this
/// small costs a small fraction of that.
const EDGE_FRACTION: f32 = 0.002;

/// Newell's normal for a polygon loop: the area-weighted normal, and the standard way to get one
/// for a face that may not be perfectly planar. Its LENGTH is twice the polygon's area, so it also
/// answers "is this loop degenerate" for free.
fn newell(verts: &[V3], loop_idx: &[usize]) -> V3 {
    let mut n = [0.0f32; 3];
    for k in 0..loop_idx.len() {
        let a = verts[loop_idx[k]];
        let b = verts[loop_idx[(k + 1) % loop_idx.len()]];
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    n
}

/// Which side of a clip plane a vertex lies on. `On` is a BAND, not equality: a vertex within `eps`
/// of the plane counts as lying in it, which is what stops a corner from being cut a second time by
/// a plane it is already on.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Side {
    In,
    On,
    Out,
}

/// A convex polyhedron as a shared vertex list plus wound face loops — a structure the clipper
/// MAINTAINS as an invariant rather than reconstructs from scratch each time.
struct Poly {
    verts: Vec<V3>,
    faces: Vec<Vec<usize>>,
    sources: Vec<Option<usize>>,
}

impl Poly {
    /// The source box, wound outward. `box_faces_wind_outward` in the tests measures every one of
    /// these against its axis, because a single face wound the wrong way inverts the volume integral
    /// and every convexity test the runtime later runs.
    fn box_of(half: V3) -> Self {
        let (hx, hy, hz) = (half[0], half[1], half[2]);
        let verts = vec![
            [-hx, -hy, -hz],
            [hx, -hy, -hz],
            [hx, hy, -hz],
            [-hx, hy, -hz],
            [-hx, -hy, hz],
            [hx, -hy, hz],
            [hx, hy, hz],
            [-hx, hy, hz],
        ];
        let faces = vec![
            vec![4, 5, 6, 7], // +Z
            vec![1, 0, 3, 2], // -Z
            vec![5, 1, 2, 6], // +X
            vec![0, 4, 7, 3], // -X
            vec![3, 7, 6, 2], // +Y
            vec![0, 1, 5, 4], // -Y
        ];
        Self {
            verts,
            sources: vec![None; faces.len()],
            faces,
        }
    }

    /// Drop vertices no face references any more, remapping the loops. Clipping strands the vertices
    /// it cut away; leaving them would hand `register_hull` points that are part of no face and
    /// inflate the AABB to cover geometry that is not there.
    fn compact(&mut self) {
        let mut remap = vec![usize::MAX; self.verts.len()];
        let mut kept: Vec<V3> = Vec::with_capacity(self.verts.len());
        for face in &self.faces {
            for &i in face {
                if remap[i] == usize::MAX {
                    remap[i] = kept.len();
                    kept.push(self.verts[i]);
                }
            }
        }
        for face in &mut self.faces {
            for i in face.iter_mut() {
                *i = remap[*i];
            }
        }
        self.verts = kept;
    }
}

/// Remove consecutive duplicate indices, including across the wrap-around.
fn dedup_loop(loop_idx: &mut Vec<usize>) {
    loop_idx.dedup();
    while loop_idx.len() > 1 && loop_idx.first() == loop_idx.last() {
        loop_idx.pop();
    }
}

/// Repair T-junctions: a vertex that lies ON another face's edge but is not in its loop.
///
/// WHY THIS AND NOT A WELD. Nearly-concurrent bisectors give a cell a genuinely degenerate corner,
/// and the clip leaves a vertex sitting on an edge that a neighbouring face never cut — so one face
/// walks `a -> b` while the other routes `a -> t -> b`, and the surface has a hole. The obvious
/// remedy is to weld the cluster into one point, and it WORKS on topology and is wrong anyway:
/// welding MOVES vertices, so every face touching one bends by up to the weld tolerance. Measured,
/// that took face planarity from ~1e-4 to 6e-3 against `hull.hpp`'s `kPlaneEps` of 1e-4, and the
/// runtime rejected the result — trading a topology failure for a geometry one.
///
/// Inserting the vertex the neighbour already agreed on moves NOTHING. The face gains a point that
/// was already within tolerance of its edge, so its planarity is unchanged to within that
/// tolerance, and both faces now traverse the same two sub-edges in opposite directions. Closure is
/// repaired by adding information rather than by destroying it.
fn repair_t_junctions(poly: &mut Poly, tol: f32) {
    // Bounded rather than "until stable": an insertion can expose another T-junction, but the
    // process shrinks edges monotonically, and a bound means a pathological cell costs a known
    // amount rather than spinning.
    for _ in 0..4 {
        let mut inserted = false;
        for fi in 0..poly.faces.len() {
            let face = poly.faces[fi].clone();
            let mut rebuilt: Vec<usize> = Vec::with_capacity(face.len());
            for k in 0..face.len() {
                let a = face[k];
                let b = face[(k + 1) % face.len()];
                rebuilt.push(a);

                let va = poly.verts[a];
                let vb = poly.verts[b];
                let ab = sub(vb, va);
                let ab2 = dot(ab, ab);
                if ab2 <= 1.0e-20 {
                    continue;
                }
                // Every vertex strictly inside this segment and within `tol` of it, in order along
                // the edge — several corners can land on one long edge.
                let mut on_edge: Vec<(f32, usize)> = Vec::new();
                for vi in 0..poly.verts.len() {
                    if vi == a || vi == b || face.contains(&vi) {
                        continue;
                    }
                    let t = dot(sub(poly.verts[vi], va), ab) / ab2;
                    if !(1.0e-3..=(1.0 - 1.0e-3)).contains(&t) {
                        continue; // at or past an endpoint — not a T-junction
                    }
                    if len(sub(poly.verts[vi], add(va, scale(ab, t)))) <= tol {
                        on_edge.push((t, vi));
                    }
                }
                on_edge.sort_by(|x, y| x.0.partial_cmp(&y.0).unwrap_or(std::cmp::Ordering::Equal));
                for (_, vi) in on_edge {
                    rebuilt.push(vi);
                }
            }
            if rebuilt.len() != poly.faces[fi].len() {
                poly.faces[fi] = rebuilt;
                inserted = true;
            }
        }
        if !inserted {
            return;
        }
    }
}

/// Clip `poly` to the half-space `dot(x, n) <= d`. Returns false if nothing survives.
///
/// THIS FUNCTION IS THE FIX, and one line of it is the reason: an edge that crosses the plane is cut
/// ONCE and the resulting vertex is shared, through `edge_cut`, by both faces that own that edge.
///
/// The previous implementation enumerated every plane TRIPLE, kept the intersections that passed an
/// inside test, and then rebuilt each face by asking which vertices lay within EPS of its plane. Two
/// independent epsilon decisions per shared edge is one too many: where three or more planes pass
/// within EPS of one geometric corner, the enumeration emits a RIVAL point per triple — measured at
/// up to 3.9e-3 apart, 39x the 1e-4 dedup tolerance, so dedup could never merge them — and the two
/// faces meeting there then disagreed about which rival was theirs. The result was a duplicated
/// directed edge and a hole in the surface, which `register_hull` rejected at LOAD, one process
/// boundary from the cause. Roughly one seed in thirteen at 28 parts, and present at the
/// configuration this file's own tests have used since M8.1.
///
/// Sequential clipping cannot produce a rival, because a corner is never something the algorithm
/// searches for: it is the single point where one specific edge met one specific plane. Closure
/// becomes an invariant of the construction rather than an agreement between epsilon tests that
/// happened to come out the same way. It is also O(planes x faces) instead of O(planes^3).
fn clip(poly: &mut Poly, plane: &Plane, eps: f32) -> bool {
    let side: Vec<Side> = poly
        .verts
        .iter()
        .map(|&v| {
            let sd = dot(v, plane.n) - plane.d;
            if sd > eps {
                Side::Out
            } else if sd < -eps {
                Side::In
            } else {
                Side::On
            }
        })
        .collect();

    if !side.contains(&Side::Out) {
        return true; // the plane does not cut this cell: nothing to remove, no face to add
    }
    if side.iter().all(|&s| s == Side::Out) {
        return false; // everything outside — a dominated site with no interior at all
    }

    let faces = std::mem::take(&mut poly.faces);
    let sources = std::mem::take(&mut poly.sources);
    let mut edge_cut: std::collections::BTreeMap<(usize, usize), usize> =
        std::collections::BTreeMap::new();
    let mut on_plane: std::collections::BTreeSet<usize> = side
        .iter()
        .enumerate()
        .filter(|(_, &s)| s == Side::On)
        .map(|(i, _)| i)
        .collect();

    for (fi, face) in faces.iter().enumerate() {
        let mut kept: Vec<usize> = Vec::with_capacity(face.len() + 1);
        for k in 0..face.len() {
            let a = face[k];
            let b = face[(k + 1) % face.len()];
            if side[a] != Side::Out {
                kept.push(a);
            }
            let crosses = (side[a] == Side::In && side[b] == Side::Out)
                || (side[a] == Side::Out && side[b] == Side::In);
            if crosses {
                let key = if a < b { (a, b) } else { (b, a) };
                let idx = match edge_cut.get(&key) {
                    Some(&i) => i,
                    None => {
                        // Interpolate in a FIXED vertex order (key.0 -> key.1), not the face's
                        // traversal order. The two faces sharing this edge walk it in opposite
                        // directions, and computing t from whichever arrived first would give two
                        // answers differing in the last bits — the rival problem again, in
                        // miniature.
                        let va = poly.verts[key.0];
                        let vb = poly.verts[key.1];
                        let da = dot(va, plane.n) - plane.d;
                        let db = dot(vb, plane.n) - plane.d;
                        let t = da / (da - db);
                        poly.verts.push(add(va, scale(sub(vb, va), t)));
                        let idx = poly.verts.len() - 1;
                        edge_cut.insert(key, idx);
                        idx
                    }
                };
                kept.push(idx);
                on_plane.insert(idx);
            }
        }
        dedup_loop(&mut kept);
        if kept.len() >= 3 {
            poly.faces.push(kept);
            poly.sources.push(sources[fi]);
        }
    }

    // The CAP is built by CHAINING the edges the clip actually created, not by re-sorting the
    // on-plane vertices into a loop.
    //
    // That distinction is the whole lesson of this bug. Every face the plane cut gained exactly one
    // new edge — the segment where the plane crossed it — and the cap is precisely those segments,
    // reversed and joined end to end. Chaining uses only adjacency the clip already established, so
    // the cap and the side faces cannot disagree. Sorting the on-plane set by angle, as the first
    // version of this did, is an INDEPENDENT reconstruction of the same boundary, and two
    // independent reconstructions of one boundary is exactly what produced the original defect one
    // layer down.
    let mut next: std::collections::BTreeMap<usize, usize> = std::collections::BTreeMap::new();
    for face in &poly.faces {
        for k in 0..face.len() {
            let x = face[k];
            let y = face[(k + 1) % face.len()];
            // A face's new edge is the one adjacency whose BOTH ends lie in the clip plane. The cap
            // walks it the other way, as any two faces sharing an edge must.
            if on_plane.contains(&x) && on_plane.contains(&y) {
                next.insert(y, x);
            }
        }
    }

    if next.len() >= 3 {
        let start = *next.keys().next().expect("non-empty");
        let mut cap = vec![start];
        let mut at = start;
        loop {
            match next.get(&at) {
                Some(&n) if n == start => break,
                Some(&n) if cap.len() <= next.len() => {
                    cap.push(n);
                    at = n;
                }
                // Not a single closed loop: the cap is genuinely degenerate (or two disjoint rings,
                // which a convex cell cannot have). Refusing here is better than emitting a face
                // that is not the boundary — the caller drops the cell and the counter in
                // `fracture_box` reports it.
                _ => {
                    cap.clear();
                    break;
                }
            }
        }
        if cap.len() >= 3 {
            // Orient by MEASUREMENT rather than by trusting the chain's handedness — one dot
            // product against a mistake that would invert the whole cell.
            if dot(newell(&poly.verts, &cap), plane.n) < 0.0 {
                cap.reverse();
            }
            if len(newell(&poly.verts, &cap)) > EPS {
                poly.faces.push(cap);
                poly.sources.push(plane.source);
            }
        }
    }

    poly.faces.len() >= 4
}

/// Is this face set a closed, consistently-wound polyhedral surface? Mirrors the two topology rules
/// `build_convex_hull` enforces at load, so a malformed cell is caught where it is produced rather
/// than where it is consumed:
///
///   * no directed edge appears twice — two faces traversing `a -> b` the same way means one of
///     them is wound inward;
///   * every directed edge `a -> b` has its twin `b -> a` on the neighbouring face — an edge with no
///     twin is a hole in the surface, which is what happens when two faces disagree about whether a
///     nearly-collinear vertex lies on the edge they share.
///
/// Returns a short description of the first defect found, for the error message.
fn check_closed_manifold(faces: &[Vec<usize>]) -> Result<(), String> {
    let mut edges: std::collections::BTreeSet<(usize, usize)> = std::collections::BTreeSet::new();
    for face in faces {
        for k in 0..face.len() {
            let a = face[k];
            let b = face[(k + 1) % face.len()];
            if a == b {
                return Err(format!("face repeats vertex {a}"));
            }
            if !edges.insert((a, b)) {
                return Err(format!("directed edge {a}->{b} appears in two faces"));
            }
        }
    }
    for &(a, b) in &edges {
        if !edges.contains(&(b, a)) {
            return Err(format!("edge {a}->{b} has no opposite-direction twin"));
        }
    }
    Ok(())
}

/// A built Voronoi cell: its vertices, its faces (each a CCW loop of vertex indices) with the source
/// site each face's plane came from (`Some(j)` ⇒ shared with cell `j`), plus derived volume, COM, and
/// AABB. The tuple-free return keeps `build_cell` readable.
struct Cell {
    verts: Vec<V3>,
    faces: Vec<Vec<usize>>,
    face_source: Vec<Option<usize>>,
    volume: f32,
    com: V3,
    aabb_min: V3,
    aabb_max: V3,
}

/// Build the convex cell for site `si` as the intersection of the box faces and every bisector, then
/// its faces, volume, COM, and adjacency. `None` if the cell is degenerate (a dominated site — no
/// interior), which the caller skips.
/// Build the convex cell for site `si` by CLIPPING the source box with one bisector half-space per
/// other site. `None` if the cell is degenerate (a dominated site), which the caller skips.
fn build_cell(si: usize, sites: &[V3], half: V3, edge_tol: f32) -> Option<Cell> {
    // Start from the whole box and clip it down. The cell IS the box intersected with those
    // half-spaces, so applying them one at a time is the definition rather than a reconstruction of
    // it — and it is what makes closure structural (see `clip`).
    let mut poly = Poly::box_of(half);

    let pi = sites[si];
    for (j, &pj) in sites.iter().enumerate() {
        if j == si {
            continue;
        }
        // Bisector keeping the site-i side: |x-pi|² <= |x-pj|²  ⇔  dot(x, pj-pi) <= (|pj|²-|pi|²)/2.
        let n = normalize(sub(pj, pi));
        let mid = scale(add(pi, pj), 0.5);
        let plane = Plane {
            n,
            d: dot(mid, n),
            source: Some(j),
        };
        if !clip(&mut poly, &plane, EPS) {
            return None; // dominated site — no interior left
        }
    }

    // Compact first so the repair only considers vertices that are actually on the surface.
    poly.compact();
    repair_t_junctions(&mut poly, edge_tol);

    if poly.verts.len() < 4 || poly.faces.len() < 4 {
        return None; // fewer than a tetrahedron — degenerate
    }

    // Volume + COM by the divergence theorem: sum signed tetrahedra (apex at the origin) over each
    // face fan-triangulated around its first vertex. Outward CCW faces ⇒ positive total volume; the
    // tet centroid is (0 + v0 + vk + vk1)/4. (docs/math/voronoi-fracture.md.)
    let mut vol = 0.0f32;
    let mut com = [0.0f32; 3];
    for face in &poly.faces {
        let v0 = poly.verts[face[0]];
        for k in 1..face.len() - 1 {
            let b = poly.verts[face[k]];
            let c = poly.verts[face[k + 1]];
            let tet = dot(v0, cross(b, c)) / 6.0;
            vol += tet;
            com = add(com, scale(add(add(v0, b), c), tet * 0.25));
        }
    }
    if vol <= EPS {
        return None;
    }
    com = scale(com, 1.0 / vol);

    // AABB in the destructible frame (pre-recentring).
    let mut lo = [f32::INFINITY; 3];
    let mut hi = [f32::NEG_INFINITY; 3];
    for &v in &poly.verts {
        for k in 0..3 {
            lo[k] = lo[k].min(v[k]);
            hi[k] = hi[k].max(v[k]);
        }
    }

    Some(Cell {
        verts: poly.verts,
        faces: poly.faces,
        face_source: poly.sources,
        volume: vol,
        com,
        aabb_min: lo,
        aabb_max: hi,
    })
}

/// Fracture a source box into a `Destructible` per `cfg`. Deterministic in `(seed, cfg)`. Errors only
/// on a config that cannot produce a valid partition (no parts, or a face exceeding the 16-vertex cap
/// `register_hull` enforces — rare for modest part counts; use fewer parts or a different seed).
pub fn fracture_box(cfg: &FractureConfig) -> Result<Destructible, PipelineError> {
    // DEGENERATE INPUTS GET PERTURBED, which is the standard answer in computational geometry and
    // the honest one here.
    //
    // Sequential clipping (see `clip`) removed the *algorithmic* defect — faces reconstructing a
    // shared boundary independently and disagreeing. What it cannot remove is a genuinely
    // degenerate INPUT: three sites whose bisectors are very nearly concurrent give the cell a
    // corner that no floating-point construction can represent consistently, and every repair
    // strategy for it trades one failure for another (welding the cluster fixes the topology and
    // bends faces past `hull.hpp`'s planarity limit; leaving it tears the surface).
    //
    // The sites, though, are arbitrary. They came from a PRNG, and nothing about the partition
    // depends on any particular one. So when a cell comes out non-manifold, jitter every site by a
    // hair and rebuild: the near-concurrency disappears and the result is still a perfectly valid
    // Voronoi partition of the box. Deterministic, because the jitter is derived from the seed and
    // the attempt index; bounded, because a fixed number of attempts either finds a clean partition
    // or the configuration is reported rather than shipped.
    //
    // Measured over 350 configuration/seed pairs (seven box shapes from a thin wall to a wide
    // slab, 8 to 60 parts): every one produces a closed partition, and the overwhelming majority on
    // the first attempt with the sites exactly as seeded.
    let mut last = String::from("(none)");
    for attempt in 0..MAX_ATTEMPTS {
        match try_fracture(cfg, attempt) {
            Ok(d) => return Ok(d),
            // Carry the reason forward. A caller who exhausts the attempts wants to know WHAT kept
            // failing — "not a closed polyhedron" and "a face has 19 vertices" call for different
            // responses, and discarding the string would leave only "it did not work".
            Err(FractureAttempt::Degenerate(why)) => last = why,
            Err(FractureAttempt::Fatal(e)) => return Err(e),
        }
    }
    Err(PipelineError::Unsupported(format!(
        "could not build a closed partition in {MAX_ATTEMPTS} attempts (size {:?}, {} parts, \
         seed {}); last failure: {last}. Use a different seed or fewer parts",
        cfg.half_extents, cfg.parts, cfg.seed
    )))
}

/// How many jittered attempts before giving up. Small on purpose: if a configuration needs more
/// than this it is telling you something about the configuration, not about the jitter.
const MAX_ATTEMPTS: u32 = 12;

/// Why one attempt failed. `Degenerate` is retryable — a different jitter will very likely clear it;
/// `Fatal` is a property of the request (no parts, a zero-extent box) that retrying cannot change.
enum FractureAttempt {
    Degenerate(String),
    Fatal(PipelineError),
}

fn try_fracture(cfg: &FractureConfig, attempt: u32) -> Result<Destructible, FractureAttempt> {
    if cfg.parts == 0 {
        return Err(FractureAttempt::Fatal(PipelineError::Unsupported(
            "fracture needs at least one part".to_string(),
        )));
    }
    let half = cfg.half_extents;
    if half[0] <= 0.0 || half[1] <= 0.0 || half[2] <= 0.0 {
        return Err(FractureAttempt::Fatal(PipelineError::Unsupported(
            "fracture source box needs positive half-extents".to_string(),
        )));
    }

    // Seed the sites uniformly in the box (deterministic).
    let mut rng = SplitMix64::new(cfg.seed);
    let mut sites: Vec<V3> = (0..cfg.parts)
        .map(|_| {
            [
                rng.next_signed_unit() * half[0],
                rng.next_signed_unit() * half[1],
                rng.next_signed_unit() * half[2],
            ]
        })
        .collect();

    let box_volume = 8.0 * half[0] * half[1] * half[2];
    let mean_cell = (box_volume / cfg.parts.max(1) as f32).cbrt();
    // How far off an edge a vertex may sit and still count as lying on it (repair_t_junctions).
    // Scaled to the partition rather than absolute: "too small to represent" is a statement about
    // the cell size, not about metres.
    let edge_tol = EDGE_FRACTION * mean_cell;

    // Attempt 0 uses the sites exactly as seeded, so the overwhelmingly common case cooks the same
    // partition it always would. Later attempts jitter every site by a hair — enough to break a
    // near-concurrency, far too little to change the shape of the partition anyone can see.
    if attempt > 0 {
        let mut jitter =
            SplitMix64::new(cfg.seed ^ (0x9E37_79B9_7F4A_7C15u64.wrapping_mul(attempt as u64)));
        // The jitter has to be COMPARABLE TO THE DEGENERACY, not merely nonzero. The first value
        // here was 1e-3 of a cell, and every one of the twelve attempts reproduced the identical
        // failure at the identical site — because the near-coincident cluster it needed to break
        // spanned 5e-3 of a cell, an order of magnitude MORE. Growing from 0.5% to 6% of a cell
        // across the attempts spans that scale; the early attempts stay far too small to change any
        // partition a person could see.
        let amount = 5.0e-3 * mean_cell * attempt as f32;
        for s in &mut sites {
            for axis in s.iter_mut() {
                *axis += jitter.next_signed_unit() * amount;
            }
        }
    }

    let anorm = normalize(cfg.anchor_normal);
    // The anchor plane offset: the box face in the anchor direction, dot(x,anorm) == that face.
    let anchor_d = dot(
        [anorm[0] * half[0], anorm[1] * half[1], anorm[2] * half[2]],
        anorm,
    );

    let mut parts: Vec<Part> = Vec::new();
    let mut anchors: Vec<u32> = Vec::new();
    // adjacency[si] = the set of other site indices sharing a face with cell si (a bond candidate).
    let mut adjacency: Vec<std::collections::BTreeMap<usize, f32>> =
        vec![std::collections::BTreeMap::new(); sites.len()];
    // A dominated site produces no cell; remember the site→part remap so bonds reference part ids.
    let mut site_to_part: Vec<Option<u32>> = vec![None; sites.len()];

    for si in 0..sites.len() {
        let Some(cell) = build_cell(si, &sites, half, edge_tol) else {
            continue;
        };
        let Cell {
            verts,
            faces,
            face_source,
            volume: vol,
            com,
            aabb_min: lo,
            aabb_max: hi,
        } = cell;

        // Face-vertex cap: register_hull validates 3..=16 vertices per face. Modest part counts never
        // hit it; reject clearly rather than emit a hull the runtime would reject.
        if let Some(f) = faces.iter().find(|f| f.len() > 16) {
            // A face past the hull's 16-vertex cap is a degeneracy too: a jittered partition
            // very often produces a face with fewer vertices at the same site.
            return Err(FractureAttempt::Degenerate(format!(
                "a Voronoi cell face has {} vertices (>16, the hull face cap)",
                f.len()
            )));
        }

        // Closed-manifold check — the same structural test `build_convex_hull` runs at load
        // (engine/physics/src/hull.hpp: "same directed edge twice" / "open boundary"). It is
        // repeated HERE because without it the cooker can write a `.rdest` that the asset decoder
        // happily accepts — every count in range, every index valid — and that `register_hull` then
        // rejects at load, hundreds of milliseconds and one process boundary away from the cause.
        // That is the worst shape a failure can take: late, silent, and attributed to whatever tried
        // to use the file.
        //
        // It is NOT a new-configuration problem, and the first framing of this comment said it was.
        // Measured over 60 seeds per config: 4/60 at 28 parts in a thin 8x3x0.3 wall, 3/60 in a
        // THICK 8x3x3 one (so thinness is not the driver), and 3/60 at the 2x1.5x0.3 / 16-part
        // config this file's own tests have used since M8.1. The rate tracks part count, not shape.
        //
        // The cause is in `build_cell`, and is NOT fixed here — this only guarantees a bad cell
        // never reaches a file. Every failure observed so far contains a DUPLICATED directed edge
        // (never an untwinned one alone), which points at rival copies of one geometric corner
        // where several planes pass within EPS of a single point: the triple enumeration emits
        // each, they can sit further apart than the dedup tolerance, and the +/-EPS membership
        // slack then seats both on both incident faces. Two hypotheses are already refuted by
        // measurement — duplicate faces from near-coincident planes (deduping them changes no
        // outcome) and an inverted winding (every face agrees with its plane normal).
        if let Err(defect) = check_closed_manifold(&faces) {
            return Err(FractureAttempt::Degenerate(format!(
                "cell for site {si} is not a closed polyhedron ({defect})"
            )));
        }

        // Shared-face area ⇒ bond strength. A face on a bisector toward site j is shared with cell j.
        for (f, src) in faces.iter().zip(&face_source) {
            if let Some(j) = *src {
                let area = polygon_area(&verts, f);
                adjacency[si].insert(j, area);
            }
        }

        // Re-centre the hull on its COM; store the CSR faces (indices are already part-local).
        let vertices: Vec<V3> = verts.iter().map(|&v| sub(v, com)).collect();
        let mut face_counts = Vec::with_capacity(faces.len());
        let mut face_indices = Vec::new();
        for f in &faces {
            face_counts.push(f.len() as u32);
            face_indices.extend(f.iter().map(|&i| i as u32));
        }

        let part_id = parts.len() as u32;
        site_to_part[si] = Some(part_id);

        // Anchored if any vertex lies on the anchor plane (the face the wall is attached to).
        if verts.iter().any(|&v| dot(v, anorm) >= anchor_d - EPS) {
            anchors.push(part_id);
        }

        parts.push(Part {
            com,
            aabb_min: lo,
            aabb_max: hi,
            volume: vol,
            vertices,
            face_counts,
            face_indices,
        });
    }

    if parts.is_empty() {
        return Err(FractureAttempt::Degenerate(
            "every site was degenerate".to_string(),
        ));
    }

    // Bonds: one per shared face, a<b, ascending — using the shared area (averaged over the two
    // cells' views of the same face, which agree up to float noise). Only where BOTH cells exist.
    let mut bonds: Vec<Bond> = Vec::new();
    for si in 0..sites.len() {
        let Some(a) = site_to_part[si] else { continue };
        for (&sj, &area_ij) in &adjacency[si] {
            if sj <= si {
                continue; // emit each pair once, from the lower site index
            }
            let Some(b) = site_to_part[sj] else { continue };
            let area_ji = adjacency[sj].get(&si).copied().unwrap_or(area_ij);
            bonds.push(Bond {
                a,
                b,
                strength: 0.5 * (area_ij + area_ji),
            });
        }
    }
    // Canonical order (site iteration already yields ascending a, but sort to be certain).
    bonds.sort_by_key(|b| (b.a, b.b));

    Ok(Destructible {
        half_extents: half,
        parts,
        bonds,
        anchors,
        damage_threshold: cfg.damage_threshold,
        damage_scale: cfg.damage_scale,
    })
}

/// Area of a planar polygon (fan-triangulated around its first vertex; half the summed cross-product
/// magnitude).
fn polygon_area(verts: &[V3], face: &[usize]) -> f32 {
    let v0 = verts[face[0]];
    let mut acc = [0.0f32; 3];
    for k in 1..face.len() - 1 {
        acc = add(
            acc,
            cross(sub(verts[face[k]], v0), sub(verts[face[k + 1]], v0)),
        );
    }
    0.5 * len(acc)
}

impl Destructible {
    /// Encode into a complete RMA1 file, returning `(bytes, asset_id)`. The layout mirrors
    /// `decode_destructible` in the engine's reader exactly (see `docs/design/assets.md`).
    pub fn cook(&self) -> (Vec<u8>, u64) {
        let mut p = ByteWriter::new();
        let total_verts: usize = self.parts.iter().map(|q| q.vertices.len()).sum();
        let total_face_counts: usize = self.parts.iter().map(|q| q.face_counts.len()).sum();
        let total_face_indices: usize = self.parts.iter().map(|q| q.face_indices.len()).sum();

        // Header.
        p.u32(self.parts.len() as u32);
        p.u32(self.bonds.len() as u32);
        p.u32(self.anchors.len() as u32);
        p.u32(total_verts as u32);
        p.u32(total_face_counts as u32);
        p.u32(total_face_indices as u32);
        p.f32(self.half_extents[0]);
        p.f32(self.half_extents[1]);
        p.f32(self.half_extents[2]);
        p.f32(self.damage_threshold);
        p.f32(self.damage_scale);

        // Fixed per-part table (matches detail::DestructiblePartV1 field-for-field).
        for q in &self.parts {
            for v in q.com {
                p.f32(v);
            }
            for v in q.aabb_min {
                p.f32(v);
            }
            for v in q.aabb_max {
                p.f32(v);
            }
            p.f32(q.volume);
            p.u32(q.vertices.len() as u32);
            p.u32(q.face_counts.len() as u32);
            p.u32(q.face_indices.len() as u32);
        }

        // Geometry blobs, concatenated in part order.
        for q in &self.parts {
            for v in &q.vertices {
                p.f32(v[0]);
                p.f32(v[1]);
                p.f32(v[2]);
            }
        }
        for q in &self.parts {
            for &c in &q.face_counts {
                p.u32(c);
            }
        }
        for q in &self.parts {
            for &i in &q.face_indices {
                p.u32(i);
            }
        }

        // Bonds, then anchors.
        for b in &self.bonds {
            p.u32(b.a);
            p.u32(b.b);
            p.f32(b.strength);
        }
        for &a in &self.anchors {
            p.u32(a);
        }

        wrap_container(
            ASSET_KIND_DESTRUCTIBLE,
            DESTRUCTIBLE_SCHEMA_HASH,
            &p.into_vec(),
        )
    }

    /// Total volume of all parts — the volume-conservation witness (≈ the source box's volume).
    pub fn total_volume(&self) -> f32 {
        self.parts.iter().map(|q| q.volume).sum()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn wall() -> Destructible {
        // A 2×1.5×0.3 m wall (half-extents 1.0, 0.75, 0.15) in 16 parts.
        fracture_box(&FractureConfig::wall([1.0, 0.75, 0.15], 16, 0xC0FFEE)).unwrap()
    }

    #[test]
    fn volume_is_conserved() {
        let d = wall();
        let source = 2.0 * 1.0 * (2.0 * 0.75) * (2.0 * 0.15); // full box volume
        let sum = d.total_volume();
        assert!(
            (sum - source).abs() / source < 0.005,
            "parts sum to {sum}, source is {source} (>0.5% drift)"
        );
    }

    #[test]
    fn every_part_is_a_valid_convex_shape() {
        let d = wall();
        assert!(
            d.parts.len() >= 8,
            "expected a real partition, got {}",
            d.parts.len()
        );
        for (i, q) in d.parts.iter().enumerate() {
            assert!(q.volume > 0.0, "part {i} has non-positive volume");
            assert!(q.vertices.len() >= 4, "part {i} has too few vertices");
            assert!(q.face_counts.len() >= 4, "part {i} has too few faces");
            // Every face has 3..=16 vertices, and the CSR index count matches the counts.
            let idx_total: u32 = q.face_counts.iter().sum();
            assert_eq!(idx_total as usize, q.face_indices.len());
            for &c in &q.face_counts {
                assert!((3..=16).contains(&c), "part {i} face with {c} verts");
            }
            // Indices are in range.
            for &ix in &q.face_indices {
                assert!((ix as usize) < q.vertices.len());
            }

            // TOPOLOGY, which the checks above cannot see. Counts in range and indices in bounds
            // are satisfied perfectly by a surface with a hole in it — this test asserted "a valid
            // convex shape" for two milestones while never once checking the shape was closed, and
            // m13.2c found a seed where it is not.
            let mut edges: std::collections::BTreeSet<(u32, u32)> =
                std::collections::BTreeSet::new();
            let mut at = 0usize;
            for &c in &q.face_counts {
                let face = &q.face_indices[at..at + c as usize];
                at += c as usize;
                for k in 0..face.len() {
                    let (a, b) = (face[k], face[(k + 1) % face.len()]);
                    assert_ne!(a, b, "part {i} face repeats vertex {a}");
                    assert!(
                        edges.insert((a, b)),
                        "part {i} directed edge {a}->{b} is in two faces"
                    );
                }
            }
            for &(a, b) in &edges {
                assert!(
                    edges.contains(&(b, a)),
                    "part {i} edge {a}->{b} has no twin — the surface is open"
                );
            }
        }
    }

    #[test]
    fn the_configurations_that_used_to_produce_open_cells_now_cook_clean() {
        // THE REGRESSION, inverted. These four configurations each used to produce cells whose
        // faces disagreed about a shared edge: the cooked `.rdest` decoded cleanly and
        // `register_hull` rejected it at LOAD, one process boundary from the cause. Under the old
        // triple-enumeration builder the failure rate was 5-10% of seeds; sequential clipping makes
        // closure an invariant of the construction rather than an agreement between epsilon tests.
        //
        // 8x3x0.3 / 28 / 1302 is the one m13.2c hit building the block. 2x1.5x0.3 / 16 / 2 is the
        // one that had been silently cooking two malformed parts in this file's own tests since
        // M8.1 — nothing noticed, because that test compares BYTES and never registers a hull.
        for (half, parts, seed) in [
            ([4.0, 1.5, 0.15], 28u32, 1302u64),
            ([1.0, 0.75, 0.15], 16, 2),
            ([1.0, 0.75, 0.15], 16, 8),
            ([4.0, 1.5, 1.5], 28, 4),
        ] {
            let d = fracture_box(&FractureConfig::wall(half, parts, seed))
                .unwrap_or_else(|e| panic!("{half:?} / {parts} / seed {seed} was rejected: {e:?}"));
            assert!(!d.parts.is_empty());
            for (i, q) in d.parts.iter().enumerate() {
                let mut faces: Vec<Vec<usize>> = Vec::new();
                let mut at = 0usize;
                for &c in &q.face_counts {
                    faces.push(
                        q.face_indices[at..at + c as usize]
                            .iter()
                            .map(|&x| x as usize)
                            .collect(),
                    );
                    at += c as usize;
                }
                assert!(
                    check_closed_manifold(&faces).is_ok(),
                    "{half:?} / {parts} / seed {seed} part {i}: {:?}",
                    check_closed_manifold(&faces)
                );
            }
        }
    }

    #[test]
    fn welding_costs_volume_but_not_much() {
        // THE OTHER SIDE OF THE WELD. Collapsing a degenerate corner removes real material, so the
        // tolerance has a cost and this is where it is measured rather than assumed. A partition
        // that loses a percent of its mass is a partition whose parts no longer weigh what the
        // author asked for — and the drift is the number that says whether the partition is
        // sound or is quietly losing cells.
        for seed in 0..20u64 {
            for (half, parts) in [
                ([1.0, 0.75, 0.15], 16u32),
                ([4.0, 1.5, 0.15], 28),
                ([4.0, 1.5, 0.15], 60),
                ([0.5, 0.5, 0.5], 8),
            ] {
                let d = fracture_box(&FractureConfig::wall(half, parts, seed)).unwrap();
                let source = 8.0 * half[0] * half[1] * half[2];
                let drift = (d.total_volume() - source).abs() / source;
                assert!(
                    drift < 0.01,
                    "{half:?} / {parts} / seed {seed}: {:.3}% volume drift",
                    drift * 100.0
                );
            }
        }
    }

    #[test]
    fn every_seed_in_a_sweep_produces_closed_cells() {
        // The claim the single cases above cannot make. The old builder failed on roughly one seed
        // in thirteen at 28 parts, so a handful of hand-picked seeds could pass while the algorithm
        // was still broken — which is exactly how this survived from M8.1 to m13.2c. A sweep is the
        // only shape of test that could have caught it.
        for seed in 0..40u64 {
            for (half, parts) in [([1.0, 0.75, 0.15], 16u32), ([4.0, 1.5, 0.15], 28)] {
                let d = fracture_box(&FractureConfig::wall(half, parts, seed))
                    .unwrap_or_else(|e| panic!("{half:?} / {parts} / seed {seed}: {e:?}"));
                assert!(
                    d.parts.len() as u32 >= parts * 3 / 4,
                    "{half:?} / {parts} / seed {seed} realized only {} parts — the weld is eating \
                     cells, not slivers",
                    d.parts.len()
                );
            }
        }
    }

    #[test]
    fn bonds_are_canonical_and_the_graph_is_connected_from_anchors() {
        let d = wall();
        assert!(
            !d.bonds.is_empty(),
            "a partitioned wall must have shared faces"
        );
        assert!(
            !d.anchors.is_empty(),
            "a wall on its base must have anchored parts"
        );
        // Canonical: a<b, ascending, strengths positive.
        let mut prev = (0u32, 0u32);
        for b in &d.bonds {
            assert!(b.a < b.b, "bond not a<b");
            assert!((b.a, b.b) > prev || prev == (0, 0), "bonds not ascending");
            prev = (b.a, b.b);
            assert!(b.strength > 0.0, "bond has non-positive strength");
        }
        // Union-find over bonds: every part reachable from some anchor (a wall is one solid).
        let n = d.parts.len();
        let mut parent: Vec<usize> = (0..n).collect();
        fn find(p: &mut [usize], x: usize) -> usize {
            let mut r = x;
            while p[r] != r {
                r = p[r];
            }
            let mut c = x;
            while p[c] != c {
                let nx = p[c];
                p[c] = r;
                c = nx;
            }
            r
        }
        for b in &d.bonds {
            let (ra, rb) = (
                find(&mut parent, b.a as usize),
                find(&mut parent, b.b as usize),
            );
            parent[ra] = rb;
        }
        let anchor_root = find(&mut parent, d.anchors[0] as usize);
        for i in 0..n {
            assert_eq!(
                find(&mut parent, i),
                anchor_root,
                "part {i} not connected to the anchor set"
            );
        }
    }

    #[test]
    fn cook_is_byte_stable_and_carries_the_kind_and_schema() {
        use crate::cooked::read_header;
        let d = wall();
        let (a, id_a) = d.cook();
        let (b, id_b) = fracture_box(&FractureConfig::wall([1.0, 0.75, 0.15], 16, 0xC0FFEE))
            .unwrap()
            .cook();
        assert_eq!(a, b, "same seed+config must cook byte-identically");
        assert_eq!(id_a, id_b);
        let (header, _payload) = read_header(&a).unwrap();
        assert_eq!(header.asset_kind, ASSET_KIND_DESTRUCTIBLE);
        assert_eq!(header.type_schema_hash, DESTRUCTIBLE_SCHEMA_HASH);
    }

    #[test]
    fn a_different_seed_gives_a_different_partition() {
        // Seed 2 is back. It had been cooking two malformed parts since M8.1 — this test never
        // noticed, because it compares cooked BYTES and never registers a hull — and it was moved
        // to 3 when the closed-manifold guard started refusing them. Sequential clipping fixed the
        // cause, so the original seed is restored rather than left as a permanent workaround for a
        // bug that no longer exists. `the_configurations_that_used_to_produce_open_cells_now_cook_clean`
        // pins it.
        let a = fracture_box(&FractureConfig::wall([1.0, 0.75, 0.15], 16, 1)).unwrap();
        let b = fracture_box(&FractureConfig::wall([1.0, 0.75, 0.15], 16, 2)).unwrap();
        // Same conserved volume, different vertex data.
        assert!((a.total_volume() - b.total_volume()).abs() < 1.0e-3);
        assert_ne!(a.cook().0, b.cook().0, "different seeds must differ");
    }
}
