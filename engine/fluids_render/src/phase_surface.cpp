// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/fluids_render/phase_surface.hpp"

namespace rime::fluids_render {
PhaseSurfaces phase_surfaces(ecs::World& world, const fluids_scene::ThermalPreview& preview) {
    PhaseSurfaces result;
    const auto* field = preview.field();
    if (!field)
        return result;
    const auto entities = preview.entities();
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const auto* surface = world.get<ThermalSurface>(entities[i]);
        if (!surface || !surface->enabled)
            continue;
        const float liquid = static_cast<float>(field->cells()[i].state().liquid_mass_fraction);
        // Presentation blend, not a mixture BRDF or volume model. The numerical oracle has
        // already admitted fraction in [0,1]; never infer phase by thresholding temperature.
        const core::Vec3 ice{0.40f, 0.78f, 0.92f}, water{0.015f, 0.14f, 0.32f};
        result.rows[result.count++] = {entities[i],
                                       ice * (1 - liquid) + water * liquid,
                                       0.22f * (1 - liquid) + 0.12f * liquid};
    }
    return result;
}
} // namespace rime::fluids_render
