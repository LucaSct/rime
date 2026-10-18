// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <algorithm>
#include <cmath>
#include <numbers>

#include "rime/fluids_render/phase_surface.hpp"

namespace rime::fluids_render {
namespace {
constexpr unsigned side = 128;
constexpr float tau = 2 * std::numbers::pi_v<float>;
using Pixels = std::array<std::uint8_t, side * side * 4>;

std::uint8_t byte(float x) {
    return static_cast<std::uint8_t>(std::clamp(x, 0.f, 1.f) * 255 + .5f);
}

// Periodic cellular veins: distance to the two closest seeded sites gives a narrow ridge
// along their boundary. This is cosmetic frost detail, not a crack/collision simulation.
float vein(float u, float v) {
    float first = 100, second = 100;
    for (unsigned i = 0; i < 12; ++i) {
        const float x = float((i * 37 + 11) % 101) / 101;
        const float y = float((i * 61 + 29) % 103) / 103;
        float dx = std::abs(u - x), dy = std::abs(v - y);
        dx = std::min(dx, 1 - dx);
        dy = std::min(dy, 1 - dy);
        const float d = std::sqrt(dx * dx + dy * dy);
        if (d < first) {
            second = first;
            first = d;
        } else if (d < second) {
            second = d;
        }
    }
    return std::exp(-(second - first) * 100);
}
} // namespace

PhaseDetail::PhaseDetail(rhi::Device& device) : device_(device) {
    Pixels pixels{};
    const auto upload = [&](rhi::TextureHandle& handle) {
        rhi::TextureDesc desc;
        desc.extent = {side, side};
        desc.mip_levels = 8;
        desc.format = rhi::Format::RGBA8Unorm;
        desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        desc.debug_name = "phase-surface-detail";
        handle = device_.create_texture(desc);
        if (!handle.is_valid())
            return false;
        // RHI uploads are blocking and have no failure return. Never rewrite a sampled map:
        // all frames stay immutable, so multiple in-flight renders cannot race with animation.
        device_.write_texture(handle, pixels.data(), pixels.size());
        return true;
    };
    for (std::size_t f = 0; f <= frames; ++f) {
        const float phase = tau * float(f) / float(frames);
        for (unsigned y = 0; y < side; ++y)
            for (unsigned x = 0; x < side; ++x) {
                const float u = float(x) / side, v = float(y) / side;
                core::Vec3 n;
                if (f == frames) {
                    const float h = 1.f / side;
                    n = {1.5f * (vein(u - h + (x == 0 ? 1.f : 0.f), v) -
                                 vein(u + h - (x == side - 1 ? 1.f : 0.f), v)),
                         1.5f * (vein(u, v - h + (y == 0 ? 1.f : 0.f)) -
                                 vein(u, v + h - (y == side - 1 ? 1.f : 0.f))),
                         1};
                } else {
                    // Two travelling waves, with integer spatial/temporal frequencies, loop
                    // continuously in UV and time. Only shading moves; silhouettes stay fixed.
                    const float a = std::cos(tau * (3 * u + 2 * v) + phase);
                    const float b = std::cos(tau * (u - 4 * v) - 2 * phase);
                    n = {.36f * a + .12f * b, .24f * a - .48f * b, 1};
                }
                n = core::normalize(n);
                const auto i = (y * side + x) * 4;
                pixels[i] = byte(.5f + .5f * n.x);
                pixels[i + 1] = byte(.5f + .5f * n.y);
                pixels[i + 2] = byte(.5f + .5f * n.z);
                pixels[i + 3] = 255;
            }
        if (!upload(normals_[f]))
            return; // partial resources remain private and are released by the destructor
    }
    for (std::size_t t = 0; t < tints; ++t) {
        const float ice = 1 - float(t) / float(tints - 1);
        for (unsigned y = 0; y < side; ++y)
            for (unsigned x = 0; x < side; ++x) {
                const float frost = vein(float(x) / side, float(y) / side);
                const auto i = (y * side + x) * 4;
                // Veins fade away into plain water. Small tint quantization is presentation
                // only; neither this lookup nor any visual detail feeds back into enthalpy.
                pixels[i] = pixels[i + 1] = pixels[i + 2] = byte(1 - ice * .6f * (1 - frost));
                pixels[i + 3] = 255;
            }
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
    // The clock commits alongside accepted state. Rebuild explicitly restarts the cosmetic loop;
    // Pause, rejected steps and repeated renders cannot advance it. It also supports variable dt.
    const auto frame = static_cast<std::size_t>(std::fmod(preview.presentation_seconds(), 1.6) *
                                                double(frames) / 1.6) %
                       frames;
    for (std::size_t r = 0; r < result.count; ++r) {
        auto& row = result.rows[r];
        for (std::size_t i = 0; i < entities.size(); ++i) {
            if (row.entity != entities[i])
                continue;
            const float liquid = static_cast<float>(field->cells()[i].state().liquid_mass_fraction);
            row.override_detail = true;
            row.normal_texture = normals_[liquid == 0 ? frames : frame];
            row.normal_scale = liquid == 0 ? .35f : liquid;
            row.base_color_texture = colors_[static_cast<std::size_t>(
                std::clamp(std::round(liquid * float(tints - 1)), 0.f, float(tints - 1)))];
            break;
        }
    }
    return result;
}
} // namespace rime::fluids_render
