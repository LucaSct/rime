// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
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
        std::size_t resolved = 0; // entities that gained (or updated) a material set
        std::size_t pending = 0;  // waiting on a material or one of its textures
        // A material OR texture id the manifest does not know. Counts per slot per round and
        // mixes the two levels, so it answers "something was missing" rather than "how many
        // distinct assets are missing" — `unresolved_count()` is the deduplicated set of ids.
        std::size_t unresolved = 0;
        std::size_t slots_defaulted = 0; // no `#materialN` line for a slot the mesh names
    };

    MaterialStats resolve_scene_materials(ecs::World& world);

    // Give every entity that names a material IN ITS OWN RIGHT (`MaterialAsset`) the `MaterialRef`
    // it resolves to (m17.8b).
    //
    // The third resolver, and the first that does not start from a mesh. `resolve_scene_materials`
    // finds a material through the mesh that owns it; a surface whose mesh is DERIVED rather than
    // cooked — the ground, whose `derive_mesh` writes its own vertices — has no cooked mesh to join
    // through, so it names the material by content id directly. Same chain from the material
    // down (material Ready → its textures Ready → a sharpened registry entry), same cache
    // (`material_of_id_`), so an entity-owned reference and a mesh-owned slot naming the same
    // cooked bytes share one registry material and one set of uploads.
    //
    // The resolved `MaterialRef` OUTRANKS a derived one: this overwrites whatever a palette or a
    // fallback stamped, and anything that derives a look must skip entities carrying a
    // `MaterialAsset` (blockkit's palette does) or the two would fight every frame. Idempotent in
    // the steady state — an entity whose MaterialRef already names the resolved material costs one
    // lookup. Tags the entity `DerivedComponents`, so a save keeps the authored reference and drops
    // the index (ADR-0039 ruling 4).
    struct MaterialAssetStats {
        std::size_t resolved = 0;   // entities that gained (or updated) a MaterialRef this call
        std::size_t pending = 0;    // waiting on the material or one of its textures
        std::size_t unresolved = 0; // no catalog, or an id the manifest does not know
        // Entities already wearing the material their `MaterialAsset` names — the steady state.
        //
        // Counted because `resolved` alone goes to ZERO once everything has converged (this
        // resolver skips an entity whose MaterialRef is already right, to avoid churning the
        // archetype every frame), and a caller asking "did the cooked ground actually take?" after
        // `settle` would read 0 and conclude nothing had. `resolved + steady` is the number of
        // entity-owned materials standing. Note the mesh-owned resolver differs deliberately: its
        // `resolved` restamps and so stays nonzero forever, which is what `settle`'s comment about
        // not checking it means.
        std::size_t steady = 0;
    };

    MaterialAssetStats resolve_material_assets(ecs::World& world);

    [[nodiscard]] const MaterialAssetStats& material_asset_stats() const noexcept {
        return material_asset_stats_;
    }

    // Drive the whole four-level chain to quiescence: pump, drain, resolve meshes, resolve
    // materials (mesh-owned and entity-owned), repeat — until a round changes nothing or
    // `max_rounds` is spent.
    //
    // Returns the number of rounds actually taken. A caller that gets `max_rounds` back should
    // treat it as "did not converge" and look at the pending counters, NOT as success: silently
    // giving up is how a scene ends up half-textured with everything reporting fine.
    std::size_t settle(ecs::World& world, std::size_t max_rounds = 8);

    [[nodiscard]] const MaterialStats& material_stats() const noexcept { return material_stats_; }

    // Meshes resolved across every resolve_scene_meshes call, cumulative. `settle` does the
    // resolving itself, so a caller that ran it and then asked resolve_scene_meshes again would
    // correctly get zero — the work already happened. This is what such a caller should report.
    [[nodiscard]] std::size_t meshes_resolved() const noexcept { return meshes_resolved_; }

    // Ids the catalog could not resolve, deduplicated. A COUNTER RATHER THAN A LOG LINE: a scene
    // that silently drew nothing is the failure this whole brick exists to end, so "which asset did
    // you not find" has to be answerable after the fact.
    [[nodiscard]] std::size_t unresolved_count() const noexcept { return unresolved_.size(); }

    [[nodiscard]] std::size_t meshes_uploaded() const noexcept { return uploaded_meshes_.size(); }

    // How many textures the bridge has uploaded to the GPU (the upload-once counter a proof
    // asserts).
    [[nodiscard]] std::size_t uploaded_count() const noexcept { return uploaded_.size(); }

    // Block-compressed textures this adapter could not accept (m17.8b). Nonzero means the cook and
    // the device disagree: the pixels are magenta and re-cooking without `--bc` is the fix. Counted
    // rather than merely logged, so a proof can assert on it.
    [[nodiscard]] std::size_t textures_refused_unsupported() const noexcept {
        return refused_unsupported_.size();
    }

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
    // `unresolved_textures` counts slots whose texture id the catalog does not know — handed back
    // rather than counted into a member, because the caller owns the stats for its own call and
    // used to overwrite the member's count at the end of the same call (a pre-m17.8b defect: a
    // material naming a texture the manifest lacked reported `unresolved == 0`).
    bool build_material(const assets::MaterialAsset& cooked,
                        PbrMaterialDesc& out,
                        std::size_t& unresolved_textures);

    // The ONE place a cooked material id becomes a registry MaterialId (m17.8b), shared by the
    // mesh-owned `#materialN` path and the entity-owned `MaterialAsset` path so they cannot drift.
    // `Resolved` writes `out` and may clear `complete` — the registry entry exists but still holds
    // the magenta placeholder in a slot whose texture has not drained, and a later call sharpens it
    // in place (MaterialRegistry::update mints no id, so every MaterialRef stays valid across the
    // swap). `Loading` means the material record itself is not Ready yet; `Unresolved` means the
    // catalog cannot turn the id into a file at all.
    enum class MaterialResolve { Unresolved, Loading, Resolved };
    MaterialResolve resolve_material(assets::AssetId id,
                                     MaterialId& out,
                                     bool& complete,
                                     std::size_t& unresolved_textures);
    // Create an RHI texture from a cooked TextureAsset and upload its whole mip chain verbatim.
    // `tracked_index` is the streaming index the texture is tracked under, when there is one, and
    // exists purely so a refusal can be counted ONCE PER TEXTURE. The constructor's placeholder
    // upload has no index and needs none: it is never block-compressed.
    [[nodiscard]] rhi::TextureHandle
    upload(const assets::TextureAsset& texture,
           std::optional<std::uint32_t> tracked_index = std::nullopt);

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
    // Materials built in an earlier round whose textures had not finished streaming, so their
    // registry entry is still holding the magenta placeholder in at least one slot. Keyed by cooked
    // content id, same as material_of_id_. Without this the "already built it" cache below is a
    // never-revisit short-circuit: the material resolves once, renders magenta forever, and — the
    // part that actually hurts — `pending` reads 0 because the skip path never touches it.
    std::unordered_set<std::uint64_t> material_incomplete_;
    std::unordered_map<std::uint32_t, MaterialSetId> set_of_entity_; // entity index → its set
    MaterialStats material_stats_{};
    MaterialAssetStats material_asset_stats_{}; // m17.8b: the entity-owned resolver's last call
    std::size_t meshes_resolved_ = 0;
    // m17.8b: BC asked of a device without BC, DEDUPLICATED BY TEXTURE. `drain` retries every
    // non-resident tracked index on every call, so a plain counter here counted retries rather than
    // textures — three textures over eight settle rounds would read as "24 textures refused", and
    // the editor's per-frame drain would grow it without bound. A set makes the number mean what
    // its name says.
    std::unordered_set<std::uint32_t> refused_unsupported_;
    bool warned_no_bc_ = false; // the warn-once, no longer inferred from a count of 1
};

} // namespace rime::render
