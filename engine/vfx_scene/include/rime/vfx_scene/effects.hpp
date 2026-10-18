// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/ecs/entity.hpp"
#include "rime/vfx/fire.hpp"

namespace rime::ecs {
class World;
}

namespace rime::vfx_scene {

struct EffectsStats {
    std::uint64_t accepted_ticks = 0;
    std::uint64_t rejected_ticks = 0;
    std::uint64_t lit_ticks = 0;
    // Counts are visits per reconcile, including paused-frame reconciliation, not unique sources.
    std::uint64_t invalid_sources = 0;
    std::uint64_t source_cap_drops = 0;
    std::uint64_t rejected_emissions = 0;
    std::uint64_t emission_budget_dropped_particles = 0;
    std::uint64_t discarded_source_particles = 0; // whole deferred debt discarded by edit/removal
    std::uint64_t deferred_particles = 0; // current gauge; pool admission has separate field stats
};

// A reusable, renderer-free consumer for editor previews or a game's cosmetic presentation.
// Single owner thread, after transforms are derived. No ECS mutations or gameplay forces.
// Admission is ordered by (entity index, generation, kind), independently of archetype layout.
// This is process-local cosmetic identity; a network authority adapter needs canonical source ids.
class Effects {
public:
    static constexpr std::size_t max_sources = 16;
    static constexpr std::size_t max_particles = 256;

    Effects();
    // Reconcile removals/disable/invalid edits even while paused, without aging or emitting.
    // Changed fire descriptions clear their previous light until the next accepted tick.
    void reconcile(ecs::World& world);
    // Strictly increasing caller ticks and 0 < dt <= 0.25 seconds. Rejected ticks mutate no
    // presentation state. Existing particles age once, then sources emit into the shared pool.
    [[nodiscard]] bool tick(ecs::World& world, std::uint64_t tick, float dt);
    // Stop/New Play discard particles, source debt, lights and the local tick clock.
    void reset();

    [[nodiscard]] const vfx::ParticleField& field() const noexcept { return field_; }

    [[nodiscard]] std::span<const vfx::FireLightContribution> lights() const noexcept {
        return {lights_.data(), light_count_};
    }

    [[nodiscard]] std::size_t source_count() const noexcept { return sources_.size(); }

    [[nodiscard]] const EffectsStats& stats() const noexcept { return stats_; }

private:
    struct Description {
        ecs::Entity entity{};
        bool fire = false;
        core::Vec3 lo{}, hi{};
        float rate = 0.0f;
        float intensity = 0.0f;
    };

    struct Source {
        Description description;
        vfx::PlumeEmitter plume{vfx::lingering_smoke(), 8, 16};
        vfx::FirePresentationSource fire;
        bool light_current = false;

        explicit Source(Description d) : description(d), fire(d.rate, 8, 16) {}
    };

    void collect_lights();

    vfx::ParticleField field_{max_particles};
    std::vector<Source> sources_, next_sources_;
    std::array<vfx::FireLightContribution, max_sources> lights_{};
    std::size_t light_count_ = 0;
    bool has_tick_ = false;
    std::uint64_t last_tick_ = 0;
    EffectsStats stats_{};
};

} // namespace rime::vfx_scene
