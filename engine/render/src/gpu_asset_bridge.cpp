// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/render/gpu_asset_bridge.hpp"

#include <span>
#include <vector>

#include "rime/assets/manifest.hpp"
#include "rime/assets/material_asset.hpp"
#include "rime/assets/mesh_asset.hpp"
#include "rime/assets/texture_asset.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/rhi/device.hpp"
#include "rime/rhi/resources.hpp"

namespace rime::render {

PbrMaterialDesc material_from_cooked(const assets::MaterialAsset& m) noexcept {
    PbrMaterialDesc d{};
    d.base_color[0] = m.base_color[0];
    d.base_color[1] = m.base_color[1];
    d.base_color[2] = m.base_color[2];
    d.base_color[3] = m.base_color[3];
    d.metallic = m.metallic;
    d.roughness = m.roughness;
    d.emissive[0] = m.emissive[0];
    d.emissive[1] = m.emissive[1];
    d.emissive[2] = m.emissive[2];
    d.normal_scale = m.normal_scale;
    d.occlusion_strength = m.occlusion_strength;
    // Only Mask discards; Opaque and Blend both leave the cutoff at zero, which the shader reads as
    // "never mask". See the header for why one float expresses all three modes.
    d.alpha_cutoff = m.alpha_mode == assets::AlphaMode::Mask ? m.alpha_cutoff : 0.0f;
    d.double_sided = m.double_sided;
    d.clamp_uv = m.clamp_uv;
    return d;
}

namespace {

rhi::Format to_rhi_format(assets::TextureFormat format) noexcept {
    // The cook tags each texture sRGB or linear by *semantic* (base-color/emissive vs
    // normal/metallic-roughness/occlusion); the RHI format must match so the GPU sampler
    // sRGB-decodes colour but not data (the M6.3 colour-space rule, now enforced at upload).
    //
    // THE BLOCK-COMPRESSED CASES WERE MISSING UNTIL m17.8b, and the way they failed is worth
    // keeping. m16.7 shipped the BC7 encoder, the reader and an RHI test, but nothing had yet
    // *cooked* a compressed texture into a scene — so this function's `default:` quietly answered
    // "RGBA8Unorm" for all three block formats. The first consumer was the cooked ground, whose
    // cook runs with `--bc`: a 512x512 BC7 payload is 350 KB and the RGBA8 image the GPU was then
    // told to fill is 1 MB, so `write_texture_mips` read a megabyte out of a 350 KB staging buffer
    // (VUID-vkCmdCopyBufferToImage-pRegions-00171, ten of them per run) and sampled whatever
    // followed it in host memory.
    //
    // Nothing caught it. The proof asserted the material's texture was resident and not the magenta
    // placeholder, which was TRUE — the upload happened, it just read past its source and decoded
    // the wrong bytes. A `default:` that silently answers for a case it has never seen is how a
    // switch stops being exhaustive without anyone editing it, so the block cases are spelled out
    // and `default:` is gone: adding a seventh format is now a compile error here, not a wrong
    // texture at runtime.
    switch (format) {
        case assets::TextureFormat::Rgba8Srgb:
            return rhi::Format::RGBA8Srgb;
        case assets::TextureFormat::Bc7Srgb:
            return rhi::Format::BC7Srgb;
        case assets::TextureFormat::Bc7Unorm:
            return rhi::Format::BC7Unorm;
        case assets::TextureFormat::Bc5Unorm:
            return rhi::Format::BC5Unorm;
        case assets::TextureFormat::Rgba8Unorm:
            break;
    }
    return rhi::Format::RGBA8Unorm;
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
            // A refused upload (see `upload`) returns an invalid handle and must NOT be recorded:
            // `texture_or_placeholder` answers from `uploaded_` first, so caching the invalid
            // handle would hand a broken texture to the sampler instead of the magenta placeholder.
            const rhi::TextureHandle uploaded = upload(*texture, index);
            if (!uploaded.is_valid()) {
                continue;
            }
            uploaded_.emplace(index, uploaded);
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

rhi::TextureHandle GpuAssetBridge::upload(const assets::TextureAsset& texture,
                                          std::optional<std::uint32_t> tracked_index) {
    rhi::TextureDesc desc{};
    desc.extent = {texture.width, texture.height};
    desc.mip_levels = static_cast<std::uint32_t>(texture.mips.size());
    desc.format = to_rhi_format(texture.format);
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    desc.debug_name = "cooked-texture";

    // BC is not universal: the Vulkan spec requires a device to support BC *or* ETC2 *or* ASTC, so
    // a mobile-class or software driver may legitimately refuse. ADR-0039's ratified rule, restated
    // on `AdapterInfo::block_compression` itself, is that such a device must FAIL the load "with a
    // named counter and a warn-once... never silently substitute the placeholder, because a
    // fallback path nothing exercises is a fallback that does not work". So: refuse, say so once,
    // and count it. Returning an invalid handle keeps the texture out of `uploaded_`, which is what
    // makes `texture_or_placeholder` keep answering magenta — visible, and now also countable.
    if (rhi::is_block_compressed(desc.format) && !device_.adapter().block_compression) {
        // Count the TEXTURE, not the attempt. `drain` retries every tracked index that is not yet
        // resident on every call, and a refused upload is deliberately never recorded as resident —
        // so it is retried and re-refused forever. Incrementing here counted those retries.
        if (tracked_index) {
            refused_unsupported_.insert(*tracked_index);
        }
        if (!warned_no_bc_) {
            warned_no_bc_ = true;
            RIME_WARN(
                "gpu_asset_bridge: this adapter has no BC texture support, so block-compressed "
                "cooked textures cannot be uploaded — they will sample as the magenta placeholder. "
                "Re-cook without --bc for this device.");
        }
        return {};
    }

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
        // Mark the entity as carrying engine-computed state, so a save excludes the MeshRef and
        // MaterialRef just stamped on it — without touching an entity that AUTHORED those same
        // components against its own registry (m16.8).
        if (world.get<ecs::DerivedComponents>(p.entity) == nullptr) {
            world.add_component(p.entity, ecs::DerivedComponents{});
        }
        ++stats.resolved;
    }
    meshes_resolved_ += stats.resolved;
    return stats;
}

assets::MaterialAssetHandle GpuAssetBridge::request_material(assets::AssetId id) {
    if (const auto it = mat_by_id_.find(id.value); it != mat_by_id_.end()) {
        return it->second;
    }
    if (catalog_ == nullptr) {
        unresolved_.insert(id.value);
        return {};
    }
    const assets::ManifestEntry* entry = catalog_->find_by_id(id);
    if (entry == nullptr) {
        unresolved_.insert(id.value);
        return {};
    }
    const assets::MaterialAssetHandle handle =
        server_.request_material(cooked_dir_ / entry->cooked_file);
    mat_by_id_.emplace(id.value, handle);
    return handle;
}

assets::TextureAssetHandle GpuAssetBridge::request_texture_by_id(assets::AssetId id) {
    if (const auto it = tex_by_id_.find(id.value); it != tex_by_id_.end()) {
        return it->second;
    }
    if (catalog_ == nullptr) {
        unresolved_.insert(id.value);
        return {};
    }
    const assets::ManifestEntry* entry = catalog_->find_by_id(id);
    if (entry == nullptr) {
        unresolved_.insert(id.value);
        return {};
    }
    const assets::TextureAssetHandle handle = request_texture(cooked_dir_ / entry->cooked_file);
    tex_by_id_.emplace(id.value, handle);
    return handle;
}

bool GpuAssetBridge::build_material(const assets::MaterialAsset& cooked,
                                    PbrMaterialDesc& out,
                                    std::size_t& unresolved_textures) {
    out = material_from_cooked(cooked);

    // The second level of the dependency: the material's texture ids are only knowable now that it
    // is Ready. Request each, then resolve it — `texture_or_placeholder` hands back magenta until
    // the upload drains, so the material is usable immediately and sharpens later.
    bool all_resident = true;
    const auto slot = [&](assets::AssetId id, rhi::TextureHandle& dest) {
        if (id.value == 0) {
            return; // an empty slot is not a pending one: the shader's 1x1 fallback is correct
        }
        const assets::TextureAssetHandle h = request_texture_by_id(id);
        if (!h.is_valid()) {
            ++unresolved_textures;
            return;
        }
        dest = texture_or_placeholder(h);
        if (dest == placeholder_) {
            all_resident = false;
        }
    };
    slot(cooked.base_color_tex, out.base_color_texture);
    slot(cooked.metallic_roughness_tex, out.metallic_roughness_texture);
    slot(cooked.normal_tex, out.normal_texture);
    slot(cooked.occlusion_tex, out.occlusion_texture);
    slot(cooked.emissive_tex, out.emissive_texture);
    return all_resident;
}

GpuAssetBridge::MaterialResolve GpuAssetBridge::resolve_material(assets::AssetId id,
                                                                 MaterialId& out,
                                                                 bool& complete,
                                                                 std::size_t& unresolved_textures) {
    if (const auto known = material_of_id_.find(id.value); known != material_of_id_.end()) {
        out = known->second;
        // Built already — but "built" is not "resident". A material assembled in an earlier round
        // can still be holding the magenta placeholder in any slot whose texture had not finished
        // uploading, and only build_material can tell. Re-run it until every slot is real,
        // sharpening the registry entry in place (MaterialRegistry::update mints no id, so every
        // MaterialRef and MaterialSet naming it stays valid across the swap).
        if (material_incomplete_.count(id.value) != 0) {
            const assets::MaterialAsset* ready = server_.get(request_material(id));
            if (ready == nullptr) {
                complete = false;
                return MaterialResolve::Resolved;
            }
            PbrMaterialDesc sharpened{};
            if (build_material(*ready, sharpened, unresolved_textures)) {
                material_incomplete_.erase(id.value);
            } else {
                complete = false;
            }
            material_sink_->update(known->second, sharpened);
        }
        return MaterialResolve::Resolved;
    }
    const assets::MaterialAssetHandle mh = request_material(id);
    if (!mh.is_valid()) {
        return MaterialResolve::Unresolved;
    }
    const assets::MaterialAsset* cooked = server_.get(mh);
    if (cooked == nullptr) {
        complete = false; // still loading — level two
        return MaterialResolve::Loading;
    }
    PbrMaterialDesc desc{};
    if (!build_material(*cooked, desc, unresolved_textures)) {
        complete = false; // a texture is still streaming — level three
        // Remember that this one is unfinished, or the cache hit above will hand its
        // placeholder-holding descriptor back on every later round without ever noticing.
        material_incomplete_.insert(id.value);
    }
    out = material_sink_->add(desc);
    material_of_id_.emplace(id.value, out);
    return MaterialResolve::Resolved;
}

GpuAssetBridge::MaterialStats GpuAssetBridge::resolve_scene_materials(ecs::World& world) {
    MaterialStats stats;
    if (mesh_sink_ == nullptr || material_sink_ == nullptr || catalog_ == nullptr) {
        return stats;
    }

    struct Pending {
        ecs::Entity entity;
        std::vector<MaterialId> materials;
    };

    std::vector<Pending> pending;

    world.query<MeshAsset>().for_each([&](ecs::Entity e, MeshAsset& asset) {
        if (asset.asset == 0) {
            return;
        }
        // The mesh must be uploaded before its submesh table — and therefore its material slots —
        // can be read at all. This is the first level of the chain.
        const auto req = by_id_.find(asset.asset);
        if (req == by_id_.end() || !req->second.is_valid()) {
            return;
        }
        const auto up = uploaded_meshes_.find(req->second.index);
        if (up == uploaded_meshes_.end()) {
            ++stats.pending; // mesh not resident yet; nothing to join against
            return;
        }
        const assets::ManifestEntry* mesh_entry =
            catalog_->find_by_id(assets::AssetId{asset.asset});
        if (mesh_entry == nullptr) {
            return; // already counted by request_mesh
        }
        const GpuMesh& gpu = mesh_sink_->get(up->second);

        // How many slots this mesh names. The table is guaranteed non-empty by MeshRegistry::add.
        std::uint32_t slot_count = 0;
        for (const SubmeshRange& sm : gpu.submeshes) {
            slot_count = std::max(slot_count, sm.material_slot + 1);
        }
        std::vector<MaterialId> materials(slot_count, kInvalidMaterialId);

        bool complete = true;
        for (std::uint32_t slot = 0; slot < slot_count; ++slot) {
            // ADR-0039 ruling 1: the edge is the manifest's `#materialN` convention.
            const std::string label = mesh_entry->source_path + "#material" + std::to_string(slot);
            const assets::ManifestEntry* mat_entry = catalog_->find_by_source(label);
            if (mat_entry == nullptr) {
                ++stats.slots_defaulted;
                continue; // no material cooked for this slot — the fallback MaterialRef stands
            }
            // From here down the chain is the same one an entity-owned reference walks, so it is
            // one function (m17.8b): material Ready, its textures Ready, a registry entry that
            // sharpens in place.
            MaterialId resolved = kInvalidMaterialId;
            switch (resolve_material(mat_entry->id, resolved, complete, stats.unresolved)) {
                case MaterialResolve::Unresolved:
                    ++stats.unresolved;
                    break;
                case MaterialResolve::Loading:
                    break; // counted through `complete`; the slot stays invalid this round
                case MaterialResolve::Resolved:
                    materials[slot] = resolved;
                    break;
            }
        }

        if (!complete) {
            ++stats.pending;
        }
        pending.push_back(Pending{e, std::move(materials)});
    });

    // Collect, then mutate — add_component moves an entity between archetypes and would reallocate
    // the chunks the query above is walking.
    for (Pending& p : pending) {
        bool any = false;
        for (const MaterialId id : p.materials) {
            any = any || id != kInvalidMaterialId;
        }
        if (!any) {
            continue; // nothing resolved for this mesh yet; leave its fallback MaterialRef alone
        }
        const auto existing = set_of_entity_.find(p.entity.index);
        if (existing == set_of_entity_.end()) {
            const MaterialSetId set = material_sets_.add(std::move(p.materials));
            set_of_entity_.emplace(p.entity.index, set);
            if (MaterialSet* comp = world.get<MaterialSet>(p.entity)) {
                comp->set = set;
            } else {
                world.add_component(p.entity, MaterialSet{set});
            }
            if (world.get<ecs::DerivedComponents>(p.entity) == nullptr) {
                world.add_component(p.entity, ecs::DerivedComponents{});
            }
        } else {
            // UPDATE IN PLACE rather than minting a new id, so a set resolved before its textures
            // streamed in sharpens without the entity's component going stale. This is the same
            // mechanism MaterialRegistry::update provides one level down, and it is what replaces
            // the never-revisited neutral grey that made this whole brick necessary.
            material_sets_.update(existing->second, std::move(p.materials));
        }
        ++stats.resolved;
    }

    material_stats_ = stats;
    return stats;
}

GpuAssetBridge::MaterialAssetStats GpuAssetBridge::resolve_material_assets(ecs::World& world) {
    MaterialAssetStats stats;
    if (material_sink_ == nullptr) {
        return stats; // no registry to resolve into — nothing can be built, and saying so beats
                      // pretending
    }

    struct Pending {
        ecs::Entity entity;
        MaterialId material;
    };

    std::vector<Pending> pending;
    world.query<MaterialAsset>().for_each([&](ecs::Entity e, MaterialAsset& asset) {
        if (asset.asset == 0) {
            return; // unset is not unresolved: the entity simply has not named anything
        }
        bool complete = true;
        MaterialId resolved = kInvalidMaterialId;
        // A missing catalog lands here as Unresolved through `request_material`, which records
        // the id in `unresolved_` as well — so "which id could you not find" stays answerable.
        switch (
            resolve_material(assets::AssetId{asset.asset}, resolved, complete, stats.unresolved)) {
            case MaterialResolve::Unresolved:
                ++stats.unresolved;
                return; // whatever MaterialRef the entity has — a palette's, a fallback's — stands
            case MaterialResolve::Loading:
                ++stats.pending;
                return;
            case MaterialResolve::Resolved:
                break;
        }
        if (!complete) {
            ++stats.pending; // drawable now (placeholder in a slot), sharpened on a later call
        }
        const MaterialRef* existing = world.get<MaterialRef>(e);
        if (existing != nullptr && existing->material == resolved) {
            ++stats.steady; // the steady state: one query, no structural change — but counted,
            return;         // or a converged resolver is indistinguishable from an idle one
        }
        pending.push_back(Pending{e, resolved});
    });

    // Collect, then mutate — add_component moves an entity between archetypes and would
    // reallocate the chunks the query above is walking.
    for (const Pending& p : pending) {
        if (MaterialRef* ref = world.get<MaterialRef>(p.entity)) {
            ref->material = p.material; // outranks the derived look that was there
        } else {
            world.add_component(p.entity, MaterialRef{p.material});
        }
        if (world.get<ecs::DerivedComponents>(p.entity) == nullptr) {
            world.add_component(p.entity, ecs::DerivedComponents{});
        }
        ++stats.resolved;
    }
    material_asset_stats_ = stats;
    return stats;
}

std::size_t GpuAssetBridge::settle(ecs::World& world, std::size_t max_rounds) {
    std::size_t round = 0;
    for (; round < max_rounds; ++round) {
        server_.wait_for_pending_loads();
        server_.pump();
        (void)drain();
        const ResolveStats meshes = resolve_scene_meshes(world);
        const MaterialStats mats = resolve_scene_materials(world);
        const MaterialAssetStats owned = resolve_material_assets(world);
        // Quiescent: nothing is waiting on a level below it. Note this deliberately does NOT check
        // `resolved`, which stays nonzero in the steady state.
        if (meshes.pending == 0 && mats.pending == 0 && owned.pending == 0) {
            return round + 1;
        }
    }
    return round;
}

} // namespace rime::render
