// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/destruction/world.hpp"

#include <numeric>
#include <utility>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/physics/shape.hpp"
#include "rime/physics/world.hpp"
#include "world_impl.hpp"

// The DestructionWorld "load, stand, bind" half (M8.2): registering patterns and standing
// instances. The tables live in world_impl.hpp, shared with damage.cpp (M8.3 — damage,
// connectivity, the fracture body swap). Nothing here steps physics — it registers geometry and
// creates bodies; the simulation is driven by whoever owns the PhysicsWorld.
namespace rime::destruction {

DestructionWorld::DestructionWorld() : impl_(std::make_unique<Impl>()) {}

DestructionWorld::~DestructionWorld() = default;

PatternId DestructionWorld::register_pattern(const assets::DestructibleAsset& asset,
                                             physics::PhysicsWorld& world) {
    Impl::Pattern pat;
    pat.part_count = static_cast<std::uint32_t>(asset.parts.size());
    pat.hulls.reserve(asset.parts.size());
    pat.part_com.reserve(asset.parts.size());
    pat.part_volume.reserve(asset.parts.size());
    pat.part_aabb_min.reserve(asset.parts.size());
    pat.part_aabb_max.reserve(asset.parts.size());

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
        pat.part_volume.push_back(p.volume);
        pat.part_aabb_min.push_back(p.aabb_min);
        pat.part_aabb_max.push_back(p.aabb_max);

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
    // The compound's combined COM in the authored (destructible) frame — spawn needs it to place
    // the body so that "body position IS the COM" and every part still lands at its authored spot
    // (the same recipe the M8.3 body swap uses; see spawn below).
    physics::CompoundInfo info;
    if (world.compound_info(compound, info)) {
        pat.compound_centroid = info.centroid;
    }
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
    // participates in collision when something hits it (which is how m8.3 hears the damage).
    //
    // Placement (the invariant every body swap repeats, M8.3): a compound body's position IS its
    // combined COM, and register_compound re-centred the stored child poses on that COM — so
    // putting the body at `placement ∘ centroid` lands every child (part) exactly at
    // `placement ∘ part.com`, its authored spot. For the intact pattern the centroid is ≈ the
    // authored origin (a Voronoi partition's volume-weighted COM is the source box's centre), but
    // using the exact value keeps intact and post-fracture placement on ONE recipe. Instance
    // placements are rigid (unit scale) — physics has no scale concept.
    physics::BodyDesc desc;
    desc.motion = physics::MotionType::Static;
    desc.shape.type = physics::ShapeType::Compound;
    desc.shape.compound = pat.compound;
    desc.position = core::transform_point(placement, pat.compound_centroid);
    desc.orientation = placement.rotation;
    const physics::BodyId body = world.create_body(desc);

    Impl::Instance inst;
    inst.pattern = pattern;
    inst.body = body;
    inst.placement = placement;
    inst.health.assign(pat.part_count, 1.0f);
    inst.alive.assign(pat.part_count, std::uint8_t{1});
    // The intact compound's children are the parts in cook order (register_pattern pushed one
    // child per part, in order; register_compound preserves it), so the initial child → part remap
    // is the identity (ADR-0029 §4). Every fracture rebuilds it as the surviving ids, ascending.
    inst.child_to_part.resize(pat.part_count);
    std::iota(inst.child_to_part.begin(), inst.child_to_part.end(), 0u);

    impl_->instances.push_back(std::move(inst));
    const auto id = InstanceId{static_cast<std::uint32_t>(impl_->instances.size() - 1), 0};
    impl_->map_insert(body, id.index); // so a contact event's body resolves back to this instance
    return id;
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

std::uint32_t DestructionWorld::part_from_child(InstanceId instance,
                                                std::uint16_t child) const noexcept {
    if (instance.index >= impl_->instances.size()) {
        return kInvalidPartIndex;
    }
    const std::vector<std::uint32_t>& map = impl_->instances[instance.index].child_to_part;
    return child < map.size() ? map[child] : kInvalidPartIndex;
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

std::size_t DestructionWorld::debris_count() const noexcept {
    return impl_->debris.size();
}

physics::BodyId DestructionWorld::debris_body(std::size_t debris) const noexcept {
    if (debris >= impl_->debris.size()) {
        return physics::BodyId{};
    }
    return impl_->debris[debris].body;
}

InstanceId DestructionWorld::debris_source(std::size_t debris) const noexcept {
    if (debris >= impl_->debris.size()) {
        return InstanceId{};
    }
    return InstanceId{impl_->debris[debris].instance, 0};
}

std::span<const std::uint32_t> DestructionWorld::debris_parts(std::size_t debris) const noexcept {
    if (debris >= impl_->debris.size()) {
        return {};
    }
    const Impl::Debris& d = impl_->debris[debris];
    return std::span<const std::uint32_t>{impl_->debris_part_pool}.subspan(d.first_part,
                                                                           d.part_count);
}

} // namespace rime::destruction
