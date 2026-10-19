// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstdio>
#include <memory>
#include <span>

#include "rime/fluids_render/phase_surface.hpp"
#include "rime/vfx_scene/effects.hpp"

namespace rime::block_play {

// Client-local cosmetic interpretation of authored components. Neither heat nor these particles
// decide gameplay forces, damage, ignition or replicated authority. The caller supplies accepted
// fixed ticks; publishing a main/scope frame only reconciles edits and copies the same snapshot.
class AuthoredEnvironment {
public:
    static constexpr std::size_t legacy_smoke_capacity = 256;
    static constexpr std::size_t smoke_capacity =
        legacy_smoke_capacity + rime::vfx_scene::Effects::max_particles;

    struct Snapshot {
        std::array<rime::render::GpuParticle, smoke_capacity> smoke{};
        std::array<rime::render::GpuParticle, rime::vfx_scene::Effects::max_flames> flames{};
        std::array<rime::render::GpuPointLight, rime::vfx_scene::Effects::max_sources> lights{};
        rime::fluids_render::PhaseSurfaces surfaces{};
        std::size_t smoke_count = 0, flame_count = 0, light_count = 0;
        std::size_t enabled_sources = 0;
        // False on an untouched scene: leave the existing --effects-preview setter/toggle alone.
        // True for one final clearing snapshot after the last authored puff retires.
        bool override_smoke = false;

        [[nodiscard]] std::span<const rime::render::GpuParticle> smoke_span() const {
            return {smoke.data(), smoke_count};
        }
    };

    // A null device supports numerical/assembly proofs without allocating an atlas. In the game,
    // declare this owner BEFORE both renderers: its lazy immutable detail textures are borrowed
    // by their copied surface rows, and must survive their destruction and outstanding GPU work.
    explicit AuthoredEnvironment(rime::rhi::Device* device = nullptr) : device_(device) {}

    ~AuthoredEnvironment();
    AuthoredEnvironment(const AuthoredEnvironment&) = delete;
    AuthoredEnvironment& operator=(const AuthoredEnvironment&) = delete;

    [[nodiscard]] bool tick(rime::ecs::World& world, std::uint64_t tick, double dt);
    // Reconciliation does not advance age, emission, phase energy or the accepted detail clock.
    [[nodiscard]] const Snapshot& prepare(rime::ecs::World& world,
                                          std::span<const rime::render::GpuParticle> legacy = {});
    void publish(rime::ecs::World& world,
                 std::span<const rime::render::GpuParticle> legacy,
                 rime::render::SceneRenderer& main,
                 rime::render::SceneRenderer* scope = nullptr);
    // A newly created scope consumes the last publication in the same frame, without reticking.
    void apply_to(rime::render::SceneRenderer& renderer) const;
    void reset();
    void report(std::FILE* output) const;

    [[nodiscard]] const Snapshot& snapshot() const noexcept { return snapshot_; }

    [[nodiscard]] const rime::vfx_scene::Effects& effects() const noexcept { return effects_; }

    [[nodiscard]] const rime::fluids_scene::ThermalPreview& thermal() const noexcept {
        return thermal_;
    }

    [[nodiscard]] bool detail_allocated() const noexcept { return detail_ != nullptr; }

    [[nodiscard]] bool detail_ready() const noexcept { return detail_ && detail_->ready(); }

    [[nodiscard]] std::uint64_t legacy_smoke_dropped() const noexcept { return legacy_dropped_; }

private:
    rime::rhi::Device* device_;
    std::unique_ptr<rime::fluids_render::PhaseDetail> detail_;
    rime::vfx_scene::Effects effects_;
    rime::fluids_scene::ThermalPreview thermal_;
    Snapshot snapshot_{};
    bool owned_smoke_ = false;
    std::uint64_t legacy_dropped_ = 0;
};
} // namespace rime::block_play
