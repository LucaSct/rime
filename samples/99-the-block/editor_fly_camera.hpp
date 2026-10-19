// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cmath>
#include <span>

#include "rime/gameplay/input_map.hpp"

namespace block_editor {

// The runtime and authoring viewport can write the same camera during Play. Compare the world
// pose with our last output before integrating input: a different pose is an ownership handoff,
// not a simulation error to correct. Resetting the input latch on that handoff also prevents a
// key held in an earlier play session from moving a restored camera before another key-down.
// Keep yaw/pitch when the pose is ours; extracting them every tick loses the continuous heading
// near the pitch poles. No ECS, renderer, socket or clock is needed to prove this policy.
class FlyCamera {
public:
    [[nodiscard]] rime::core::Transform update(const rime::core::Transform& current,
                                               std::span<const rime::platform::Event> events,
                                               double dt) {
        const auto& a = current.translation;
        const auto& b = last_.translation;
        const auto& q = current.rotation;
        const auto& r = last_.rotation;
        if (!seeded_ || a.x != b.x || a.y != b.y || a.z != b.z || q.x != r.x || q.y != r.y ||
            q.z != r.z || q.w != r.w) {
            fly_ = rime::gameplay::FlyCamera{};
            fly_.position = current.translation;
            const auto forward = rime::core::rotate(current.rotation, {0.0f, 0.0f, -1.0f});
            fly_.view.yaw = std::atan2(-forward.x, -forward.z);
            fly_.view.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
            seeded_ = true;
        }
        // Browser pointer lock already owns capture; native editor movement arrives as a pose.
        fly_.bindings.look_requires_drag = false;
        fly_.update(events, static_cast<float>(dt));
        last_ = current;
        const auto eye = fly_.transform();
        last_.translation = eye.translation;
        last_.rotation = eye.rotation;
        return last_; // scale belongs to the author, even though the camera lens ignores it
    }

private:
    rime::gameplay::FlyCamera fly_{};
    rime::core::Transform last_{};
    bool seeded_ = false;
};

} // namespace block_editor
