// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

#include "rime/fluids/thermal_contact.hpp"

using namespace rime::fluids;

namespace {
constexpr double kMelting = 273.15;

ThermalSample water(double mass, double temperature, double fraction) {
    auto sample = ThermalSample::create(mass, {temperature, fraction});
    REQUIRE(sample);
    return *sample;
}

ThermalSample binary_sample(double mass, double temperature, double fraction = 1.0) {
    PhaseChangeConfig config{256.0, 2.0, 4.0, 16.0, 128.0, 512.0};
    auto sample = ThermalSample::create(mass, {temperature, fraction}, config);
    REQUIRE(sample);
    return *sample;
}

void unchanged(const ThermalSample& a, const ThermalSample& before) {
    CHECK(std::bit_cast<std::uint64_t>(a.energy_joules()) ==
          std::bit_cast<std::uint64_t>(before.energy_joules()));
    CHECK(std::bit_cast<std::uint64_t>(a.state().temperature_kelvin) ==
          std::bit_cast<std::uint64_t>(before.state().temperature_kelvin));
    CHECK(std::bit_cast<std::uint64_t>(a.state().liquid_mass_fraction) ==
          std::bit_cast<std::uint64_t>(before.state().liquid_mass_fraction));
    CHECK(a.mass_kg() == before.mass_kg());
}

void exchanged(const ThermalContactResult& r) {
    REQUIRE(r.status == ThermalContactStatus::Exchanged);
    CHECK(r.first.status == HeatStatus::Accepted);
    CHECK(r.second.status == HeatStatus::Accepted);
    CHECK(r.first.requested_joules == -r.second.requested_joules);
    CHECK(std::abs(r.first.requested_joules) == r.lower_joules);
    CHECK(r.lower_joules > 0.0);
    CHECK(r.upper_joules >= r.lower_joules);
    CHECK((r.upper_joules - r.lower_joules) / r.upper_joules <=
          ThermalContactResult::relative_heat_tolerance);
    CHECK(r.iterations <= ThermalContactResult::maximum_iterations);
}

void refused(const ThermalContactResult& r,
             const ThermalSample& a,
             const ThermalSample& b,
             const ThermalSample& before_a,
             const ThermalSample& before_b) {
    CHECK(r.status != ThermalContactStatus::Exchanged);
    CHECK(r.status != ThermalContactStatus::NoExchange);
    CHECK(r.first.status == HeatStatus::InvalidInput);
    CHECK(r.second.status == HeatStatus::InvalidInput);
    CHECK(r.first.applied_joules == 0.0);
    CHECK(r.second.applied_joules == 0.0);
    CHECK(r.lower_joules == 0.0);
    CHECK(r.upper_joules == 0.0);
    unchanged(a, before_a);
    unchanged(b, before_b);
}
} // namespace

TEST_CASE("thermal contact: analytic sensible exchange and swapped arguments") {
    auto hot = binary_sample(1.0, 288.0);
    auto cold = binary_sample(1.0, 272.0);
    auto swapped_hot = hot;
    auto swapped_cold = cold;
    const auto r = exchange_heat(hot, cold, 2.0, 1.0);
    exchanged(r);
    CHECK(r.lower_joules == 16.0);
    CHECK(r.first.requested_joules == -16.0);
    CHECK(hot.state().temperature_kelvin == 284.0);
    CHECK(cold.state().temperature_kelvin == 276.0);
    CHECK(r.first.roundoff_joules == 0.0);
    CHECK(r.second.roundoff_joules == 0.0);
    const auto reversed = exchange_heat(swapped_cold, swapped_hot, 2.0, 1.0);
    exchanged(reversed);
    unchanged(hot, swapped_hot);
    unchanged(cold, swapped_cold);
    CHECK(reversed.first.requested_joules == 16.0);

    auto heavy_cold = binary_sample(3.0, 272.0);
    hot = binary_sample(1.0, 288.0);
    const auto unequal = exchange_heat(hot, heavy_cold, 2.0, 1.0);
    exchanged(unequal);
    CHECK(unequal.lower_joules == doctest::Approx(19.2).epsilon(2e-11));
}

TEST_CASE("thermal contact: disabled exchange and isothermal phases keep exact state") {
    for (const auto& inputs : {std::pair{0.0, 1.0}, std::pair{1.0, -0.0}}) {
        auto a = water(1.0, kMelting + 10.0, 1.0);
        auto b = water(1.0, kMelting - 10.0, 0.0);
        const auto before_a = a;
        const auto before_b = b;
        const auto r = exchange_heat(a, b, inputs.first, inputs.second);
        CHECK(r.status == ThermalContactStatus::NoExchange);
        CHECK(r.first.status == HeatStatus::Accepted);
        CHECK(r.second.status == HeatStatus::Accepted);
        CHECK(r.first.requested_joules == 0.0);
        CHECK(r.second.requested_joules == 0.0);
        unchanged(a, before_a);
        unchanged(b, before_b);
    }
    auto ice = water(1.0, kMelting, 0.1);
    auto liquid = water(3.0, kMelting, 0.9);
    const auto before_ice = ice;
    const auto before_liquid = liquid;
    CHECK(exchange_heat(ice, liquid, 1e308, 1e308).status == ThermalContactStatus::NoExchange);
    unchanged(ice, before_ice);
    unchanged(liquid, before_liquid);
}

TEST_CASE("thermal contact: latent reservoirs melt and freeze from conductive heat") {
    auto hot = water(100.0, kMelting + 10.0, 1.0);
    auto ice = water(1.0, kMelting, 0.0);
    auto r = exchange_heat(hot, ice, 10000.0, 1.0);
    exchanged(r);
    const double melt_heat = 10.0 / (1.0 / 10000.0 + 1.0 / 417900.0);
    CHECK(r.lower_joules == doctest::Approx(melt_heat).epsilon(2e-11));
    CHECK(ice.state().temperature_kelvin == kMelting);
    CHECK(ice.state().liquid_mass_fraction == doctest::Approx(melt_heat / 333500.0));
    CHECK(hot.state().temperature_kelvin > kMelting);

    auto liquid = water(1.0, kMelting, 1.0);
    auto cold = water(100.0, kMelting - 10.0, 0.0);
    r = exchange_heat(liquid, cold, 10000.0, 1.0);
    exchanged(r);
    const double freeze_heat = 10.0 / (1.0 / 10000.0 + 1.0 / 205200.0);
    CHECK(r.lower_joules == doctest::Approx(freeze_heat).epsilon(2e-11));
    CHECK(liquid.state().temperature_kelvin == kMelting);
    CHECK(liquid.state().liquid_mass_fraction == doctest::Approx(1.0 - freeze_heat / 333500.0));
    CHECK(cold.state().temperature_kelvin < kMelting);

    // Sensitivity: doubling latent heat halves melted mass fraction for this same transferred
    // heat, while doubling conductance changes the transfer itself. Zero exchange above cannot
    // produce either result, so a stationary phase is not mistaken for successful coupling.
    PhaseChangeConfig twice_latent;
    twice_latent.latent_heat_j_kg *= 2.0;
    auto slower = ThermalSample::create(1.0, {kMelting, 0.0}, twice_latent);
    REQUIRE(slower);
    hot = water(100.0, kMelting + 10.0, 1.0);
    r = exchange_heat(hot, *slower, 10000.0, 1.0);
    exchanged(r);
    CHECK(slower->state().liquid_mass_fraction ==
          doctest::Approx(ice.state().liquid_mass_fraction * 0.5));
    hot = water(100.0, kMelting + 10.0, 1.0);
    auto faster = water(1.0, kMelting, 0.0);
    exchanged(exchange_heat(hot, faster, 20000.0, 1.0));
    CHECK(faster.state().liquid_mass_fraction > ice.state().liquid_mass_fraction * 1.5);
}

TEST_CASE("thermal contact: large steps cross phase endpoints without temperature reversal") {
    auto hot = water(10.0, kMelting + 30.0, 1.0);
    auto cold = water(1.0, kMelting - 10.0, 0.0);
    exchanged(exchange_heat(hot, cold, 1e10, 1.0));
    CHECK(cold.state().liquid_mass_fraction == 1.0);
    CHECK(cold.state().temperature_kelvin > kMelting + 19.0);
    CHECK(hot.state().temperature_kelvin >= cold.state().temperature_kelvin);
    CHECK(hot.state().temperature_kelvin - cold.state().temperature_kelvin < 0.001);
    hot = water(1.0, kMelting + 1.0, 1.0);
    cold = water(100.0, kMelting - 19.0, 0.0);
    exchanged(exchange_heat(hot, cold, 1e10, 1.0));
    CHECK(hot.state().liquid_mass_fraction == 0.0);
    CHECK(hot.state().temperature_kelvin < kMelting);
    CHECK(hot.state().temperature_kelvin >= cold.state().temperature_kelvin);
}

TEST_CASE("thermal contact: approach to a shared plateau stops before phase redistribution") {
    auto hot = water(1.0, kMelting + 10.0, 1.0);
    auto cold = water(1.0, kMelting - 10.0, 0.0);
    const auto r = exchange_heat(hot, cold, 1e12, 1.0);
    exchanged(r);
    CHECK(r.lower_joules == doctest::Approx(41790.0).epsilon(1e-7));
    CHECK(hot.state().liquid_mass_fraction == 1.0);
    CHECK(cold.state().temperature_kelvin == kMelting);
    CHECK(cold.state().liquid_mass_fraction == doctest::Approx(21270.0 / 333500.0));
    CHECK(hot.state().temperature_kelvin >= cold.state().temperature_kelvin);
}

TEST_CASE("thermal contact: rounding residuals disprove exact stored-energy conservation") {
    auto hot = binary_sample(1.0, 288.0);
    auto cold = binary_sample(1e6, 272.0);
    const auto r = exchange_heat(hot, cold, 2.0, 1.0);
    exchanged(r);
    CHECK(r.first.roundoff_joules == 0.0);
    CHECK(r.second.roundoff_joules != 0.0);
    // The offline Fraction checker proves the six-component identity without rounding the
    // low parts away. Opposite requests alone are insufficient: this witness has a nonzero
    // exact change in total stored energy, balanced by the explicitly returned residual.
}

TEST_CASE("thermal contact: different melting points exchange without fraction synchronization") {
    PhaseChangeConfig config{280.0, 2000.0, 4000.0, 300000.0, 250.0, 320.0};
    auto high_melt = ThermalSample::create(1.0, {280.0, 0.5}, config);
    REQUIRE(high_melt);
    auto low_melt = water(1.0, kMelting, 0.5);
    const auto r = exchange_heat(*high_melt, low_melt, 10.0, 1.0);
    exchanged(r);
    CHECK(r.lower_joules == doctest::Approx(68.5));
    CHECK(high_melt->state().temperature_kelvin == 280.0);
    CHECK(low_melt.state().temperature_kelvin == kMelting);
    CHECK(high_melt->state().liquid_mass_fraction < 0.5);
    CHECK(low_melt.state().liquid_mass_fraction > 0.5);
}

TEST_CASE("thermal contact: first order convergence and mass scaling") {
    const auto integrate = [](unsigned steps) {
        auto a = binary_sample(1.0, 288.0);
        auto b = binary_sample(1.0, 272.0);
        for (unsigned i = 0; i < steps; ++i)
            exchanged(exchange_heat(a, b, 1.0, 2.0 / steps));
        return a.state().temperature_kelvin - b.state().temperature_kelvin;
    };
    const double exact_gap = 16.0 * std::exp(-1.0);
    const double coarse_error = integrate(16) - exact_gap;
    const double fine_error = integrate(32) - exact_gap;
    CHECK(coarse_error > 0.0);
    CHECK(fine_error > 0.0);
    CHECK(coarse_error / fine_error > 1.9);
    CHECK(coarse_error / fine_error < 2.1);
    for (double scale : {1e-6, 1.0, 1e6}) {
        auto hot = binary_sample(scale, 288.0);
        auto cold = binary_sample(scale, 272.0);
        exchanged(exchange_heat(hot, cold, 2.0 * scale, 1.0));
        CHECK(hot.state().temperature_kelvin == doctest::Approx(284.0));
        CHECK(cold.state().temperature_kelvin == doctest::Approx(276.0));
    }
}

TEST_CASE("thermal contact: limits and all numerical refusals roll back both samples") {
    const auto initial_a = water(1.0, kMelting + 30.0, 1.0);
    PhaseChangeConfig config;
    config.maximum_kelvin = kMelting + 1.0;
    auto initial_b = ThermalSample::create(1.0, {kMelting, 1.0}, config);
    REQUIRE(initial_b);
    auto a = initial_a;
    auto b = *initial_b;
    auto r = exchange_heat(a, b, 1e6, 1.0);
    CHECK(r.status == ThermalContactStatus::TemperatureLimit);
    refused(r, a, b, initial_a, *initial_b);
    for (auto inputs : {std::pair{-1.0, 1.0},
                        std::pair{1.0, -1.0},
                        std::pair{std::numeric_limits<double>::infinity(), 1.0},
                        std::pair{1.0, std::numeric_limits<double>::quiet_NaN()}}) {
        r = exchange_heat(a, b, inputs.first, inputs.second);
        CHECK(r.status == ThermalContactStatus::InvalidInput);
        refused(r, a, b, initial_a, *initial_b);
    }
    CHECK(exchange_heat(a, a, 0.0, 0.0).status == ThermalContactStatus::InvalidInput);
    unchanged(a, initial_a);
    for (auto inputs : {std::pair{1e308, 1e308},
                        std::pair{1e-300, 1e-300},
                        std::pair{1e-300, 1.0},
                        std::pair{1e307, 1.0}}) {
        r = exchange_heat(a, b, inputs.first, inputs.second);
        CHECK(r.status == ThermalContactStatus::Unrepresentable);
        refused(r, a, b, initial_a, *initial_b);
    }
    // A finite demand spanning too many binary exponents must not return a fake zero success.
    r = exchange_heat(a, b, 1e100, 1.0);
    CHECK(r.status == ThermalContactStatus::IterationLimit);
    CHECK(r.iterations == ThermalContactResult::maximum_iterations);
    refused(r, a, b, initial_a, *initial_b);
}

TEST_CASE("thermal contact: asymmetric energy ulps refuse a one-sided ineffective update") {
    auto a = water(1e100, kMelting + 10.0, 1.0);
    auto b = water(1.0, kMelting - 10.0, 0.0);
    const auto before_a = a;
    const auto before_b = b;
    const auto r = exchange_heat(a, b, 1.0, 1.0);
    CHECK(r.status == ThermalContactStatus::Unrepresentable);
    refused(r, a, b, before_a, before_b);
}

TEST_CASE("thermal contact: subnormal heat keeps the bracket contract or refuses accuracy") {
    for (int exponent : {-1000, -1020, -1040, -1060, -1070}) {
        const double mass = std::ldexp(1.0, exponent);
        auto a = binary_sample(mass, 288.0);
        auto b = binary_sample(mass, 272.0);
        exchanged(exchange_heat(a, b, 2.0 * mass, 1.0));
        CHECK(a.state().temperature_kelvin == 284.0);
        CHECK(b.state().temperature_kelvin == 276.0);
        a = binary_sample(mass, 288.0);
        b = binary_sample(mass, 272.0);
        const auto before_a = a;
        const auto before_b = b;
        const auto r = exchange_heat(a, b, 1.3 * mass, 1.0);
        if (exponent >= -1040) {
            exchanged(r);
        } else {
            CHECK(r.status == ThermalContactStatus::IterationLimit);
            refused(r, a, b, before_a, before_b);
        }
    }
}

TEST_CASE("thermal contact: unsupported rounding mode refuses even a disabled contact") {
    auto a = water(1.0, kMelting + 10.0, 1.0);
    auto b = water(1.0, kMelting - 10.0, 0.0);
    const auto before_a = a;
    const auto before_b = b;

    struct RestoreRounding {
        int saved = std::fegetround();

        ~RestoreRounding() { std::fesetround(saved); }
    } restore;

    REQUIRE(std::fesetround(FE_DOWNWARD) == 0);
    const auto r = exchange_heat(a, b, 0.0, 1.0);
    CHECK(r.status == ThermalContactStatus::UnsupportedEnvironment);
    refused(r, a, b, before_a, before_b);
}
