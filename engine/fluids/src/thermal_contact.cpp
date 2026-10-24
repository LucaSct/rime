// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/fluids/thermal_contact.hpp"

#include <cmath>
#include <numeric>

#if defined(__FAST_MATH__)
#error "The thermal reference requires IEEE round-to-nearest operations without fast-math."
#endif

namespace rime::fluids {
namespace {
ThermalContactResult refuse(ThermalContactStatus status, unsigned iterations = 0) noexcept {
    ThermalContactResult result;
    result.status = status;
    result.iterations = iterations;
    return result;
}
} // namespace

ThermalContactResult exchange_heat(ThermalSample& first,
                                   ThermalSample& second,
                                   double conductance_w_k,
                                   double dt_seconds) noexcept {
    if (&first == &second || !std::isfinite(conductance_w_k) || conductance_w_k < 0.0 ||
        !std::isfinite(dt_seconds) || dt_seconds < 0.0)
        return refuse(ThermalContactStatus::InvalidInput);
    auto probe = first;
    if (probe.apply_heat(0.0).status == HeatStatus::UnsupportedEnvironment)
        return refuse(ThermalContactStatus::UnsupportedEnvironment);
    if (conductance_w_k == 0.0 || dt_seconds == 0.0 ||
        first.state().temperature_kelvin == second.state().temperature_kelvin) {
        auto result = refuse(ThermalContactStatus::NoExchange);
        result.first.status = result.second.status = HeatStatus::Accepted;
        return result;
    }

    const bool first_hot = first.state().temperature_kelvin > second.state().temperature_kelvin;
    auto& hot = first_hot ? first : second;
    auto& cold = first_hot ? second : first;
    const double initial_gap = hot.state().temperature_kelvin - cold.state().temperature_kelvin;
    const double conductance_time = conductance_w_k * dt_seconds;
    const double explicit_heat = conductance_time * initial_gap;
    if (!std::isfinite(conductance_time) || conductance_time == 0.0 ||
        !std::isfinite(explicit_heat) || explicit_heat == 0.0)
        return refuse(ThermalContactStatus::Unrepresentable);

    // Backward Euler evaluates the heat rate at the END of the step. Its scalar residual
    // q/(G*dt) - (Th(q)-Tc(q)) increases through both sensible and latent branches. Unlike a
    // capped explicit step, it does not snap to equilibrium early or traverse a shared plateau
    // merely to equalize fractions. Bisection uses the forward-Euler demand as an upper bound.
    // Failed range trials tighten that bound, but cannot certify a root: accepting at a material
    // limit would silently replace the requested physical law with clipping. Rounded temperature
    // caches make the residual a staircase: the certificate brackets its sign change, rather
    // than asserting an exact zero exists or an exact-real material law has been solved.
    double lower = 0.0;
    double upper = explicit_heat;
    bool upper_certified = false;
    auto next_hot = hot;
    auto next_cold = cold;
    HeatExchange hot_receipt{};
    HeatExchange cold_receipt{};
    unsigned iterations = 0;
    for (; iterations < ThermalContactResult::maximum_iterations; ++iterations) {
        const double heat = iterations == 0 ? upper : std::midpoint(lower, upper);
        auto trial_hot = hot;
        auto trial_cold = cold;
        const auto hr = trial_hot.apply_heat(-heat);
        const auto cr = trial_cold.apply_heat(heat);
        if (hr.status == HeatStatus::Unrepresentable || cr.status == HeatStatus::Unrepresentable)
            return refuse(ThermalContactStatus::Unrepresentable, iterations + 1);
        if (hr.status == HeatStatus::TemperatureLimit ||
            cr.status == HeatStatus::TemperatureLimit) {
            upper = heat;
            upper_certified = false;
        } else if (hr.status != HeatStatus::Accepted || cr.status != HeatStatus::Accepted) {
            return refuse(ThermalContactStatus::UnsupportedEnvironment, iterations + 1);
        } else {
            const double gap =
                trial_hot.state().temperature_kelvin - trial_cold.state().temperature_kelvin;
            // Normalize both sides by their initial values, keeping comparisons bounded even
            // for large coefficients. This defines the binary64 residual used by the bracket;
            // it is not a certificate for an exact-real constitutive equation.
            const double demand = gap / initial_gap;
            const double supplied = heat / explicit_heat;
            if (gap >= 0.0 && supplied <= demand) {
                lower = heat;
                next_hot = trial_hot;
                next_cold = trial_cold;
                hot_receipt = hr;
                cold_receipt = cr;
            }
            if (gap < 0.0 || supplied >= demand) {
                upper = heat;
                upper_certified = true;
            }
        }
        // Strict comparison also covers a subnormal tolerance product rounded UP by half an
        // ulp: a nonzero representable width below it is still below the exact tolerance.
        if (upper == lower ||
            upper - lower < upper * ThermalContactResult::relative_heat_tolerance) {
            if (!upper_certified)
                return refuse(ThermalContactStatus::TemperatureLimit, iterations + 1);
            if (lower == 0.0)
                return refuse(ThermalContactStatus::Unrepresentable, iterations + 1);
            hot = next_hot;
            cold = next_cold;
            ThermalContactResult result;
            result.status = ThermalContactStatus::Exchanged;
            result.first = first_hot ? hot_receipt : cold_receipt;
            result.second = first_hot ? cold_receipt : hot_receipt;
            result.lower_joules = lower;
            result.upper_joules = upper;
            result.iterations = iterations + 1;
            return result;
        }
    }
    return refuse(ThermalContactStatus::IterationLimit, iterations);
}

} // namespace rime::fluids
