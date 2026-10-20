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

// Renderer-independent presentation: games and the editor use the same accepted source state.
// These are cosmetic billboards, not burning matter, fuel, heat or gameplay collision.
struct FlamePresentation {
    core::Vec3 position{};
    float half_size = 0.0f;
    core::Vec3 color{1.0f, 0.35f, 0.06f};
    float opacity = 0.0f;
    bool core = false; // broad, persistent fuel lobe; tongues/embers use the tall profile
};

struct SmokeAppearance {
    core::Vec3 color{};
    float opacity = 0.0f;
};

// Soft birth/retirement and bounded local fire illumination. No wall clock, random stream or
// renderer state enters this function; a paused particle/light snapshot gives identical output.
[[nodiscard]] SmokeAppearance
smoke_appearance(const vfx::Particle& particle,
                 std::span<const vfx::FireLightContribution> lights) noexcept;

// A reusable, renderer-free consumer for editor previews or a game's cosmetic presentation.
// Single owner thread, after transforms are derived. No ECS mutations or gameplay forces.
// Admission is ordered by (entity index, generation, kind), independently of archetype layout.
// This is process-local cosmetic identity; a network authority adapter needs canonical source ids.
class Effects {
public:
    static constexpr std::size_t max_sources = 16;
    static constexpr std::size_t max_particles = 256;
    static constexpr std::size_t max_fire_motes_per_source = 12;
    static constexpr std::size_t core_flames_per_source = 4;
    static constexpr std::size_t upper_tongues_per_source = 4;
    static constexpr std::size_t max_flames =
        max_sources *
        (3 + core_flames_per_source + upper_tongues_per_source + max_fire_motes_per_source);

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

    [[nodiscard]] std::span<const FlamePresentation> flames() const noexcept {
        return {flames_.data(), flame_count_};
    }

    // The renderer sees the three stable tongues, four fuel cores and capped flame/ember motes.
    [[nodiscard]] std::span<const FlamePresentation> render_flames() const noexcept {
        return {render_flames_.data(), render_flame_count_};
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
        struct FireMote {
            core::Vec3 position{}, velocity{};
            float age = 0.0f, lifetime = 1.0f, size = 0.1f;
            bool ember = false;
        };

        Description description;
        vfx::PlumeEmitter plume{vfx::lingering_smoke(), 8, 16};
        vfx::FirePresentationSource fire;
        bool light_current = false;
        std::array<FireMote, max_fire_motes_per_source> motes{};
        std::size_t mote_count = 0;
        double mote_debt = 0.0;
        std::uint32_t mote_sequence = 0;

        explicit Source(Description d) : description(d), fire(d.rate, 8, 16) {}
    };

    void collect_lights();
    void advance_fire(Source& source, float dt);

    vfx::ParticleField field_{max_particles};
    std::vector<Source> sources_, next_sources_;
    std::array<vfx::FireLightContribution, max_sources> lights_{};
    std::size_t light_count_ = 0;
    std::array<FlamePresentation, max_flames> flames_{};
    std::size_t flame_count_ = 0;
    std::array<FlamePresentation, max_flames> render_flames_{};
    std::size_t render_flame_count_ = 0;
    double presentation_seconds_ = 0.0;
    bool has_tick_ = false;
    std::uint64_t last_tick_ = 0;
    EffectsStats stats_{};
};

} // namespace rime::vfx_scene
