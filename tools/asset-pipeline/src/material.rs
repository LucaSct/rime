// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! The cooker's in-memory material and its RMA1 material-payload encoder (M6.4). A material is the
//! metallic-roughness parameter set plus references to the textures that drive it, written in the
//! byte layout `engine/assets`' `decode_material` validates (see `docs/design/assets.md`).
//!
//! Unlike a mesh or texture, a material has **no variable-length data** — it is a single fixed record,
//! so `cook()` is a straight run of little-endian fields. The texture references are `AssetId`s (the
//! content hash of a *cooked* texture), not paths or inline pixels: the material names the cooked
//! bytes it needs and the engine resolves each id to one shared GPU upload. An id of 0 means "no
//! texture" for that slot, and the renderer binds a 1×1 fallback (white / flat-normal / white-AO).
//!
//! Materials are not standalone source files — they come out of a glTF alongside its meshes — so this
//! module is the *encoder* the glTF-material import path (a later M6.4 step) will drive, exactly as
//! `mesh.rs` is driven by the mesh import rather than a `rime cook foo.material` command.

use crate::cooked::{wrap_container, ByteWriter, ASSET_KIND_MATERIAL, MATERIAL_SCHEMA_HASH};

/// How a material's alpha is interpreted — the glTF `alphaMode`. Wire values match
/// `engine/assets/material_asset.hpp`'s `AlphaMode` (append, never renumber).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AlphaMode {
    #[default]
    Opaque,
    Mask,
    Blend,
}

impl AlphaMode {
    /// The wire `u32` written into the payload.
    pub fn wire_value(self) -> u32 {
        match self {
            AlphaMode::Opaque => 0,
            AlphaMode::Mask => 1,
            AlphaMode::Blend => 2,
        }
    }
}

/// A cook-ready material. Every field defaults to the glTF default, so a source material that omits a
/// property cooks to exactly what glTF specifies. Colours are LINEAR (the cook converts sRGB authoring
/// values), matching the space the engine's PBR shader works in.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Material {
    /// Base-color factor, multiplied with the base-color texture. RGBA (alpha feeds the alpha test).
    pub base_color: [f32; 4],
    /// Emissive factor (linear RGB), added after the BRDF.
    pub emissive: [f32; 3],
    pub metallic: f32,
    pub roughness: f32,
    pub normal_scale: f32,
    pub occlusion_strength: f32,
    pub alpha_cutoff: f32,
    pub alpha_mode: AlphaMode,
    /// Texture references, each the `AssetId` of a cooked texture (0 = none, use the fallback).
    pub base_color_tex: u64,
    pub metallic_roughness_tex: u64,
    pub normal_tex: u64,
    pub occlusion_tex: u64,
    pub emissive_tex: u64,
}

impl Default for Material {
    fn default() -> Self {
        // The glTF metallic-roughness defaults, so an all-defaults source cooks to a valid material.
        Material {
            base_color: [1.0, 1.0, 1.0, 1.0],
            emissive: [0.0, 0.0, 0.0],
            metallic: 1.0,
            roughness: 1.0,
            normal_scale: 1.0,
            occlusion_strength: 1.0,
            alpha_cutoff: 0.5,
            alpha_mode: AlphaMode::Opaque,
            base_color_tex: 0,
            metallic_roughness_tex: 0,
            normal_tex: 0,
            occlusion_tex: 0,
            emissive_tex: 0,
        }
    }
}

impl Material {
    /// Encode this material into a complete RMA1 file, returning `(bytes, asset_id)`. The field order
    /// here IS the wire format: it mirrors `decode_material` in the engine's reader and the reflected
    /// `MaterialV1` record that fingerprints it, so both languages agree by construction.
    pub fn cook(&self) -> (Vec<u8>, u64) {
        let mut p = ByteWriter::new();
        for c in self.base_color {
            p.f32(c);
        }
        for c in self.emissive {
            p.f32(c);
        }
        p.f32(self.metallic);
        p.f32(self.roughness);
        p.f32(self.normal_scale);
        p.f32(self.occlusion_strength);
        p.f32(self.alpha_cutoff);
        p.u32(self.alpha_mode.wire_value());
        p.u64(self.base_color_tex);
        p.u64(self.metallic_roughness_tex);
        p.u64(self.normal_tex);
        p.u64(self.occlusion_tex);
        p.u64(self.emissive_tex);
        wrap_container(ASSET_KIND_MATERIAL, MATERIAL_SCHEMA_HASH, &p.into_vec())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cooked::{read_header, ASSET_KIND_MATERIAL, MATERIAL_SCHEMA_HASH};

    /// A material with every field set to a distinct, non-default value — the same values the C++
    /// round-trip test uses, so the two languages exercise an identical record.
    fn distinct_material() -> Material {
        Material {
            base_color: [0.8, 0.4, 0.2, 1.0],
            emissive: [0.1, 0.2, 0.3],
            metallic: 0.25,
            roughness: 0.6,
            normal_scale: 0.5,
            occlusion_strength: 0.75,
            alpha_cutoff: 0.3,
            alpha_mode: AlphaMode::Mask,
            base_color_tex: 0x1111_1111_1111_1111,
            metallic_roughness_tex: 0x2222_2222_2222_2222,
            normal_tex: 0x3333_3333_3333_3333,
            occlusion_tex: 0,
            emissive_tex: 0x5555_5555_5555_5555,
        }
    }

    #[test]
    fn cook_is_byte_stable_and_carries_the_material_kind_and_schema() {
        let mat = distinct_material();
        let (a, id_a) = mat.cook();
        let (b, id_b) = mat.cook();
        assert_eq!(a, b, "cook must be deterministic");
        assert_eq!(id_a, id_b);

        let (header, payload) = read_header(&a).unwrap();
        assert_eq!(header.asset_kind, ASSET_KIND_MATERIAL);
        assert_eq!(header.type_schema_hash, MATERIAL_SCHEMA_HASH);
        // A material is a fixed record: 12 f32 + 1 u32 factors (52 bytes) + 5 u64 texture ids (40).
        assert_eq!(payload.len(), 92);
    }

    #[test]
    fn schema_hash_is_the_constant_the_engine_pins() {
        // The cross-language contract in one line: the engine's material_schema_hash() returns this
        // exact value (pinned in cooked_material_test.cpp). If the C++ MaterialV1 record ever changes,
        // that test and this constant diverge until both are updated together.
        assert_eq!(MATERIAL_SCHEMA_HASH, 0xCA4E_D4CC_434C_941A);
    }

    #[test]
    fn defaults_are_the_gltf_defaults() {
        let m = Material::default();
        assert_eq!(m.base_color, [1.0, 1.0, 1.0, 1.0]);
        assert_eq!(m.emissive, [0.0, 0.0, 0.0]);
        assert_eq!(m.metallic, 1.0);
        assert_eq!(m.roughness, 1.0);
        assert_eq!(m.alpha_mode, AlphaMode::Opaque);
        assert_eq!(m.alpha_cutoff, 0.5);
        assert_eq!(m.base_color_tex, 0);
    }

    #[test]
    fn alpha_mode_wire_values_match_the_engine_enum() {
        assert_eq!(AlphaMode::Opaque.wire_value(), 0);
        assert_eq!(AlphaMode::Mask.wire_value(), 1);
        assert_eq!(AlphaMode::Blend.wire_value(), 2);
    }
}
