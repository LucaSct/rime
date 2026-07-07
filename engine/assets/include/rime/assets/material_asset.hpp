// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/assets/asset_id.hpp"

// The runtime, in-memory form of a cooked material (M6.4). A material is the metallic-roughness
// parameter set (the shared vocabulary of glTF/UE/Unity/Frostbite) plus references to the textures
// that drive it. It is a small, FIXED record — no variable-length tail — so its cooked payload is a
// constant number of bytes and its reader is a straight-line read-and-validate.
//
// The texture references are AssetIds (content hashes), not paths or inline pixels: a material
// names the *cooked bytes* it needs, and the loader resolves each id to an already-loaded
// rhi::Texture, sharing one GPU upload across every material that references the same image
// (ADR-0024, decision 2). An id of 0 (kInvalidAssetId) means "no texture for this slot"; the
// renderer binds a 1x1 fallback (white / flat-normal / white-AO) so a single shader serves every
// material permutation (M6.4).
namespace rime::assets {

// How a material's alpha is interpreted — the glTF alphaMode enum. Stored as a wire u32; append,
// never renumber. Opaque ignores alpha; Mask discards fragments below `alpha_cutoff` (a hard 1-bit
// edge, no blending); Blend is order-dependent transparency. v1 cooks all three but the forward
// pipeline draws Blend as Opaque for now (an honest limitation — real OIT/sorting is a later
// brick).
enum class AlphaMode : std::uint32_t {
    Opaque = 0,
    Mask = 1,
    Blend = 2,
};

// A cooked material in memory. Every factor defaults to the glTF default, so a material that omits
// a property decodes to exactly what glTF specifies. Colours are LINEAR (the cook converts sRGB
// authoring values), matching the space the PBR shader works in.
struct MaterialAsset {
    // Multiplied with the base-color texture (or standing alone when there is none). RGBA; alpha
    // feeds the alpha_mode test. glTF default is opaque white.
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // Emissive radiance added after the BRDF. RGB; glTF default is black (no emission).
    float emissive[3] = {0.0f, 0.0f, 0.0f};

    float metallic = 1.0f;     // 0 = dielectric, 1 = metal (glTF default 1)
    float roughness = 1.0f;    // 0 = mirror, 1 = fully rough (glTF default 1)
    float normal_scale = 1.0f; // scales the tangent-space XY of the normal map (glTF default 1)
    float occlusion_strength = 1.0f; // lerps AO toward 1 (no occlusion) at 0 (glTF default 1)
    float alpha_cutoff = 0.5f;       // the Mask threshold (glTF default 0.5)
    AlphaMode alpha_mode = AlphaMode::Opaque;

    // Texture slots, each an AssetId (0 = none, use the fallback). The cook picks each texture's
    // colour space from its *usage*: base-color and emissive are sRGB; normal, metallic-roughness,
    // and occlusion are linear data (M6.3's colour-space rule of record).
    AssetId base_color_tex{};         // sRGB colour
    AssetId metallic_roughness_tex{}; // linear: G = roughness, B = metallic (the glTF packing)
    AssetId normal_tex{};             // linear: tangent-space normal, (128,128,255) = flat
    AssetId occlusion_tex{};          // linear: R = ambient occlusion
    AssetId emissive_tex{};           // sRGB colour
};

} // namespace rime::assets
