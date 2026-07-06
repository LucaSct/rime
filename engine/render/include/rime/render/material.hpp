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
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // linear-space RGBA
    float metallic = 0.0f;                          // 0 = dielectric, 1 = metal
    float roughness = 0.5f;                         // 0 = mirror, 1 = fully diffuse

    // Optional base-color map, multiplied with the factor above (the glTF convention; a plain
    // white texture is the identity). BORROWED, not owned: the caller keeps the texture alive as
    // long as any material references it — ownership stays wherever the pixels came from until
    // the M6 asset pipeline gives textures a real home. Author base-color pixels in sRGB and
    // create the texture with an Srgb format so sampling decodes to linear (the shader assumes
    // it). Invalid (the default) = untextured; the scene renderer binds its 1x1 white fallback.
    rhi::TextureHandle base_color_texture{};
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
