// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/fluids/phase_change.hpp"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <limits>

#if defined(__FAST_MATH__)
#error "The thermal reference requires IEEE round-to-nearest operations without fast-math."
#endif

namespace rime::fluids {
namespace {
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);

bool supported_environment() noexcept {
    if (std::fegetround() != FE_TONEAREST)
        return false;
    // Volatile forces a real arithmetic probe rather than a compile-time subnormal constant.
    // This detects both flushed results and denormal inputs treated as zero without reaching
    // into architecture-specific control registers or changing the caller's floating-point mode.
    volatile double normal = std::numeric_limits<double>::min();
    volatile double subnormal = normal * 0.5;
    return subnormal != 0.0 && subnormal * 2.0 == normal;
}

struct Sum {
    double rounded;
    double error;
};

std::optional<Sum> exact_sum(double a, double b) noexcept {
    // FastTwoSum: order by magnitude, round the sum, then recover the part of the smaller
    // operand that rounded away. With nearest rounding and gradual underflow, rounded+error
    // equals the exact sum of the two input doubles. This small expansion is enough for a
    // transaction ledger; no arbitrary-precision runtime or cumulative hidden energy is needed.
    if (std::abs(a) < std::abs(b))
        std::swap(a, b);
    const double rounded = a + b;
    if (!std::isfinite(rounded))
        return std::nullopt;
    const double error = b - (rounded - a);
    if (!std::isfinite(error))
        return std::nullopt;
    return Sum{rounded, error};
}

bool positive_finite(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

HeatExchange refuse(HeatStatus status, double heat = 0.0) noexcept {
    return {status, std::isfinite(heat) ? heat : 0.0, 0.0, 0.0};
}
} // namespace

std::optional<ThermalSample>
ThermalSample::create(double mass_kg, ThermalState initial, const PhaseChangeConfig& c) noexcept {
    if (!supported_environment() || !positive_finite(mass_kg) ||
        !positive_finite(c.melting_kelvin) || !positive_finite(c.solid_heat_capacity_j_kg_k) ||
        !positive_finite(c.liquid_heat_capacity_j_kg_k) || !positive_finite(c.latent_heat_j_kg) ||
        !positive_finite(c.minimum_kelvin) || !std::isfinite(c.maximum_kelvin) ||
        !(c.minimum_kelvin < c.melting_kelvin && c.melting_kelvin < c.maximum_kelvin) ||
        !std::isfinite(initial.temperature_kelvin) ||
        initial.temperature_kelvin < c.minimum_kelvin ||
        initial.temperature_kelvin > c.maximum_kelvin ||
        !std::isfinite(initial.liquid_mass_fraction) || initial.liquid_mass_fraction < 0.0 ||
        initial.liquid_mass_fraction > 1.0 ||
        (initial.temperature_kelvin < c.melting_kelvin && initial.liquid_mass_fraction != 0.0) ||
        (initial.temperature_kelvin > c.melting_kelvin && initial.liquid_mass_fraction != 1.0))
        return std::nullopt;

    ThermalSample sample;
    sample.config_ = c;
    sample.mass_kg_ = mass_kg;
    sample.solid_capacity_j_k_ = mass_kg * c.solid_heat_capacity_j_kg_k;
    sample.liquid_capacity_j_k_ = mass_kg * c.liquid_heat_capacity_j_kg_k;
    sample.latent_energy_j_ = mass_kg * c.latent_heat_j_kg;
    sample.minimum_energy_j_ = sample.solid_capacity_j_k_ * (c.minimum_kelvin - c.melting_kelvin);
    sample.maximum_energy_j_ = sample.latent_energy_j_ +
                               sample.liquid_capacity_j_k_ * (c.maximum_kelvin - c.melting_kelvin);
    if (!positive_finite(sample.solid_capacity_j_k_) ||
        !positive_finite(sample.liquid_capacity_j_k_) ||
        !positive_finite(sample.latent_energy_j_) || !std::isfinite(sample.minimum_energy_j_) ||
        !(sample.minimum_energy_j_ < 0.0) || !std::isfinite(sample.maximum_energy_j_) ||
        !(sample.maximum_energy_j_ > sample.latent_energy_j_))
        return std::nullopt;

    const double delta_t = initial.temperature_kelvin - c.melting_kelvin;
    if (delta_t < 0.0)
        sample.energy_joules_ = sample.solid_capacity_j_k_ * delta_t;
    else if (delta_t > 0.0)
        sample.energy_joules_ = sample.latent_energy_j_ + sample.liquid_capacity_j_k_ * delta_t;
    else
        sample.energy_joules_ = sample.latent_energy_j_ * initial.liquid_mass_fraction;
    if (!std::isfinite(sample.energy_joules_) || sample.energy_joules_ < sample.minimum_energy_j_ ||
        sample.energy_joules_ > sample.maximum_energy_j_ ||
        (delta_t < 0.0 && !(sample.energy_joules_ < 0.0)) ||
        (delta_t > 0.0 && !(sample.energy_joules_ > sample.latent_energy_j_)) ||
        (delta_t == 0.0 && initial.liquid_mass_fraction > 0.0 &&
         initial.liquid_mass_fraction < 1.0 &&
         !(sample.energy_joules_ > 0.0 && sample.energy_joules_ < sample.latent_energy_j_)))
        return std::nullopt;
    const auto decoded = sample.decode(sample.energy_joules_);
    if (!decoded)
        return std::nullopt;
    sample.state_ = *decoded;
    return sample;
}

std::optional<ThermalState> ThermalSample::decode(double energy) const noexcept {
    const auto& c = config_;
    ThermalState result{c.melting_kelvin, 0.0};
    if (energy < 0.0)
        result.temperature_kelvin += energy / solid_capacity_j_k_;
    else if (energy <= latent_energy_j_) {
        result.liquid_mass_fraction = energy / latent_energy_j_;
        if (energy > 0.0 && energy < latent_energy_j_ &&
            !(result.liquid_mass_fraction > 0.0 && result.liquid_mass_fraction < 1.0))
            return std::nullopt;
    } else {
        result.temperature_kelvin += (energy - latent_energy_j_) / liquid_capacity_j_k_;
        result.liquid_mass_fraction = 1.0;
    }
    if (!std::isfinite(result.temperature_kelvin) ||
        result.temperature_kelvin < std::nextafter(c.minimum_kelvin, 0.0) ||
        result.temperature_kelvin >
            std::nextafter(c.maximum_kelvin, std::numeric_limits<double>::infinity()))
        return std::nullopt;
    // Only the last bit of the decoded temperature may be clamped at configured bounds.
    // Energy admission checks the exact prospective sum, so this cannot hide excess heat.
    result.temperature_kelvin =
        std::clamp(result.temperature_kelvin, c.minimum_kelvin, c.maximum_kelvin);
    return result;
}

HeatExchange ThermalSample::apply_heat(double heat_joules) noexcept {
    if (!supported_environment())
        return refuse(HeatStatus::UnsupportedEnvironment, heat_joules);
    if (!std::isfinite(heat_joules))
        return refuse(HeatStatus::InvalidInput);
    if (heat_joules == 0.0)
        return {HeatStatus::Accepted, heat_joules, 0.0, 0.0};
    const auto sum = exact_sum(energy_joules_, heat_joules);
    if (!sum)
        return refuse(HeatStatus::Unrepresentable, heat_joules);
    const double next = sum->rounded;
    if (next < minimum_energy_j_ || next > maximum_energy_j_ ||
        (next == minimum_energy_j_ && sum->error < 0.0) ||
        (next == maximum_energy_j_ && sum->error > 0.0))
        return refuse(HeatStatus::TemperatureLimit, heat_joules);
    const auto decoded = decode(next);
    if (next == energy_joules_ || !decoded)
        return refuse(HeatStatus::Unrepresentable, heat_joules);
    const auto applied = exact_sum(next, -energy_joules_);
    if (!applied)
        return refuse(HeatStatus::Unrepresentable, heat_joules);
    energy_joules_ = next;
    state_ = *decoded;
    return {HeatStatus::Accepted, heat_joules, applied->rounded, sum->error, applied->error};
}

HeatExchange ThermalSample::apply_power(double power_watts, double dt_seconds) noexcept {
    if (!supported_environment())
        return refuse(HeatStatus::UnsupportedEnvironment);
    if (!std::isfinite(power_watts) || !std::isfinite(dt_seconds) || dt_seconds < 0.0)
        return refuse(HeatStatus::InvalidInput);
    if (power_watts == 0.0 || dt_seconds == 0.0)
        return apply_heat(0.0);
    const double heat = power_watts * dt_seconds;
    if (!std::isfinite(heat) || heat == 0.0)
        return refuse(HeatStatus::Unrepresentable);
    return apply_heat(heat);
}

} // namespace rime::fluids
