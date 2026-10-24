// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/vfx/fire.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rime::vfx {
namespace {

[[nodiscard]] bool finite(core::Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool valid_bounds(core::Vec3 lo, core::Vec3 hi) noexcept {
    return finite(lo) && finite(hi) && hi.x >= lo.x && hi.y >= lo.y && hi.z >= lo.z &&
           finite(core::Vec3{0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z)}) &&
           finite(hi - lo);
}

void increment(std::uint64_t& counter) noexcept {
    if (counter != std::numeric_limits<std::uint64_t>::max()) {
        ++counter;
    }
}

} // namespace

FirePresentationSource::FirePresentationSource(float particles_per_second,
                                               std::uint32_t max_smoke_per_update,
                                               std::uint32_t max_smoke_deferred) noexcept
    : smoke_(lingering_smoke(), max_smoke_per_update, max_smoke_deferred),
      particles_per_second_(particles_per_second) {}

bool FirePresentationSource::consume(ParticleField& field,
                                     const IgnitionSnapshot& snapshot,
                                     float dt) {
    if (!std::isfinite(snapshot.intensity) || snapshot.intensity < 0.0f || !std::isfinite(dt) ||
        !(dt > 0.0f) || !std::isfinite(particles_per_second_) || !(particles_per_second_ > 0.0f) ||
        !valid_bounds(snapshot.bounds_min, snapshot.bounds_max)) {
        increment(stats_.rejected_snapshots);
        return false;
    }
    if (bound_ && snapshot.source_key != source_key_) {
        increment(stats_.rejected_snapshots);
        increment(stats_.source_cap_rejections);
        return false;
    }
    if (bound_ && snapshot.tick <= last_tick_) {
        increment(stats_.rejected_snapshots);
        increment(stats_.stale_ticks);
        return false;
    }

    const float intensity = std::min(snapshot.intensity, 4.0f);
    if (snapshot.intensity > 4.0f) {
        increment(stats_.intensity_clamps);
    }
    if (intensity > 0.0f && !smoke_.update(field,
                                           snapshot.bounds_min,
                                           snapshot.bounds_max,
                                           dt,
                                           particles_per_second_,
                                           intensity)) {
        increment(stats_.rejected_snapshots);
        return false;
    }

    bound_ = true;
    source_key_ = snapshot.source_key;
    last_tick_ = snapshot.tick;
    increment(stats_.accepted_snapshots);
    if (intensity == 0.0f) {
        if (light_) {
            increment(stats_.extinguishes);
        }
        light_.reset();
        smoke_.reset();
        return true;
    }
    const core::Vec3 extent = snapshot.bounds_max - snapshot.bounds_min;
    const core::Vec3 center{0.5f * (snapshot.bounds_min.x + snapshot.bounds_max.x),
                            0.5f * (snapshot.bounds_min.y + snapshot.bounds_max.y),
                            0.5f * (snapshot.bounds_min.z + snapshot.bounds_max.z)};
    const float radius = std::min(8.0f, 0.5f + std::max({extent.x, extent.y, extent.z}));
    light_ = FireLightContribution{center, {1.0f, 0.35f, 0.08f}, intensity, radius};
    return true;
}

bool FirePresentationSource::stop(std::uint64_t source_key, std::uint64_t tick) noexcept {
    if (!bound_ || source_key != source_key_) {
        increment(stats_.rejected_snapshots);
        increment(stats_.source_cap_rejections);
        return false;
    }
    if (tick <= last_tick_) {
        increment(stats_.rejected_snapshots);
        increment(stats_.stale_ticks);
        return false;
    }
    last_tick_ = tick;
    light_.reset();
    smoke_.reset();
    increment(stats_.stops);
    return true;
}

void FirePresentationSource::reset() noexcept {
    bound_ = false;
    source_key_ = 0;
    last_tick_ = 0;
    light_.reset();
    smoke_.reset();
}

} // namespace rime::vfx
