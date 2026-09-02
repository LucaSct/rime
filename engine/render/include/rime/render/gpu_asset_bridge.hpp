// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/asset_server.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/rhi/types.hpp"

// The GPU asset bridge: turns CPU-resident cooked assets (engine/assets) into live GPU resources on
// the frame thread. engine/assets links neither rhi nor render, so a "Ready" asset is validated CPU
// bytes, not an uploaded texture — the render layer owns GPU residency. This is the single allowed
// assets↔render edge: render *consumes* cooked assets; assets never depends on render (ADR-0025).
//
// Each frame, after AssetServer::pump() readies CPU loads, drain() uploads any newly-ready texture
// through the RHI and caches its handle, so a material's borrowed placeholder can be swapped for
// the real texture. This is the FIRST consumer of the RHI's per-mip upload path
// (Device::write_texture_mips, built in M6.3): the cook generated the whole gamma-correct mip chain
// offline, so the bridge uploads each level verbatim rather than regenerating on the GPU.
//
// MESHES JOIN AT m15.1, and they close the gap ADR-0038 calls the one architectural item in M15:
// there was no engine-level answer to "how does a scene file name a mesh". `render::MeshRef` is a
// dense index into a runtime registry — correct only while every loader builds its registries in
// identical order — and `render::MeshAsset{u64}` was a stable content id that nothing resolved. So
// the editor's asset browser could place a cooked mesh and the result neither drew nor moved, and
// every game had to reinvent `blockkit`'s SlabRole + apply_palette to get a look onto a scene.
//
// The chain this closes, and every link but one already existed:
//
//   MeshAsset{u64}  ->  assets::AssetId
//                   ->  Manifest::find_by_id   (the id -> cooked-file resolver, `rime cook` writes
//                   it)
//                   ->  AssetServer::request_mesh (async IO + parse, path-coalesced)
//                   ->  render::mesh_from_cooked  (the byte blob -> the 48-byte vertex)
//                   ->  MeshRegistry::add        (the GPU upload)
//                   ->  MeshRef{MeshId}          (what the renderer consumes every frame)
//
// MeshRef STAYS A DENSE INDEX. That is the right thing for the hot path; the fix is that a SCENE
// FILE stops carrying one, not that the runtime stops using one (ADR-0038, alternatives).
namespace rime::rhi {
class Device; // used only by reference here — forward-declared to keep this header light
}

namespace rime::assets {
class Manifest;
}

namespace rime::ecs {
class World;
}

namespace rime::render {

class GpuAssetBridge {
public:
    // Uploads the AssetServer's magenta placeholder texture once, so texture_or_placeholder() can
    // return a valid handle for a still-loading texture without a per-call branch.
    GpuAssetBridge(rhi::Device& device, assets::AssetServer& server);

    GpuAssetBridge(const GpuAssetBridge&) = delete;
    GpuAssetBridge& operator=(const GpuAssetBridge&) = delete;

    // Request a texture and track it for GPU upload. Forwards to the AssetServer (so
    // path-coalescing, the async job, and the CPU placeholder all apply) and remembers the handle
    // so drain() uploads it once the load completes. Repeat requests for the same path coalesce to
    // one handle and one upload.
    [[nodiscard]] assets::TextureAssetHandle request_texture(const std::filesystem::path& path);

    // Frame thread, once per frame right after AssetServer::pump(): upload every tracked texture
    // that has become Ready and is not yet on the GPU. Returns how many were newly uploaded.
    // Idempotent — an already-uploaded or still-loading texture is skipped.
    std::size_t drain();

    // The GPU handle for a requested texture: the uploaded texture once drained, otherwise the
    // magenta placeholder. Never invalid, so material binding never branches on "is it loaded
    // yet?".
    [[nodiscard]] rhi::TextureHandle
    texture_or_placeholder(assets::TextureAssetHandle handle) const;

    [[nodiscard]] rhi::TextureHandle placeholder_texture() const noexcept { return placeholder_; }

    // ── Meshes by content id (m15.1) ────────────────────────────────────────────────────
    //
    // Where uploaded meshes land, and the neutral material a freshly-placed one is shaded with.
    // Both are the caller's, because a game owns its registries — the bridge only fills them.
    //
    // The material matters more than it looks: SceneRenderer draws
    // `<WorldTransform, MeshRef, MaterialRef>`, so a mesh with no material is not a dim object, it
    // is an absent one. Giving a placed mesh a neutral grey is the same argument as the magenta
    // texture placeholder above — the pipeline never branches on "is it set up yet?", and the
    // failure a user sees is "that looks untextured", not "nothing happened".
    void set_mesh_sink(MeshRegistry& meshes, MaterialRegistry& materials);

    // The id -> cooked-file resolver. `rime cook` writes the manifest beside the cooked files;
    // `cooked_dir` is the directory those filenames are relative to. Without a catalog,
    // `request_mesh(AssetId)` cannot resolve and says so through `unresolved()` rather than
    // silently doing nothing.
    void set_catalog(const assets::Manifest& manifest, std::filesystem::path cooked_dir);

    // Request a cooked mesh by its content id. Coalesces per id; the upload happens in drain().
    [[nodiscard]] assets::MeshAssetHandle request_mesh(assets::AssetId id);

    // The registry id for a requested mesh once uploaded, or the placeholder cube until then.
    [[nodiscard]] MeshId mesh_or_placeholder(assets::MeshAssetHandle handle) const;

    // Walk `world` and give every entity that names a mesh by content id (`MeshAsset`) the
    // `MeshRef` — and, if it has none, the `MaterialRef` — that the renderer needs to draw it.
    //
    // Call once per frame after drain(). Idempotent: an entity that already has a MeshRef pointing
    // at that asset's uploaded mesh is skipped, so the steady state is one query.
    struct ResolveStats {
        std::size_t resolved = 0;   // entities that gained (or updated) a MeshRef this call
        std::size_t pending = 0;    // still loading — they hold the placeholder for now
        std::size_t unresolved = 0; // no catalog, or an id the manifest does not know
    };

    ResolveStats resolve_scene_meshes(ecs::World& world);

    // Give every entity whose mesh has resolved the MATERIALS its submeshes name (m16.3).
    //
    // This is the half m15.1 left out, and the reason a scene-placed mesh has drawn neutral grey
    // ever since. The join is the manifest's `#materialN` convention, per ADR-0039 ruling 1: a
    // mesh's manifest entry gives its source path, a submesh's `material_slot` gives the N, and
    // `find_by_source` gives the material's content id. It is a string convention rather than a
    // field in the mesh payload because an AssetId is the hash of its payload — embedding material
    // ids in a mesh would make recompressing a texture change every mesh id, and mesh ids are what
    // `.rscene` files carry.
    //
    // FOUR LEVELS DEEP, which is why `settle` exists: mesh Ready → read material_slot → material
    // Ready → read its five texture ids → textures Ready → final desc. Each level needs a pump and
    // a drain before the next can even be requested, so one blocking round cannot converge.
    struct MaterialStats {
        std::size_t resolved = 0;        // entities that gained (or updated) a material set
        std::size_t pending = 0;         // waiting on a material or one of its textures
        std::size_t unresolved = 0;      // no `#materialN` line for a slot the mesh names
        std::size_t slots_defaulted = 0; // a submesh slot no material could be found for
    };

    MaterialStats resolve_scene_materials(ecs::World& world);

    // Drive the whole four-level chain to quiescence: pump, drain, resolve meshes, resolve
    // materials, repeat — until a round changes nothing or `max_rounds` is spent.
    //
    // Returns the number of rounds actually taken. A caller that gets `max_rounds` back should
    // treat it as "did not converge" and look at the pending counters, NOT as success: silently
    // giving up is how a scene ends up half-textured with everything reporting fine.
    std::size_t settle(ecs::World& world, std::size_t max_rounds = 8);

    [[nodiscard]] const MaterialStats& material_stats() const noexcept { return material_stats_; }

    // Ids the catalog could not resolve, deduplicated. A COUNTER RATHER THAN A LOG LINE: a scene
    // that silently drew nothing is the failure this whole brick exists to end, so "which asset did
    // you not find" has to be answerable after the fact.
    [[nodiscard]] std::size_t unresolved_count() const noexcept { return unresolved_.size(); }

    [[nodiscard]] std::size_t meshes_uploaded() const noexcept { return uploaded_meshes_.size(); }

    // How many textures the bridge has uploaded to the GPU (the upload-once counter a proof
    // asserts).
    [[nodiscard]] std::size_t uploaded_count() const noexcept { return uploaded_.size(); }

    // Where per-submesh material sets are minted. Null until set_mesh_sink; exposed so the renderer
    // can resolve a submesh's slot when it builds the draw list.
    [[nodiscard]] const MaterialSetRegistry* material_sets() const noexcept {
        return &material_sets_;
    }

private:
    // Request a cooked material by content id, coalescing per id like request_mesh.
    [[nodiscard]] assets::MaterialAssetHandle request_material(assets::AssetId id);

    // The by-content-id sibling of request_texture(path): materials name their maps by id, and the
    // manifest is what turns one into a file.
    [[nodiscard]] assets::TextureAssetHandle request_texture_by_id(assets::AssetId id);

    // Build the final desc for a Ready material: factors from the cook, textures requested and
    // resolved to placeholders until they drain. Returns false while any texture is still pending.
    bool build_material(const assets::MaterialAsset& cooked, PbrMaterialDesc& out);
    // Create an RHI texture from a cooked TextureAsset and upload its whole mip chain verbatim.
    [[nodiscard]] rhi::TextureHandle upload(const assets::TextureAsset& texture);

    rhi::Device& device_;
    assets::AssetServer& server_;
    rhi::TextureHandle placeholder_{};          // magenta, uploaded once at ctor
    std::unordered_set<std::uint32_t> tracked_; // requested texture-handle indices
    std::unordered_map<std::uint32_t, rhi::TextureHandle> uploaded_; // handle index → GPU texture

    // Meshes (m15.1). All null/empty until set_mesh_sink + set_catalog, which is what makes this
    // additive: a caller that only wants textures is untouched.
    MeshRegistry* mesh_sink_ = nullptr;
    MaterialRegistry* material_sink_ = nullptr;
    const assets::Manifest* catalog_ = nullptr;
    std::filesystem::path cooked_dir_;
    MeshId placeholder_mesh_{};
    MaterialId neutral_material_{};
    bool placeholders_built_ = false;
    std::unordered_map<std::uint64_t, assets::MeshAssetHandle> by_id_; // AssetId → request
    std::unordered_map<std::uint32_t, MeshId> uploaded_meshes_;        // handle index → registry id
    std::unordered_set<std::uint64_t> unresolved_;

    // Materials (m16.3).
    MaterialSetRegistry material_sets_;
    std::unordered_map<std::uint64_t, assets::MaterialAssetHandle> mat_by_id_; // AssetId → request
    std::unordered_map<std::uint64_t, assets::TextureAssetHandle> tex_by_id_;
    std::unordered_map<std::uint64_t, MaterialId> material_of_id_; // content id → registry material
    std::unordered_map<std::uint32_t, MaterialSetId> set_of_entity_; // entity index → its set
    MaterialStats material_stats_{};
};

} // namespace rime::render
