// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rime/assets/destructible_asset.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/destruction/ids.hpp"
#include "rime/destruction/world.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"

namespace rime::physics {
class PhysicsWorld;
}

// Per-part RENDER LEAVES for destructibles (m13.2d) — the system that has been missing since M8.
//
// THE GAP THIS CLOSES. A destructible's *parts* are what you see: 12 to 28 convex chunks per slab,
// each standing in the intact compound, then flying off in a debris body, then frozen where it
// landed. Nothing in the engine ever turned that into something the renderer could draw.
// `destruction/world.hpp` says so outright — "per-part render leaves land with the [sample]" — and
// so `10-destructible-wall` hand-rolls its own `build_leaves`/`refresh_leaves` against one instance
// of 60 parts. That is fine for one wall and is not a strategy for a block: m13.2c assembles 140
// instances totalling 2,016 parts, and m13.3 and m13.5 would each have copied the same loop.
//
// WHY A BRIDGE MODULE AND NOT A METHOD ON EITHER SIDE. `destruction` must not depend on `render`
// (it sits below it and is proven GPU-free), and `render` must not depend on `destruction` (a
// renderer that knows what a fracture is has lost the seam). So the glue is its own module that
// depends on both — exactly the guardrail-2 argument that produced `replication` and
// `destruction_net`. Remove this directory and the engine still builds; the block simply is not
// drawn.
//
// MESH PER (PATTERN, PART) — NOT PER (INSTANCE, PART). This is the whole reason the block is
// affordable. The 140 instances of m13.2c's block share NINE cooked patterns, so the geometry is
// ~148 distinct part meshes uploaded once, instanced 2,016 times by transform alone. Uploading per
// instance would mean 2,016 vertex buffers of identical data — the naive loop the wall sample can
// afford and the block cannot.
namespace rime::destruction_render {

// What one `update()` did. Every field exists because the failure it names is otherwise INVISIBLE:
// a block that draws nothing and a block that draws everything both produce a frame.
struct LeafStats {
    std::size_t leaves_created = 0; // new leaf entities spawned this call
    std::size_t leaves_retired = 0; // despawned because their debris crossed the C6 visual budget
    std::size_t leaves_live = 0;    // leaf entities that exist right now
    std::size_t placements_written = 0; // leaves whose WorldTransform was updated this call

    // An instance whose pattern was never registered here — its parts have no meshes, so it stands
    // in the physics world and appears nowhere on screen. Without this counter that is a building
    // that quietly is not there, which is the exact failure mode m13.2a's cull counter was added to
    // catch one layer up.
    std::size_t instances_without_meshes = 0;

    // An instance whose slab entity carried no usable `MaterialRef` when its leaves were built.
    // Leaf materials are captured ONCE, at build — so calling `apply_palette` after the first
    // `update()` leaves every one of that structure's parts permanently unshaded, and
    // `scene_renderer` skips a draw with an invalid material without a word. Same failure shape as
    // `instances_without_meshes` and just as invisible without a number: a whole building that is
    // not there. Ordering is the bug; this is what makes the ordering visible.
    std::size_t instances_without_material = 0;
};

// Owns the pattern → part-mesh table and the leaf entities. One per rendered world.
//
// Not copyable: it owns entity ids in a specific `ecs::World` and mesh ids in a specific registry.
class PartLeafRenderer {
public:
    PartLeafRenderer() = default;

    PartLeafRenderer(const PartLeafRenderer&) = delete;
    PartLeafRenderer& operator=(const PartLeafRenderer&) = delete;

    // COLD PATH: upload one mesh per part of `asset` and remember them for `pattern`. Call once per
    // registered pattern, alongside `DestructionWorld::register_pattern`.
    //
    // `meshes` may be NULL, and that is a designed seam rather than a convenience: `MeshRegistry`
    // holds an `rhi::Device&` and uploads on add, so minting a MeshId needs a GPU. With null, the
    // part COMs are still recorded and leaves are still created and posed — they simply carry no
    // MeshRef. That is what lets the leaf LIFECYCLE (creation, following the sim, retirement on
    // eviction) be proven headless on every CI OS and under the sanitizers, leaving only "are the
    // pixels there" to the lavapipe proof.
    std::size_t register_pattern(destruction::PatternId pattern,
                                 const assets::DestructibleAsset& asset,
                                 render::MeshRegistry* meshes);

    // Per-frame: give every newly-bound destructible its leaves, then pose every leaf.
    //
    // A leaf's pose comes from one of three places, and the three are the destructible's whole
    // visual life cycle (ADR-0029 §8):
    //   * STANDING — the part's placement in its instance's compound.
    //   * DETACHED — its debris body's pose, composed with the part's offset within that island.
    //   * FROZEN — left exactly where it was. m8.5 destroys a settled debris body but deliberately
    //     keeps the roster row, because "a render leaf can outlive the physics body at its last
    //     pose" is the cheap way to keep rubble on screen.
    // A fourth state ends the leaf: RETIRED, when m13.2b's visual budget evicts the debris.
    //
    // Call once per tick AFTER `DestructionWorld::update`.
    LeafStats update(ecs::World& world,
                     const destruction::DestructionWorld& destruction,
                     const physics::PhysicsWorld& physics);

    // Leaf entities for `instance`, in part order; empty if it has none. For tests and tooling.
    [[nodiscard]] std::vector<ecs::Entity> leaves_of(destruction::InstanceId instance) const;

    [[nodiscard]] std::size_t pattern_count() const noexcept { return patterns_.size(); }

private:
    struct PatternMeshes {
        // One mesh per part, in cook order. Invalid ids when registered without a device.
        std::vector<render::MeshId> part_mesh;
        // Cooked part COMs and volumes — needed to place a part INSIDE a multi-part debris body,
        // whose pose is about the island's VOLUME-WEIGHTED combined centre of mass rather than any
        // one part's (ADR-0028: register_compound re-centres on the combined COM).
        std::vector<core::Vec3> part_com;
        std::vector<float> part_volume;
    };

    struct InstanceLeaves {
        bool built = false;
        destruction::PatternId pattern{};
        std::vector<ecs::Entity> leaf; // one per part; kNullEntity once retired
    };

    std::unordered_map<std::uint32_t, PatternMeshes> patterns_;
    std::vector<InstanceLeaves> instances_; // indexed by InstanceId::index
};

} // namespace rime::destruction_render
