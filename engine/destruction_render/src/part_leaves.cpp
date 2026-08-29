// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction_render/part_leaves.hpp"

#include <cmath>
#include <span>

#include "rime/core/math.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/physics/physics.hpp"
#include "rime/render/components.hpp"

namespace rime::destruction_render {
namespace {

// One cooked convex part → a drawable mesh. Each face is fan-triangulated with its OWN copies of
// the vertices, so the flat face normal is never averaged across the shared edge with a neighbour:
// rubble should read as faceted chunks, not as smooth pebbles. (Lifted from the m8.6 wall sample,
// which is where this loop has been living for two milestones.)
[[nodiscard]] render::CpuMesh part_to_cpu_mesh(const assets::DestructiblePart& part) {
    render::CpuMesh mesh;
    std::uint32_t base = 0; // running offset into the CSR face_indices
    for (const std::uint32_t count : part.face_counts) {
        if (count >= 3) {
            const core::Vec3 v0 = part.vertices[part.face_indices[base]];
            const core::Vec3 v1 = part.vertices[part.face_indices[base + 1]];
            const core::Vec3 v2 = part.vertices[part.face_indices[base + 2]];
            core::Vec3 nrm = core::cross(v1 - v0, v2 - v0);
            const float len = core::length(nrm);
            nrm = len > 1e-8f ? nrm * (1.0f / len) : core::Vec3{0.0f, 1.0f, 0.0f};
            const auto push = [&](core::Vec3 p) {
                render::MeshVertex v;
                v.px = p.x;
                v.py = p.y;
                v.pz = p.z;
                v.nx = nrm.x;
                v.ny = nrm.y;
                v.nz = nrm.z;
                mesh.vertices.push_back(v);
            };
            for (std::uint32_t k = 1; k + 1 < count; ++k) {
                const auto i = static_cast<std::uint32_t>(mesh.vertices.size());
                push(v0);
                push(part.vertices[part.face_indices[base + k]]);
                push(part.vertices[part.face_indices[base + k + 1]]);
                mesh.indices.push_back(i);
                mesh.indices.push_back(i + 1);
                mesh.indices.push_back(i + 2);
            }
        }
        base += count;
    }
    render::compute_tangents(mesh); // the forward pipeline is always-tangented (M6.4)
    return mesh;
}

// The combined centre of mass of a debris island, volume-weighted over its member parts. A debris
// body's pose is about THIS point (register_compound re-centres on the combined COM, ADR-0028's
// "position IS COM"), so a member part's offset within the body is `part.com - island_com`.
// Getting this wrong does not make anything vanish — it scatters every multi-part island's chunks
// by a fixed offset, which reads as "the fracture looks wrong" rather than as a bug.
[[nodiscard]] core::Vec3 island_com(const std::vector<core::Vec3>& part_com,
                                    const std::vector<float>& part_volume,
                                    std::span<const std::uint32_t> members) {
    core::Vec3 sum{0.0f, 0.0f, 0.0f};
    float total = 0.0f;
    for (const std::uint32_t p : members) {
        if (p >= part_com.size()) {
            continue;
        }
        const float w = part_volume[p];
        sum = sum + part_com[p] * w;
        total += w;
    }
    return total > 0.0f ? sum * (1.0f / total) : core::Vec3{0.0f, 0.0f, 0.0f};
}

} // namespace

std::size_t PartLeafRenderer::register_pattern(destruction::PatternId pattern,
                                               const assets::DestructibleAsset& asset,
                                               render::MeshRegistry* meshes) {
    PatternMeshes entry;
    entry.part_mesh.resize(asset.parts.size(), render::kInvalidMeshId);
    entry.part_com.reserve(asset.parts.size());
    entry.part_volume.reserve(asset.parts.size());

    for (std::size_t p = 0; p < asset.parts.size(); ++p) {
        entry.part_com.push_back(asset.parts[p].com);
        entry.part_volume.push_back(asset.parts[p].volume);
        if (meshes != nullptr) {
            entry.part_mesh[p] = meshes->add(part_to_cpu_mesh(asset.parts[p]), "destructible_part");
        }
    }

    const std::size_t uploaded = meshes != nullptr ? asset.parts.size() : 0;
    patterns_[pattern.index] = std::move(entry);
    return uploaded;
}

LeafStats PartLeafRenderer::update(ecs::World& world,
                                   const destruction::DestructionWorld& destruction,
                                   const physics::PhysicsWorld& physics) {
    using ecs::WorldTransform;

    render::register_render_components(world);
    (void)world.register_component<WorldTransform>();

    LeafStats stats;
    instances_.resize(destruction.instance_count());

    // ── 1. Give every newly-bound destructible its leaves ────────────────────────────────────────
    // Collected first, spawned after: spawning relocates archetypes, and the query contract forbids
    // structural change while iterating.
    struct Pending {
        destruction::InstanceId instance;
        render::MaterialId material;
    };

    std::vector<Pending> pending;
    world.query<destruction::Destructible, destruction::DestructibleInstanceRef>().for_each(
        [&](ecs::Entity e, destruction::Destructible&, destruction::DestructibleInstanceRef& ref) {
            if (ref.instance == destruction::kUnboundInstance ||
                ref.instance >= instances_.size() || instances_[ref.instance].built) {
                return;
            }
            // The slab's own material is what its parts wear. That is the whole reason m13.2c puts
            // a MaterialRef on an entity that is never itself drawn: the palette decides the look
            // once per structure, and the 12-28 chunks it shatters into inherit it for free.
            const auto* mat = world.get<render::MaterialRef>(e);
            pending.push_back(Pending{destruction::InstanceId{ref.instance},
                                      mat != nullptr ? mat->material : render::kInvalidMaterialId});
        });

    for (const Pending& p : pending) {
        InstanceLeaves& slot = instances_[p.instance.index];
        slot.built = true;
        slot.pattern = destruction.pattern_of(p.instance);

        const auto it = patterns_.find(slot.pattern.index);
        if (it == patterns_.end()) {
            // Bound in the sim, invisible on screen. Counted rather than skipped: this is a whole
            // building that is not there, and nothing else in the frame would say so.
            ++stats.instances_without_meshes;
            continue;
        }

        if (p.material == render::kInvalidMaterialId) {
            ++stats.instances_without_material;
        }

        const PatternMeshes& pm = it->second;
        slot.leaf.assign(pm.part_mesh.size(), ecs::kNullEntity);
        for (std::size_t part = 0; part < pm.part_mesh.size(); ++part) {
            ecs::Entity leaf = world.spawn_with(WorldTransform{destruction.part_placement(
                                                    p.instance, static_cast<std::uint32_t>(part))},
                                                render::MaterialRef{p.material});
            if (pm.part_mesh[part] != render::kInvalidMeshId) {
                (void)world.add_component(leaf, render::MeshRef{pm.part_mesh[part]});
            }
            slot.leaf[part] = leaf;
            ++stats.leaves_created;
        }
    }

    // ── 2. Standing parts follow their instance's placement ──────────────────────────────────────
    for (std::size_t i = 0; i < instances_.size(); ++i) {
        const InstanceLeaves& slot = instances_[i];
        if (slot.leaf.empty()) {
            continue;
        }
        const auto inst = destruction::InstanceId{static_cast<std::uint32_t>(i)};
        for (std::size_t part = 0; part < slot.leaf.size(); ++part) {
            if (slot.leaf[part] == ecs::kNullEntity) {
                continue;
            }
            if (!destruction.part_alive(inst, static_cast<std::uint32_t>(part))) {
                continue; // detached — posed from its debris body below
            }
            if (auto* tf = world.get<WorldTransform>(slot.leaf[part])) {
                tf->value = destruction.part_placement(inst, static_cast<std::uint32_t>(part));
                ++stats.placements_written;
            }
        }
    }

    // ── 3. Detached parts ride their debris body; retired ones stop existing ─────────────────────
    std::vector<ecs::Entity> to_retire;
    for (std::size_t d = 0; d < destruction.debris_count(); ++d) {
        const destruction::InstanceId src = destruction.debris_source(d);
        if (src.index >= instances_.size() || instances_[src.index].leaf.empty()) {
            continue;
        }
        InstanceLeaves& slot = instances_[src.index];
        const std::span<const std::uint32_t> members = destruction.debris_parts(d);

        if (destruction.debris_retired(d)) {
            // m13.2b's visual budget evicted this rubble. Despawn the leaves — the whole point of
            // C6 is that a render leaf must NOT outlive the thing it draws forever, and marking it
            // invisible while leaving the entity in the world would keep paying for it in every
            // extraction and cull.
            for (const std::uint32_t part : members) {
                if (part < slot.leaf.size() && slot.leaf[part] != ecs::kNullEntity) {
                    to_retire.push_back(slot.leaf[part]);
                    slot.leaf[part] = ecs::kNullEntity;
                    ++stats.leaves_retired;
                }
            }
            continue;
        }

        physics::BodyState state{};
        if (!physics.get_body_state(destruction.debris_body(d), state)) {
            continue; // frozen: the body is gone and the leaf stays at its last pose, by design
        }

        const auto it = patterns_.find(slot.pattern.index);
        if (it == patterns_.end()) {
            continue;
        }
        const core::Vec3 com = island_com(it->second.part_com, it->second.part_volume, members);
        for (const std::uint32_t part : members) {
            if (part >= slot.leaf.size() || slot.leaf[part] == ecs::kNullEntity) {
                continue;
            }
            if (auto* tf = world.get<WorldTransform>(slot.leaf[part])) {
                tf->value.rotation = state.orientation;
                tf->value.translation =
                    state.position +
                    core::rotate(state.orientation, it->second.part_com[part] - com);
                ++stats.placements_written;
            }
        }
    }
    for (const ecs::Entity e : to_retire) {
        world.despawn(e);
    }

    for (const InstanceLeaves& slot : instances_) {
        for (const ecs::Entity e : slot.leaf) {
            if (e != ecs::kNullEntity) {
                ++stats.leaves_live;
            }
        }
    }
    return stats;
}

std::vector<ecs::Entity> PartLeafRenderer::leaves_of(destruction::InstanceId instance) const {
    if (instance.index >= instances_.size()) {
        return {};
    }
    return instances_[instance.index].leaf;
}

} // namespace rime::destruction_render
