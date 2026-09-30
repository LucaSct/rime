// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Proofs for the replacement-DAG cook. Every check here reads the COOKED `Asset` — page bytes,
//! cluster and group tables — never the builder's internals, so a proof cannot pass on data the
//! file does not actually carry.

use super::*;
use crate::mesh::{Submesh, Vertex};
use std::collections::BTreeMap;
use std::path::Path;

// ------------------------------------------------------------------------------------------------
// Test meshes.

/// A UV sphere. `weld_seam` shares the longitude seam's vertices (a closed, seam-free surface);
/// otherwise the seam is duplicated with u = 0 / u = 1 like an exported mesh, which the cook must
/// lock. `split_materials` gives the southern hemisphere material 1.
fn uv_sphere(segments: u32, rings: u32, weld_seam: bool, split_materials: bool) -> Mesh {
    let columns = if weld_seam { segments } else { segments + 1 };
    let mut vertices = Vec::new();
    let pole = |y: f32, v: f32| Vertex {
        position: [0.0, y, 0.0],
        normal: [0.0, y.signum(), 0.0],
        uv: [0.5, v],
    };
    vertices.push(pole(1.0, 0.0));
    for r in 1..rings {
        let theta = std::f32::consts::PI * r as f32 / rings as f32;
        for s in 0..columns {
            let phi = 2.0 * std::f32::consts::PI * s as f32 / segments as f32;
            let p = [
                theta.sin() * phi.cos(),
                theta.cos(),
                theta.sin() * phi.sin(),
            ];
            vertices.push(Vertex {
                position: p,
                normal: p,
                uv: [s as f32 / segments as f32, r as f32 / rings as f32],
            });
        }
    }
    vertices.push(pole(-1.0, 1.0));
    let south = vertices.len() as u32 - 1;
    let ring = |r: u32, s: u32| 1 + (r - 1) * columns + (s % columns);
    let mut north_tris = Vec::new();
    let mut south_tris = Vec::new();
    for s in 0..segments {
        let s1 = if weld_seam { (s + 1) % segments } else { s + 1 };
        north_tris.extend([0, ring(1, s1), ring(1, s)]);
        for r in 1..rings - 1 {
            let quad = [
                ring(r, s),
                ring(r, s1),
                ring(r + 1, s1),
                ring(r, s),
                ring(r + 1, s1),
                ring(r + 1, s),
            ];
            if r < rings / 2 {
                north_tris.extend(quad);
            } else {
                south_tris.extend(quad);
            }
        }
        south_tris.extend([south, ring(rings - 1, s), ring(rings - 1, s1)]);
    }
    let mut indices = north_tris;
    let north_count = indices.len() as u32;
    indices.extend(south_tris);
    let submeshes = if split_materials {
        vec![
            Submesh {
                first_index: 0,
                index_count: north_count,
                material_slot: 0,
            },
            Submesh {
                first_index: north_count,
                index_count: indices.len() as u32 - north_count,
                material_slot: 1,
            },
        ]
    } else {
        vec![Submesh {
            first_index: 0,
            index_count: indices.len() as u32,
            material_slot: 0,
        }]
    };
    Mesh {
        vertices,
        indices,
        submeshes,
        ..Default::default()
    }
}

/// A flat, open grid in the XZ plane with a gentle bump, so errors are non-zero but small.
fn bumpy_grid(n: u32) -> Mesh {
    let mut vertices = Vec::new();
    for z in 0..=n {
        for x in 0..=n {
            let fx = x as f32 / n as f32;
            let fz = z as f32 / n as f32;
            let y = 0.1 * (fx * 6.0).sin() * (fz * 4.0).cos();
            vertices.push(Vertex {
                position: [fx, y, fz],
                normal: [0.0, 1.0, 0.0],
                uv: [fx, fz],
            });
        }
    }
    let mut indices = Vec::new();
    for z in 0..n {
        for x in 0..n {
            let i = z * (n + 1) + x;
            indices.extend([i, i + n + 1, i + 1, i + 1, i + n + 1, i + n + 2]);
        }
    }
    let count = indices.len() as u32;
    Mesh {
        vertices,
        indices,
        submeshes: vec![Submesh {
            first_index: 0,
            index_count: count,
            material_slot: 3,
        }],
        ..Default::default()
    }
}

fn cook(mesh: &Mesh) -> DagCook {
    Asset::from_mesh_dag(0xda6, mesh).expect("DAG cook")
}

// ------------------------------------------------------------------------------------------------
// Readers over the cooked asset.

type P = [u32; 3]; // a position by its exact f32 bits

fn read_u32(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap())
}

/// The triangles of one cooked cluster, as exact position bit patterns, decoded from page bytes.
fn cluster_triangles(asset: &Asset, cluster: usize) -> Vec<[P; 3]> {
    let c = &asset.clusters[cluster];
    let page = &asset.pages[c.page as usize];
    let bytes = &asset.page_bytes[page.byte_offset as usize..][..page.byte_size as usize];
    let stride = asset.vertex_stride as usize;
    let vertex_end = (page.first_cluster..page.first_cluster + page.cluster_count)
        .map(|k| {
            let k = &asset.clusters[k as usize];
            k.vertex_offset as usize + k.vertex_count as usize * stride
        })
        .max()
        .unwrap();
    let position = |local: u32| -> P {
        assert!(
            local < c.vertex_count,
            "index outside the cluster's vertices"
        );
        let at = c.vertex_offset as usize + local as usize * stride;
        [
            read_u32(bytes, at),
            read_u32(bytes, at + 4),
            read_u32(bytes, at + 8),
        ]
    };
    (0..c.index_count as usize / 3)
        .map(|t| {
            let at =
                |k: usize| read_u32(bytes, vertex_end + 4 * (c.first_index as usize + 3 * t + k));
            [position(at(0)), position(at(1)), position(at(2))]
        })
        .collect()
}

fn group_clusters(asset: &Asset, g: usize) -> std::ops::Range<usize> {
    let group = &asset.groups[g];
    group.first_cluster as usize..(group.first_cluster + group.cluster_count) as usize
}

fn children(asset: &Asset, g: usize) -> &[u32] {
    let group = &asset.groups[g];
    &asset.child_groups[group.first_child as usize..][..group.child_count as usize]
}

/// Undirected edges used by exactly one triangle of `tris` (the region's boundary), sorted.
fn boundary_edges(tris: &[[P; 3]]) -> Vec<(P, P)> {
    let mut count: BTreeMap<(P, P), u32> = BTreeMap::new();
    for t in tris {
        for k in 0..3 {
            let (a, b) = (t[k], t[(k + 1) % 3]);
            *count
                .entry(if a < b { (a, b) } else { (b, a) })
                .or_insert(0) += 1;
        }
    }
    count
        .into_iter()
        .filter(|&(_, n)| n == 1)
        .map(|(e, _)| e)
        .collect()
}

/// Structural checks mirroring the C++ `validate_virtual_geometry()`, plus the single-parent
/// shape the tree-walking selectors need. Returns each group's parent.
fn check_structure(asset: &Asset) -> Vec<Option<usize>> {
    assert_eq!(asset.coarse_group, 0);
    assert!(asset.groups[0].permanently_resident);
    assert!(asset.pages[0].permanently_resident);
    let mut covered = 0;
    for (p, page) in asset.pages.iter().enumerate() {
        assert_eq!(page.first_cluster, covered, "pages tile the cluster table");
        covered += page.cluster_count;
        for k in page.first_cluster..page.first_cluster + page.cluster_count {
            assert_eq!(asset.clusters[k as usize].page, p as u32);
        }
        for d in 0..page.dependency_count {
            let dep = asset.page_dependencies[(page.first_dependency + d) as usize];
            assert!(
                dep < p as u32,
                "dependencies point coarser (acyclic by construction)"
            );
        }
        assert_eq!(page.permanently_resident, p == 0);
    }
    assert_eq!(covered as usize, asset.clusters.len());
    let mut parent = vec![None; asset.groups.len()];
    for g in 0..asset.groups.len() {
        for &c in group_clusters(asset, g).collect::<Vec<_>>().iter() {
            assert_eq!(asset.clusters[c].replacement_group, g as u32);
            assert!(asset.clusters[c].index_count / 3 <= MAX_TRIANGLES_PER_CLUSTER as u32);
            assert_eq!(asset.clusters[c].lod_error_m, asset.groups[g].lod_error_m);
        }
        for &child in children(asset, g) {
            assert!(child as usize > g, "children are numbered after parents");
            assert!(
                parent[child as usize].replace(g).is_none(),
                "group {child} has two parents — a tree walk would select it twice"
            );
        }
    }
    for (g, p) in parent.iter().enumerate() {
        assert_eq!(p.is_none(), g == 0, "every group but the root has a parent");
    }
    parent
}

/// MONOTONICITY: across every replacement edge, parent error ≥ child error and the parent's LOD
/// sphere contains the child's (same double-precision formula and guard as the C++ validator).
fn monotone_violations(asset: &Asset) -> Vec<String> {
    let mut out = Vec::new();
    for g in 0..asset.groups.len() {
        let parent = &asset.groups[g];
        for &c in children(asset, g) {
            let child = &asset.groups[c as usize];
            if child.lod_error_m > parent.lod_error_m {
                out.push(format!(
                    "error {g}->{c}: {} < {}",
                    parent.lod_error_m, child.lod_error_m
                ));
            }
            let reach =
                distance_f64(parent.lod_center, child.lod_center) + f64::from(child.lod_radius);
            if reach > f64::from(parent.lod_radius) * (1.0 + 1.0 / 1_048_576.0) {
                out.push(format!("sphere {g}->{c}: {reach} > {}", parent.lod_radius));
            }
        }
    }
    out
}

/// A replica of `select_virtual_geometry()` (engine/render) for fast sweeps in Rust: refine when
/// `error > threshold` and every child's pages (and their dependencies) are resident. The C++ test
/// runs the real one on the committed fixture.
fn select(asset: &Asset, threshold: f32, resident: &[bool]) -> Vec<usize> {
    let page_ready = |p: usize| {
        let mut stack = vec![p];
        while let Some(p) = stack.pop() {
            if !(asset.pages[p].permanently_resident || resident[p]) {
                return false;
            }
            let page = &asset.pages[p];
            stack.extend(
                asset.page_dependencies[page.first_dependency as usize..]
                    [..page.dependency_count as usize]
                    .iter()
                    .map(|&d| d as usize),
            );
        }
        true
    };
    let group_ready =
        |g: usize| group_clusters(asset, g).all(|c| page_ready(asset.clusters[c].page as usize));
    let mut selected = Vec::new();
    let mut pending = vec![0usize];
    while let Some(g) = pending.pop() {
        let kids = children(asset, g);
        let refine = asset.groups[g].lod_error_m > threshold
            && !kids.is_empty()
            && kids.iter().all(|&c| group_ready(c as usize));
        if refine {
            pending.extend(kids.iter().rev().map(|&c| c as usize));
        } else {
            selected.push(g);
        }
    }
    selected
}

/// A cut is valid when no group is selected twice, every leaf group has exactly one selected
/// ancestor-or-self, and — for a closed input — the selected triangles are watertight: every edge
/// (by exact position bits) is used by exactly two triangles.
fn check_cut(asset: &Asset, parent: &[Option<usize>], selected: &[usize], closed: bool) {
    let mut chosen = vec![false; asset.groups.len()];
    for &g in selected {
        assert!(!chosen[g], "group {g} selected twice");
        chosen[g] = true;
    }
    for g in 0..asset.groups.len() {
        if !children(asset, g).is_empty() {
            continue;
        }
        let mut hits = 0;
        let mut at = Some(g);
        while let Some(a) = at {
            hits += usize::from(chosen[a]);
            at = parent[a];
        }
        assert_eq!(hits, 1, "leaf group {g} is covered {hits} times");
    }
    if closed {
        let tris: Vec<[P; 3]> = selected
            .iter()
            .flat_map(|&g| group_clusters(asset, g))
            .flat_map(|c| cluster_triangles(asset, c))
            .collect();
        assert!(
            boundary_edges(&tris).is_empty(),
            "cut has {} open edges",
            boundary_edges(&tris).len()
        );
        let mut count: BTreeMap<(P, P), u32> = BTreeMap::new();
        for t in &tris {
            for k in 0..3 {
                let (a, b) = (t[k], t[(k + 1) % 3]);
                *count
                    .entry(if a < b { (a, b) } else { (b, a) })
                    .or_insert(0) += 1;
            }
        }
        assert!(
            count.values().all(|&n| n == 2),
            "cut has a non-manifold (overlapping) edge"
        );
    }
}

/// A deterministic pseudo-random page residency mask (permanent pages are resident regardless).
fn residency(asset: &Asset, seed: u64, keep_percent: u64) -> Vec<bool> {
    let mut state = seed.wrapping_mul(6_364_136_223_846_793_005).wrapping_add(1);
    (0..asset.pages.len())
        .map(|_| {
            state = state
                .wrapping_mul(6_364_136_223_846_793_005)
                .wrapping_add(1_442_695_040_888_963_407);
            (state >> 33) % 100 < keep_percent
        })
        .collect()
}

fn thresholds(asset: &Asset) -> Vec<f32> {
    let top = asset.groups[0].lod_error_m.max(1e-6);
    let mut t = vec![0.0, top * 2.0];
    for k in 1..24 {
        t.push(top * (k as f32 / 24.0).powi(3));
    }
    t
}

// ------------------------------------------------------------------------------------------------
// The proofs.

#[test]
fn dag_cook_is_byte_deterministic() {
    for mesh in [
        uv_sphere(64, 32, true, false),
        uv_sphere(48, 24, false, true),
        bumpy_grid(40),
    ] {
        let first = cook(&mesh);
        let second = cook(&mesh);
        assert_eq!(first.asset.cook().0, second.asset.cook().0);
        assert_eq!(first.stats, second.stats);
        // And through a round-trip of the mesh value itself (fresh allocations, same bytes).
        let third = cook(&mesh.clone());
        assert_eq!(first.asset.encode_payload(), third.asset.encode_payload());
    }
}

#[test]
fn dag_has_many_levels_and_roughly_halves_triangles_per_level() {
    let mesh = uv_sphere(128, 64, true, false);
    let dag = cook(&mesh);
    let levels = &dag.stats.levels;
    eprintln!("{:#?}", dag.stats);
    assert!(
        levels.len() >= 5,
        "expected a deep DAG, got {} levels",
        levels.len()
    );
    assert_eq!(levels[0].triangles as usize, mesh.indices.len() / 3);
    // Every level halves-ish; locked group boundaries make the achieved ratio a little worse than
    // the 50% target, and the last few levels (a handful of clusters, mostly boundary) worse
    // still, so the upper bound is looser there.
    for (k, pair) in levels.windows(2).enumerate() {
        let ratio = pair[1].triangles as f64 / pair[0].triangles as f64;
        let upper = if pair[0].clusters >= 16 { 0.62 } else { 0.8 };
        assert!(
            (0.4..=upper).contains(&ratio),
            "level {k}->{}: {} -> {} triangles (ratio {ratio:.3})",
            k + 1,
            pair[0].triangles,
            pair[1].triangles
        );
        assert!(pair[1].max_error_m >= pair[0].max_error_m);
    }
    assert_eq!(dag.stats.stop, DagStop::SingleCluster);
    assert_eq!(dag.asset.groups[0].cluster_count, 1);
    // It is a DAG, not a tree: regrouping spreads one swap unit's outputs over several next-level
    // groups, which shows up as a page depending on more than one coarser page. (A tree would keep
    // every level's seams locked up to the root.)
    let shared = dag
        .asset
        .pages
        .iter()
        .filter(|p| p.dependency_count > 1)
        .count();
    assert!(
        shared * 4 >= dag.asset.pages.len(),
        "only {shared} of {} units feed >1 group",
        dag.asset.pages.len()
    );
    assert_eq!(dag.stats.no_progress_levels_discarded, 0);
}

#[test]
fn every_replacement_edge_is_monotone() {
    for mesh in [
        uv_sphere(128, 64, true, false),
        uv_sphere(48, 24, false, true),
        bumpy_grid(40),
    ] {
        let dag = cook(&mesh);
        check_structure(&dag.asset);
        let violations = monotone_violations(&dag.asset);
        assert!(violations.is_empty(), "{violations:#?}");
        // The root's error is positive for curved input: the proof is not vacuous.
        assert!(dag.asset.groups[0].lod_error_m > 0.0);
    }
}

/// LOCKED BOUNDARIES: for every swap unit, the boundary of the region its fine clusters cover is
/// bit-identical to the boundary of its coarse clusters. Units are recovered from the cooked file
/// alone: a parent's children all live in one page, so "the page my children are in" names the
/// unit; the root (which spans every top-level unit) merges those pages into one.
#[test]
fn group_boundaries_are_bit_identical_across_every_level() {
    for mesh in [
        uv_sphere(128, 64, true, false),
        uv_sphere(48, 24, false, true),
        bumpy_grid(40),
    ] {
        let dag = cook(&mesh);
        let asset = &dag.asset;
        // Union pages that one parent's children span.
        let mut root_of: Vec<usize> = (0..asset.pages.len()).collect();
        fn find(root_of: &mut [usize], mut p: usize) -> usize {
            while root_of[p] != p {
                p = root_of[p];
            }
            p
        }
        let mut coarse_of_page: BTreeMap<usize, Vec<usize>> = BTreeMap::new();
        for g in 0..asset.groups.len() {
            let kids = children(asset, g);
            if kids.is_empty() {
                continue;
            }
            let pages: Vec<usize> = kids
                .iter()
                .map(|&c| {
                    asset.clusters[asset.groups[c as usize].first_cluster as usize].page as usize
                })
                .collect();
            let first = find(&mut root_of, pages[0]);
            for &p in &pages[1..] {
                let r = find(&mut root_of, p);
                root_of[r] = first;
            }
            coarse_of_page.entry(pages[0]).or_default().push(g);
        }
        let mut units: BTreeMap<usize, (Vec<usize>, Vec<usize>)> = BTreeMap::new();
        for (page, groups) in coarse_of_page {
            let unit = units.entry(find(&mut root_of, page)).or_default();
            unit.1.extend(groups);
        }
        for p in 0..asset.pages.len() {
            let r = find(&mut root_of, p);
            if let Some(unit) = units.get_mut(&r) {
                unit.0.push(p);
            }
        }
        let mut checked = 0;
        for (fine_pages, coarse_groups) in units.values() {
            let fine: Vec<[P; 3]> = fine_pages
                .iter()
                .flat_map(|&p| {
                    let page = &asset.pages[p];
                    page.first_cluster as usize..(page.first_cluster + page.cluster_count) as usize
                })
                .flat_map(|c| cluster_triangles(asset, c))
                .collect();
            let mut coarse_groups = coarse_groups.clone();
            coarse_groups.sort_unstable();
            coarse_groups.dedup();
            let coarse: Vec<[P; 3]> = coarse_groups
                .iter()
                .flat_map(|&g| group_clusters(asset, g))
                .flat_map(|c| cluster_triangles(asset, c))
                .collect();
            assert!(coarse.len() < fine.len(), "a swap unit must simplify");
            assert_eq!(
                boundary_edges(&fine),
                boundary_edges(&coarse),
                "swap unit over pages {fine_pages:?} moved its boundary"
            );
            checked += 1;
        }
        eprintln!(
            "boundary proof: {checked} swap units, {} pages",
            asset.pages.len()
        );
        assert!(checked >= 4, "only {checked} swap units checked");
    }
}

/// Every cut the (replica) selector can produce, over a sweep of thresholds and residency masks,
/// is a valid replacement cut — and, on the closed sphere, watertight.
#[test]
fn every_selected_cut_is_complete_unique_and_watertight() {
    for (mesh, closed) in [
        (uv_sphere(128, 64, true, false), true),
        (uv_sphere(48, 24, false, true), false), // seam duplicates positions: not closed by bits
        (bumpy_grid(40), false),
    ] {
        let dag = cook(&mesh);
        let parent = check_structure(&dag.asset);
        let all = vec![true; dag.asset.pages.len()];
        let mut distinct = BTreeSet::new();
        for t in thresholds(&dag.asset) {
            let cut = select(&dag.asset, t, &all);
            check_cut(&dag.asset, &parent, &cut, closed);
            distinct.insert(cut);
            for seed in 0..4 {
                let resident = residency(&dag.asset, seed, 70);
                check_cut(
                    &dag.asset,
                    &parent,
                    &select(&dag.asset, t, &resident),
                    closed,
                );
            }
        }
        assert!(
            distinct.len() >= 5,
            "sweep only reached {} cuts",
            distinct.len()
        );
    }
}

#[test]
fn material_ranges_survive_every_level() {
    let dag = cook(&uv_sphere(48, 24, false, true));
    let materials: BTreeSet<u32> = dag.asset.clusters.iter().map(|c| c.material_slot).collect();
    assert_eq!(materials, BTreeSet::from([0, 1]));
    // The root keeps both materials: a coarse cut never paints one material over another.
    let root: BTreeSet<u32> = group_clusters(&dag.asset, 0)
        .map(|c| dag.asset.clusters[c].material_slot)
        .collect();
    assert_eq!(root, BTreeSet::from([0, 1]));
    // Attribute seams and material boundaries are locked, and counted.
    assert!(dag.stats.locked_vertices > 0);
}

#[test]
fn tiny_and_degenerate_inputs_cook_to_a_valid_single_level() {
    // One cluster's worth: no simplification is needed; the root is the leaf.
    let dag = cook(&uv_sphere(8, 4, true, false));
    assert_eq!(dag.stats.stop, DagStop::SingleCluster);
    assert_eq!(dag.asset.groups.len(), 1);
    assert_eq!(dag.asset.groups[0].child_count, 0);
    check_structure(&dag.asset);

    // Many disconnected triangles cannot simplify (every vertex is a border): the cook must stop
    // on "no progress", say so, and still emit a valid (single-level) asset.
    let mut mesh = Mesh::default();
    for i in 0..600u32 {
        let x = i as f32 * 2.0;
        let base = mesh.vertices.len() as u32;
        for p in [[x, 0.0, 0.0], [x + 1.0, 0.0, 0.0], [x, 1.0, 0.0]] {
            mesh.vertices.push(Vertex {
                position: p,
                normal: [0.0, 0.0, 1.0],
                uv: [0.0, 0.0],
            });
        }
        mesh.indices.extend([base, base + 1, base + 2]);
    }
    mesh.submeshes.push(Submesh {
        first_index: 0,
        index_count: mesh.indices.len() as u32,
        material_slot: 0,
    });
    let dag = cook(&mesh);
    assert_eq!(dag.stats.stop, DagStop::NoProgress);
    assert_eq!(dag.stats.no_progress_levels_discarded, 1);
    assert_eq!(dag.stats.levels.len(), 1);
    check_structure(&dag.asset);
    assert_eq!(
        dag.asset.groups[0].cluster_count as usize,
        dag.asset.clusters.len()
    );
}

/// The committed fixture the C++ test (`tests/render/virtual_geometry_dag_cut_test.cpp`) runs the
/// real `select_virtual_geometry()` on. This is the cross-language drift alarm: if the cook's
/// output changes, this fails until the fixture is regenerated deliberately with
/// `RIME_REGENERATE_FIXTURES=1 cargo test -p asset-pipeline dag_fixture`.
pub(crate) fn fixture_mesh() -> Mesh {
    uv_sphere(48, 24, true, true)
}

#[test]
fn dag_fixture_matches_the_committed_bytes() {
    let path =
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tests/assets/fixtures/vg_dag_sphere.rvg");
    let cooked = cook(&fixture_mesh()).asset.cook().0;
    if std::env::var_os("RIME_REGENERATE_FIXTURES").is_some() {
        std::fs::write(&path, &cooked).unwrap();
    }
    let committed = std::fs::read(&path).expect("committed DAG fixture");
    assert_eq!(
        cooked, committed,
        "DAG cook output diverged from the committed fixture — regenerate it deliberately"
    );
}
