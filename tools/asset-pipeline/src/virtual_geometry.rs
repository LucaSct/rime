// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The M18 virtualized-geometry companion payload writer. This module owns the stable, versioned
//! RMA1 byte encoding and the single-page leaf cook; the replacement-DAG cook (clustering,
//! simplification, grouping and page packing) is `virtual_geometry_dag`. The field order here
//! mirrors `decode_virtual_geometry` in the C++ reader exactly.

use crate::cooked::{
    wrap_container, ByteWriter, ASSET_KIND_VIRTUAL_GEOMETRY, VIRTUAL_GEOMETRY_SCHEMA_HASH,
};
use crate::mesh::{
    Mesh, ATTR_JOINTS, ATTR_NORMAL, ATTR_POSITION, ATTR_TANGENT, ATTR_UV, ATTR_WEIGHTS, SKIN_BYTES,
    STRIDE_NO_TANGENT, TANGENT_BYTES,
};

/// Version of the kind-specific virtual-geometry payload (independent of the RMA1 envelope).
///
/// v2 (M18.6, ADR-0059) appends a per-group LOD bounding sphere after v1's group record, so a
/// view-dependent selector can bound projected error per group. The C++ reader still accepts v1
/// (spheres read as zero); this writer only emits v2.
pub const PAYLOAD_VERSION: u32 = 2;

/// Conservative leaf-cluster triangle cap for the first M18 cook stage. The partitioner only
/// cuts between complete source triangles, so an indexed submesh's material range is preserved.
pub const MAX_TRIANGLES_PER_CLUSTER: usize = 128;

/// One page in the cooked companion payload.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Page {
    pub byte_offset: u64,
    pub byte_size: u32,
    pub first_cluster: u32,
    pub cluster_count: u32,
    pub first_dependency: u32,
    pub dependency_count: u32,
    pub permanently_resident: bool,
}

/// One cluster record. Bounds are `[min, max]` in source-mesh local space.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Cluster {
    pub bounds_min: [f32; 3],
    pub bounds_max: [f32; 3],
    pub lod_error_m: f32,
    pub page: u32,
    pub vertex_offset: u32,
    pub vertex_count: u32,
    pub first_index: u32,
    pub index_count: u32,
    pub material_slot: u32,
    pub replacement_group: u32,
}

/// One replacement group and its child-group adjacency slice.
///
/// `lod_center`/`lod_radius` (payload v2) is the group's LOD bounding sphere in local space: it
/// encloses every source triangle this representation stands for, and — the DAG cook guarantees
/// it — every child group's LOD sphere. Together with a monotone `lod_error_m` that is what lets a
/// view-dependent selector compute a projected error that can only grow toward the root.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Group {
    pub first_cluster: u32,
    pub cluster_count: u32,
    pub first_child: u32,
    pub child_count: u32,
    pub lod_error_m: f32,
    pub permanently_resident: bool,
    pub lod_center: [f32; 3],
    pub lod_radius: f32,
}

/// CPU-side virtual-geometry payload data ready to wrap in an RMA1 container.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Asset {
    pub source_mesh: u64,
    pub attribs: u32,
    pub vertex_stride: u32,
    pub pages: Vec<Page>,
    pub clusters: Vec<Cluster>,
    pub groups: Vec<Group>,
    pub child_groups: Vec<u32>,
    pub page_dependencies: Vec<u32>,
    pub page_bytes: Vec<u8>,
    pub coarse_group: u32,
}

/// A failure while making the first, rigid leaf companion from a cooked mesh.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LeafCookError {
    EmptyMesh,
    MissingSubmeshes,
    MalformedOptionalArray,
    InvalidIndex,
    InvalidSubmesh,
    NonFiniteVertex,
    OversizedPayload,
    /// The DAG cook broke one of its own structural invariants (a simplification produced more
    /// clusters than it consumed, so a parent would be left without a child to replace it). This
    /// is a cooker bug, reported rather than written as an asset whose cuts would overlap.
    DagInvariant,
}

/// The page-byte contract for the M18 leaf cook is deliberately simple and stable: the page starts
/// with the mesh's interleaved vertex records in the exact attribute/stride order used by `Mesh::cook`,
/// followed immediately by little-endian u32 indices. `Cluster::vertex_offset` and
/// `Cluster::first_index` are respectively byte and index offsets within this page. There is no
/// padding or private header in the page; the companion header carries the attribute flags and
/// vertex stride. This makes the first resident leaf consumable by a future renderer without
/// teaching it a second mesh format.
impl Asset {
    /// Build one permanently resident page and replacement group from an already-cooked mesh.
    /// Each source submesh is split into deterministic contiguous triangle ranges no larger than
    /// [`MAX_TRIANGLES_PER_CLUSTER`]. Splitting never crosses a source submesh, so material slots
    /// remain intact. The helper intentionally does not merge primitives or invent a material when
    /// the source record is malformed.
    pub fn from_mesh(source_mesh: u64, mesh: &Mesh) -> Result<Self, LeafCookError> {
        validate_source_mesh(mesh)?;
        let (attribs, vertex_stride) = vertex_layout(mesh);

        let vertex_bytes = u64::from(vertex_stride) * mesh.vertices.len() as u64;
        let index_bytes = 4u64 * mesh.indices.len() as u64;
        let page_size = vertex_bytes + index_bytes;
        if page_size > u64::from(u32::MAX) {
            return Err(LeafCookError::OversizedPayload);
        }

        let mut page_bytes = ByteWriter::new();
        for i in 0..mesh.vertices.len() {
            write_vertex(&mut page_bytes, mesh, i)?;
        }
        for &index in &mesh.indices {
            page_bytes.u32(index);
        }

        let vertex_count =
            u32::try_from(mesh.vertices.len()).map_err(|_| LeafCookError::OversizedPayload)?;
        let mut clusters = Vec::new();
        for submesh in &mesh.submeshes {
            let triangle_count = submesh.index_count as usize / 3;
            for triangle_start in (0..triangle_count).step_by(MAX_TRIANGLES_PER_CLUSTER) {
                let cluster_triangle_count =
                    (triangle_count - triangle_start).min(MAX_TRIANGLES_PER_CLUSTER);
                let first_index = submesh.first_index as usize + triangle_start * 3;
                let index_count = cluster_triangle_count * 3;
                let mut bounds_min = [f32::INFINITY; 3];
                let mut bounds_max = [f32::NEG_INFINITY; 3];
                for &index in &mesh.indices[first_index..first_index + index_count] {
                    let position = mesh.vertices[index as usize].position;
                    for axis in 0..3 {
                        bounds_min[axis] = bounds_min[axis].min(position[axis]);
                        bounds_max[axis] = bounds_max[axis].max(position[axis]);
                    }
                }
                clusters.push(Cluster {
                    bounds_min,
                    bounds_max,
                    lod_error_m: 0.0,
                    page: 0,
                    vertex_offset: 0,
                    vertex_count,
                    first_index: first_index as u32,
                    index_count: index_count as u32,
                    material_slot: submesh.material_slot,
                    replacement_group: 0,
                });
            }
        }
        let cluster_count_u32 = clusters.len() as u32;
        let (lod_center, lod_radius) = bounding_sphere_of_points(
            mesh.indices
                .iter()
                .map(|&index| mesh.vertices[index as usize].position),
        );
        Ok(Self {
            source_mesh,
            attribs,
            vertex_stride,
            pages: vec![Page {
                byte_offset: 0,
                byte_size: page_size as u32,
                first_cluster: 0,
                cluster_count: clusters.len() as u32,
                first_dependency: 0,
                dependency_count: 0,
                permanently_resident: true,
            }],
            clusters,
            groups: vec![Group {
                first_cluster: 0,
                // Every cluster, not every submesh: a submesh longer than one cluster splits, and
                // a group that stopped short would leave clusters naming a group that does not
                // contain them (the C++ validator's InvalidCluster).
                cluster_count: cluster_count_u32,
                first_child: 0,
                child_count: 0,
                lod_error_m: 0.0,
                permanently_resident: true,
                lod_center,
                lod_radius,
            }],
            child_groups: Vec::new(),
            page_dependencies: Vec::new(),
            page_bytes: page_bytes.into_vec(),
            coarse_group: 0,
        })
    }
}

/// Validate the parts of a cooked mesh both the leaf and the DAG cooks rely on: non-empty, in-range
/// indices, well-formed submeshes, finite attributes and parallel optional arrays.
pub(crate) fn validate_source_mesh(mesh: &Mesh) -> Result<(), LeafCookError> {
    if mesh.vertices.is_empty() || mesh.indices.is_empty() {
        return Err(LeafCookError::EmptyMesh);
    }
    if mesh.submeshes.is_empty() {
        return Err(LeafCookError::MissingSubmeshes);
    }
    if mesh
        .tangents
        .as_ref()
        .is_some_and(|v| v.len() != mesh.vertices.len())
        || mesh.skin.as_ref().is_some_and(|s| {
            s.joints.len() != mesh.vertices.len() || s.weights.len() != mesh.vertices.len()
        })
    {
        return Err(LeafCookError::MalformedOptionalArray);
    }
    for vertex in &mesh.vertices {
        if vertex
            .position
            .into_iter()
            .chain(vertex.normal)
            .chain(vertex.uv)
            .any(|v| !v.is_finite())
        {
            return Err(LeafCookError::NonFiniteVertex);
        }
    }
    for &index in &mesh.indices {
        if index as usize >= mesh.vertices.len() {
            return Err(LeafCookError::InvalidIndex);
        }
    }
    for submesh in &mesh.submeshes {
        if submesh.index_count == 0
            || submesh.index_count % 3 != 0
            || u64::from(submesh.first_index) + u64::from(submesh.index_count)
                > mesh.indices.len() as u64
        {
            return Err(LeafCookError::InvalidSubmesh);
        }
    }
    Ok(())
}

/// The attribute flags and interleaved stride a page vertex uses — the same order `Mesh::cook`
/// writes, so a page vertex decodes with the source mesh's own layout.
pub(crate) fn vertex_layout(mesh: &Mesh) -> (u32, u32) {
    let mut attribs = ATTR_POSITION | ATTR_NORMAL | ATTR_UV;
    let mut vertex_stride = STRIDE_NO_TANGENT;
    if mesh.tangents.is_some() {
        attribs |= ATTR_TANGENT;
        vertex_stride += TANGENT_BYTES;
    }
    if mesh.skin.is_some() {
        attribs |= ATTR_JOINTS | ATTR_WEIGHTS;
        vertex_stride += SKIN_BYTES;
    }
    (attribs, vertex_stride)
}

/// Append source vertex `i` in the page vertex layout.
pub(crate) fn write_vertex(
    out: &mut ByteWriter,
    mesh: &Mesh,
    i: usize,
) -> Result<(), LeafCookError> {
    let vertex = &mesh.vertices[i];
    for value in vertex.position {
        out.f32(value);
    }
    for value in vertex.normal {
        out.f32(value);
    }
    for value in vertex.uv {
        out.f32(value);
    }
    if let Some(tangents) = &mesh.tangents {
        for value in tangents[i] {
            out.f32(value);
        }
    }
    if let Some(skin) = &mesh.skin {
        for joint in skin.joints[i] {
            out.u16(joint);
        }
        for weight in skin.weights[i] {
            if !weight.is_finite() {
                return Err(LeafCookError::NonFiniteVertex);
            }
            out.f32(weight);
        }
    }
    Ok(())
}

/// A conservative bounding sphere of a point set: the centre is the AABB centre rounded to f32,
/// and the radius is measured in f64 *from that rounded centre* and then rounded up, so the sphere
/// written to disk really contains every point. (Measuring from the unrounded centre and rounding
/// to nearest could leave a point a few ULPs outside — invisible on screen, but it would break the
/// containment the DAG's monotonicity proof checks exactly.)
pub(crate) fn bounding_sphere_of_points(
    points: impl Iterator<Item = [f32; 3]> + Clone,
) -> ([f32; 3], f32) {
    let mut lo = [f64::INFINITY; 3];
    let mut hi = [f64::NEG_INFINITY; 3];
    for p in points.clone() {
        for axis in 0..3 {
            lo[axis] = lo[axis].min(f64::from(p[axis]));
            hi[axis] = hi[axis].max(f64::from(p[axis]));
        }
    }
    if lo[0] > hi[0] {
        return ([0.0; 3], 0.0);
    }
    let center = [
        ((lo[0] + hi[0]) * 0.5) as f32,
        ((lo[1] + hi[1]) * 0.5) as f32,
        ((lo[2] + hi[2]) * 0.5) as f32,
    ];
    let mut radius = 0.0f64;
    for p in points {
        radius = radius.max(distance_f64(center, p));
    }
    (center, round_radius_up(radius))
}

/// Euclidean distance evaluated in f64 from f32 inputs. The C++ validator uses the same formula,
/// so both sides agree on containment to the last bit.
pub(crate) fn distance_f64(a: [f32; 3], b: [f32; 3]) -> f64 {
    let dx = f64::from(a[0]) - f64::from(b[0]);
    let dy = f64::from(a[1]) - f64::from(b[1]);
    let dz = f64::from(a[2]) - f64::from(b[2]);
    (dx * dx + dy * dy + dz * dz).sqrt()
}

/// Round an f64 radius to the smallest f32 that is not below it, plus a relative guard of 2^-20 so
/// a reader evaluating containment with a slightly different operation order still agrees.
pub(crate) fn round_radius_up(radius: f64) -> f32 {
    let guarded = radius * (1.0 + 1.0 / 1_048_576.0);
    let mut r = guarded as f32;
    if f64::from(r) < guarded {
        r = f32::from_bits(r.to_bits() + 1);
    }
    r
}

fn count(value: usize, field: &str) -> u32 {
    u32::try_from(value).unwrap_or_else(|_| panic!("virtual geometry {field} exceeds u32"))
}

impl Asset {
    /// Encode the kind-specific payload, excluding the 24-byte RMA1 envelope.
    pub fn encode_payload(&self) -> Vec<u8> {
        let mut p = ByteWriter::new();
        p.u32(PAYLOAD_VERSION);
        p.u64(self.source_mesh);
        p.u32(self.attribs);
        p.u32(self.vertex_stride);
        p.u32(count(self.pages.len(), "page count"));
        p.u32(count(self.clusters.len(), "cluster count"));
        p.u32(count(self.groups.len(), "group count"));
        p.u32(count(self.child_groups.len(), "child count"));
        p.u32(count(self.page_dependencies.len(), "dependency count"));
        p.u32(count(self.page_bytes.len(), "page byte count"));
        p.u32(self.coarse_group);

        for page in &self.pages {
            p.u64(page.byte_offset);
            p.u32(page.byte_size);
            p.u32(page.first_cluster);
            p.u32(page.cluster_count);
            p.u32(page.first_dependency);
            p.u32(page.dependency_count);
            p.bytes(&[u8::from(page.permanently_resident)]);
        }
        for cluster in &self.clusters {
            for value in cluster.bounds_min {
                p.f32(value);
            }
            for value in cluster.bounds_max {
                p.f32(value);
            }
            p.f32(cluster.lod_error_m);
            p.u32(cluster.page);
            p.u32(cluster.vertex_offset);
            p.u32(cluster.vertex_count);
            p.u32(cluster.first_index);
            p.u32(cluster.index_count);
            p.u32(cluster.material_slot);
            p.u32(cluster.replacement_group);
        }
        for group in &self.groups {
            p.u32(group.first_cluster);
            p.u32(group.cluster_count);
            p.u32(group.first_child);
            p.u32(group.child_count);
            p.f32(group.lod_error_m);
            p.bytes(&[u8::from(group.permanently_resident)]);
            // v2 tail: appended after v1's record so a v1 reader's prefix is unchanged.
            for value in group.lod_center {
                p.f32(value);
            }
            p.f32(group.lod_radius);
        }
        for &child in &self.child_groups {
            p.u32(child);
        }
        for &dependency in &self.page_dependencies {
            p.u32(dependency);
        }
        p.bytes(&self.page_bytes);
        p.into_vec()
    }

    /// Encode the complete virtual-geometry RMA1 companion file and its payload content id.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        let payload = self.encode_payload();
        wrap_container(
            ASSET_KIND_VIRTUAL_GEOMETRY,
            VIRTUAL_GEOMETRY_SCHEMA_HASH,
            &payload,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cooked::fnv1a_64;
    use crate::mesh::{Submesh, Vertex};

    fn fixture() -> Asset {
        Asset {
            source_mesh: 0x1020_3040_5060_7080,
            attribs: 7,
            vertex_stride: 32,
            pages: vec![Page {
                byte_offset: 0,
                byte_size: 3,
                first_cluster: 0,
                cluster_count: 1,
                first_dependency: 0,
                dependency_count: 0,
                permanently_resident: true,
            }],
            clusters: vec![Cluster {
                bounds_min: [-1.0, -2.0, -3.0],
                bounds_max: [1.0, 2.0, 3.0],
                lod_error_m: 0.25,
                page: 0,
                vertex_offset: 8,
                vertex_count: 4,
                first_index: 12,
                index_count: 6,
                material_slot: 2,
                replacement_group: 0,
            }],
            groups: vec![Group {
                first_cluster: 0,
                cluster_count: 1,
                first_child: 0,
                child_count: 0,
                lod_error_m: 0.5,
                permanently_resident: true,
                lod_center: [0.0, 0.5, 1.0],
                lod_radius: 4.0,
            }],
            child_groups: Vec::new(),
            page_dependencies: Vec::new(),
            page_bytes: vec![0xaa, 0xbb, 0xcc],
            coarse_group: 0,
        }
    }

    fn leaf_mesh() -> Mesh {
        Mesh {
            vertices: vec![
                Vertex {
                    position: [0.0, 0.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 0.0],
                },
                Vertex {
                    position: [1.0, 0.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [1.0, 0.0],
                },
                Vertex {
                    position: [0.0, 1.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 1.0],
                },
            ],
            indices: vec![0, 1, 2],
            submeshes: vec![Submesh {
                first_index: 0,
                index_count: 3,
                material_slot: 7,
            }],
            ..Default::default()
        }
    }

    #[test]
    fn from_mesh_is_deterministic_and_preserves_leaf_ranges() {
        let mesh = leaf_mesh();
        let first = Asset::from_mesh(0x55, &mesh).unwrap();
        let second = Asset::from_mesh(0x55, &mesh).unwrap();
        assert_eq!(first, second);
        assert_eq!(first.attribs, 1 | 2 | 4);
        assert_eq!(first.vertex_stride, 32);
        assert_eq!(first.pages[0].byte_size as usize, first.page_bytes.len());
        assert!(first.pages[0].permanently_resident);
        assert_eq!(first.clusters.len(), 1);
        assert_eq!(first.clusters[0].material_slot, 7);
        assert_eq!(first.clusters[0].first_index, 0);
        assert_eq!(first.clusters[0].index_count, 3);
        assert_eq!(first.groups[0].cluster_count, 1);
        assert!(first.groups[0].permanently_resident);
        assert_eq!(
            &first.page_bytes[32 * 3..],
            &[0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0]
        );
    }

    #[test]
    fn from_mesh_keeps_multiple_material_ranges_as_clusters() {
        let mut mesh = leaf_mesh();
        mesh.vertices.extend([
            Vertex {
                position: [0.0, 0.0, 1.0],
                normal: [0.0, 0.0, 1.0],
                uv: [0.0, 0.0],
            },
            Vertex {
                position: [1.0, 0.0, 1.0],
                normal: [0.0, 0.0, 1.0],
                uv: [1.0, 0.0],
            },
            Vertex {
                position: [0.0, 1.0, 1.0],
                normal: [0.0, 0.0, 1.0],
                uv: [0.0, 1.0],
            },
        ]);
        mesh.indices.extend([3, 4, 5]);
        mesh.submeshes.push(Submesh {
            first_index: 3,
            index_count: 3,
            material_slot: 11,
        });
        let asset = Asset::from_mesh(0x55, &mesh).unwrap();
        assert_eq!(asset.clusters.len(), 2);
        assert_eq!(asset.clusters[0].material_slot, 7);
        assert_eq!(asset.clusters[1].material_slot, 11);
        assert_eq!(asset.clusters[1].first_index, 3);
        assert_eq!(asset.pages[0].cluster_count, 2);
    }

    #[test]
    fn from_mesh_partitions_triangles_deterministically_with_local_bounds() {
        let triangle_count = MAX_TRIANGLES_PER_CLUSTER + 1;
        let mut mesh = Mesh::default();
        for triangle in 0..triangle_count {
            let x = triangle as f32;
            let base = mesh.vertices.len() as u32;
            mesh.vertices.extend([
                Vertex {
                    position: [x, -1.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 0.0],
                },
                Vertex {
                    position: [x + 0.5, 2.0, 0.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [1.0, 0.0],
                },
                Vertex {
                    position: [x + 1.0, 0.0, 3.0],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 1.0],
                },
            ]);
            mesh.indices.extend([base, base + 1, base + 2]);
        }
        mesh.submeshes.push(Submesh {
            first_index: 0,
            index_count: mesh.indices.len() as u32,
            material_slot: 9,
        });

        let first = Asset::from_mesh(0x55, &mesh).unwrap();
        let second = Asset::from_mesh(0x55, &mesh).unwrap();
        assert_eq!(first, second);
        assert_eq!(first.clusters.len(), 2);
        // The one group must own both clusters (it once owned `submeshes.len()` = 1, leaving
        // cluster 1 naming a group that did not contain it).
        assert_eq!(first.groups[0].cluster_count, 2);
        assert_eq!(
            first.clusters[0].index_count,
            (MAX_TRIANGLES_PER_CLUSTER * 3) as u32
        );
        assert_eq!(
            first.clusters[1].first_index,
            (MAX_TRIANGLES_PER_CLUSTER * 3) as u32
        );
        assert_eq!(first.clusters[1].index_count, 3);
        assert!(first
            .clusters
            .iter()
            .all(|cluster| cluster.index_count / 3 <= MAX_TRIANGLES_PER_CLUSTER as u32));
        assert_eq!(first.clusters[0].material_slot, 9);
        assert_eq!(first.clusters[1].material_slot, 9);
        assert_eq!(first.clusters[0].bounds_min, [0.0, -1.0, 0.0]);
        assert_eq!(first.clusters[0].bounds_max, [128.0, 2.0, 3.0]);
        assert_eq!(first.clusters[1].bounds_min, [128.0, -1.0, 0.0]);
        assert_eq!(first.clusters[1].bounds_max, [129.0, 2.0, 3.0]);
    }

    #[test]
    fn from_mesh_never_crosses_material_boundaries_when_partitioning() {
        let mut mesh = Mesh::default();
        for triangle in 0..(MAX_TRIANGLES_PER_CLUSTER + 1) {
            let base = mesh.vertices.len() as u32;
            let z = if triangle < MAX_TRIANGLES_PER_CLUSTER {
                0.0
            } else {
                10.0
            };
            mesh.vertices.extend([
                Vertex {
                    position: [0.0, 0.0, z],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 0.0],
                },
                Vertex {
                    position: [1.0, 0.0, z],
                    normal: [0.0, 0.0, 1.0],
                    uv: [1.0, 0.0],
                },
                Vertex {
                    position: [0.0, 1.0, z],
                    normal: [0.0, 0.0, 1.0],
                    uv: [0.0, 1.0],
                },
            ]);
            mesh.indices.extend([base, base + 1, base + 2]);
        }
        let first_submesh_indices = (MAX_TRIANGLES_PER_CLUSTER * 3) as u32;
        mesh.submeshes = vec![
            Submesh {
                first_index: 0,
                index_count: first_submesh_indices,
                material_slot: 4,
            },
            Submesh {
                first_index: first_submesh_indices,
                index_count: 3,
                material_slot: 5,
            },
        ];

        let asset = Asset::from_mesh(0x55, &mesh).unwrap();
        assert_eq!(asset.clusters.len(), 2);
        assert_eq!(asset.clusters[0].material_slot, 4);
        assert_eq!(asset.clusters[0].index_count, first_submesh_indices);
        assert_eq!(asset.clusters[1].material_slot, 5);
        assert_eq!(asset.clusters[1].first_index, first_submesh_indices);
        assert_eq!(asset.clusters[1].bounds_min[2], 10.0);
        assert_eq!(asset.clusters[1].bounds_max[2], 10.0);
    }

    #[test]
    fn from_mesh_rejects_malformed_ranges() {
        let mut mesh = leaf_mesh();
        mesh.indices[2] = 99;
        assert_eq!(Asset::from_mesh(1, &mesh), Err(LeafCookError::InvalidIndex));
        let mut mesh = leaf_mesh();
        mesh.submeshes[0].index_count = 2;
        assert_eq!(
            Asset::from_mesh(1, &mesh),
            Err(LeafCookError::InvalidSubmesh)
        );
        let mesh = Mesh::default();
        assert_eq!(Asset::from_mesh(1, &mesh), Err(LeafCookError::EmptyMesh));
    }

    #[test]
    fn payload_has_the_cxx_header_and_table_sizes() {
        let payload = fixture().encode_payload();
        // 48-byte payload header + 29-byte page + 56-byte cluster + 37-byte v2 group (v1's 21
        // bytes + a 16-byte LOD sphere) + 3-byte tail.
        assert_eq!(payload.len(), 173);
        let group = 48 + 29 + 56;
        assert_eq!(&payload[group + 21..group + 25], &0.0f32.to_le_bytes());
        assert_eq!(&payload[group + 33..group + 37], &4.0f32.to_le_bytes());
        assert_eq!(&payload[0..4], &PAYLOAD_VERSION.to_le_bytes());
        assert_eq!(
            u64::from_le_bytes(payload[4..12].try_into().unwrap()),
            0x1020_3040_5060_7080
        );
        assert_eq!(payload[44], 0); // coarse_group's low byte; header is field-by-field LE.
        assert_eq!(&payload[payload.len() - 3..], &[0xaa, 0xbb, 0xcc]);
    }

    #[test]
    fn cook_wraps_as_virtual_geometry_rma1_and_hashes_payload() {
        use crate::cooked::read_header;

        let (file, id) = fixture().cook();
        let (header, payload) = read_header(&file).unwrap();
        assert_eq!(header.asset_kind, ASSET_KIND_VIRTUAL_GEOMETRY);
        assert_eq!(header.type_schema_hash, VIRTUAL_GEOMETRY_SCHEMA_HASH);
        assert_eq!(id, fnv1a_64(payload));
        assert_eq!(payload, fixture().encode_payload());
    }
}
