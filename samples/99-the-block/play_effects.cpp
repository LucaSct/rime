// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_effects.hpp"

#include <algorithm>
#include <cmath>

namespace rime::block_play {
namespace {

[[nodiscard]] vfx::EmitParams ambient_params() noexcept {
    vfx::EmitParams p = vfx::lingering_smoke();
    p.count = 1; // PlumeEmitter replaces this with its accrued whole-particle count.
    p.speed = 0.15f;
    p.lift = 0.4f;
    p.gravity = -0.12f;
    p.min_lifetime = 4.0f;
    p.max_lifetime = 7.0f;
    p.min_size = 0.15f;
    p.max_size = 0.25f;
    p.growth = 0.10f;
    return p;
}

[[nodiscard]] vfx::EmitParams collapse_params(vfx::Family family) noexcept {
    vfx::EmitParams p =
        family == vfx::Family::ImpactDust ? vfx::impact_dust() : vfx::lingering_smoke();
    p.count = 8;
    if (family == vfx::Family::ImpactDust) {
        p.min_size = 0.06f;
        p.max_size = 0.12f;
        p.growth = 0.15f;
    }
    return p;
}

[[nodiscard]] bool finite(core::Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool valid_bounds(const physics::Aabb& b) noexcept {
    return finite(b.min) && finite(b.max) && b.min.x <= b.max.x && b.min.y <= b.max.y &&
           b.min.z <= b.max.z && finite(b.max - b.min) && finite((b.min + b.max) * 0.5f);
}

} // namespace

PlayEffects::PlayEffects(core::Vec3 source_position)
    : source_position_(source_position), ambient_(ambient_params(), 8, 16) {}

bool PlayEffects::tick(std::uint64_t tick,
                       std::span<const destruction::DestructionEvent> events,
                       float dt) {
    if (!std::isfinite(dt) || !(dt > 0.0f) || !finite(source_position_) ||
        (has_tick_ && tick < last_tick_)) {
        ++stats_.rejected_ticks;
        return false;
    }

    if (!has_tick_ || tick > last_tick_) {
        has_tick_ = true;
        last_tick_ = tick;
        key_count_ = 0;
        bursts_this_tick_ = 0;
        ++stats_.accepted_ticks;

        // Retire and move OLD particles first. New particles start at age zero for the entire
        // tick, and a second event delivery cannot simulate them twice.
        field_.simulate(dt);
        const core::Vec3 ambient_half{0.15f, 0.15f, 0.15f};
        (void)ambient_.update(field_,
                              source_position_ - ambient_half,
                              source_position_ + ambient_half,
                              dt,
                              6.0f,
                              1.0f);

        const core::Vec3 fire_center = source_position_ + core::Vec3{2.0f, 0.0f, 0.0f};
        const core::Vec3 fire_half{0.15f, 0.15f, 0.15f};
        const vfx::IgnitionSnapshot ignition{
            1, tick, fire_center - fire_half, fire_center + fire_half, fire_enabled_ ? 1.0f : 0.0f};
        (void)fire_.consume(field_, ignition, dt);
    } else {
        ++stats_.same_tick_deliveries;
    }

    for (const auto& event : events) {
        if (event.kind != destruction::DestructionEventKind::PartDied &&
            event.kind != destruction::DestructionEventKind::IslandDetached) {
            ++stats_.ignored_events;
            continue;
        }
        if (!valid_bounds(event.world_bounds)) {
            ++stats_.invalid_events;
            continue;
        }
        const EventKey key{event.kind, event.instance, event.part, event.body};
        if (std::find(keys_.begin(), keys_.begin() + key_count_, key) !=
            keys_.begin() + key_count_) {
            ++stats_.duplicate_events;
            continue;
        }
        if (key_count_ == kEventKeysPerTick) {
            ++stats_.budget_dropped_events;
            continue;
        }
        keys_[key_count_++] = key;
        if (bursts_this_tick_ == kBurstsPerTick) {
            ++stats_.budget_dropped_events;
            continue;
        }

        // Some support-loss detachments carry zero impulse. A positive cosmetic baseline makes
        // their visibly falling island produce smoke without pretending it had a damage impulse.
        const auto family = event.kind == destruction::DestructionEventKind::PartDied
                                ? vfx::Family::ImpactDust
                                : vfx::Family::LingeringSmoke;
        field_.emit_burst(
            event.world_bounds.min, event.world_bounds.max, 1.0f, collapse_params(family));
        ++bursts_this_tick_;
        ++stats_.emitted_bursts;
    }
    return true;
}

} // namespace rime::block_play
