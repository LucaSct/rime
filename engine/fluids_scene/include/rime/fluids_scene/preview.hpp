// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once
#include <array>
#include <span>

#include "rime/fluids/thermal_field.hpp"
#include "rime/fluids_scene/components.hpp"

namespace rime::fluids_scene {
enum class PreviewStatus { Empty, Ready, InvalidDescription, CapacityExceeded, StepRefused };

struct PreviewStats {
    std::uint64_t accepted_ticks = 0;
    std::uint64_t refused_ticks = 0;
    std::uint64_t invalid_builds = 0;
    std::uint64_t capacity_refusals = 0;
};

// Single-owner preview, above ECS and the numerical solver. An enabled graph is admitted whole;
// malformed/capped descriptions clear old values. Every relevant edit reconstructs ALL cells
// from authored initial conditions, including while paused. Unchanged and unrelated edits retain
// live state. This reset policy is explicit: no guessing how edited mass/material inherits heat.
class ThermalPreview {
public:
    static constexpr std::size_t max_cells = 16, max_contacts = 32, max_sources = 16;
    void reconcile(ecs::World& world);
    [[nodiscard]] bool tick(ecs::World& world, std::uint64_t host_tick, double dt);
    // Stop/New Play discards runtime and cached descriptions; lifetime counters/revision remain.
    void reset();

    [[nodiscard]] PreviewStatus status() const noexcept { return status_; }

    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

    [[nodiscard]] std::uint64_t accepted_host_tick() const noexcept { return accepted_host_tick_; }

    // Cosmetic clock only: sum of accepted requested durations in this graph revision. It is
    // NOT an energy/transport receipt. Rebuild/Stop reset it; Pause and refusal leave it unchanged.
    [[nodiscard]] double presentation_seconds() const noexcept { return presentation_seconds_; }

    [[nodiscard]] ecs::Entity problem_entity() const noexcept { return problem_; }

    [[nodiscard]] const PreviewStats& stats() const noexcept { return stats_; }

    [[nodiscard]] const fluids::ThermalField* field() const noexcept {
        return field_ ? &*field_ : nullptr;
    }

    [[nodiscard]] std::span<const ecs::Entity> entities() const noexcept {
        return {entities_.data(), field_ ? descriptions_.cell_count : 0};
    }

    [[nodiscard]] const fluids::ThermalFieldResult& last_result() const noexcept {
        return last_result_;
    }

    [[nodiscard]] bool active() const noexcept {
        return descriptions_.cell_count || descriptions_.contact_count ||
               descriptions_.source_count;
    }

private:
    template <class T> struct Row {
        ecs::Entity entity{};
        T value{};
    };

    struct Description {
        std::array<Row<ThermalCell>, max_cells> cells{};
        std::array<Row<ThermalContact>, max_contacts> contacts{};
        std::array<Row<ThermalSource>, max_sources> sources{};
        std::size_t cell_count = 0, contact_count = 0, source_count = 0;
        bool overflow = false;
    } descriptions_{};

    std::optional<fluids::ThermalField> field_;
    std::array<ecs::Entity, max_cells> entities_{};
    PreviewStatus status_ = PreviewStatus::Empty;
    ecs::Entity problem_{};
    bool has_description_ = false, has_tick_ = false;
    std::uint64_t revision_ = 0, last_host_tick_ = 0, accepted_host_tick_ = 0;
    fluids::ThermalFieldResult last_result_{};
    double presentation_seconds_ = 0;
    PreviewStats stats_{};
};
} // namespace rime::fluids_scene
