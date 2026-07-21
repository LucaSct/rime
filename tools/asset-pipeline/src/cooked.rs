// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The RMA1 cooked container — the *writer* of record (ADR-0024 §3). Every cooked file is a fixed,
//! versioned header followed by a kind-specific payload, all little-endian and written field by
//! field so the bytes are identical on every platform. The C++ `engine/assets` reader is the exact
//! counterpart; the byte layout lives in `docs/design/assets.md` and `FORMAT.md`, and the
//! cross-language fixture test is the drift alarm.

/// FNV-1a 64 — the engine's content hash (mirrors `engine/core/hash.hpp`). Used for the asset id
/// (the hash of a cooked payload) and, via the reflection scheme, the schema fingerprint.
pub const FNV1A64_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
/// The FNV-1a 64 prime.
pub const FNV1A64_PRIME: u64 = 0x0000_0100_0000_01b3;

/// Hash a byte run with FNV-1a 64. Deterministic and identical to the engine's implementation.
pub fn fnv1a_64(bytes: &[u8]) -> u64 {
    let mut hash = FNV1A64_OFFSET_BASIS;
    for &b in bytes {
        hash ^= u64::from(b);
        hash = hash.wrapping_mul(FNV1A64_PRIME);
    }
    hash
}

/// The 4-byte magic, written literally so a cooked file shows "RMA1" in a hex dump.
pub const COOKED_MAGIC: [u8; 4] = *b"RMA1";
/// The container envelope version this cooker writes.
pub const CONTAINER_VERSION: u16 = 1;

/// The cooker version for the cook cache (ADR-0024 §8) — **not** the container version. Bump this
/// whenever any importer or encoder could produce different output bytes from the same source (a new
/// tangent basis, a mip-filter change, a schema-hash update), so a stale cache re-cooks instead of
/// serving outdated bytes. Kept independent of `CARGO_PKG_VERSION`, which tracks releases, not cook
/// semantics.
pub const COOKER_VERSION: u32 = 1;

/// `asset_kind` wire value for a mesh (matches `engine/assets/asset_id.hpp`; append, never renumber).
pub const ASSET_KIND_MESH: u16 = 1;
/// `asset_kind` wire value for a texture (matches `engine/assets/asset_id.hpp`).
pub const ASSET_KIND_TEXTURE: u16 = 2;
/// `asset_kind` wire value for a material (matches `engine/assets/asset_id.hpp`).
pub const ASSET_KIND_MATERIAL: u16 = 3;
/// `asset_kind` wire value for a skeleton (matches `engine/assets/asset_id.hpp`).
pub const ASSET_KIND_SKELETON: u16 = 4;
/// `asset_kind` wire value for an animation clip (matches `engine/assets/asset_id.hpp`).
pub const ASSET_KIND_CLIP: u16 = 5;
/// `asset_kind` wire value for a destructible / fracture pattern (matches `engine/assets/asset_id.hpp`).
pub const ASSET_KIND_DESTRUCTIBLE: u16 = 6;
/// `asset_kind` wire value for a cooked mesh signed-distance field (matches
/// `engine/assets/asset_id.hpp`; M10.4a, ADR-0032 §2).
pub const ASSET_KIND_MESH_SDF: u16 = 7;

/// The mesh schema fingerprint: the reflection `type_hash` of the v1 position/normal/uv vertex
/// layout, computed and pinned by the C++ engine (`engine/assets`). The cooker embeds the same
/// constant; the reader rejects a mismatch, so both languages agree on the mesh format by
/// construction. If the vertex layout ever changes, update this in lockstep with the engine.
pub const MESH_SCHEMA_HASH: u64 = 0x1987_38A2_DDE2_50AC;

/// The texture schema fingerprint: the reflection `type_hash` of the v1 `{width, height, offset,
/// size}` mip-descriptor record, computed and pinned by the C++ engine (`texture_schema_hash()`).
/// Same contract as the mesh hash — the cooker embeds it, the reader rejects a mismatch — so the two
/// languages agree on the cooked-texture layout by construction. Update in lockstep with the engine
/// if the mip record ever changes (a new *pixel format* is an appended enum value, not a change here).
pub const TEXTURE_SCHEMA_HASH: u64 = 0xAB8A_2B88_4141_F736;

/// The material schema fingerprint: the reflection `type_hash` of the v1 material record (factors +
/// five texture-reference AssetIds), computed and pinned by the C++ engine (`material_schema_hash()`).
/// Same contract as the mesh/texture hashes — the cooker embeds it, the reader rejects a mismatch — so
/// the two languages agree on the cooked-material layout by construction. Update in lockstep with the
/// engine if the material record ever gains, loses, or reorders a field.
pub const MATERIAL_SCHEMA_HASH: u64 = 0xCA4E_D4CC_434C_941A;

/// The skeleton schema fingerprint: the reflection `type_hash` of the v1 per-joint record (parent,
/// name hash, inverse-bind matrix, bind-pose TRS), computed and pinned by the C++ engine
/// (`skeleton_schema_hash()`). Same contract as the hashes above — the cooker embeds it, the reader
/// rejects a mismatch — so the two languages agree on the cooked-skeleton layout by construction.
pub const SKELETON_SCHEMA_HASH: u64 = 0xD90A_5CB8_EBA3_6DED;

/// The clip schema fingerprint: the reflection `type_hash` of the v1 channel record (target joint,
/// path, interpolation, key count), computed and pinned by the C++ engine (`clip_schema_hash()`).
/// Same contract as the hashes above; update in lockstep with the engine if the channel record ever
/// changes (a new value *type* is an appended path enum, not a record change).
pub const CLIP_SCHEMA_HASH: u64 = 0x6C84_D2A2_AAAB_CE49;

/// The destructible schema fingerprint: the reflection `type_hash` of the v1 per-part record (COM,
/// AABB, volume, and the vertex/face/index counts that slice the geometry blobs), computed and pinned
/// by the C++ engine (`destructible_schema_hash()`). Same contract as the hashes above — the cooker
/// embeds it, the reader rejects a mismatch — so the two languages agree on the cooked-destructible
/// layout by construction (M8.1). Update in lockstep with the engine if the per-part record changes;
/// the bond/anchor tables and the geometry blobs are structure the header sizes, not fingerprinted
/// (exactly as mesh vertices past the vertex record are not).
pub const DESTRUCTIBLE_SCHEMA_HASH: u64 = 0x8F2D_17FB_F584_85E2;

/// The mesh-SDF schema fingerprint: the reflection `type_hash` of the v1 fixed header record (local
/// bounds, grid placement, voxel size, resolution, encoding, max_abs_distance), computed and pinned
/// by the C++ engine (`sdf_schema_hash()`). Same contract as the hashes above — the cooker embeds it,
/// the reader rejects a mismatch — so the two languages agree on the cooked-SDF layout by
/// construction (M10.4a). Unlike Mesh/Texture/Skeleton/Clip (which fingerprint a REPEATED table
/// record), this mirrors Material: the header IS the whole structured part of the payload — the
/// trailing distances blob is bare f32 scalars with no per-element layout of its own to protect.
pub const MESH_SDF_SCHEMA_HASH: u64 = 0x6EFF_A981_1903_3990;

/// A little-endian byte sink. Every multi-byte value is decomposed to its LE bytes explicitly, so
/// the output never depends on the host's endianness — the same discipline as the reader's cursor.
#[derive(Default)]
pub struct ByteWriter {
    buf: Vec<u8>,
}

impl ByteWriter {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn u16(&mut self, v: u16) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn u32(&mut self, v: u32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn i32(&mut self, v: i32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn u64(&mut self, v: u64) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn f32(&mut self, v: f32) {
        self.buf.extend_from_slice(&v.to_le_bytes());
    }

    pub fn bytes(&mut self, b: &[u8]) {
        self.buf.extend_from_slice(b);
    }

    pub fn len(&self) -> usize {
        self.buf.len()
    }

    pub fn is_empty(&self) -> bool {
        self.buf.is_empty()
    }

    pub fn into_vec(self) -> Vec<u8> {
        self.buf
    }
}

/// Wrap a kind-specific payload in the RMA1 container header and return `(file_bytes, asset_id)`,
/// where the id is the FNV-1a content hash of the payload (matches the reader's `content_hash`).
pub fn wrap_container(kind: u16, schema_hash: u64, payload: &[u8]) -> (Vec<u8>, u64) {
    let mut w = ByteWriter::new();
    w.bytes(&COOKED_MAGIC);
    w.u16(CONTAINER_VERSION);
    w.u16(kind);
    w.u64(schema_hash);
    w.u64(payload.len() as u64);
    w.bytes(payload);
    (w.into_vec(), fnv1a_64(payload))
}

/// The parsed container header (for `rime-cli inspect`). Mirrors the reader's `CookedHeader`.
#[derive(Debug, Clone, Copy)]
pub struct CookedHeader {
    pub container_version: u16,
    pub asset_kind: u16,
    pub type_schema_hash: u64,
    pub payload_size: u64,
}

/// Read and validate just the container header of a cooked file. Returns the header and the payload
/// slice, or an error string. Deliberately small — the engine's reader is the authoritative,
/// exhaustively-tested one; this is only for the CLI's `inspect`.
pub fn read_header(file: &[u8]) -> Result<(CookedHeader, &[u8]), String> {
    if file.len() < 24 {
        return Err("file shorter than the 24-byte RMA1 header".to_string());
    }
    if file[0..4] != COOKED_MAGIC {
        return Err("bad magic (not an RMA1 file)".to_string());
    }
    let container_version = u16::from_le_bytes([file[4], file[5]]);
    let asset_kind = u16::from_le_bytes([file[6], file[7]]);
    let type_schema_hash = u64::from_le_bytes(file[8..16].try_into().unwrap());
    let payload_size = u64::from_le_bytes(file[16..24].try_into().unwrap());
    let payload = &file[24..];
    if payload.len() as u64 != payload_size {
        return Err(format!(
            "payload_size {} disagrees with the {} bytes present",
            payload_size,
            payload.len()
        ));
    }
    let header = CookedHeader {
        container_version,
        asset_kind,
        type_schema_hash,
        payload_size,
    };
    Ok((header, payload))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fnv_matches_published_vectors() {
        // Same vectors the engine's core hash test pins, so the two implementations can't drift.
        assert_eq!(fnv1a_64(b""), FNV1A64_OFFSET_BASIS);
        assert_eq!(fnv1a_64(b"a"), 0xaf63_dc4c_8601_ec8c);
        assert_eq!(fnv1a_64(b"foobar"), 0x8594_4171_f739_67e8);
    }

    #[test]
    fn container_round_trips_through_read_header() {
        let payload = [1u8, 2, 3, 4, 5];
        let (file, id) = wrap_container(ASSET_KIND_MESH, MESH_SCHEMA_HASH, &payload);
        assert_eq!(id, fnv1a_64(&payload));
        let (header, back) = read_header(&file).unwrap();
        assert_eq!(header.container_version, CONTAINER_VERSION);
        assert_eq!(header.asset_kind, ASSET_KIND_MESH);
        assert_eq!(header.type_schema_hash, MESH_SCHEMA_HASH);
        assert_eq!(back, payload);
    }

    #[test]
    fn read_header_rejects_bad_magic_and_short_files() {
        assert!(read_header(b"short").is_err());
        let mut file = wrap_container(ASSET_KIND_MESH, MESH_SCHEMA_HASH, &[0u8; 4]).0;
        file[0] = b'X';
        assert!(read_header(&file).is_err());
    }
}
