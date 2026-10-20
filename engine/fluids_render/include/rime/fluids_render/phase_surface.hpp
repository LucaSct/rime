// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once
#include <array>

#include "rime/fluids_scene/preview.hpp"
#include "rime/render/scene_renderer.hpp"

namespace rime::fluids_render {
// Opt-in cosmetic appearance. Fixed mass/collision: this is not simulated ice growth,
// transparency or flowing water. The shader distinguishes clear water, ice and frost;
// creation-time SurfaceGeometry optionally supplies geometric cosmetic waves.
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
    // normals_[0] is the static frost map; 1 + (k-1)*32 + frame holds liquid level k (see
    // phase_maps.hpp for the quantization and exact memory).
    static constexpr std::size_t normal_maps = 1 + 8 * 32, tints = 9;
    rhi::Device& device_;
    std::array<rhi::TextureHandle, normal_maps> normals_{};
    std::array<rhi::TextureHandle, tints> colors_{};
    bool ready_ = false;
};
} // namespace rime::fluids_render
RIME_REFLECT_BEGIN(rime::fluids_render::ThermalSurface)
RIME_REFLECT_FIELD(enabled)
RIME_REFLECT_END()
