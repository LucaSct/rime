// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/temporal_jitter.hpp"

namespace rime::render {
namespace {

// Radical inverse: write `index` in `base`, mirror the digits about the radix point.
// Base 2: 1 = 1b -> 0.1b = 0.5; 2 = 10b -> 0.01b = 0.25; 3 = 11b -> 0.11b = 0.75.
constexpr float halton_ct(std::uint32_t index, std::uint32_t base) noexcept {
    float result = 0.0f;
    float f = 1.0f / static_cast<float>(base);
    while (index > 0) {
        result += f * static_cast<float>(index % base);
        index /= base;
        f /= static_cast<float>(base);
    }
    return result;
}

// Mean of the first kPeriod Halton values (indices 1..kPeriod) on one axis. Subtracting this,
// rather than a flat 0.5, makes the sequence zero-mean over one period.
constexpr float period_mean(std::uint32_t base) noexcept {
    float sum = 0.0f;
    for (std::uint32_t i = 1; i <= TemporalJitter::kPeriod; ++i) {
        sum += halton_ct(i, base);
    }
    return sum / static_cast<float>(TemporalJitter::kPeriod);
}

constexpr float kCentreX = period_mean(2);
constexpr float kCentreY = period_mean(3);

} // namespace

float halton(std::uint32_t index, std::uint32_t base) noexcept {
    return halton_ct(index, base);
}

core::Vec2 TemporalJitter::offset_for(std::uint64_t index) noexcept {
    const auto i = static_cast<std::uint32_t>(index % kPeriod) + 1; // Halton is 1-based
    return core::Vec2{halton_ct(i, 2) - kCentreX, halton_ct(i, 3) - kCentreY};
}

core::Mat4 jitter_projection(const core::Mat4& proj,
                             core::Vec2 offset_px,
                             std::uint32_t width,
                             std::uint32_t height) noexcept {
    if ((offset_px.x == 0.0f && offset_px.y == 0.0f) || width == 0 || height == 0) {
        return proj;
    }
    const float sx = 2.0f * offset_px.x / static_cast<float>(width);
    const float sy = 2.0f * offset_px.y / static_cast<float>(height);
    core::Mat4 r = proj;
    for (int c = 0; c < 4; ++c) {
        r.at(0, c) += sx * proj.at(3, c);
        r.at(1, c) += sy * proj.at(3, c);
    }
    return r;
}

} // namespace rime::render
