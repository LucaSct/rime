// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/assets/asset_id.hpp"

// A cooked TERRAIN LAYER (M19.7a, ADR-0066): one entry a terrain splat palette can name — the
// surface a painted weight channel stands for, plus what terrain alone needs to draw it: a texture
// tiled in WORLD space, and a per-texel height for height-blended transitions (gravel showing
// through grass along the cracks, rather than a soft cross-fade between them).
//
// WHY A TERRAIN-SPECIFIC ASSET RATHER THAN NEW MATERIAL FIELDS. Every mesh in the engine reads
// MaterialAsset. Giving it a height slot, a UV scale and a contrast would change its schema hash —
// so every material ever cooked would be refused with SchemaMismatch until re-cooked — to carry
// three fields only terrain reads. And the obvious place to put height, the base-colour alpha,
// already means opacity there (AlphaMode). So the general contract stays exactly as it is, and
// terrain gets a small record of its own that REFERENCES a material for the parts they share
// (base-colour factor, metallic, roughness).
//
// WHY THE HEIGHTFIELD FORMAT DOES NOT CHANGE. A heightfield's palette slot is an AssetId, and every
// cooked file's RMA1 header already says what KIND of asset it is. So a slot may name either a
// Material (as every v2 terrain does today) or a TerrainLayer, and the code that builds the GPU
// terrain from a heightfield dispatches on the referenced file's AssetKind. No heightfield payload
// version bump is needed, and a v2 file whose palette names materials keeps loading unchanged.
//
// Like MaterialAsset this is a small, FIXED record: no variable-length tail, so the cooked payload
// is a constant 28 bytes and the reader is a straight read-and-validate (cooked_reader.hpp:
// decode_terrain_layer / read_terrain_layer). The Rust writer of record is
// tools/asset-pipeline/src/terrain_layer.rs; the byte layout is in tools/asset-pipeline/FORMAT.md.
namespace rime::assets {

struct TerrainLayerAsset {
    // The cooked Material supplying the layer's base-colour factor, metallic and roughness. Never
    // 0: a layer with no material has nothing to shade with, and the reader refuses it.
    AssetId material{};

    // A cooked RGBA8 texture packing the layer's albedo and height into one fetch:
    //   RGB = albedo, sRGB-encoded colour;
    //   A   = height, LINEAR in [0, 1] (0 = the deepest point of the layer's relief).
    // Cooked with the sRGB format (TextureFormat::Rgba8Srgb), and that is exactly right for BOTH
    // halves: the Vulkan specification's definition of VK_FORMAT_R8G8B8A8_SRGB ("Formats" chapter)
    // applies the sRGB nonlinear encoding to the R, G and B components only — "an 8-bit A
    // component in byte 3" carries no encoding — so the sampler decodes the colour to linear and
    // hands the height back untouched. The cook mirrors this on the CPU: colour mips are averaged
    // in linear light, height mips are averaged directly, and the alpha-coverage rescale a cutout
    // texture gets is NOT applied (height is not coverage). Never 0.
    AssetId albedo_height{};

    // Metres of world space per texture repeat, along world X and world Z. Terrain samples layer
    // textures at WORLD-XZ coordinates (not per-tile UVs) so the pattern runs continuously across
    // tile seams. Finite and > 0.
    float uv_scale[2] = {1.0f, 1.0f};

    // How strongly height redistributes the painted weights at a transition (ADR-0066 records the
    // blend: b_k = w_k * g_k / sum_j(w_j * g_j), g_k = exp2(c * (h_k - h_max))). 0 = no height
    // effect at all, i.e. the plain weighted blend terrain draws today. Finite and >= 0.
    float height_contrast = 0.0f;
};

} // namespace rime::assets
