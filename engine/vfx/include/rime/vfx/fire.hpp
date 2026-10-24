// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>

#include "rime/vfx/plume.hpp"

namespace rime::vfx {

// Canonical gameplay owns ignition and supplies one snapshot per increasing source tick. This
// visual adapter does not decide whether a source burns, spreads, deals damage or consumes fuel.
struct IgnitionSnapshot {
    std::uint64_t source_key = 0;
    std::uint64_t tick = 0;
    core::Vec3 bounds_min{0.0f, 0.0f, 0.0f};
    core::Vec3 bounds_max{0.0f, 0.0f, 0.0f};
    float intensity = 0.0f; // zero is an authoritative extinguish
};

// Plain scene-extraction data. Consumers choose how to append and cap light rows.
struct FireLightContribution {
    core::Vec3 position{0.0f, 0.0f, 0.0f};
    core::Vec3 color{1.0f, 0.35f, 0.08f};
    float intensity = 0.0f;
    float radius = 0.0f;
};

struct FirePresentationStats {
    std::uint64_t accepted_snapshots = 0;
    std::uint64_t rejected_snapshots = 0;
    std::uint64_t stale_ticks = 0;
    std::uint64_t source_cap_rejections = 0; // this object holds exactly one source
    std::uint64_t intensity_clamps = 0;
    std::uint64_t extinguishes = 0;
    std::uint64_t stops = 0;
};

// One bounded source, intended for a scripted demo or a caller-managed capped source array.
// Positive snapshots drive smoke at the configured rate and publish one light. An off snapshot
// clears light and plume debt; previously spawned smoke lives out its normal ParticleField life.
// stop() retires presentation without advancing a clock. reset() explicitly permits a new key.
class FirePresentationSource {
public:
    explicit FirePresentationSource(float particles_per_second = 18.0f,
                                    std::uint32_t max_smoke_per_update = 128,
                                    std::uint32_t max_smoke_deferred = 1024) noexcept;

    [[nodiscard]] bool consume(ParticleField& field, const IgnitionSnapshot& snapshot, float dt);
    [[nodiscard]] bool stop(std::uint64_t source_key, std::uint64_t tick) noexcept;
    void reset() noexcept;

    [[nodiscard]] std::optional<FireLightContribution> light() const noexcept { return light_; }

    [[nodiscard]] const FirePresentationStats& stats() const noexcept { return stats_; }

    [[nodiscard]] const PlumeStats& smoke_stats() const noexcept { return smoke_.stats(); }

    [[nodiscard]] bool bound() const noexcept { return bound_; }

    [[nodiscard]] bool burning() const noexcept { return light_.has_value(); }

private:
    PlumeEmitter smoke_;
    float particles_per_second_;
    bool bound_ = false;
    std::uint64_t source_key_ = 0;
    std::uint64_t last_tick_ = 0;
    std::optional<FireLightContribution> light_;
    FirePresentationStats stats_{};
};

} // namespace rime::vfx
