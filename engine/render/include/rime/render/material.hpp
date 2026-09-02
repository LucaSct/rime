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

// Forward-declared so this header keeps its light dependency set; the definition arrives with
// `rime/assets/material_asset.hpp`, which the one .cpp that implements the conversion includes.
} // namespace rime::render

namespace rime::assets {
struct MaterialAsset;
}

namespace rime::render {

struct PbrMaterialDesc {
    // The first three fields keep their M5.5 order, because positional aggregate init
    // `{base_color, metallic, roughness}` is used across the render tests — inserting a field ahead
    // of them would silently misassign every such call. New factors are appended, never inserted.
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // linear RGBA, multiplies the base-color map
    float metallic = 0.0f;                  // 0 = dielectric, 1 = metal (multiplies MR blue)
    float roughness = 0.5f;                 // 0 = mirror, 1 = fully diffuse (multiplies MR green)
    float emissive[3] = {0.0f, 0.0f, 0.0f}; // linear radiance added after the BRDF (M6.4)
    float normal_scale = 1.0f;              // scales the normal map's tangent-plane XY (M6.4)
    float occlusion_strength = 1.0f;        // lerps AO toward 1 (no occlusion) at 0 (M6.4)

    // ALPHA MASKING (m15.5/m15.6): discard fragments whose base-color alpha is below this. Zero —
    // the default — means no masking at all, which is what makes one float enough to express glTF's
    // three alpha modes here: Opaque and Blend both leave it 0, Mask sets it to the material's
    // cutoff. An enum would have to be kept in sync with the shader's branch; a threshold of zero
    // simply cannot mask anything.
    //
    // `assets::AlphaMode` and `assets::MaterialAsset::alpha_cutoff` have been cooked and reflected
    // since M6.3 and read by NOTHING, so every alpha-tested glTF — foliage, fences, decals — has
    // rendered as an opaque quad with no warning. This is the field that ends that.
    //
    // Appended after the factors and before the maps: verified by grep that nothing positionally
    // aggregate-initialises past `roughness` (the hazard the note at the top of this struct names).
    float alpha_cutoff = 0.0f;

    // m16.5. Both cooked from glTF, and both dropped entirely before that brick: every surface was
    // back-face culled and every texture repeated, whatever the author asked for.
    bool double_sided = false; // draw both faces — foliage cards, cloth, thin geometry
    bool clamp_uv = false;     // sample with ClampToEdge, not Repeat: atlases and trim sheets

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

// Convert a cooked material record into the renderer's description — FACTORS ONLY.
//
// This was `build_desc`, an anonymous-namespace function inside `samples/08-gltf-zoo/main.cpp`,
// which meant the only place in the engine that knew how to turn a `.rmat` into something drawable
// was a sample. Every game either copied it or drew neutral grey. Promoting it is most of what
// makes cooked materials reach a scene-placed mesh at all (m16.3, ADR-0039).
//
// It deliberately does NOT resolve the five texture slots. Those need a GPU upload path and a
// residency policy — placeholder until drained, fallback when a slot is empty — which belongs to
// `GpuAssetBridge`, not to a pure conversion. Keeping the split means this function is exact,
// GPU-free, and unit-testable, and the sample can still apply its own map-stripping control on top.
//
// The one non-trivial mapping is alpha: glTF's three modes collapse to a single threshold, because
// `alpha_cutoff == 0` already means "never mask" to the shader. Opaque and Blend both leave it at
// zero — which is also an honest statement of a limitation, since Blend genuinely draws as Opaque
// (no transparency pass exists; ADR-0039 says so out loud rather than letting it be discovered).
[[nodiscard]] PbrMaterialDesc material_from_cooked(const assets::MaterialAsset& cooked) noexcept;

using MaterialSetId = std::uint32_t;
inline constexpr MaterialSetId kInvalidMaterialSetId = 0xFFFFFFFFu;

// One mesh's materials, indexed by a submesh's `material_slot` (m16.3, ADR-0039 ruling 2).
//
// An entity cannot simply hold `std::vector<MaterialId>`: components are trivially-copyable PODs,
// so the vector lives here and the entity holds a `MaterialSet{MaterialSetId}` index. One entity,
// N draws — which is what keeps picking and the outliner honest, as against the alternative of
// spawning a child entity per submesh and polluting the scene the author actually made.
//
// Rows are append-only and mutated in place, exactly like MaterialRegistry, so a set resolved
// before its textures have streamed in can be filled without minting a new id.
class MaterialSetRegistry {
public:
    [[nodiscard]] MaterialSetId add(std::vector<MaterialId> materials) {
        sets_.push_back(std::move(materials));
        return static_cast<MaterialSetId>(sets_.size() - 1);
    }

    void update(MaterialSetId id, std::vector<MaterialId> materials) {
        sets_[id] = std::move(materials);
    }

    [[nodiscard]] bool contains(MaterialSetId id) const noexcept {
        return id != kInvalidMaterialSetId && id < sets_.size();
    }

    // The material for `slot`, or `fallback` when the set does not cover it. A mesh whose submesh
    // names a slot its material set never resolved is a content error, not a crash: it draws with
    // the fallback and the bridge counts it.
    [[nodiscard]] MaterialId
    material_for(MaterialSetId id, std::uint32_t slot, MaterialId fallback) const noexcept {
        if (!contains(id) || slot >= sets_[id].size() || sets_[id][slot] == kInvalidMaterialId) {
            return fallback;
        }
        return sets_[id][slot];
    }

    [[nodiscard]] std::size_t size() const noexcept { return sets_.size(); }

private:
    std::vector<std::vector<MaterialId>> sets_;
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

    // Replace an existing material's parameters in place. The scene renderer holds a *const* ref
    // and re-reads get(id) every frame, so mutating a desc here is seen on the next frame — which
    // is how a material's borrowed placeholder textures get swapped for the real cooked ones as the
    // GPU asset bridge streams them in (M6.10). No id is minted or invalidated.
    void update(MaterialId id, const PbrMaterialDesc& desc) { materials_[id] = desc; }

    [[nodiscard]] const PbrMaterialDesc& get(MaterialId id) const { return materials_[id]; }

    [[nodiscard]] std::size_t size() const noexcept { return materials_.size(); }

private:
    std::vector<PbrMaterialDesc> materials_;
};

} // namespace rime::render
