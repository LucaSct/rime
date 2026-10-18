// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "rime/fluids/thermal_contact.hpp"

namespace rime::fluids {

struct ThermalLink {
    std::size_t first = 0;
    std::size_t second = 0;
    double conductance_w_k = 0.0;
};

struct ThermalPowerSource {
    std::size_t cell = 0;
    double power_watts = 0.0; // signed: negative removes heat; at most one source per cell
};

struct ThermalFieldConfig {
    double max_substep_seconds = 1.0 / 60.0;
    std::size_t max_substeps = 256;
    std::size_t receipt_budget = 4096;
    // Preflight assumes every contact takes its full 128 trials, even a disabled contact.
    std::size_t contact_trial_budget = 65536;
};

enum class ThermalOperationKind { Source, Contact };

struct ThermalOperation {
    ThermalOperationKind kind = ThermalOperationKind::Source;
    std::size_t descriptor = 0; // index in the field's canonical sources() or links()
    std::size_t substep = 0;
    HeatExchange first{};
    HeatExchange second{}; // empty for a source; contact receipts follow canonical cell order
    double first_before_joules = 0.0;
    double first_after_joules = 0.0;
    double second_before_joules = 0.0;
    double second_after_joules = 0.0;
};

enum class ThermalFieldStatus {
    Accepted,
    InvalidInput,
    UnsupportedEnvironment,
    SubstepBudgetExceeded,
    ReceiptBudgetExceeded,
    WorkBudgetExceeded,
    SourceRefused,
    ContactRefused,
    TickLimit
};

struct ThermalFieldResult {
    ThermalFieldStatus status = ThermalFieldStatus::InvalidInput;
    std::size_t attempted_operations =
        0; // includes a failed primitive; preflight failures use zero
    std::size_t failed_substep = 0;
    std::size_t failed_descriptor = 0;
    HeatStatus heat_status = HeatStatus::Accepted; // relevant on SourceRefused
    ThermalContactStatus contact_status = ThermalContactStatus::NoExchange; // on ContactRefused
};

struct ThermalFieldStep {
    double requested_dt_seconds = 0.0;
    double substep_dt_seconds = 0.0;
    std::size_t substeps = 0;
    std::size_t exchanged_contacts = 0;
    std::size_t unchanged_contacts = 0;
};

// Fixed-mass graph reference: caller supplies cells and geometry-derived links. There is no
// automatic grid/neighbor construction, moving water, mass transfer, expansion or rendering.
// Each substep applies sources by cell index, then links by normalized (first,second) index.
// This ordered first-order split is NOT a simultaneous global backward-Euler solve. Node
// relabeling or timestep subdivision can change results; no network determinism is claimed.
//
// Creation allocates bounded buffers. advance() allocates nothing and commits cells, tick,
// last_step and the complete journal together, or leaves all four unchanged. Returned failure
// diagnostics describe attempted work, never partially committed energy. Zero dt is unchanged.
// The published journal survives refusals and zero dt, and is replaced on the next positive
// successful advance. Retain/copy it externally if a history is needed. Copying a field is
// disabled so vector copies cannot lose preallocated capacities and reintroduce tick allocation.
class ThermalField {
public:
    static constexpr std::size_t max_cells = 256;
    static constexpr std::size_t max_links = 1024;
    static constexpr std::size_t max_sources = 256;
    static constexpr std::size_t max_receipts = 16384;
    static constexpr std::size_t max_allowed_substeps = 1024;
    static constexpr std::size_t max_contact_trials =
        max_receipts * ThermalContactResult::maximum_iterations;

    ThermalField(const ThermalField&) = delete;
    ThermalField& operator=(const ThermalField&) = delete;
    ThermalField(ThermalField&&) noexcept = default;
    ThermalField& operator=(ThermalField&&) noexcept = default;

    // Copies descriptions. Reject empty/oversized cells, malformed/self/duplicate links,
    // duplicate source cells, nonfinite powers, or invalid budgets. Explicit zero links and
    // sources are valid, journaled and counted against preflight work/receipt budgets.
    [[nodiscard]] static std::optional<ThermalField>
    create(std::span<const ThermalSample> cells,
           std::span<const ThermalLink> links = {},
           std::span<const ThermalPowerSource> sources = {},
           const ThermalFieldConfig& config = {});

    [[nodiscard]] std::span<const ThermalSample> cells() const noexcept { return cells_; }

    [[nodiscard]] std::span<const ThermalLink> links() const noexcept { return links_; }

    [[nodiscard]] std::span<const ThermalPowerSource> sources() const noexcept { return sources_; }

    [[nodiscard]] std::span<const ThermalOperation> journal() const noexcept { return journal_; }

    [[nodiscard]] const ThermalFieldConfig& config() const noexcept { return config_; }

    [[nodiscard]] const ThermalFieldStep& last_step() const noexcept { return last_step_; }

    [[nodiscard]] std::uint64_t tick() const noexcept { return tick_; }

    // h=round(dt/N), N chosen from max_substep_seconds. Receipts account for rounded P*h
    // requests, NOT ideal P*dt. Immutable powers plus retained h,N,dt let an offline exact
    // checker distinguish product rounding and duration partitioning from energy-update
    // roundoff. No floating-point elapsed-time accumulator or automatic retry is hidden here.
    [[nodiscard]] ThermalFieldResult advance(double dt_seconds) noexcept;

private:
    ThermalField() = default;
    ThermalFieldConfig config_{};
    std::vector<ThermalSample> cells_;
    std::vector<ThermalSample> work_;
    std::vector<ThermalLink> links_;
    std::vector<ThermalPowerSource> sources_;
    std::vector<ThermalOperation> journal_;
    std::vector<ThermalOperation> work_journal_;
    ThermalFieldStep last_step_{};
    std::uint64_t tick_ = 0;
};

} // namespace rime::fluids
