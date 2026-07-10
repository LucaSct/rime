// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/rhi/types.hpp"

// Materials for the scene layer (M5.5): the metallic-roughness parameter set the M5.6 PBR
// pipeline shades with, stored in a registry behind small dense ids — an entity's component
// carries a MaterialId, never the data (ADR-0018: components are trivially-copyable PODs).
//
// Why these three parameters: the metallic-roughness model is the industry's shared vocabulary
// (glTF, UE, Unity, Frostbite all speak it). `base_color` is the surface's own color — diffuse
// albedo for dielectrics, F0 reflectance for metals; `metallic` interpolates between those two
// interpretations (real surfaces are one or the other; the slider exists for blends at material
// boundaries); `roughness` is microfacet spread — 0 = mirror, 1 = matte. The full derivation of
// how these feed the BRDF is docs/math/pbr.md.
namespace rime::render {

using MaterialId = std::uint32_t;
inline constexpr MaterialId kInvalidMaterialId = 0xFFFFFFFFu;

struct PbrMaterialDesc {
    // The first three fields keep their M5.5 order, because positional aggregate init
    // `{base_color, metallic, roughness}` is used across the render tests — inserting a field ahead
    // of them would silently misassign every such call. New factors are appended, never inserted.
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // linear RGBA, multiplies the base-color map
    float metallic = 0.0f;                          // 0 = dielectric, 1 = metal (multiplies MR blue)
    float roughness = 0.5f;                         // 0 = mirror, 1 = fully diffuse (multiplies MR green)
    float emissive[3] = {0.0f, 0.0f, 0.0f};         // linear radiance added after the BRDF (M6.4)
    float normal_scale = 1.0f;                      // scales the normal map's tangent-plane XY (M6.4)
    float occlusion_strength = 1.0f;                // lerps AO toward 1 (no occlusion) at 0 (M6.4)

    // Optional maps, each driving / multiplied with its factor above (the glTF convention; the
    // fallback is the identity). BORROWED, not owned: the caller keeps them alive as long as any
    // material references them — ownership stays wherever the pixels came from until the M6 loader
    // gives textures a real home. Invalid (the default) = "no map"; the scene renderer binds a 1x1
    // fallback (white, or flat-normal for the normal slot), so ONE forward pipeline serves every
    // material permutation without shader variants (M6.4). Colour space is by usage (M6.3): author
    // base-color & emissive sRGB (create them Srgb so sampling decodes to linear); normal,
    // metallic-roughness, and occlusion are linear data (Unorm). metallic_roughness packs roughness
    // in G and metallic in B (the glTF packing); occlusion is R.
    rhi::TextureHandle base_color_texture{};
    rhi::TextureHandle metallic_roughness_texture{};
    rhi::TextureHandle normal_texture{};
    rhi::TextureHandle occlusion_texture{};
    rhi::TextureHandle emissive_texture{};
};

// A plain store: add during setup, read while building the frame. CPU data only — the PBR pass
// (M5.6) uploads the active materials into its per-frame uniform data; there is nothing GPU-side
// to own here yet.
class MaterialRegistry {
public:
    [[nodiscard]] MaterialId add(const PbrMaterialDesc& desc) {
        materials_.push_back(desc);
        return static_cast<MaterialId>(materials_.size() - 1);
    }

    [[nodiscard]] const PbrMaterialDesc& get(MaterialId id) const { return materials_[id]; }

    [[nodiscard]] std::size_t size() const noexcept { return materials_.size(); }

private:
    std::vector<PbrMaterialDesc> materials_;
};

} // namespace rime::render
