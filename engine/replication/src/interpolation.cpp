// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/interpolation.hpp"

#include <algorithm>

#include "rime/core/math/quat.hpp"
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

    const float t = std::clamp(alpha, 0.0f, 1.0f);
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

} // namespace rime::replication
