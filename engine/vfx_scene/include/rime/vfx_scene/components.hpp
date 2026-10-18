// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/reflect.hpp"
#include "rime/ecs/world.hpp"

namespace rime::vfx_scene {

// These are authoring descriptions, never live particle pools or ignition authority. Reflection
// makes the same bytes inspectable, undoable and saveable through the existing editor channel.
// Extents describe a local box; the consumer transforms all eight corners into world bounds.
// Adding a source is deliberately inert until the author opts in with enabled=true and Play/Step.
struct SmokeEmitter {
    float half_x = 0.2f;
    float half_y = 0.2f;
    float half_z = 0.2f;
    float particles_per_second = 12.0f;
    float intensity = 1.0f;
    bool enabled = false;
};

struct FireEmitter {
    float half_x = 0.2f;
    float half_y = 0.2f;
    float half_z = 0.2f;
    float particles_per_second = 18.0f;
    float intensity = 1.0f;
    bool enabled = false;
};

inline void register_vfx_scene_components(ecs::World& world) {
    (void)world.register_component<SmokeEmitter>();
    (void)world.register_component<FireEmitter>();
}

} // namespace rime::vfx_scene

RIME_REFLECT_BEGIN(rime::vfx_scene::SmokeEmitter)
RIME_REFLECT_FIELD(half_x)
RIME_REFLECT_FIELD(half_y)
RIME_REFLECT_FIELD(half_z)
RIME_REFLECT_FIELD(particles_per_second)
RIME_REFLECT_FIELD(intensity)
RIME_REFLECT_FIELD(enabled)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(rime::vfx_scene::FireEmitter)
RIME_REFLECT_FIELD(half_x)
RIME_REFLECT_FIELD(half_y)
RIME_REFLECT_FIELD(half_z)
RIME_REFLECT_FIELD(particles_per_second)
RIME_REFLECT_FIELD(intensity)
RIME_REFLECT_FIELD(enabled)
RIME_REFLECT_END()
