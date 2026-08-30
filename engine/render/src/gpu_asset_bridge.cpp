// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/render/gpu_asset_bridge.hpp"

#include <span>
#include <vector>

#include "rime/assets/manifest.hpp"
#include "rime/assets/mesh_asset.hpp"
#include "rime/assets/texture_asset.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/rhi/device.hpp"
#include "rime/rhi/resources.hpp"

namespace rime::render {
namespace {

rhi::Format to_rhi_format(assets::TextureFormat format) noexcept {
    // The cook tags each texture sRGB or linear by *semantic* (base-color/emissive vs
    // normal/metallic-roughness/occlusion); the RHI format must match so the GPU sampler
    // sRGB-decodes colour but not data (the M6.3 colour-space rule, now enforced at upload).
    switch (format) {
        case assets::TextureFormat::Rgba8Srgb:
            return rhi::Format::RGBA8Srgb;
        case assets::TextureFormat::Rgba8Unorm:
        default:
            return rhi::Format::RGBA8Unorm;
    }
}

} // namespace

// placeholder_ is uploaded in the initializer list: device_ and server_ (declared before it) are
// already bound, so upload() may use them. The magenta placeholder is a valid 2×2 TextureAsset with
// a full mip chain, so it flows through the same upload path as any cooked texture.
GpuAssetBridge::GpuAssetBridge(rhi::Device& device, assets::AssetServer& server)
    : device_(device), server_(server), placeholder_(upload(server.placeholder_texture())) {}

assets::TextureAssetHandle GpuAssetBridge::request_texture(const std::filesystem::path& path) {
    const assets::TextureAssetHandle handle = server_.request_texture(path);
    tracked_.insert(handle.index); // a set, so a coalesced repeat request adds no duplicate work
    return handle;
}

std::size_t GpuAssetBridge::drain() {
    std::size_t newly_uploaded = 0;
    for (const std::uint32_t index : tracked_) {
        if (uploaded_.count(index) != 0) {
            continue; // already resident on the GPU
        }
        // get() returns the asset ONLY once it is Ready (nullptr while Loading, or if it Failed),
        // so this both gates on readiness and hands us the CPU bytes to upload. A failed load
        // simply never uploads, and texture_or_placeholder() keeps returning magenta for it — the
        // honest "this texture is missing" signal.
        if (const assets::TextureAsset* texture = server_.get(assets::TextureAssetHandle{index})) {
            uploaded_.emplace(index, upload(*texture));
            ++newly_uploaded;
        }
    }
    // Meshes, on the same seam and with the same readiness rule (m15.1). `MeshRegistry::add` does
    // the GPU upload, so a mesh becomes drawable the moment its id lands in uploaded_meshes_ — the
    // next resolve_scene_meshes() swaps the placeholder cube for it.
    if (mesh_sink_ != nullptr) {
        for (const auto& [id, handle] : by_id_) {
            if (!handle.is_valid() || uploaded_meshes_.count(handle.index) != 0) {
                continue;
            }
            if (const assets::MeshAsset* mesh = server_.get(handle)) {
                uploaded_meshes_.emplace(handle.index,
                                         mesh_sink_->add(mesh_from_cooked(*mesh), "cooked"));
                ++newly_uploaded;
            }
        }
    }
    return newly_uploaded;
}

rhi::TextureHandle GpuAssetBridge::texture_or_placeholder(assets::TextureAssetHandle handle) const {
    const auto it = uploaded_.find(handle.index);
    return it != uploaded_.end() ? it->second : placeholder_;
}

rhi::TextureHandle GpuAssetBridge::upload(const assets::TextureAsset& texture) {
    rhi::TextureDesc desc{};
    desc.extent = {texture.width, texture.height};
    desc.mip_levels = static_cast<std::uint32_t>(texture.mips.size());
    desc.format = to_rhi_format(texture.format);
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    desc.debug_name = "cooked-texture";
    const rhi::TextureHandle handle = device_.create_texture(desc);

    // The cook laid the chain out level-0-first, each mip a byte slice of `pixels` — exactly what
    // write_texture_mips consumes, so upload is one memcpy per level, no transform.
    std::vector<rhi::MipData> levels;
    levels.reserve(texture.mips.size());
    for (const assets::TextureMip& mip : texture.mips) {
        levels.push_back(
            rhi::MipData{std::span<const std::byte>(texture.pixels.data() + mip.offset, mip.size)});
    }
    device_.write_texture_mips(handle, levels);
    return handle;
}

// ── Meshes by content id (m15.1) ─────────────────────────────────────────────────────────────────

void GpuAssetBridge::set_mesh_sink(MeshRegistry& meshes, MaterialRegistry& materials) {
    mesh_sink_ = &meshes;
    material_sink_ = &materials;
    if (placeholders_built_) {
        return;
    }
    // A unit cube and a neutral grey, uploaded once. They are what a placed-but-not-yet-loaded mesh
    // shows, for the same reason the magenta texture exists: the draw path never branches on
    // readiness, so "still loading" looks like a grey box rather than like nothing at all.
    placeholder_mesh_ = meshes.add(make_cube(0.5f), "asset-placeholder");
    PbrMaterialDesc neutral{};
    neutral.base_color[0] = 0.72f;
    neutral.base_color[1] = 0.72f;
    neutral.base_color[2] = 0.74f;
    neutral.metallic = 0.0f;
    neutral.roughness = 0.7f;
    neutral_material_ = materials.add(neutral);
    placeholders_built_ = true;
}

void GpuAssetBridge::set_catalog(const assets::Manifest& manifest,
                                 std::filesystem::path cooked_dir) {
    catalog_ = &manifest;
    cooked_dir_ = std::move(cooked_dir);
}

assets::MeshAssetHandle GpuAssetBridge::request_mesh(assets::AssetId id) {
    if (const auto it = by_id_.find(id.value); it != by_id_.end()) {
        return it->second; // coalesced: one request and one upload per content id
    }
    if (catalog_ == nullptr) {
        unresolved_.insert(id.value);
        return {};
    }
    const assets::ManifestEntry* entry = catalog_->find_by_id(id);
    if (entry == nullptr) {
        // The manifest is the authority on what has been cooked. An id it does not know is a scene
        // referencing content this build does not have — counted, never guessed at.
        unresolved_.insert(id.value);
        return {};
    }
    const assets::MeshAssetHandle handle = server_.request_mesh(cooked_dir_ / entry->cooked_file);
    by_id_.emplace(id.value, handle);
    return handle;
}

MeshId GpuAssetBridge::mesh_or_placeholder(assets::MeshAssetHandle handle) const {
    if (handle.is_valid()) {
        if (const auto it = uploaded_meshes_.find(handle.index); it != uploaded_meshes_.end()) {
            return it->second;
        }
    }
    return placeholder_mesh_;
}

GpuAssetBridge::ResolveStats GpuAssetBridge::resolve_scene_meshes(ecs::World& world) {
    ResolveStats stats;
    if (mesh_sink_ == nullptr) {
        return stats; // no sink: nothing to resolve into, and saying so beats pretending
    }

    // Collect, then mutate. `add_component` moves an entity between archetypes, which reallocates
    // the chunk vector a query is walking — the same trap m13.5 hit twice.
    struct Pending {
        ecs::Entity entity;
        MeshId mesh;
        bool needs_material;
    };

    std::vector<Pending> pending;
    world.query<MeshAsset>().for_each([&](ecs::Entity e, MeshAsset& asset) {
        if (asset.asset == 0) {
            return;
        }
        const assets::MeshAssetHandle handle = request_mesh(assets::AssetId{asset.asset});
        if (!handle.is_valid()) {
            ++stats.unresolved;
            return;
        }
        const MeshId id = mesh_or_placeholder(handle);
        if (id == placeholder_mesh_) {
            ++stats.pending;
        }
        const MeshRef* existing = world.get<MeshRef>(e);
        const bool needs_material = world.get<MaterialRef>(e) == nullptr;
        if (existing != nullptr && existing->mesh == id && !needs_material) {
            return; // already correct — the steady state costs one query and nothing else
        }
        pending.push_back(Pending{e, id, needs_material});
    });

    for (const Pending& p : pending) {
        if (MeshRef* ref = world.get<MeshRef>(p.entity)) {
            ref->mesh = p.mesh;
        } else {
            world.add_component(p.entity, MeshRef{p.mesh});
        }
        if (p.needs_material) {
            world.add_component(p.entity, MaterialRef{neutral_material_});
        }
        ++stats.resolved;
    }
    return stats;
}

} // namespace rime::render
