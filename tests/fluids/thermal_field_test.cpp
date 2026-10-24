// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <type_traits>
#include <utility>
#include <vector>

#include "rime/fluids/thermal_field.hpp"

using namespace rime::fluids;

namespace {
ThermalSample field_sample(double mass,
                           double temperature,
                           double fraction = 1.0,
                           PhaseChangeConfig config = {256.0, 2.0, 4.0, 16.0, 128.0, 512.0}) {
    auto sample = ThermalSample::create(mass, {temperature, fraction}, config);
    REQUIRE(sample);
    return *sample;
}

ThermalField field(std::vector<ThermalSample> cells,
                   std::vector<ThermalLink> links = {},
                   std::vector<ThermalPowerSource> sources = {},
                   ThermalFieldConfig config = {1.0, 256, 4096, 65536}) {
    auto result = ThermalField::create(cells, links, sources, config);
    REQUIRE(result);
    return std::move(*result);
}

std::vector<std::uint64_t> snapshot(const ThermalField& f) {
    std::vector<std::uint64_t> words{f.tick(),
                                     f.last_step().substeps,
                                     f.last_step().exchanged_contacts,
                                     f.last_step().unchanged_contacts};
    const auto push = [&](double d) { words.push_back(std::bit_cast<std::uint64_t>(d)); };
    push(f.last_step().requested_dt_seconds);
    push(f.last_step().substep_dt_seconds);
    for (const auto& cell : f.cells()) {
        push(cell.energy_joules());
        push(cell.state().temperature_kelvin);
        push(cell.state().liquid_mass_fraction);
        push(cell.mass_kg());
    }
    for (const auto& op : f.journal()) {
        words.insert(words.end(),
                     {static_cast<std::uint64_t>(op.kind),
                      op.descriptor,
                      op.substep,
                      static_cast<std::uint64_t>(op.first.status),
                      static_cast<std::uint64_t>(op.second.status)});
        for (const auto& r : {op.first, op.second}) {
            push(r.requested_joules);
            push(r.applied_joules);
            push(r.applied_error_joules);
            push(r.roundoff_joules);
        }
        push(op.first_before_joules);
        push(op.first_after_joules);
        push(op.second_before_joules);
        push(op.second_after_joules);
    }
    return words;
}
} // namespace

TEST_CASE("thermal field: copied topology is canonical and malformed descriptions refuse") {
    static_assert(!std::is_copy_constructible_v<ThermalField>);
    static_assert(std::is_nothrow_move_constructible_v<ThermalField>);
    std::vector cells{field_sample(1.0, 300.0), field_sample(1.0, 288.0), field_sample(1.0, 276.0)};
    std::vector<ThermalLink> links{{2, 1, 1.0}, {1, 0, 2.0}};
    std::vector<ThermalPowerSource> sources{{2, 0.0}, {0, 1.0}};
    auto f = ThermalField::create(cells, links, sources);
    REQUIRE(f);
    CHECK(f->links()[0].first == 0);
    CHECK(f->links()[0].second == 1);
    CHECK(f->links()[1].first == 1);
    CHECK(f->sources()[0].cell == 0);
    CHECK(links[0].first == 2);
    CHECK(sources[0].cell == 2);
    CHECK_FALSE(ThermalField::create({}));
    for (auto bad : {ThermalLink{0, 0, 1.0},
                     ThermalLink{0, 3, 1.0},
                     ThermalLink{0, 1, -1.0},
                     ThermalLink{0, 1, std::numeric_limits<double>::infinity()}}) {
        CHECK_FALSE(ThermalField::create(cells, std::array{bad}));
    }
    CHECK_FALSE(ThermalField::create(cells, std::array{links[0], ThermalLink{1, 2, 0.0}}));
    CHECK_FALSE(ThermalField::create(cells, {}, std::array{sources[0], sources[0]}));
    CHECK_FALSE(ThermalField::create(cells, {}, std::array{ThermalPowerSource{3, 0.0}}));
    CHECK_FALSE(ThermalField::create(
        cells, {}, std::array{ThermalPowerSource{0, std::numeric_limits<double>::quiet_NaN()}}));
    for (auto config : {ThermalFieldConfig{0.0},
                        ThermalFieldConfig{-1.0},
                        ThermalFieldConfig{1.0, 0},
                        ThermalFieldConfig{1.0, 1025},
                        ThermalFieldConfig{1.0, 1, 0},
                        ThermalFieldConfig{1.0, 1, 16385},
                        ThermalFieldConfig{1.0, 1, 1, 0},
                        ThermalFieldConfig{std::numeric_limits<double>::infinity()}})
        CHECK_FALSE(ThermalField::create(cells, {}, {}, config));
    std::vector many(ThermalField::max_cells + 1, cells[0]);
    CHECK_FALSE(ThermalField::create(many));
    std::vector many_links(ThermalField::max_links + 1, links[0]);
    CHECK_FALSE(ThermalField::create(cells, many_links));
    std::vector many_sources(ThermalField::max_sources + 1, sources[0]);
    CHECK_FALSE(ThermalField::create(cells, {}, many_sources));
}

TEST_CASE("thermal field: source-first substeps journal every operation and canonical replay") {
    std::vector cells{field_sample(1.0, 288.0), field_sample(1.0, 272.0), field_sample(1.0, 280.0)};
    auto f = field(cells, {{1, 2, 1.0}, {1, 0, 2.0}}, {{2, 0.0}, {0, 4.0}}, {0.25});
    auto same = field(cells, {{0, 1, 2.0}, {2, 1, 1.0}}, {{0, 4.0}, {2, 0.0}}, {0.25});
    const auto r = f.advance(0.5);
    REQUIRE(r.status == ThermalFieldStatus::Accepted);
    CHECK(r.attempted_operations == 8);
    CHECK(f.tick() == 1);
    CHECK(f.last_step().substeps == 2);
    CHECK(f.last_step().substep_dt_seconds == 0.25);
    REQUIRE(f.journal().size() == 8);
    CHECK(f.journal()[0].kind == ThermalOperationKind::Source);
    CHECK(f.journal()[0].first.requested_joules == 1.0);
    CHECK(f.journal()[0].first_before_joules == cells[0].energy_joules());
    CHECK(f.journal()[1].first.requested_joules == 0.0);
    CHECK(f.journal()[2].kind == ThermalOperationKind::Contact);
    CHECK(f.journal()[2].first_before_joules == f.journal()[0].first_after_joules);
    CHECK(f.journal()[4].substep == 1);
    CHECK(same.advance(0.5).status == ThermalFieldStatus::Accepted);
    CHECK(snapshot(f) == snapshot(same));
    const auto before = snapshot(f);
    const auto* journal = f.journal().data();
    CHECK(f.advance(-0.0).status == ThermalFieldStatus::Accepted);
    CHECK(snapshot(f) == before);
    CHECK(f.journal().data() == journal);
    auto moved = std::move(f);
    CHECK(moved.advance(0.5).status == ThermalFieldStatus::Accepted);
    CHECK(moved.tick() == 2);
}

TEST_CASE("thermal field: disabled and disconnected cells remain bitwise unchanged") {
    auto f = field({field_sample(1.0, 300.0), field_sample(1.0, 276.0), field_sample(2.0, 280.0)},
                   {{0, 1, 0.0}},
                   {{2, 0.0}});
    std::vector before(f.cells().begin(), f.cells().end());
    REQUIRE(f.advance(1.0).status == ThermalFieldStatus::Accepted);
    CHECK(f.last_step().exchanged_contacts == 0);
    CHECK(f.last_step().unchanged_contacts == 1);
    for (std::size_t i = 0; i < before.size(); ++i) {
        CHECK(std::bit_cast<std::uint64_t>(f.cells()[i].energy_joules()) ==
              std::bit_cast<std::uint64_t>(before[i].energy_joules()));
        CHECK(f.cells()[i].state().temperature_kelvin == before[i].state().temperature_kelvin);
    }
    auto isolated = field({field_sample(1.0, 288.0)});
    CHECK(isolated.advance(0.5).status == ThermalFieldStatus::Accepted);
    CHECK(isolated.tick() == 1);
    CHECK(isolated.journal().empty());
}

TEST_CASE("thermal field: shared melting temperature preserves distinct phase reservoirs") {
    auto f = field({field_sample(1.0, 256.0, 0.1),
                    field_sample(2.0, 256.0, 0.9),
                    field_sample(3.0, 256.0, 0.5)},
                   {{0, 1, 100.0}, {1, 2, 100.0}},
                   {},
                   {0.25});
    std::vector<double> before;
    for (const auto& cell : f.cells())
        before.push_back(cell.energy_joules());
    REQUIRE(f.advance(1.0).status == ThermalFieldStatus::Accepted);
    CHECK(f.last_step().exchanged_contacts == 0);
    CHECK(f.last_step().unchanged_contacts == 8);
    for (std::size_t i = 0; i < before.size(); ++i)
        CHECK(f.cells()[i].energy_joules() == before[i]);
}

TEST_CASE("thermal field: preflight budget and invalid input refusals leave committed state") {
    for (auto config : {ThermalFieldConfig{0.25, 1},
                        ThermalFieldConfig{0.25, 4, 3},
                        ThermalFieldConfig{0.25, 4, 8, 255}}) {
        auto f = field({field_sample(1.0, 288.0), field_sample(1.0, 272.0)},
                       {{0, 1, 2.0}},
                       {{0, 4.0}},
                       config);
        REQUIRE(f.advance(0.25).status == ThermalFieldStatus::Accepted);
        const auto before = snapshot(f);
        const auto r = f.advance(0.5);
        const auto expected = config.max_substeps == 1 ? ThermalFieldStatus::SubstepBudgetExceeded
                              : config.receipt_budget == 3
                                  ? ThermalFieldStatus::ReceiptBudgetExceeded
                                  : ThermalFieldStatus::WorkBudgetExceeded;
        CHECK(r.status == expected);
        CHECK(r.attempted_operations == 0);
        CHECK(snapshot(f) == before);
        for (double dt : {-1.0,
                          std::numeric_limits<double>::quiet_NaN(),
                          std::numeric_limits<double>::infinity()}) {
            CHECK(f.advance(dt).status == ThermalFieldStatus::InvalidInput);
            CHECK(snapshot(f) == before);
        }
        CHECK(f.advance(1e308).status == ThermalFieldStatus::SubstepBudgetExceeded);
        CHECK(snapshot(f) == before);
    }
}

TEST_CASE("thermal field: late source refusal rolls back cells clock and published journal") {
    auto f = field({field_sample(1.0, 288.0), field_sample(1.0, 511.0)},
                   {},
                   {{0, 1.0}, {1, 4.0}},
                   {0.25, 256});
    REQUIRE(f.advance(0.25).status == ThermalFieldStatus::Accepted);
    const auto before = snapshot(f);
    const auto* journal = f.journal().data();
    const auto r = f.advance(1.0);
    CHECK(r.status == ThermalFieldStatus::SourceRefused);
    CHECK(r.failed_descriptor == 1);
    CHECK(r.failed_substep == 3);
    CHECK(r.attempted_operations == 8);
    CHECK(r.heat_status == HeatStatus::TemperatureLimit);
    CHECK(snapshot(f) == before);
    CHECK(f.journal().data() == journal);
    CHECK(f.advance(0.125).status == ThermalFieldStatus::Accepted);
    CHECK(f.tick() == 2);
}

TEST_CASE("thermal field: final contact failure rolls back earlier sources and contacts") {
    auto f = field({field_sample(1.0, 300.0), field_sample(1.0, 280.0), field_sample(1e100, 288.0)},
                   {{0, 1, 2.0}, {1, 2, 1.0}},
                   {{0, 4.0}});
    const auto before = snapshot(f);
    const auto r = f.advance(0.25);
    CHECK(r.status == ThermalFieldStatus::ContactRefused);
    CHECK(r.failed_descriptor == 1);
    CHECK(r.contact_status == ThermalContactStatus::Unrepresentable);
    CHECK(r.attempted_operations == 3);
    CHECK(snapshot(f) == before);
    CHECK(f.journal().empty());
}

TEST_CASE("thermal field: source-free graph preserves extrema over substeps") {
    auto f = field({field_sample(1.0, 300.0),
                    field_sample(2.0, 270.0),
                    field_sample(3.0, 280.0),
                    field_sample(1.0, 290.0)},
                   {{0, 1, 2.0}, {1, 2, 3.0}, {2, 3, 1.0}, {0, 3, 4.0}},
                   {},
                   {0.125});
    for (unsigned i = 0; i < 16; ++i) {
        double low = 512.0, high = 0.0;
        for (const auto& cell : f.cells()) {
            low = std::min(low, cell.state().temperature_kelvin);
            high = std::max(high, cell.state().temperature_kelvin);
        }
        REQUIRE(f.advance(0.5).status == ThermalFieldStatus::Accepted);
        for (const auto& cell : f.cells()) {
            CHECK(cell.state().temperature_kelvin >= low);
            CHECK(cell.state().temperature_kelvin <= high);
        }
    }
}

TEST_CASE("thermal field: cold and hot sources drive multi-hop freezing and melting") {
    constexpr double melting = 273.15;
    for (bool heating : {false, true}) {
        const PhaseChangeConfig water;
        auto f = field(
            {field_sample(100.0, melting + (heating ? 10.0 : -10.0), heating ? 1.0 : 0.0, water),
             field_sample(0.01, melting, heating ? 0.0 : 1.0, water),
             field_sample(0.01, melting, heating ? 0.0 : 1.0, water),
             field_sample(0.01, melting, heating ? 0.0 : 1.0, water)},
            {{0, 1, 100.0}, {1, 2, 100.0}},
            {{0, heating ? 100.0 : -100.0}},
            {0.1});
        const auto isolated = f.cells()[3].energy_joules();
        REQUIRE(f.advance(0.1).status == ThermalFieldStatus::Accepted);
        CHECK(f.cells()[2].state().liquid_mass_fraction == (heating ? 0.0 : 1.0));
        for (unsigned i = 1; i < 160; ++i)
            REQUIRE(f.advance(0.1).status == ThermalFieldStatus::Accepted);
        CHECK(f.cells()[1].state().liquid_mass_fraction == (heating ? 1.0 : 0.0));
        if (heating)
            CHECK(f.cells()[2].state().liquid_mass_fraction > 0.5);
        else
            CHECK(f.cells()[2].state().liquid_mass_fraction < 0.5);
        CHECK(f.cells()[3].energy_joules() == isolated);
    }
}

TEST_CASE("thermal field: temporal refinement and node relabeling reveal first order split") {
    const auto integrate = [](unsigned steps, bool reverse) {
        auto f = field({field_sample(1.0, reverse ? 276.0 : 300.0),
                        field_sample(1.0, 288.0),
                        field_sample(1.0, reverse ? 300.0 : 276.0)},
                       {{0, 1, 1.0}, {1, 2, 1.0}});
        for (unsigned i = 0; i < steps; ++i)
            REQUIRE(f.advance(1.0 / steps).status == ThermalFieldStatus::Accepted);
        std::array<double, 3> result{};
        for (std::size_t i = 0; i < 3; ++i)
            result[i] = f.cells()[reverse ? 2 - i : i].state().temperature_kelvin;
        return result;
    };
    const auto error = [](auto result) {
        const double amplitude = 12.0 * std::exp(-0.25);
        return std::sqrt((std::pow(result[0] - (288.0 + amplitude), 2) +
                          std::pow(result[1] - 288.0, 2) +
                          std::pow(result[2] - (288.0 - amplitude), 2)) /
                         3.0);
    };
    const auto coarse = integrate(16, false);
    const auto fine = integrate(32, false);
    CHECK(error(coarse) / error(fine) > 1.9);
    CHECK(error(coarse) / error(fine) < 2.1);
    const auto relabeled = integrate(16, true);
    CHECK(std::abs(coarse[1] - relabeled[1]) > 0.01);
    CHECK(error(relabeled) == doctest::Approx(error(coarse)).epsilon(1e-7));
}

TEST_CASE("thermal field: insulated cosine rod converges under spatial refinement") {
    const auto integrate = [](std::size_t n, unsigned refinement) {
        const double dx = 1.0 / static_cast<double>(n);
        const double pi = std::numbers::pi;
        const double average_factor = std::sin(pi * dx * 0.5) / (pi * dx * 0.5);
        const auto steps = static_cast<unsigned>(n * n * n * n / 2) * refinement;
        const double h = 0.05 / steps;
        std::vector<ThermalSample> cells;
        std::vector<ThermalLink> links;
        for (std::size_t i = 0; i < n; ++i) {
            const double temperature = 288.0 + 8.0 * average_factor * std::cos(pi * (i + 0.5) * dx);
            cells.push_back(field_sample(dx, temperature));
            if (i != 0)
                links.push_back({i - 1, i, 1.0 / dx});
        }
        auto f = field(std::move(cells), std::move(links), {}, {h});
        for (unsigned i = 0; i < steps; ++i)
            if (f.advance(h).status != ThermalFieldStatus::Accepted) {
                FAIL("cosine rod refused");
                return 0.0;
            }
        double squared = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double exact = 288.0 + 8.0 * average_factor * std::exp(-0.25 * pi * pi * 0.05) *
                                             std::cos(pi * (i + 0.5) * dx);
            squared += std::pow(f.cells()[i].state().temperature_kelvin - exact, 2);
        }
        return std::sqrt(squared / n);
    };
    const double coarse = integrate(4, 1);
    const double medium = integrate(8, 1);
    const double fine = integrate(16, 1);
    const double finer_time = integrate(16, 2);
    CAPTURE(coarse);
    CAPTURE(medium);
    CAPTURE(fine);
    CAPTURE(finer_time);
    CHECK(coarse / medium > 3.5);
    CHECK(coarse / medium < 4.5);
    CHECK(medium / fine > 3.5);
    CHECK(medium / fine < 4.5);
    CHECK(std::abs(fine - finer_time) < fine * 0.1);
}

TEST_CASE("thermal field: unsupported floating point refuses zero steps and construction") {
    auto f = field({field_sample(1.0, 288.0)});
    const auto before = snapshot(f);

    struct Restore {
        int rounding = std::fegetround();

        ~Restore() { std::fesetround(rounding); }
    } restore;

    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    CHECK(f.advance(0.0).status == ThermalFieldStatus::UnsupportedEnvironment);
    CHECK_FALSE(ThermalField::create(f.cells()));
    CHECK(snapshot(f) == before);
}
