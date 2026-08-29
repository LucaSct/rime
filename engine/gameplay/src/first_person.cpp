// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/gameplay/first_person.hpp"

#include <cmath>
#include <numbers>

#include "rime/core/math/quat.hpp"

namespace rime::gameplay {
namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// Wrap into (−π, π]. Done every frame rather than never: yaw that accumulates for an hour of play
// loses mantissa precision exactly where the player is aiming, and the wrapped value is what the
// input tape carries.
[[nodiscard]] float wrap_pi(float a) noexcept {
    a = std::fmod(a + kPi, 2.0f * kPi);
    if (a <= 0.0f) {
        a += 2.0f * kPi;
    }
    return a - kPi;
}

[[nodiscard]] bool finite(float v) noexcept {
    return std::isfinite(v);
}

} // namespace

void apply_look(FirstPersonView& view, float dx, float dy) noexcept {
    // A non-finite delta is dropped rather than propagated. A single NaN from a device or a replay
    // would otherwise poison yaw permanently — every subsequent frame adds to NaN — and the symptom
    // is a black screen, not a number anyone can trace back to one bad event.
    if (!finite(dx) || !finite(dy) || !finite(view.sensitivity)) {
        return;
    }
    view.yaw = wrap_pi(view.yaw - dx * view.sensitivity); // right turns right
    view.pitch -= dy * view.sensitivity;                  // pointer down looks down
    view.pitch = std::fmin(std::fmax(view.pitch, -kMaxPitch), kMaxPitch);
}

core::Vec3 look_direction(const FirstPersonView& view) noexcept {
    // Derived rather than rotated, so the weapon ray and the camera cannot drift apart by a
    // different arithmetic path. Yaw-then-pitch applied to −Z gives:
    //     (−cos(pitch)·sin(yaw), sin(pitch), −cos(pitch)·cos(yaw))
    // At pitch = 0 this is (−sin yaw, 0, −cos yaw) — EXACTLY the forward basis `step_character`
    // builds from the same angle, which is the property that makes "walk forward" walk where you
    // are looking rather than 90° off it.
    const float cp = std::cos(view.pitch);
    return {-cp * std::sin(view.yaw), std::sin(view.pitch), -cp * std::cos(view.yaw)};
}

core::Transform eye_transform(const FirstPersonView& view, core::Vec3 character_position) noexcept {
    core::Transform tf;
    tf.translation = {
        character_position.x, character_position.y + view.eye_height, character_position.z};
    // Yaw about world +Y first, then pitch about the yawed local X. Composing in the other order
    // would roll the horizon as you look up while turning, which is the classic wrong-order
    // symptom.
    tf.rotation = core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, view.yaw) *
                                  core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, view.pitch));
    return tf;
}

} // namespace rime::gameplay
