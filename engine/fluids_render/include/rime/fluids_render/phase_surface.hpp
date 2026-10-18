// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once
#include <array>

#include "rime/fluids_scene/preview.hpp"
#include "rime/render/scene_renderer.hpp"

namespace rime::fluids_render {
// Opt-in diagnostic appearance. Fixed mesh and mass: this is not geometric ice thickness,
// transparency or flowing water. Blue frost and darker water distinguish the endpoints.
struct ThermalSurface {
    bool enabled = false;
};

inline void register_fluids_render_components(ecs::World& world) {
    (void)world.register_component<ThermalSurface>();
}

struct PhaseSurfaces {
    std::array<render::SurfaceAppearance, fluids_scene::ThermalPreview::max_cells> rows{};
    std::size_t count = 0;

    [[nodiscard]] std::span<const render::SurfaceAppearance> span() const {
        return {rows.data(), count};
    }
};

// Reads the current committed field only. Invalid/reset graphs have no field; refused steps
// retain their previous appearance, with refusal reported by the independent editor diagnostics.
[[nodiscard]] PhaseSurfaces phase_surfaces(ecs::World& world,
                                           const fluids_scene::ThermalPreview& preview);

// Optional immutable GPU detail. Create before the renderer, destroy after it; device outlives
// both. Destruction waits for GPU completion. Failure keeps the RGB/roughness-only fallback.
class PhaseDetail {
public:
    explicit PhaseDetail(rhi::Device& device);
    ~PhaseDetail();
    PhaseDetail(const PhaseDetail&) = delete;
    PhaseDetail& operator=(const PhaseDetail&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }

    [[nodiscard]] PhaseSurfaces surfaces(ecs::World& world,
                                         const fluids_scene::ThermalPreview& preview) const;

private:
    static constexpr std::size_t frames = 64, tints = 9;
    rhi::Device& device_;
    std::array<rhi::TextureHandle, frames + 1> normals_{};
    std::array<rhi::TextureHandle, tints> colors_{};
    bool ready_ = false;
};
} // namespace rime::fluids_render
RIME_REFLECT_BEGIN(rime::fluids_render::ThermalSurface)
RIME_REFLECT_FIELD(enabled)
RIME_REFLECT_END()
