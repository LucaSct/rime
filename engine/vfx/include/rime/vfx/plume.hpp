// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/vfx/dust.hpp"

namespace rime::vfx {

// A CPU adapter for a continuous, cosmetic source. The caller owns time, world bounds and the
// ParticleField. Intensity multiplies particles_per_second; emitted bursts always use intensity 1
// so ParticleField does not round the count a second time or change particle speed with rate.
// The same valid call sequence is deterministic. Different dt partitions can produce different
// scatter/particle ages and are not promised to have identical visual state.
struct PlumeStats {
    std::uint64_t requested_particles = 0; // whole particles accrued from accepted time steps
    std::uint64_t emitted_particles = 0;   // forwarded to ParticleField (it may drop at capacity)
    std::uint64_t dropped_particles = 0;   // overflow beyond finite deferred storage
    std::uint64_t deferred_particles = 0;  // current whole-particle debt, a gauge
    std::uint64_t rejected_updates = 0;
    std::uint64_t capped_updates = 0;
    std::uint64_t resets = 0;
};

class PlumeEmitter {
public:
    explicit PlumeEmitter(EmitParams params = lingering_smoke(),
                          std::uint32_t max_per_update = 128,
                          std::uint32_t max_deferred = 1024) noexcept;

    // dt, rate and intensity must be finite and positive; bounds must be finite, ordered and
    // representable by ParticleField. Rejected calls leave debt and the field RNG unchanged.
    // A valid update forwards at most max_per_update particles, independent of elapsed dt.
    [[nodiscard]] bool update(ParticleField& field,
                              core::Vec3 bounds_min,
                              core::Vec3 bounds_max,
                              float dt,
                              float particles_per_second,
                              float intensity);

    // Extinguishing or replacing a source deliberately discards all fractional and whole debt.
    // Existing particles continue their ordinary lifetime in the caller's ParticleField.
    void reset() noexcept;

    [[nodiscard]] const PlumeStats& stats() const noexcept { return stats_; }

    void reset_stats() noexcept;

private:
    EmitParams params_;
    std::uint32_t max_per_update_;
    std::uint32_t max_deferred_;
    double fractional_debt_ = 0.0;
    PlumeStats stats_{};
};

} // namespace rime::vfx
