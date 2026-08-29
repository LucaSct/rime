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

/// Solve the 3×3 system whose rows are the three planes' normals for their common point (Cramer's
/// rule via the adjugate: `x = (d_a·(n_b×n_c) + d_b·(n_c×n_a) + d_c·(n_a×n_b)) / det`). `None` if the
/// planes are (near-)parallel — no single intersection.
fn intersect3(a: &Plane, b: &Plane, c: &Plane) -> Option<V3> {
    let bc = cross(b.n, c.n);
    let det = dot(a.n, bc);
    if det.abs() < 1.0e-9 {
        return None;
    }
    let ca = cross(c.n, a.n);
    let ab = cross(a.n, b.n);
    let inv = 1.0 / det;
    Some(scale(
        add(add(scale(bc, a.d), scale(ca, b.d)), scale(ab, c.d)),
        inv,
    ))
}

/// Order the vertices of one face (all on plane normal `n`) into a CCW loop viewed from outside
/// (+`n`), dropping near-duplicates and collinear points. Returns the ordered indices into `verts`.
/// This is the one place a "hull" is computed — a 2D convex sort in the face plane; the cell itself
/// is already convex from the half-space intersection.
fn order_face(verts: &[V3], on_plane: &[usize], n: V3) -> Vec<usize> {
    // Face centroid, and an in-plane basis (u, w) to measure angles in.
    let mut c = [0.0f32; 3];
    for &i in on_plane {
        c = add(c, verts[i]);
    }
    c = scale(c, 1.0 / on_plane.len() as f32);
    // Pick any edge direction not parallel to n as the u axis.
    let mut u = [0.0f32; 3];
    for &i in on_plane {
        let d = sub(verts[i], c);
        if len(d) > EPS {
            u = normalize(sub(d, scale(n, dot(d, n)))); // project into the plane
            if len(u) > 0.5 {
                break;
            }
        }
    }
    let w = cross(n, u); // completes a right-handed in-plane frame; +angle is CCW around +n

    let mut ordered: Vec<(f32, usize)> = on_plane
        .iter()
        .map(|&i| {
            let d = sub(verts[i], c);
            (dot(d, w).atan2(dot(d, u)), i)
        })
        .collect();
    // Sort by angle; a stable tie-break on index keeps coincident-angle points deterministic.
    ordered.sort_by(|x, y| {
        x.0.partial_cmp(&y.0)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then(x.1.cmp(&y.1))
    });

    // Drop consecutive duplicates and collinear midpoints (they would make a > actual-sided face).
    let mut loop_idx: Vec<usize> = Vec::with_capacity(ordered.len());
    for &(_, i) in &ordered {
        if loop_idx
            .last()
            .is_some_and(|&p| len(sub(verts[i], verts[p])) < EPS)
        {
            continue;
        }
        loop_idx.push(i);
    }
    loop_idx
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
fn build_cell(si: usize, sites: &[V3], half: V3) -> Option<Cell> {
    // The bounding half-spaces: six box faces, then one bisector per other site.
    let mut planes: Vec<Plane> = vec![
        Plane {
            n: [1.0, 0.0, 0.0],
            d: half[0],
            source: None,
        },
        Plane {
            n: [-1.0, 0.0, 0.0],
            d: half[0],
            source: None,
        },
        Plane {
            n: [0.0, 1.0, 0.0],
            d: half[1],
            source: None,
        },
        Plane {
            n: [0.0, -1.0, 0.0],
            d: half[1],
            source: None,
        },
        Plane {
            n: [0.0, 0.0, 1.0],
            d: half[2],
            source: None,
        },
        Plane {
            n: [0.0, 0.0, -1.0],
            d: half[2],
            source: None,
        },
    ];
    let pi = sites[si];
    for (j, &pj) in sites.iter().enumerate() {
        if j == si {
            continue;
        }
        // Bisector keeping the site-i side: |x-pi|² <= |x-pj|²  ⇔  dot(x, pj-pi) <= (|pj|²-|pi|²)/2.
        let n = normalize(sub(pj, pi));
        let mid = scale(add(pi, pj), 0.5);
        planes.push(Plane {
            n,
            d: dot(mid, n),
            source: Some(j),
        });
    }

    // Vertices: every plane triple's intersection that lies inside all half-spaces, deduped.
    let mut verts: Vec<V3> = Vec::new();
    let np = planes.len();
    for a in 0..np {
        for b in (a + 1)..np {
            for c in (b + 1)..np {
                let Some(x) = intersect3(&planes[a], &planes[b], &planes[c]) else {
                    continue;
                };
                // Keep the point only if it is inside every half-space (a real cell corner) and not
                // already found (triples of planes meeting at one corner would each report it).
                if planes.iter().all(|p| dot(x, p.n) <= p.d + EPS)
                    && !verts.iter().any(|&v| len(sub(v, x)) < EPS)
                {
                    verts.push(x);
                }
            }
        }
    }
    if verts.len() < 4 {
        return None; // degenerate / dominated site — no 3D interior
    }

    // Faces: the vertices lying on each plane, ordered into a CCW loop. A plane with <3 survivors
    // only grazed the cell (touched at an edge/vertex) and contributes no face.
    let mut faces: Vec<Vec<usize>> = Vec::new();
    let mut face_source: Vec<Option<usize>> = Vec::new();
    for p in &planes {
        let on: Vec<usize> = (0..verts.len())
            .filter(|&i| (dot(verts[i], p.n) - p.d).abs() < EPS)
            .collect();
        if on.len() < 3 {
            continue;
        }
        let loop_idx = order_face(&verts, &on, p.n);
        if loop_idx.len() >= 3 {
            faces.push(loop_idx);
            face_source.push(p.source);
        }
    }
    if faces.len() < 4 {
        return None; // fewer than a tetrahedron's faces — degenerate
    }

    // Volume + COM by the divergence theorem: sum signed tetrahedra (apex at the origin) over each
    // face fan-triangulated around its first vertex. Outward CCW faces ⇒ positive total volume; the
    // tet centroid is (0 + v0 + vk + vk1)/4. (docs/math/voronoi-fracture.md.)
    let mut vol = 0.0f32;
    let mut com = [0.0f32; 3];
    for face in &faces {
        let v0 = verts[face[0]];
        for k in 1..face.len() - 1 {
            let b = verts[face[k]];
            let c = verts[face[k + 1]];
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
    for &v in &verts {
        for k in 0..3 {
            lo[k] = lo[k].min(v[k]);
            hi[k] = hi[k].max(v[k]);
        }
    }

    Some(Cell {
        verts,
        faces,
        face_source,
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
    if cfg.parts == 0 {
        return Err(PipelineError::Unsupported(
            "fracture needs at least one part".to_string(),
        ));
    }
    let half = cfg.half_extents;
    if half[0] <= 0.0 || half[1] <= 0.0 || half[2] <= 0.0 {
        return Err(PipelineError::Unsupported(
            "fracture source box needs positive half-extents".to_string(),
        ));
    }

    // Seed the sites uniformly in the box (deterministic).
    let mut rng = SplitMix64::new(cfg.seed);
    let sites: Vec<V3> = (0..cfg.parts)
        .map(|_| {
            [
                rng.next_signed_unit() * half[0],
                rng.next_signed_unit() * half[1],
                rng.next_signed_unit() * half[2],
            ]
        })
        .collect();

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
        let Some(cell) = build_cell(si, &sites, half) else {
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
            return Err(PipelineError::Unsupported(format!(
                "a Voronoi cell face has {} vertices (>16, the hull face cap); use fewer parts or another seed",
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
            return Err(PipelineError::Unsupported(format!(
                "Voronoi cell for site {si} is not a closed polyhedron ({defect}); \
                 use a different seed or fewer parts"
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
        return Err(PipelineError::Unsupported(
            "fracture produced no parts (all sites degenerate)".to_string(),
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
    fn a_cell_that_is_not_closed_is_an_error_rather_than_a_file() {
        // The regression: an 8x3x0.3 wall at 28 parts, seed 1302, produced a cell whose faces
        // disagreed about a shared edge. The cooked `.rdest` decoded cleanly and `register_hull`
        // rejected it at load, one process boundary from the cause. The cooker must refuse instead.
        //
        // This asserts the GUARANTEE (a bad cell never becomes a file), not the absence of bad
        // cells — the thresholding defect in `build_cell` that produces them is still there.
        let cfg = FractureConfig::wall([4.0, 1.5, 0.15], 28, 1302);
        match fracture_box(&cfg) {
            Err(PipelineError::Unsupported(msg)) => {
                assert!(
                    msg.contains("not a closed polyhedron"),
                    "expected the manifold check to fire, got: {msg}"
                );
            }
            Err(e) => panic!("expected a closed-polyhedron rejection, got {e:?}"),
            Ok(d) => panic!(
                "expected a rejection; cooked {} parts instead",
                d.parts.len()
            ),
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
        // Seed 3, not seed 2: at this configuration seed 2 produces two cells whose faces
        // disagree about a shared edge, and the closed-manifold check now refuses to cook them.
        // That defect is not new — this test has been cooking those broken parts since M8.1 and
        // never noticed, because it only compares BYTES and never registers a hull. See
        // `a_cell_that_is_not_closed_is_an_error_rather_than_a_file`.
        let a = fracture_box(&FractureConfig::wall([1.0, 0.75, 0.15], 16, 1)).unwrap();
        let b = fracture_box(&FractureConfig::wall([1.0, 0.75, 0.15], 16, 3)).unwrap();
        // Same conserved volume, different vertex data.
        assert!((a.total_volume() - b.total_volume()).abs() < 1.0e-3);
        assert_ne!(a.cook().0, b.cook().0, "different seeds must differ");
    }
}
