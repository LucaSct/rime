// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>

#include "rime/core/math/vec.hpp"

namespace rime::fluids {

// A caller-provided, locally horizontal water surface sample. No grid or GPU readback is implied.
// authoritative_tick identifies the source snapshot; the caller must enforce its tick policy.
struct WaterSurfaceSample {
    float height_metres = 0.0f;
    core::Vec3 velocity_m_s{};
    float density_kg_m3 = 1000.0f;
    std::uint64_t authoritative_tick = 0;
};

// A spherical volume proxy, centered at the body's world-space center of mass for this first seam.
// Multiple probes must not overlap: this function cannot remove double-counted volume.
struct BuoyancyProbe {
    std::uint64_t stable_body_key = 0;
    core::Vec3 center_world{};
    core::Vec3 velocity_world{};
    float radius_metres = 0.0f;
    float drag_coefficient = 0.0f;
};

// An exchange record, not an applied fluid update. The caller applies body_impulse through its
// physics interface in stable_body_key order; no code here applies water_reaction_impulse.
struct BuoyancyExchange {
    std::uint64_t body_key = 0;
    std::uint64_t water_tick = 0;
    core::Vec3 application_point{};
    float displaced_volume_m3 = 0.0f;
    core::Vec3 body_impulse{};
    core::Vec3 water_reaction_impulse{};
};

// Exact submerged volume for a sphere cut by a horizontal plane. Hydrostatic lift is rho*g*V.
// Optional quadratic drag uses relative water/body velocity and projected area pi*r^2*(V/Vfull),
// a pontoon approximation rather than a resolved pressure integral. Returns a valid zero exchange
// for a dry sphere; invalid/non-finite inputs or non-representable outputs return nullopt.
[[nodiscard]] std::optional<BuoyancyExchange> evaluate_buoyancy(const WaterSurfaceSample& sample,
                                                                const BuoyancyProbe& probe,
                                                                float gravity_m_s2,
                                                                float dt_seconds) noexcept;

} // namespace rime::fluids
