// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <algorithm>
#include <cmath>
#include <new>

#include "rime/fluids_render/phase_maps.hpp"
#include "rime/fluids_render/phase_surface.hpp"

namespace rime::fluids_render {
static_assert(detail::normal_count == 1 + 8 * 32 && detail::tint_levels == 9,
              "PhaseDetail's private table sizes must match the pure generator");

PhaseDetail::PhaseDetail(rhi::Device& device) : device_(device) {
    detail::Pixels pixels{};
    detail::FrostField frost;
    try {
        frost = detail::make_frost_field();
    } catch (const std::bad_alloc&) {
        return; // factor-only fallback; nothing created, nothing to tear down
    }
    const auto upload = [&](rhi::TextureHandle& handle) {
        rhi::TextureDesc desc;
        desc.extent = {detail::map_side, detail::map_side};
        desc.mip_levels = detail::map_mips;
        desc.format = rhi::Format::RGBA8Unorm;
        desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        desc.debug_name = "phase-surface-detail";
        handle = device_.create_texture(desc);
        if (!handle.is_valid())
            return false;
        // RHI uploads are blocking and have no failure return. Never rewrite a sampled map:
        // all maps stay immutable, so multiple in-flight renders cannot race with animation.
        device_.write_texture(handle, pixels.data(), pixels.size());
        return true;
    };
    // Ice and water are baked TOGETHER into one normal atlas (static frost slopes + travelling
    // water slopes, blended per liquid level) because a sampled normal map has a single
    // normal_scale: scaling cannot fade water while holding frost. Selection is a pure lookup.
    for (std::size_t n = 0; n < normals_.size(); ++n) {
        const std::size_t level = n == 0 ? 0 : 1 + (n - 1) / detail::time_frames;
        const std::size_t frame = n == 0 ? 0 : (n - 1) % detail::time_frames;
        detail::fill_normal(pixels, frost, level, frame);
        if (!upload(normals_[n]))
            return; // partial resources remain private and are released by the destructor
    }
    for (std::size_t t = 0; t < tints; ++t) {
        detail::fill_tint(pixels, frost, t);
        if (!upload(colors_[t]))
            return;
    }
    ready_ = true;
}

PhaseDetail::~PhaseDetail() {
    // Cold teardown only. A renderer may still have submitted work borrowing these handles.
    device_.wait_idle();
    for (const auto t : normals_)
        device_.destroy(t);
    for (const auto t : colors_)
        device_.destroy(t);
}

PhaseSurfaces PhaseDetail::surfaces(ecs::World& world,
                                    const fluids_scene::ThermalPreview& preview) const {
    auto result = phase_surfaces(world, preview);
    if (!ready_ || !result.count)
        return result;
    const auto* field = preview.field();
    const auto entities = preview.entities();
    for (std::size_t r = 0; r < result.count; ++r) {
        auto& row = result.rows[r];
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (row.entity != entities[i])
                continue;
            // The clock commits alongside accepted state. Rebuild restarts the cosmetic loop;
            // Pause, rejected steps and repeated renders cannot advance it. It also supports
            // variable dt.
            const auto pick = detail::select_detail(preview.presentation_seconds(),
                                                    field->cells()[i].state().liquid_mass_fraction);
            row.override_detail = true;
            row.normal_texture = normals_[pick.normal_index];
            row.normal_scale = 1.f; // strength is baked per liquid level, so no endpoint jump
            row.base_color_texture = colors_[pick.tint_index];
            break;
        }
    }
    return result;
}
} // namespace rime::fluids_render
