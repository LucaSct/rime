// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/vfx/plume.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rime::vfx {
namespace {

[[nodiscard]] bool finite(core::Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool valid(core::Vec3 lo, core::Vec3 hi) noexcept {
    if (!finite(lo) || !finite(hi) || hi.x < lo.x || hi.y < lo.y || hi.z < lo.z) {
        return false;
    }
    return finite(core::Vec3{0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)}) &&
           finite(hi - lo);
}

[[nodiscard]] bool valid(const EmitParams& p) noexcept {
    return std::isfinite(p.speed) && p.speed >= 0.0f && std::isfinite(p.lift) &&
           std::isfinite(p.gravity) && std::isfinite(p.min_lifetime) &&
           std::isfinite(p.max_lifetime) && p.min_lifetime > 0.0f &&
           p.max_lifetime >= p.min_lifetime && std::isfinite(p.min_size) &&
           std::isfinite(p.max_size) && p.min_size >= 0.0f && p.max_size >= p.min_size &&
           std::isfinite(p.growth) &&
           (p.family == Family::ImpactDust || p.family == Family::LingeringSmoke ||
            p.family == Family::MuzzleFlash);
}

void add_saturating(std::uint64_t& destination, std::uint64_t amount) noexcept {
    destination += std::min(amount, std::numeric_limits<std::uint64_t>::max() - destination);
}

[[nodiscard]] std::uint64_t saturating_floor(double value) noexcept {
    if (value >= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(value);
}

} // namespace

PlumeEmitter::PlumeEmitter(EmitParams params,
                           std::uint32_t max_per_update,
                           std::uint32_t max_deferred) noexcept
    : params_(params), max_per_update_(max_per_update), max_deferred_(max_deferred) {}

bool PlumeEmitter::update(ParticleField& field,
                          core::Vec3 bounds_min,
                          core::Vec3 bounds_max,
                          float dt,
                          float particles_per_second,
                          float intensity) {
    if (!valid(bounds_min, bounds_max) || !valid(params_) || !std::isfinite(dt) || !(dt > 0.0f) ||
        !std::isfinite(particles_per_second) || !(particles_per_second > 0.0f) ||
        !std::isfinite(intensity) || !(intensity > 0.0f)) {
        add_saturating(stats_.rejected_updates, 1);
        return false;
    }

    const double produced = static_cast<double>(dt) * particles_per_second * intensity;
    // Every input is finite float, so this product fits in double. Retain fractional emission
    // only while the finite queue has room. A saturated queue discards excess work explicitly.
    const double accrued = produced + fractional_debt_;
    const std::uint64_t newly_requested = saturating_floor(accrued);
    const double pending = static_cast<double>(stats_.deferred_particles) + accrued;
    const std::uint64_t whole = saturating_floor(pending);
    add_saturating(stats_.requested_particles, newly_requested);
    const std::uint64_t available = std::min(whole, static_cast<std::uint64_t>(max_per_update_));
    const std::uint32_t emitted = static_cast<std::uint32_t>(available);
    const std::uint64_t remainder = whole - available;
    const std::uint64_t deferred = std::min(remainder, static_cast<std::uint64_t>(max_deferred_));
    add_saturating(stats_.dropped_particles, remainder - deferred);
    stats_.deferred_particles = deferred;
    fractional_debt_ = remainder > max_deferred_ ? 0.0 : pending - std::floor(pending);
    if (whole > available) {
        add_saturating(stats_.capped_updates, 1);
    }
    add_saturating(stats_.emitted_particles, emitted);
    if (emitted != 0) {
        EmitParams batch = params_;
        batch.count = emitted;
        field.emit_burst(bounds_min, bounds_max, 1.0f, batch);
    }
    return true;
}

void PlumeEmitter::reset() noexcept {
    fractional_debt_ = 0.0;
    add_saturating(stats_.discarded_on_reset, stats_.deferred_particles);
    stats_.deferred_particles = 0;
    add_saturating(stats_.resets, 1);
}

void PlumeEmitter::reset_stats() noexcept {
    const std::uint64_t debt = stats_.deferred_particles;
    stats_ = {};
    stats_.deferred_particles = debt;
}

} // namespace rime::vfx
