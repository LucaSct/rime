// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "rime/core/math/vec.hpp"
#include "rime/destruction/events.hpp"
#include "rime/vfx/dust.hpp"
#include "rime/vfx/fire.hpp"
#include "rime/vfx/plume.hpp"

namespace rime::block_play {

// CPU-only bridge from this sample's scripted sources and immutable destruction events to the
// removable VFX module. The source is presentation only: it neither ignites gameplay fire nor
// changes destruction. One field caps all ambient, fire, and collapse particles together.
class PlayEffects {
public:
    static constexpr std::uint32_t kParticleCapacity = 256;
    static constexpr std::size_t kEventKeysPerTick = 128;
    static constexpr std::size_t kBurstsPerTick = 16;

    struct Stats {
        std::uint64_t accepted_ticks = 0;
        std::uint64_t same_tick_deliveries = 0;
        std::uint64_t rejected_ticks = 0;
        std::uint64_t ignored_events = 0;
        std::uint64_t invalid_events = 0;
        std::uint64_t duplicate_events = 0;
        std::uint64_t budget_dropped_events = 0;
        std::uint64_t emitted_bursts = 0;
    };

    explicit PlayEffects(core::Vec3 source_position);

    // A new tick simulates existing particles once before any emission. More deliveries for that
    // SAME tick may add distinct events without aging or resampling ambient/fire. Older ticks and
    // invalid dt are rejected. The caller supplies its canonical fixed 60 Hz step.
    [[nodiscard]] bool
    tick(std::uint64_t tick, std::span<const destruction::DestructionEvent> events, float dt);

    void set_fire_enabled(bool enabled) noexcept { fire_enabled_ = enabled; }

    [[nodiscard]] const vfx::ParticleField& field() const noexcept { return field_; }

    [[nodiscard]] std::optional<vfx::FireLightContribution> light() const noexcept {
        return fire_.light();
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct EventKey {
        destruction::DestructionEventKind kind;
        destruction::InstanceId instance;
        std::uint32_t part;
        physics::BodyId body;
        [[nodiscard]] bool operator==(const EventKey&) const noexcept = default;
    };

    core::Vec3 source_position_;
    vfx::ParticleField field_{kParticleCapacity};
    vfx::PlumeEmitter ambient_;
    vfx::FirePresentationSource fire_{18.0f};
    bool fire_enabled_ = true;
    bool has_tick_ = false;
    std::uint64_t last_tick_ = 0;
    std::array<EventKey, kEventKeysPerTick> keys_{};
    std::size_t key_count_ = 0;
    std::size_t bursts_this_tick_ = 0;
    Stats stats_{};
};

} // namespace rime::block_play
