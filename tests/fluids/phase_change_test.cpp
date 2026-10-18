// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "rime/fluids/phase_change.hpp"

using namespace rime::fluids;

namespace {
constexpr double kMelting = 273.15;
constexpr double fusion = 333500.0;
constexpr double solid_cp = 2052.0;
constexpr double liquid_cp = 4179.0;

std::uint64_t bits(double value) {
    return std::bit_cast<std::uint64_t>(value);
}
} // namespace

TEST_CASE("thermal reference: temperature and phase initialization are unambiguous") {
    static_assert(!std::is_constructible_v<ThermalState, double>);
    static_assert(!std::is_default_constructible_v<ThermalState>);
    const auto cold = ThermalSample::create(2.0, {kMelting - 10.0, 0.0});
    REQUIRE(cold);
    CHECK(cold->state().temperature_kelvin == kMelting - 10.0);
    CHECK(cold->state().liquid_mass_fraction == 0.0);
    CHECK(cold->energy_joules() == -2.0 * solid_cp * 10.0);
    const auto warm = ThermalSample::create(2.0, {kMelting + 10.0, 1.0});
    REQUIRE(warm);
    CHECK(warm->energy_joules() == 2.0 * (fusion + liquid_cp * 10.0));
    CHECK(warm->state().temperature_kelvin == kMelting + 10.0);
    for (double fraction : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const auto mixture = ThermalSample::create(2.0, {kMelting, fraction});
        REQUIRE(mixture);
        CHECK(mixture->state().temperature_kelvin == kMelting);
        CHECK(mixture->state().liquid_mass_fraction == fraction);
        CHECK(mixture->energy_joules() == 2.0 * fusion * fraction);
    }
    CHECK_FALSE(ThermalSample::create(1.0, {kMelting - 1.0, 0.5}));
    CHECK_FALSE(ThermalSample::create(1.0, {kMelting + 1.0, 0.5}));
    CHECK_FALSE(ThermalSample::create(1.0, {kMelting, -0.1}));
    CHECK_FALSE(ThermalSample::create(1.0, {kMelting, 1.1}));
}

TEST_CASE("thermal reference: cold ice warms melts and heats then reverses with accounted heat") {
    auto sample = ThermalSample::create(2.0, {kMelting - 10.0, 0.0});
    REQUIRE(sample);
    const double initial = sample->energy_joules();
    const double heat[] = {2.0 * solid_cp * 10.0, fusion, fusion, 2.0 * liquid_cp * 10.0};
    const ThermalState expected[] = {
        {kMelting, 0.0}, {kMelting, 0.5}, {kMelting, 1.0}, {kMelting + 10.0, 1.0}};
    double applied_sum = 0.0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto exchange = sample->apply_heat(heat[i]);
        REQUIRE(exchange.status == HeatStatus::Accepted);
        CHECK(exchange.applied_joules == heat[i]);
        CHECK(exchange.roundoff_joules == 0.0);
        CHECK(sample->state().temperature_kelvin == expected[i].temperature_kelvin);
        CHECK(sample->state().liquid_mass_fraction == expected[i].liquid_mass_fraction);
        applied_sum += exchange.applied_joules;
    }
    CHECK(sample->energy_joules() - initial == applied_sum);
    for (unsigned i = 4; i > 0; --i) {
        const auto exchange = sample->apply_heat(-heat[i - 1]);
        REQUIRE(exchange.status == HeatStatus::Accepted);
        applied_sum += exchange.applied_joules;
    }
    CHECK(sample->energy_joules() == initial);
    CHECK(applied_sum == 0.0);
    CHECK(sample->state().temperature_kelvin == kMelting - 10.0);
    CHECK(sample->state().liquid_mass_fraction == 0.0);
    CHECK(sample->mass_kg() == 2.0);
}

TEST_CASE("thermal reference: melt duration has a latent heat sensitivity control") {
    auto sample = ThermalSample::create(1.0, {kMelting, 0.0});
    REQUIRE(sample);
    // 1 kW for 333.5 s melts 1 kg: 1334 quarter-second steps, with no temperature rise.
    // This fixes the physical witness before measuring it; there is no tuned time tolerance.
    for (unsigned tick = 1; tick <= 1334; ++tick) {
        REQUIRE(sample->apply_power(1000.0, 0.25).status == HeatStatus::Accepted);
        CHECK(sample->state().temperature_kelvin == kMelting);
        CHECK(sample->state().liquid_mass_fraction >= 0.0);
        CHECK(sample->state().liquid_mass_fraction <= 1.0);
        if (tick == 667) {
            CHECK(sample->state().liquid_mass_fraction == 0.5);
            // Counterfactual: without latent heat this energy would already warm liquid by
            // almost 40 K. It must distinguish a temperature-only threshold implementation.
            const double no_latent_temperature = kMelting + (1000.0 * 0.25 * tick) / liquid_cp;
            CHECK(no_latent_temperature - sample->state().temperature_kelvin > 39.0);
        }
    }
    CHECK(sample->energy_joules() == fusion);
    CHECK(sample->state().liquid_mass_fraction == 1.0);
    REQUIRE(sample->apply_power(1000.0, 0.25).status == HeatStatus::Accepted);
    CHECK(sample->state().temperature_kelvin == doctest::Approx(kMelting + 250.0 / liquid_cp));
    CHECK(sample->state().temperature_kelvin > kMelting);

    auto doubled_latent = PhaseChangeConfig{};
    doubled_latent.latent_heat_j_kg = 2.0 * fusion;
    auto slower = ThermalSample::create(1.0, {kMelting, 0.0}, doubled_latent);
    REQUIRE(slower);
    REQUIRE(slower->apply_heat(fusion).status == HeatStatus::Accepted);
    CHECK(slower->state().liquid_mass_fraction == 0.5);
    CHECK(slower->state().temperature_kelvin == kMelting);
    REQUIRE(slower->apply_heat(fusion).status == HeatStatus::Accepted);
    CHECK(slower->state().liquid_mass_fraction == 1.0);
    auto doubled_capacity = PhaseChangeConfig{};
    doubled_capacity.solid_heat_capacity_j_kg_k *= 2.0;
    doubled_capacity.liquid_heat_capacity_j_kg_k *= 2.0;
    auto same_melt_heat = ThermalSample::create(1.0, {kMelting, 0.0}, doubled_capacity);
    REQUIRE(same_melt_heat);
    REQUIRE(same_melt_heat->apply_heat(fusion).status == HeatStatus::Accepted);
    CHECK(same_melt_heat->state().liquid_mass_fraction == 1.0);
    CHECK(same_melt_heat->state().temperature_kelvin == kMelting);
}

TEST_CASE("thermal reference: mass scales heat while fraction and temperature agree") {
    for (double mass : {1.0e-6, 1.0, 1.0e6}) {
        auto sample = ThermalSample::create(mass, {kMelting, 0.0});
        REQUIRE(sample);
        REQUIRE(sample->apply_heat(mass * fusion * 0.25).status == HeatStatus::Accepted);
        CHECK(sample->state().liquid_mass_fraction == doctest::Approx(0.25));
        CHECK(sample->state().temperature_kelvin == kMelting);
        REQUIRE(sample->apply_heat(mass * fusion * 0.75 + mass * liquid_cp * 10.0).status ==
                HeatStatus::Accepted);
        CHECK(sample->state().liquid_mass_fraction == 1.0);
        CHECK(sample->state().temperature_kelvin == doctest::Approx(kMelting + 10.0));
    }
}

TEST_CASE("thermal reference: zero input preserves even signed zero energy bits") {
    auto sample = ThermalSample::create(1.0, {kMelting, -0.0});
    REQUIRE(sample);
    const auto initial = bits(sample->energy_joules());
    for (double zero : {0.0, -0.0}) {
        CHECK(sample->apply_heat(zero).status == HeatStatus::Accepted);
        CHECK(sample->apply_power(1000.0, zero).status == HeatStatus::Accepted);
        CHECK(sample->apply_power(zero, 1.0e300).status == HeatStatus::Accepted);
        CHECK(bits(sample->energy_joules()) == initial);
    }
    CHECK(sample->apply_power(std::numeric_limits<double>::infinity(), 0.0).status ==
          HeatStatus::InvalidInput);
    CHECK(bits(sample->energy_joules()) == initial);
}

TEST_CASE("thermal reference: inconsistent and unrepresentable configurations refuse") {
    const ThermalState start{kMelting, 0.5};
    const double infinity = std::numeric_limits<double>::infinity();
    CHECK_FALSE(ThermalSample::create(0.0, start));
    CHECK_FALSE(ThermalSample::create(-1.0, start));
    CHECK_FALSE(ThermalSample::create(infinity, start));
    CHECK_FALSE(ThermalSample::create(1.0, {infinity, 0.0}));
    CHECK_FALSE(ThermalSample::create(1.0, {kMelting, std::numeric_limits<double>::quiet_NaN()}));
    CHECK_FALSE(ThermalSample::create(1.0, {kMelting - 21.0, 0.0}));
    CHECK_FALSE(ThermalSample::create(1.0, {kMelting + 41.0, 1.0}));
    auto config = PhaseChangeConfig{};
    config.latent_heat_j_kg = 0.0;
    CHECK_FALSE(ThermalSample::create(1.0, start, config));
    config = {};
    config.minimum_kelvin = kMelting;
    CHECK_FALSE(ThermalSample::create(1.0, start, config));
    config = {};
    config.maximum_kelvin = kMelting;
    CHECK_FALSE(ThermalSample::create(1.0, start, config));
    config = {};
    config.solid_heat_capacity_j_kg_k = infinity;
    CHECK_FALSE(ThermalSample::create(1.0, start, config));
    config = {};
    config.liquid_heat_capacity_j_kg_k = -1.0;
    CHECK_FALSE(ThermalSample::create(1.0, start, config));
    CHECK_FALSE(ThermalSample::create(std::numeric_limits<double>::max(), start));
    config = {};
    config.latent_heat_j_kg = 1.0e300; // no representable warm-energy interval above latent heat
    CHECK_FALSE(ThermalSample::create(1.0, start, config));
    config = {};
    config.latent_heat_j_kg = std::numeric_limits<double>::denorm_min();
    CHECK_FALSE(ThermalSample::create(1.0, start, config)); // half-phase energy underflows
}

TEST_CASE("thermal reference: refused heat leaves state unchanged and never clamps energy") {
    auto sample = ThermalSample::create(1.0, {kMelting + 10.0, 1.0});
    REQUIRE(sample);
    const double initial = sample->energy_joules();
    const auto check_refusal = [&](HeatExchange result, HeatStatus status) {
        CHECK(result.status == status);
        CHECK(result.applied_joules == 0.0);
        CHECK(result.roundoff_joules == 0.0);
        CHECK(result.applied_error_joules == 0.0);
        CHECK(std::isfinite(result.requested_joules));
        CHECK(bits(sample->energy_joules()) == bits(initial));
        CHECK(sample->state().temperature_kelvin == kMelting + 10.0);
        CHECK(sample->state().liquid_mass_fraction == 1.0);
    };
    check_refusal(sample->apply_heat(1.0e9), HeatStatus::TemperatureLimit);
    check_refusal(sample->apply_heat(-1.0e9), HeatStatus::TemperatureLimit);
    check_refusal(sample->apply_heat(std::numeric_limits<double>::quiet_NaN()),
                  HeatStatus::InvalidInput);
    check_refusal(sample->apply_heat(std::numeric_limits<double>::infinity()),
                  HeatStatus::InvalidInput);
    check_refusal(sample->apply_power(1000.0, -1.0), HeatStatus::InvalidInput);
    check_refusal(sample->apply_power(1000.0, std::numeric_limits<double>::infinity()),
                  HeatStatus::InvalidInput);
    check_refusal(sample->apply_power(1.0e300, 1.0e300), HeatStatus::Unrepresentable);
    check_refusal(sample->apply_power(1.0e-300, 1.0e-300), HeatStatus::Unrepresentable);
    const double ulp = std::nextafter(initial, std::numeric_limits<double>::infinity()) - initial;
    check_refusal(sample->apply_heat(ulp * 0.25), HeatStatus::Unrepresentable);
    const PhaseChangeConfig config;
    const double maximum = fusion + liquid_cp * (config.maximum_kelvin - kMelting);
    REQUIRE(sample->apply_heat(maximum - initial).status == HeatStatus::Accepted);
    CHECK(sample->energy_joules() == maximum);
    CHECK(sample->state().temperature_kelvin == config.maximum_kelvin);
    CHECK(sample->apply_heat(1.0).status == HeatStatus::TemperatureLimit);
    CHECK(sample->energy_joules() == maximum);
}

TEST_CASE(
    "thermal reference: finite rounding is exposed and partitioned heat crosses both phases") {
    auto rounded = ThermalSample::create(1.0, {kMelting + 20.0, 1.0});
    REQUIRE(rounded);
    const double initial = rounded->energy_joules();
    const double ulp = std::nextafter(initial, std::numeric_limits<double>::infinity()) - initial;
    const auto exchange = rounded->apply_heat(0.75 * ulp);
    REQUIRE(exchange.status == HeatStatus::Accepted);
    CHECK(exchange.applied_joules == ulp);
    CHECK(exchange.roundoff_joules == -0.25 * ulp);
    CHECK(exchange.requested_joules == exchange.applied_joules + exchange.roundoff_joules);
    CHECK(rounded->energy_joules() - initial == exchange.applied_joules);

    auto one_step = ThermalSample::create(1.0, {kMelting - 10.0, 0.0});
    REQUIRE(one_step);
    auto split = *one_step;
    const double heat = solid_cp * 10.0 + fusion + liquid_cp * 20.0;
    REQUIRE(one_step->apply_heat(heat).status == HeatStatus::Accepted);
    for (unsigned i = 0; i < 64; ++i)
        REQUIRE(split.apply_power(heat / 64.0, 1.0).status == HeatStatus::Accepted);
    CHECK(split.energy_joules() == one_step->energy_joules());
    CHECK(split.state().temperature_kelvin == kMelting + 20.0);
    for (unsigned i = 0; i < 64; ++i)
        REQUIRE(split.apply_power(-heat / 64.0, 1.0).status == HeatStatus::Accepted);
    CHECK(split.energy_joules() == -solid_cp * 10.0);
    CHECK(split.state().temperature_kelvin == kMelting - 10.0);
    CHECK(split.state().liquid_mass_fraction == 0.0);
}

TEST_CASE("thermal reference: a rounded boundary cannot admit heat outside the model") {
    const PhaseChangeConfig config;
    auto sample = ThermalSample::create(1.0, {config.maximum_kelvin, 1.0});
    REQUIRE(sample);
    const double maximum = sample->energy_joules();
    const double ulp = maximum - std::nextafter(maximum, 0.0);
    REQUIRE(sample->apply_heat(-ulp).status == HeatStatus::Accepted);
    const double before = sample->energy_joules();
    // The rounded sum equals maximum, but the exact requested target is a quarter ulp above it.
    // Accepting it would silently keep a temperature-limited step by assigning loss to roundoff.
    CHECK(sample->apply_heat(1.25 * ulp).status == HeatStatus::TemperatureLimit);
    CHECK(sample->energy_joules() == before);

    sample = ThermalSample::create(1.0, {config.minimum_kelvin, 0.0});
    REQUIRE(sample);
    const double minimum = sample->energy_joules();
    const double low_ulp = std::nextafter(minimum, 0.0) - minimum;
    REQUIRE(sample->apply_heat(low_ulp).status == HeatStatus::Accepted);
    const double low_before = sample->energy_joules();
    CHECK(sample->apply_heat(-1.25 * low_ulp).status == HeatStatus::TemperatureLimit);
    CHECK(sample->energy_joules() == low_before);
}

TEST_CASE("thermal reference: an interior mixture cannot round into an all-solid phase") {
    auto sample = ThermalSample::create(1.0, {kMelting, 0.0});
    REQUIRE(sample);
    CHECK(sample->apply_heat(std::numeric_limits<double>::denorm_min()).status ==
          HeatStatus::Unrepresentable);
    CHECK(sample->energy_joules() == 0.0);
}

TEST_CASE("thermal reference: unsupported rounding refuses before changing the sample") {
    auto sample = ThermalSample::create(1.0, {kMelting, 0.5});
    REQUIRE(sample);
    const auto initial = bits(sample->energy_joules());

    struct RestoreRounding {
        int mode = std::fegetround();

        ~RestoreRounding() { (void)std::fesetround(mode); }
    } restore;

    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    CHECK_FALSE(ThermalSample::create(1.0, {kMelting, 0.5}));
    CHECK(sample->apply_heat(1.0).status == HeatStatus::UnsupportedEnvironment);
    CHECK(sample->apply_power(1.0, 1.0).status == HeatStatus::UnsupportedEnvironment);
    CHECK(bits(sample->energy_joules()) == initial);
    CHECK(sample->state().temperature_kelvin == kMelting);
    CHECK(sample->state().liquid_mass_fraction == 0.5);
}
