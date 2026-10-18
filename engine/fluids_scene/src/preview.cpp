// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/fluids_scene/preview.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <tuple>
#include <vector>

#include "rime/ecs/query.hpp"

namespace rime::fluids_scene {
namespace {
auto key(ecs::Entity e) {
    return std::tuple{e.index, e.generation};
}

bool same(double a, double b) {
    return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

bool same(const ThermalCell& a, const ThermalCell& b) {
    const auto& x = a.material;
    const auto& y = b.material;
    return same(a.mass_kg, b.mass_kg) && same(a.temperature_kelvin, b.temperature_kelvin) &&
           same(a.liquid_mass_fraction, b.liquid_mass_fraction) &&
           same(x.melting_kelvin, y.melting_kelvin) &&
           same(x.solid_heat_capacity_j_kg_k, y.solid_heat_capacity_j_kg_k) &&
           same(x.liquid_heat_capacity_j_kg_k, y.liquid_heat_capacity_j_kg_k) &&
           same(x.latent_heat_j_kg, y.latent_heat_j_kg) &&
           same(x.minimum_kelvin, y.minimum_kelvin) && same(x.maximum_kelvin, y.maximum_kelvin);
}

bool same(const ThermalContact& a, const ThermalContact& b) {
    return a.first == b.first && a.second == b.second && same(a.conductance_w_k, b.conductance_w_k);
}

bool same(const ThermalSource& a, const ThermalSource& b) {
    return a.cell == b.cell && same(a.power_watts, b.power_watts);
}
} // namespace

void ThermalPreview::reset() {
    field_.reset();
    descriptions_ = {};
    entities_ = {};
    status_ = PreviewStatus::Empty;
    problem_ = {};
    has_description_ = has_tick_ = false;
    last_host_tick_ = accepted_host_tick_ = 0;
    last_result_ = {fluids::ThermalFieldStatus::Accepted};
    ++revision_;
}

void ThermalPreview::reconcile(ecs::World& world) {
    Description next;
    // Retain only bounded lowest keys while scanning, independent of archetype order. A cap
    // violation REFUSES the whole graph, never runs a silently truncated thermal network.
    const auto gather = [&](auto& rows, std::size_t& count, ecs::Entity e, const auto& component) {
        if (!component.enabled)
            return;
        std::size_t at = 0;
        while (at < count && key(rows[at].entity) < key(e))
            ++at;
        if (count == rows.size()) {
            next.overflow = true;
            if (at == count)
                return;
        } else
            ++count;
        for (std::size_t i = count - 1; i > at; --i)
            rows[i] = rows[i - 1];
        rows[at] = {e, component};
    };
    ecs::Query<ThermalCell>(world).for_each(
        [&](ecs::Entity e, ThermalCell& c) { gather(next.cells, next.cell_count, e, c); });
    ecs::Query<ThermalContact>(world).for_each(
        [&](ecs::Entity e, ThermalContact& c) { gather(next.contacts, next.contact_count, e, c); });
    ecs::Query<ThermalSource>(world).for_each(
        [&](ecs::Entity e, ThermalSource& c) { gather(next.sources, next.source_count, e, c); });
    const auto equal_rows = [](const auto& a, const auto& b, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i)
            if (a[i].entity != b[i].entity || !same(a[i].value, b[i].value))
                return false;
        return true;
    };
    // Compare value bits, never struct padding or a hash alone. Identical invalid NaN payloads
    // are stable, so a bad authored field does not trigger a rebuild on every paused frame.
    if (has_description_ && next.overflow == descriptions_.overflow &&
        next.cell_count == descriptions_.cell_count &&
        next.contact_count == descriptions_.contact_count &&
        next.source_count == descriptions_.source_count &&
        equal_rows(next.cells, descriptions_.cells, next.cell_count) &&
        equal_rows(next.contacts, descriptions_.contacts, next.contact_count) &&
        equal_rows(next.sources, descriptions_.sources, next.source_count))
        return;
    descriptions_ = next;
    has_description_ = true;
    ++revision_;
    field_.reset();
    entities_ = {};
    problem_ = {};
    accepted_host_tick_ = 0;
    last_result_ = {fluids::ThermalFieldStatus::Accepted};
    if (!active()) {
        status_ = PreviewStatus::Empty;
        return;
    }
    if (next.overflow) {
        status_ = PreviewStatus::CapacityExceeded;
        ++stats_.capacity_refusals;
        return;
    }
    status_ = PreviewStatus::InvalidDescription;
    const auto invalid = [&](ecs::Entity e) {
        problem_ = e;
        ++stats_.invalid_builds;
    };
    std::vector<fluids::ThermalSample> cells;
    cells.reserve(next.cell_count);
    for (std::size_t i = 0; i < next.cell_count; ++i) {
        const auto& row = next.cells[i];
        const auto& c = row.value;
        auto sample = fluids::ThermalSample::create(
            c.mass_kg, {c.temperature_kelvin, c.liquid_mass_fraction}, c.material);
        if (!sample) {
            invalid(row.entity);
            return;
        }
        entities_[i] = row.entity;
        cells.push_back(*sample);
    }
    const auto index = [&](ecs::Entity e) {
        const auto end = entities_.begin() + static_cast<std::ptrdiff_t>(next.cell_count);
        const auto it = std::find(entities_.begin(), end, e);
        return it == end ? max_cells : static_cast<std::size_t>(it - entities_.begin());
    };
    std::vector<fluids::ThermalLink> links;
    links.reserve(next.contact_count);
    for (std::size_t i = 0; i < next.contact_count; ++i) {
        const auto& row = next.contacts[i];
        const auto& c = row.value;
        const auto a = index(c.first), b = index(c.second);
        if (a == max_cells || b == max_cells || a == b || !std::isfinite(c.conductance_w_k) ||
            c.conductance_w_k < 0.0) {
            invalid(row.entity);
            return;
        }
        for (const auto& link : links)
            if (link.first == std::min(a, b) && link.second == std::max(a, b)) {
                invalid(row.entity);
                return;
            }
        links.push_back({std::min(a, b), std::max(a, b), c.conductance_w_k});
    }
    std::vector<fluids::ThermalPowerSource> sources;
    sources.reserve(next.source_count);
    for (std::size_t i = 0; i < next.source_count; ++i) {
        const auto& row = next.sources[i];
        const auto& c = row.value;
        const auto target = index(c.cell);
        if (target == max_cells || !std::isfinite(c.power_watts)) {
            invalid(row.entity);
            return;
        }
        for (const auto& s : sources)
            if (s.cell == target) {
                invalid(row.entity);
                return;
            }
        sources.push_back({target, c.power_watts});
    }
    if (cells.empty()) {
        invalid({});
        return;
    }
    field_ = fluids::ThermalField::create(cells, links, sources);
    if (!field_) {
        invalid({});
        return;
    }
    status_ = PreviewStatus::Ready;
}

bool ThermalPreview::tick(ecs::World& world, std::uint64_t host_tick, double dt) {
    if (!std::isfinite(dt) || dt <= 0.0 || dt > 0.25 ||
        (has_tick_ && host_tick <= last_host_tick_)) {
        ++stats_.refused_ticks;
        return false;
    }
    reconcile(world);
    if (status_ == PreviewStatus::InvalidDescription ||
        status_ == PreviewStatus::CapacityExceeded) {
        ++stats_.refused_ticks;
        return false;
    }
    if (field_) {
        last_result_ = field_->advance(dt);
        if (last_result_.status != fluids::ThermalFieldStatus::Accepted) {
            status_ = PreviewStatus::StepRefused;
            ++stats_.refused_ticks;
            return false;
        }
        status_ = PreviewStatus::Ready;
        accepted_host_tick_ = host_tick;
        ++stats_.accepted_ticks;
    }
    has_tick_ = true;
    last_host_tick_ = host_tick;
    return true;
}
} // namespace rime::fluids_scene
