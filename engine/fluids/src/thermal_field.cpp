// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/fluids/thermal_field.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

#if defined(__FAST_MATH__)
#error "The thermal reference requires IEEE round-to-nearest operations without fast-math."
#endif

namespace rime::fluids {

std::optional<ThermalField> ThermalField::create(std::span<const ThermalSample> cells,
                                                 std::span<const ThermalLink> links,
                                                 std::span<const ThermalPowerSource> sources,
                                                 const ThermalFieldConfig& config) {
    if (cells.empty() || cells.size() > max_cells || links.size() > max_links ||
        sources.size() > max_sources || !std::isfinite(config.max_substep_seconds) ||
        config.max_substep_seconds <= 0.0 || config.max_substeps == 0 ||
        config.max_substeps > max_allowed_substeps || config.receipt_budget == 0 ||
        config.receipt_budget > max_receipts || config.contact_trial_budget == 0 ||
        config.contact_trial_budget > max_contact_trials)
        return std::nullopt;
    auto probe = cells.front();
    if (probe.apply_heat(0.0).status != HeatStatus::Accepted)
        return std::nullopt;
    ThermalField field;
    field.config_ = config;
    field.links_.assign(links.begin(), links.end());
    field.sources_.assign(sources.begin(), sources.end());
    // Canonicalize copied descriptions, not caller data. Reject duplicate undirected links:
    // quietly summing them would change the physical conductance and hide authored mistakes.
    for (auto& link : field.links_) {
        if (link.first >= cells.size() || link.second >= cells.size() ||
            link.first == link.second || !std::isfinite(link.conductance_w_k) ||
            link.conductance_w_k < 0.0)
            return std::nullopt;
        if (link.first > link.second)
            std::swap(link.first, link.second);
    }
    std::sort(field.links_.begin(), field.links_.end(), [](const auto& a, const auto& b) {
        return std::tie(a.first, a.second) < std::tie(b.first, b.second);
    });
    for (std::size_t i = 1; i < field.links_.size(); ++i)
        if (field.links_[i - 1].first == field.links_[i].first &&
            field.links_[i - 1].second == field.links_[i].second)
            return std::nullopt;
    for (const auto& source : field.sources_)
        if (source.cell >= cells.size() || !std::isfinite(source.power_watts))
            return std::nullopt;
    std::sort(field.sources_.begin(), field.sources_.end(), [](const auto& a, const auto& b) {
        return a.cell < b.cell;
    });
    for (std::size_t i = 1; i < field.sources_.size(); ++i)
        if (field.sources_[i - 1].cell == field.sources_[i].cell)
            return std::nullopt;
    field.cells_.assign(cells.begin(), cells.end());
    field.work_ = field.cells_;
    const auto capacity =
        std::min(config.receipt_budget, (links.size() + sources.size()) * config.max_substeps);
    field.journal_.reserve(capacity);
    field.work_journal_.reserve(capacity);
    return field;
}

ThermalFieldResult ThermalField::advance(double dt_seconds) noexcept {
    if (cells_.empty() || !std::isfinite(dt_seconds) || dt_seconds < 0.0)
        return {ThermalFieldStatus::InvalidInput};
    auto probe = cells_.front();
    if (probe.apply_heat(0.0).status != HeatStatus::Accepted)
        return {ThermalFieldStatus::UnsupportedEnvironment};
    if (dt_seconds == 0.0)
        return {ThermalFieldStatus::Accepted};
    if (tick_ == std::numeric_limits<std::uint64_t>::max())
        return {ThermalFieldStatus::TickLimit};

    const double count = std::ceil(dt_seconds / config_.max_substep_seconds);
    if (!std::isfinite(count) || count > static_cast<double>(config_.max_substeps))
        return {ThermalFieldStatus::SubstepBudgetExceeded};
    auto substeps = static_cast<std::size_t>(std::max(1.0, count));
    double h = dt_seconds / static_cast<double>(substeps);
    // The rounded quotient used by ceil can land exactly on an integer from below. Check the
    // actual h as well, rather than admit a step slightly larger than the configured limit.
    if (h > config_.max_substep_seconds) {
        if (substeps == config_.max_substeps)
            return {ThermalFieldStatus::SubstepBudgetExceeded};
        ++substeps;
        h = dt_seconds / static_cast<double>(substeps);
    }
    const auto per_substep = links_.size() + sources_.size();
    if (per_substep != 0 && substeps > config_.receipt_budget / per_substep)
        return {ThermalFieldStatus::ReceiptBudgetExceeded};
    if (!links_.empty() && substeps > config_.contact_trial_budget /
                                          ThermalContactResult::maximum_iterations / links_.size())
        return {ThermalFieldStatus::WorkBudgetExceeded};

    // Source-first Lie splitting: every primitive sees earlier staged updates. This is a
    // bounded, first-order graph reference, not a global implicit diffusion solve. Two fixed
    // cell buffers and two pre-reserved journals make late refusal atomic without allocating
    // in the tick. A published journal is never overwritten while testing a new candidate.
    std::copy(cells_.begin(), cells_.end(), work_.begin());
    work_journal_.clear();
    ThermalFieldStep step{dt_seconds, h, substeps};
    ThermalFieldResult result{ThermalFieldStatus::Accepted};
    for (std::size_t substep = 0; substep < substeps; ++substep) {
        for (std::size_t i = 0; i < sources_.size(); ++i) {
            const auto& source = sources_[i];
            auto& cell = work_[source.cell];
            ThermalOperation op;
            op.kind = ThermalOperationKind::Source;
            op.descriptor = i;
            op.substep = substep;
            op.first_before_joules = cell.energy_joules();
            ++result.attempted_operations;
            op.first = cell.apply_power(source.power_watts, h);
            if (op.first.status != HeatStatus::Accepted) {
                result.status = ThermalFieldStatus::SourceRefused;
                result.failed_substep = substep;
                result.failed_descriptor = i;
                result.heat_status = op.first.status;
                return result;
            }
            op.first_after_joules = cell.energy_joules();
            work_journal_.push_back(op);
        }
        for (std::size_t i = 0; i < links_.size(); ++i) {
            const auto& link = links_[i];
            auto& first = work_[link.first];
            auto& second = work_[link.second];
            ThermalOperation op;
            op.kind = ThermalOperationKind::Contact;
            op.descriptor = i;
            op.substep = substep;
            op.first_before_joules = first.energy_joules();
            op.second_before_joules = second.energy_joules();
            ++result.attempted_operations;
            const auto contact = exchange_heat(first, second, link.conductance_w_k, h);
            if (contact.status != ThermalContactStatus::Exchanged &&
                contact.status != ThermalContactStatus::NoExchange) {
                result.status = ThermalFieldStatus::ContactRefused;
                result.failed_substep = substep;
                result.failed_descriptor = i;
                result.contact_status = contact.status;
                return result;
            }
            if (contact.status == ThermalContactStatus::Exchanged)
                ++step.exchanged_contacts;
            else
                ++step.unchanged_contacts;
            op.first = contact.first;
            op.second = contact.second;
            op.first_after_joules = first.energy_joules();
            op.second_after_joules = second.energy_joules();
            work_journal_.push_back(op);
        }
    }
    cells_.swap(work_);
    journal_.swap(work_journal_);
    last_step_ = step;
    ++tick_;
    return result;
}

} // namespace rime::fluids
