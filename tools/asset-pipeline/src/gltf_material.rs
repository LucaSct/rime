// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! glTF metallic-roughness material import (M6.4). Turns each glTF material into a cooked
//! `material::Material` — factors plus references to its textures — and cooks the textures it names.
//! The renderer's shading model *is* metallic-roughness (the glTF/UE/Unity/Frostbite shared
//! vocabulary), so the mapping is nearly one-to-one; the interesting work is on the textures.
//!
//! **Colour space by usage (M6.3's rule of record).** A texture's bytes are cooked sRGB or linear
//! according to *what the material uses it for*, not the image itself: base-color and emissive are
//! colour (sRGB, decoded to linear when sampled); normal, metallic-roughness, and occlusion are data
//! (linear, sampled verbatim). So the *same source image* can cook to two different assets — glTF's
//! ORM packing (occlusion+roughness+metallic in one image) is linear, while that same image used as a
//! base color would be sRGB. The cook key is therefore `(image, colour-space)`, and each unique pair
//! is cooked exactly once (the `cache`), so a shared image becomes one shared GPU upload downstream
//! (ADR-0024 decision 2). An absent texture slot cooks to id 0; the renderer binds a 1×1 fallback.

use std::collections::HashMap;

use crate::material::{AlphaMode, Material};
use crate::texture::{ColorSpace, Texture};
use crate::PipelineError;

/// A cooked texture ready to write: its RMA1 bytes, content id, cooked filename, and a manifest label.
pub struct CookedTexture {
    pub id: u64,
    pub bytes: Vec<u8>,
    pub cooked_file: String,
    pub source_label: String,
}

/// A cooked material ready to write, plus whether it carries a normal map (which gates whether the
/// mesh it shades needs tangents — the M6.4 tangent policy lives in `cook_gltf`).
pub struct CookedMaterial {
    pub id: u64,
    pub bytes: Vec<u8>,
    pub material: Material,
    pub has_normal_map: bool,
}

/// The materials and textures a glTF cooks to. `materials` is in glTF material order, so a
/// primitive's `material_slot` indexes straight into it; `textures` holds each unique
/// `(image, colour-space)` cooked once.
pub struct MaterialImport {
    pub materials: Vec<CookedMaterial>,
    pub textures: Vec<CookedTexture>,
}

/// Import and cook every material in `document`, cooking each referenced texture from the already-
/// decoded `images` (so this works for `.glb` and embedded data-URI images, not just external files).
/// `stem` names the cooked files deterministically (`<stem>.img<N>.<srgb|lin>.rtex`).
pub fn import_materials(
    document: &gltf::Document,
    images: &[gltf::image::Data],
    stem: &str,
) -> Result<MaterialImport, PipelineError> {
    let mut cache: HashMap<(usize, ColorSpace), u64> = HashMap::new();
    let mut textures: Vec<CookedTexture> = Vec::new();
    let mut materials: Vec<CookedMaterial> = Vec::new();

    for mat in document.materials() {
        let pbr = mat.pbr_metallic_roughness();

        // Each slot's colour space follows its usage, not the image (see the module note).
        let base_color_tex = cook_slot(
            pbr.base_color_texture()
                .map(|i| i.texture().source().index()),
            ColorSpace::Srgb,
            images,
            stem,
            &mut cache,
            &mut textures,
        )?;
        let metallic_roughness_tex = cook_slot(
            pbr.metallic_roughness_texture()
                .map(|i| i.texture().source().index()),
            ColorSpace::Linear,
            images,
            stem,
            &mut cache,
            &mut textures,
        )?;
        let normal_tex = cook_slot(
            mat.normal_texture().map(|n| n.texture().source().index()),
            ColorSpace::Linear,
            images,
            stem,
            &mut cache,
            &mut textures,
        )?;
        let occlusion_tex = cook_slot(
            mat.occlusion_texture()
                .map(|o| o.texture().source().index()),
            ColorSpace::Linear,
            images,
            stem,
            &mut cache,
            &mut textures,
        )?;
        let emissive_tex = cook_slot(
            mat.emissive_texture().map(|i| i.texture().source().index()),
            ColorSpace::Srgb,
            images,
            stem,
            &mut cache,
            &mut textures,
        )?;

        // baseColorFactor and emissiveFactor are LINEAR in glTF (only the *textures* are sRGB-encoded),
        // so they copy straight across into the shader's linear working space. normal_scale and
        // occlusion_strength live on their texture infos and default to 1 when the texture is absent.
        let material = Material {
            base_color: pbr.base_color_factor(),
            emissive: mat.emissive_factor(),
            metallic: pbr.metallic_factor(),
            roughness: pbr.roughness_factor(),
            normal_scale: mat.normal_texture().map(|n| n.scale()).unwrap_or(1.0),
            occlusion_strength: mat.occlusion_texture().map(|o| o.strength()).unwrap_or(1.0),
            alpha_cutoff: mat.alpha_cutoff().unwrap_or(0.5),
            alpha_mode: map_alpha_mode(mat.alpha_mode()),
            base_color_tex,
            metallic_roughness_tex,
            normal_tex,
            occlusion_tex,
            emissive_tex,
        };
        let (bytes, id) = material.cook();
        materials.push(CookedMaterial {
            id,
            bytes,
            material,
            has_normal_map: normal_tex != 0,
        });
    }

    Ok(MaterialImport {
        materials,
        textures,
    })
}

/// Cook (or reuse) the texture behind an optional image index for a given usage colour space,
/// returning its content id — 0 when the slot is empty. The `(image, colour-space)` cache makes a
/// shared image (or the glTF ORM packing) one cooked asset, not one per reference.
fn cook_slot(
    image_index: Option<usize>,
    color_space: ColorSpace,
    images: &[gltf::image::Data],
    stem: &str,
    cache: &mut HashMap<(usize, ColorSpace), u64>,
    textures: &mut Vec<CookedTexture>,
) -> Result<u64, PipelineError> {
    let Some(image_index) = image_index else {
        return Ok(0); // no texture in this slot → the renderer's 1×1 fallback
    };
    if let Some(&id) = cache.get(&(image_index, color_space)) {
        return Ok(id);
    }

    let data = images.get(image_index).ok_or_else(|| {
        PipelineError::Unsupported(format!("material references missing image {image_index}"))
    })?;
    let rgba = to_rgba8(data)?;
    let (bytes, id) = Texture::from_rgba8(data.width, data.height, color_space, rgba).cook();

    // "srgb"/"lin" tags the cooked file, so the two colour-space cooks of one image don't collide.
    let tag = match color_space {
        ColorSpace::Srgb => "srgb",
        ColorSpace::Linear => "lin",
    };
    cache.insert((image_index, color_space), id);
    textures.push(CookedTexture {
        id,
        bytes,
        cooked_file: format!("{stem}.img{image_index}.{tag}.rtex"),
        source_label: format!("{stem}.gltf#image{image_index}.{tag}"),
    });
    Ok(id)
}

/// Normalise a decoded glTF image to RGBA8, the one pixel layout the texture cooker takes. glTF PBR
/// textures are 8-bit; a fewer-channel image expands to RGBA (missing colour → 0, missing alpha →
/// opaque, single channel → grey so an R-only map like occlusion keeps its value in `.r`). 16/32-bit
/// source images are rejected with a clear message rather than silently truncated (a later brick if a
/// real asset needs them).
fn to_rgba8(data: &gltf::image::Data) -> Result<Vec<u8>, PipelineError> {
    use gltf::image::Format;
    let px = &data.pixels;
    let rgba = match data.format {
        Format::R8G8B8A8 => px.clone(),
        Format::R8G8B8 => px
            .chunks_exact(3)
            .flat_map(|c| [c[0], c[1], c[2], 255])
            .collect(),
        Format::R8G8 => px
            .chunks_exact(2)
            .flat_map(|c| [c[0], c[1], 0, 255])
            .collect(),
        Format::R8 => px.iter().flat_map(|&r| [r, r, r, 255]).collect(),
        other => {
            return Err(PipelineError::Unsupported(format!(
                "glTF image format {other:?}: v1 cooks only 8-bit textures (PBR maps are 8-bit)"
            )))
        }
    };
    Ok(rgba)
}

/// Map glTF's alpha mode to the cooker's (identical set; kept explicit so a glTF-side change is a
/// compile error here, not a silent misencode).
fn map_alpha_mode(mode: gltf::material::AlphaMode) -> AlphaMode {
    match mode {
        gltf::material::AlphaMode::Opaque => AlphaMode::Opaque,
        gltf::material::AlphaMode::Mask => AlphaMode::Mask,
        gltf::material::AlphaMode::Blend => AlphaMode::Blend,
    }
}
