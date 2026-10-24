// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once
#include <array>

#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/fluids_render/phase_surface.hpp"
#include "rime/render/mesh.hpp"
#include "rime/vfx_scene/components.hpp"
#include "rime/worldkit/profile.hpp"

namespace courtyard {
// Geometry/palette are sample content, never numerical state. Reconstruct runtime mesh/material
// ids from these persistent roles on every load so the saved scene survives another process.
struct Part {
    std::uint32_t shape = 0, palette = 0;
};

struct Station {
    std::uint32_t kind = 0;
}; // left cooling, center melting, right insulated control

void register_components(rime::ecs::World& world);
void build_scene(rime::ecs::World& world);
void prepare(rime::ecs::World& world,
             rime::render::MeshRegistry& meshes,
             rime::render::MaterialRegistry& materials);
} // namespace courtyard
RIME_REFLECT_BEGIN(courtyard::Part)
RIME_REFLECT_FIELD(shape)
RIME_REFLECT_FIELD(palette)
RIME_REFLECT_END()
RIME_REFLECT_BEGIN(courtyard::Station)
RIME_REFLECT_FIELD(kind)
RIME_REFLECT_END()
