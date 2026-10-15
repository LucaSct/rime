// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// play_sunset — the evening preset `--play` applies to the block (sample-local, header-only).
//
// The authored rig puts the sun BEHIND the street-view camera, which makes a fine shadow-raking
// shot and a poor sunset: you never see the disc. This preset turns the sun round to sit ahead of
// the player, low over the end of the street. It is applied to PLAY ONLY — `--headless`, the perf
// harness and the authored captures keep the rig exactly as cooked, because their claims and
// baselines were measured against it.
//
// ONE DirectionalLight drives everything. The sky draws its analytic disc from the scene's first
// DirectionalLight (`SkyParams::use_scene_sun`), so rotating that one entity moves the shadows,
// the direct light and the visible sun together; there is no second sun direction to disagree.

#pragma once

#include <numbers>

#include "rime/core/math.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/sky.hpp"

namespace rime::play_sunset {

inline constexpr float kPi = std::numbers::pi_v<float>;

// Elevation of the sun's centre above the horizon [rad] (~6.9 deg): low evening light, high
// enough to remain visible above the generated hills from the initial street viewpoint.
inline constexpr float kSunElevation = 0.12f;
// View: yaw -pi/2 makes forward (-sin yaw, 0, -cos yaw) = +X, down the street; a slight upward
// pitch keeps the disc inside the frame.
inline constexpr float kViewYaw = -kPi * 0.5f;
inline constexpr float kViewPitch = 0.04f;

// A real optic's ocular is 90 mm from the eye. The default 100 mm near plane
// clips that glass completely, even though its housing and second-camera feed draw.
// Keep the common world/weapon projection, but give the PLAY rig room for the lens.
inline void configure_camera(render::Camera& camera) noexcept {
    camera.z_near = .02f;
    camera.z_far = 3072.0f;
}

// Warm direct light, linear RGB.
inline constexpr float kSunColor[3] = {1.0f, 0.66f, 0.36f};
inline constexpr float kSunIntensity = 1.7f;

// A light SHINES along its entity's forward axis (rotation applied to -z), i.e. the direction
// light TRAVELS. The direction TOWARD the sun is the opposite. Wanting the sun toward +X and
// elevated, light travels toward -X and downward: yaw +pi/2 turns forward from -Z to -X, then a
// pitch of -elevation about local X tips it downward. The result is
//   travel = (-cos e, -sin e, 0),  toward-sun = (cos e, sin e, 0).
[[nodiscard]] inline core::Quat sun_rotation(float elevation = kSunElevation) noexcept {
    const core::Quat yaw = core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, kPi * 0.5f);
    const core::Quat pitch = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -elevation);
    return core::normalize(yaw * pitch);
}

// Retarget the EXISTING sun in `world`: no entity or light is created. Local and world transform
// are both written because nothing re-propagates transforms before the next draw on a peer that
// already ran its first frame. Returns the number of lights edited (the authored block has one).
inline int apply(ecs::World& world) {
    int edited = 0;
    const core::Quat q = sun_rotation();
    world.query<render::DirectionalLight>().for_each(
        [&](ecs::Entity e, render::DirectionalLight& l) {
            l.color_r = kSunColor[0];
            l.color_g = kSunColor[1];
            l.color_b = kSunColor[2];
            l.intensity = kSunIntensity;
            if (auto* lt = world.get<ecs::LocalTransform>(e)) {
                lt->value.rotation = q;
            }
            if (auto* wt = world.get<ecs::WorldTransform>(e)) {
                wt->value.rotation = q;
            }
            ++edited;
        });
    return edited;
}

// The physical sky's solar source is NOT read from the DirectionalLight's colour or intensity:
// the renderer couples only the DIRECTION (use_scene_sun), so `sun_radiance` has to be set to the
// same warm source the light uses, or the visible sun and the direct light would be two sources.
// The authored `intensity` is an exposure for a sun BEHIND the camera. Looking INTO a low sun, the
// forward-scattering halo around the disc is huge and at the authored exposure it saturates the
// whole horizon, so the disc has no contrast to read against. A lower exposure keeps the same
// physical atmosphere and disc and lets the disc resolve. Clouds and atmosphere are untouched.
[[nodiscard]] inline render::SkyParams sky(render::SkyParams authored) noexcept {
    for (int i = 0; i < 3; ++i) {
        authored.sun_radiance[i] = kSunColor[i] * kSunIntensity;
    }
    authored.intensity = 0.06f;
    authored.use_scene_sun = true;
    authored.angular_radius = 0.012f;
    return authored;
}

} // namespace rime::play_sunset
