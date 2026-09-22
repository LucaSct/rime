// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The M18 virtualized-geometry companion payload writer. This module owns only the stable,
//! versioned RMA1 byte encoding; clustering, simplification, and page packing are separate cook
//! stages. Its field order mirrors `decode_virtual_geometry` in the C++ reader exactly.

use crate::cooked::{
    wrap_container, ByteWriter, ASSET_KIND_VIRTUAL_GEOMETRY, VIRTUAL_GEOMETRY_SCHEMA_HASH,
};

/// Version of the kind-specific virtual-geometry payload (independent of the RMA1 envelope).
pub const PAYLOAD_VERSION: u32 = 1;

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
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Group {
    pub first_cluster: u32,
    pub cluster_count: u32,
    pub first_child: u32,
    pub child_count: u32,
    pub lod_error_m: f32,
    pub permanently_resident: bool,
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
            }],
            child_groups: Vec::new(),
            page_dependencies: Vec::new(),
            page_bytes: vec![0xaa, 0xbb, 0xcc],
            coarse_group: 0,
        }
    }

    #[test]
    fn payload_has_the_cxx_header_and_table_sizes() {
        let payload = fixture().encode_payload();
        // 48-byte payload header + 29-byte page + 56-byte cluster + 21-byte group + 3-byte tail.
        assert_eq!(payload.len(), 157);
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
