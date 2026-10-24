// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once
#include "rime/core/reflect.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/fluids/phase_change.hpp"

namespace rime::fluids_scene {
// Authored initial conditions, not live simulation state. Temperature and MASS fraction are
// both explicit at the melting point. Defaults are inert until enabled and Play/Step begins.
struct ThermalCell {
    double mass_kg = 1.0;
    double temperature_kelvin = 273.15;
    double liquid_mass_fraction = 1.0;
    fluids::PhaseChangeConfig material{};
    bool enabled = false;
};

struct ThermalContact {
    ecs::Entity first{};
    ecs::Entity second{};
    double conductance_w_k = 1.0;
    bool enabled = false;
};

struct ThermalSource {
    ecs::Entity cell{};
    double power_watts = 0.0;
    bool enabled = false;
};

inline void register_fluids_scene_components(ecs::World& world) {
    (void)world.register_component<ThermalCell>();
    (void)world.register_component<ThermalContact>();
    (void)world.register_component<ThermalSource>();
}
} // namespace rime::fluids_scene

// Reflection belongs to this adapter; the numerical fluids module keeps its core-only seam.
RIME_REFLECT_BEGIN(rime::fluids::PhaseChangeConfig)
RIME_REFLECT_FIELD(melting_kelvin)
RIME_REFLECT_FIELD(solid_heat_capacity_j_kg_k)
RIME_REFLECT_FIELD(liquid_heat_capacity_j_kg_k)
RIME_REFLECT_FIELD(latent_heat_j_kg)
RIME_REFLECT_FIELD(minimum_kelvin)
RIME_REFLECT_FIELD(maximum_kelvin)
RIME_REFLECT_END()
RIME_REFLECT_BEGIN(rime::fluids_scene::ThermalCell)
RIME_REFLECT_FIELD(mass_kg)
RIME_REFLECT_FIELD(temperature_kelvin)
RIME_REFLECT_FIELD(liquid_mass_fraction)
RIME_REFLECT_FIELD(material)
RIME_REFLECT_FIELD(enabled)
RIME_REFLECT_END()
RIME_REFLECT_BEGIN(rime::fluids_scene::ThermalContact)
RIME_REFLECT_FIELD(first)
RIME_REFLECT_FIELD(second)
RIME_REFLECT_FIELD(conductance_w_k)
RIME_REFLECT_FIELD(enabled)
RIME_REFLECT_END()
RIME_REFLECT_BEGIN(rime::fluids_scene::ThermalSource)
RIME_REFLECT_FIELD(cell)
RIME_REFLECT_FIELD(power_watts)
RIME_REFLECT_FIELD(enabled)
RIME_REFLECT_END()
