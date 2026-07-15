// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction/world.hpp"

#include <utility>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/physics/shape.hpp"
#include "rime/physics/world.hpp"

// The DestructionWorld implementation (M8.2). Two append-only tables behind the seam: `patterns`
// (registered fracture geometry, one per distinct asset) and `instances` (standing bodies +
// per-part state). Nothing here steps physics — it registers geometry and creates bodies; the
// simulation is driven by whoever owns the PhysicsWorld.
namespace rime::destruction {

struct DestructionWorld::Impl {
    // A registered pattern: the physics compound that IS the intact shape, the per-part hull ids
    // and COMs (kept for the m8.3 fracture body-swap, which re-registers subsets), and the cooked
    // connectivity + damage material.
    struct Pattern {
        physics::CompoundId compound{};
        std::uint32_t part_count = 0;
        std::vector<physics::HullId> hulls;
        std::vector<core::Vec3> part_com;
        std::vector<assets::DestructibleBond> bonds;
        std::vector<std::uint32_t> anchors;
        core::Vec3 half_extents{0.0f, 0.0f, 0.0f};
        float damage_threshold = 0.0f;
        float damage_scale = 0.0f;
    };

    // A standing instance: its pattern, the static compound body, where it was placed, and the
    // per-part state m8.3 mutates.
    struct Instance {
        PatternId pattern{};
        physics::BodyId body{};
        core::Transform placement{};
        std::vector<float> health;
        std::vector<std::uint8_t> alive;
    };

    std::vector<Pattern> patterns;
    std::vector<Instance> instances;
};

DestructionWorld::DestructionWorld() : impl_(std::make_unique<Impl>()) {}

DestructionWorld::~DestructionWorld() = default;

PatternId DestructionWorld::register_pattern(const assets::DestructibleAsset& asset,
                                             physics::PhysicsWorld& world) {
    Impl::Pattern pat;
    pat.part_count = static_cast<std::uint32_t>(asset.parts.size());
    pat.hulls.reserve(asset.parts.size());
    pat.part_com.reserve(asset.parts.size());

    // Each part becomes a hull; the compound's children are those hulls at their cooked COMs. The
    // hull vertices are already COM-centred (the cook re-centred them), so the child pose is a pure
    // translation to the COM — register_compound re-centres the whole on the combined COM, which
    // for a symmetric wall is ≈ the authored origin (ADR-0028's "position IS COM" contract).
    std::vector<physics::CompoundChildDesc> children;
    children.reserve(asset.parts.size());
    for (const assets::DestructiblePart& p : asset.parts) {
        const physics::HullDesc hull_desc{p.vertices, p.face_counts, p.face_indices};
        const physics::HullId hull = world.register_hull(hull_desc);
        if (!hull.is_valid()) {
            return PatternId{}; // a malformed part — reject the whole pattern (no partial
                                // registration)
        }
        pat.hulls.push_back(hull);
        pat.part_com.push_back(p.com);

        physics::ShapeDesc shape;
        shape.type = physics::ShapeType::ConvexHull;
        shape.hull = hull;
        children.push_back(physics::CompoundChildDesc{shape, p.com, core::quat_identity()});
    }

    const physics::CompoundId compound = world.register_compound(physics::CompoundDesc{children});
    if (!compound.is_valid()) {
        return PatternId{};
    }
    pat.compound = compound;
    pat.bonds = asset.bonds;
    pat.anchors = asset.anchors;
    pat.half_extents = asset.half_extents;
    pat.damage_threshold = asset.damage_threshold;
    pat.damage_scale = asset.damage_scale;

    impl_->patterns.push_back(std::move(pat));
    return PatternId{static_cast<std::uint32_t>(impl_->patterns.size() - 1), 0};
}

InstanceId DestructionWorld::spawn(PatternId pattern,
                                   const core::Transform& placement,
                                   physics::PhysicsWorld& world) {
    if (pattern.index >= impl_->patterns.size()) {
        return InstanceId{};
    }
    const Impl::Pattern& pat = impl_->patterns[pattern.index];

    // The intact wall: one STATIC compound body. Static ⇒ it never integrates or solves, so a
    // standing-but-untouched destructible adds nothing to the tick's awake/solve load — it only
    // participates in collision when something hits it (which is how m8.3 will hear the damage).
    physics::BodyDesc desc;
    desc.motion = physics::MotionType::Static;
    desc.shape.type = physics::ShapeType::Compound;
    desc.shape.compound = pat.compound;
    desc.position = placement.translation;
    desc.orientation = placement.rotation;
    const physics::BodyId body = world.create_body(desc);

    Impl::Instance inst;
    inst.pattern = pattern;
    inst.body = body;
    inst.placement = placement;
    inst.health.assign(pat.part_count, 1.0f);
    inst.alive.assign(pat.part_count, std::uint8_t{1});

    impl_->instances.push_back(std::move(inst));
    return InstanceId{static_cast<std::uint32_t>(impl_->instances.size() - 1), 0};
}

std::size_t DestructionWorld::pattern_count() const noexcept {
    return impl_->patterns.size();
}

std::size_t DestructionWorld::instance_count() const noexcept {
    return impl_->instances.size();
}

physics::BodyId DestructionWorld::body_of(InstanceId instance) const noexcept {
    if (instance.index >= impl_->instances.size()) {
        return physics::BodyId{};
    }
    return impl_->instances[instance.index].body;
}

std::uint32_t DestructionWorld::part_count(PatternId pattern) const noexcept {
    if (pattern.index >= impl_->patterns.size()) {
        return 0;
    }
    return impl_->patterns[pattern.index].part_count;
}

std::uint32_t DestructionWorld::instance_part_count(InstanceId instance) const noexcept {
    if (instance.index >= impl_->instances.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(impl_->instances[instance.index].alive.size());
}

bool DestructionWorld::part_alive(InstanceId instance, std::uint32_t part) const noexcept {
    if (instance.index >= impl_->instances.size()) {
        return false;
    }
    const Impl::Instance& inst = impl_->instances[instance.index];
    return part < inst.alive.size() && inst.alive[part] != 0;
}

float DestructionWorld::part_health(InstanceId instance, std::uint32_t part) const noexcept {
    if (instance.index >= impl_->instances.size()) {
        return 0.0f;
    }
    const Impl::Instance& inst = impl_->instances[instance.index];
    return part < inst.health.size() ? inst.health[part] : 0.0f;
}

std::span<const assets::DestructibleBond>
DestructionWorld::bonds(PatternId pattern) const noexcept {
    if (pattern.index >= impl_->patterns.size()) {
        return {};
    }
    return impl_->patterns[pattern.index].bonds;
}

std::span<const std::uint32_t> DestructionWorld::anchors(PatternId pattern) const noexcept {
    if (pattern.index >= impl_->patterns.size()) {
        return {};
    }
    return impl_->patterns[pattern.index].anchors;
}

core::Transform DestructionWorld::part_placement(InstanceId instance,
                                                 std::uint32_t part) const noexcept {
    if (instance.index >= impl_->instances.size()) {
        return {};
    }
    const Impl::Instance& inst = impl_->instances[instance.index];
    if (inst.pattern.index >= impl_->patterns.size()) {
        return {};
    }
    const Impl::Pattern& pat = impl_->patterns[inst.pattern.index];
    if (part >= pat.part_com.size()) {
        return {};
    }
    core::Transform t;
    t.rotation = inst.placement.rotation; // an intact part inherits the instance's orientation
    t.scale = core::Vec3{1.0f, 1.0f, 1.0f};
    t.translation = core::transform_point(inst.placement, pat.part_com[part]);
    return t;
}

} // namespace rime::destruction
