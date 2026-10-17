// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/fluids/buoyancy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace rime::fluids {
namespace {

[[nodiscard]] bool finite(core::Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool as_float(double value, float& out) noexcept {
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
        return false;
    out = static_cast<float>(value);
    return std::isfinite(out);
}

} // namespace

std::optional<BuoyancyExchange> evaluate_buoyancy(const WaterSurfaceSample& sample,
                                                  const BuoyancyProbe& probe,
                                                  float gravity_m_s2,
                                                  float dt_seconds) noexcept {
    if (!std::isfinite(sample.height_metres) || !finite(sample.velocity_m_s) ||
        !std::isfinite(sample.density_kg_m3) || sample.density_kg_m3 <= 0.0f ||
        !finite(probe.center_world) || !finite(probe.velocity_world) ||
        !std::isfinite(probe.radius_metres) || probe.radius_metres <= 0.0f ||
        !std::isfinite(probe.drag_coefficient) || probe.drag_coefficient < 0.0f ||
        !std::isfinite(gravity_m_s2) || gravity_m_s2 <= 0.0f || !std::isfinite(dt_seconds) ||
        dt_seconds <= 0.0f)
        return std::nullopt;

    BuoyancyExchange result{};
    result.body_key = probe.stable_body_key;
    result.water_tick = sample.authoritative_tick;
    result.application_point = probe.center_world;

    const double r = probe.radius_metres;
    const double depth = static_cast<double>(sample.height_metres) - probe.center_world.y + r;
    const double h = std::clamp(depth, 0.0, 2.0 * r);
    const double full_volume = (4.0 / 3.0) * std::numbers::pi_v<double> * r * r * r;
    const double volume = std::numbers::pi_v<double> * h * h * (r - h / 3.0);
    if (!std::isfinite(full_volume) || !as_float(volume, result.displaced_volume_m3))
        return std::nullopt;
    if (h == 0.0)
        return result;

    const double rel_x = static_cast<double>(sample.velocity_m_s.x) - probe.velocity_world.x;
    const double rel_y = static_cast<double>(sample.velocity_m_s.y) - probe.velocity_world.y;
    const double rel_z = static_cast<double>(sample.velocity_m_s.z) - probe.velocity_world.z;
    const double speed = std::sqrt(rel_x * rel_x + rel_y * rel_y + rel_z * rel_z);
    const double area = std::numbers::pi_v<double> * r * r * (volume / full_volume);
    const double drag = 0.5 * sample.density_kg_m3 * probe.drag_coefficient * area * speed;
    const double lift = static_cast<double>(sample.density_kg_m3) * gravity_m_s2 * volume;
    const double dt = dt_seconds;
    if (!as_float(drag * rel_x * dt, result.body_impulse.x) ||
        !as_float((lift + drag * rel_y) * dt, result.body_impulse.y) ||
        !as_float(drag * rel_z * dt, result.body_impulse.z))
        return std::nullopt;
    result.water_reaction_impulse = -result.body_impulse;
    return result;
}

} // namespace rime::fluids
