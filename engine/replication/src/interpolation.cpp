// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/interpolation.hpp"

#include <algorithm>

#include "rime/core/math/quat.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/transform.hpp"

namespace rime::replication {

core::Transform
interpolated_transform(const ecs::World& world, ecs::Entity entity, float alpha) noexcept {
    const auto* current = world.get<ecs::LocalTransform>(entity);
    if (current == nullptr) {
        return core::Transform{};
    }
    const auto* previous = world.get<PreviousTransform>(entity);
    if (previous == nullptr || !previous->valid) {
        return current->value; // nothing to blend from — snap (see the header on why not a default)
    }

    // v2 (m12.5): the fraction is measured against the interval this value actually covers, not
    // against one tick. `span_ticks` is never 0 (interpolation.hpp), but the guard is cheap and a
    // divide-by-zero here would be a NaN transform reaching the renderer.
    const float span = static_cast<float>(previous->span_ticks > 0 ? previous->span_ticks : 1);
    const float progress =
        static_cast<float>(previous->elapsed_ticks) + std::clamp(alpha, 0.0f, 1.0f);
    const float t = std::clamp(progress / span, 0.0f, 1.0f);
    core::Transform out;
    out.translation.x = previous->value.translation.x +
                        (current->value.translation.x - previous->value.translation.x) * t;
    out.translation.y = previous->value.translation.y +
                        (current->value.translation.y - previous->value.translation.y) * t;
    out.translation.z = previous->value.translation.z +
                        (current->value.translation.z - previous->value.translation.z) * t;
    // Shortest-arc for rotation. A component-wise lerp of two quaternions is not a rotation and
    // would both change speed through the arc and, past 90 degrees, take the long way round.
    out.rotation = core::slerp(previous->value.rotation, current->value.rotation, t);
    out.scale.x = previous->value.scale.x + (current->value.scale.x - previous->value.scale.x) * t;
    out.scale.y = previous->value.scale.y + (current->value.scale.y - previous->value.scale.y) * t;
    out.scale.z = previous->value.scale.z + (current->value.scale.z - previous->value.scale.z) * t;
    return out;
}

std::size_t update_render_transforms(ecs::World& world, float alpha) {
    std::size_t written = 0;
    // The query is the filter: RenderTransform is added alongside PreviousTransform and only there,
    // so this visits exactly the mirrors that have ever moved twice — not the world. Deliberately
    // NOT for_each_changed: that cursor tracks changes to the DATA, and the input that changes here
    // every frame is `alpha`, which the ECS cannot see. Gating on "changed since last frame" would
    // sample each entity once per tick and freeze it at whatever alpha that frame happened to hold,
    // which is the interpolation not happening at all.
    world.query<PreviousTransform, ecs::LocalTransform, ecs::RenderTransform>().for_each(
        [&](ecs::Entity e, PreviousTransform&, ecs::LocalTransform&, ecs::RenderTransform& out) {
            // Reuses the tested blend rather than repeating the lerp/slerp here. It already returns
            // the current value untouched for a settled or first-appearing mirror, which is exactly
            // what those cases must draw.
            const core::Transform blended = interpolated_transform(world, e, alpha);

            // The history is in LOCAL space (it is the value that replicates), so a parented mirror
            // has to be re-composed against its parent to become a world pose. Blending world poses
            // directly would be the cheaper-looking mistake: a child at a fixed offset from a
            // rotating parent sweeps an ARC through world space, and a straight line between its
            // two world positions cuts the corner. Blend the local, re-derive the world.
            //
            // Reachable only by hand today — `Parent` carries an Entity field, which
            // WireSchema::is_replicable refuses, so a replicated entity cannot HAVE a replicated
            // parent — but a local entity may be parented to a mirror, and the composition costs
            // one multiply. The fallback matches propagate_transforms': no parent, dead parent, or
            // a parent with no world pose all mean "treat it as a root".
            const auto* parent = world.get<ecs::Parent>(e);
            if (parent != nullptr && parent->value != ecs::kNullEntity) {
                if (const auto* parent_world = world.get<ecs::WorldTransform>(parent->value)) {
                    out.value = parent_world->value * blended;
                    ++written;
                    return;
                }
            }
            out.value = blended;
            ++written;
        });
    return written;
}

} // namespace rime::replication
