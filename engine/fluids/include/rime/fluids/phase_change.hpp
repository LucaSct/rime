// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <optional>

namespace rime::fluids {

// Constant-property, fixed-pressure reference material. Defaults approximate fresh water/ice;
// the capacities are frozen example values, not a temperature-dependent property database.
// See docs/research/2026-10-18-water-ice-phase.md for sources and the intended temperature range.
struct PhaseChangeConfig {
    double melting_kelvin = 273.15;
    double solid_heat_capacity_j_kg_k = 2052.0;
    double liquid_heat_capacity_j_kg_k = 4179.0;
    double latent_heat_j_kg = 333500.0;
    double minimum_kelvin = 253.15;
    double maximum_kelvin = 313.15;
};

struct ThermalState {
    double temperature_kelvin;
    double liquid_mass_fraction;

    // Temperature alone cannot select an energy reservoir at the melting point. Require both
    // values in construction rather than default an omitted fraction to ice or liquid.
    constexpr ThermalState(double temperature, double liquid_fraction) noexcept
        : temperature_kelvin(temperature), liquid_mass_fraction(liquid_fraction) {}
};

enum class HeatStatus {
    Accepted,
    InvalidInput,
    TemperatureLimit,
    Unrepresentable,
    UnsupportedEnvironment
};

struct HeatExchange {
    HeatStatus status = HeatStatus::InvalidInput;
    // For an accepted exchange, as an exact real-number identity between the returned doubles:
    // requested = applied + applied_error + roundoff. The two applied components represent the
    // exact change between stored energy doubles; ordinary summation may round away the low part.
    // Refusals apply zero heat; both errors are zero and
    // requested is retained only when a finite heat amount could be calculated.
    double requested_joules = 0.0;
    double applied_joules = 0.0;
    double roundoff_joules = 0.0;
    double applied_error_joules = 0.0; // low component of the exact stored-energy subtraction
};

// A bounded, allocation-free calorimetry oracle for ONE well-mixed fixed mass. There is no grid,
// geometry, conduction, water transport or physical expansion in this reference. It establishes
// the energy/temperature/phase relation before any of those consumers can depend on it.
// Requires binary64 round-to-nearest with gradual underflow throughout use. Creation and heat
// mutation check this mode. state() returns an already derived cache without doing arithmetic.
// Fast-math is unsupported.
//
// Total enthalpy E is measured relative to all-solid material at melting temperature Tm:
//   E < 0:    T = Tm + E/(m*cs), liquid fraction = 0
//   0 <= E <= m*L: T = Tm,      liquid fraction = E/(m*L)
//   E > m*L:  T = Tm + (E-m*L)/(m*cl), liquid fraction = 1.
// Keeping E as state, rather than integrating temperature and guessing phase from its sign,
// retains the latent-heat plateau. Large heat steps may cross both endpoints without substeps.
class ThermalSample {
public:
    // At Tm any liquid fraction in [0,1] is valid; below it the fraction MUST be zero, above it
    // one. Reject inconsistent authoring instead of silently discarding a latent-heat reservoir.
    [[nodiscard]] static std::optional<ThermalSample>
    create(double mass_kg, ThermalState initial, const PhaseChangeConfig& config = {}) noexcept;

    [[nodiscard]] ThermalState state() const noexcept { return state_; }

    [[nodiscard]] double mass_kg() const noexcept { return mass_kg_; }

    [[nodiscard]] double energy_joules() const noexcept { return energy_joules_; }

    [[nodiscard]] const PhaseChangeConfig& config() const noexcept { return config_; }

    // Positive heat warms/melts; negative heat cools/freezes. Temperature bounds REFUSE the whole
    // exchange, never clamp accepted energy. Nonfinite arithmetic or nonzero heat too small to
    // move the stored double also refuses. Otherwise roundoff is explicitly returned to caller.
    // Every refusal leaves the entire sample unchanged. Exact zero is an unchanged accepted call.
    [[nodiscard]] HeatExchange apply_heat(double heat_joules) noexcept;
    // Caller owns the clock. Zero dt or power is unchanged; negative/nonfinite dt refuses.
    // Requested heat is the rounded binary64 product P*dt; product rounding is not in the ledger.
    [[nodiscard]] HeatExchange apply_power(double power_watts, double dt_seconds) noexcept;

private:
    ThermalSample() = default;
    [[nodiscard]] std::optional<ThermalState> decode(double energy_joules) const noexcept;
    PhaseChangeConfig config_{};
    ThermalState state_{273.15,
                        0.0}; // derived and committed with energy, never integrated separately
    double mass_kg_ = 0.0;
    double energy_joules_ = 0.0;
    double solid_capacity_j_k_ = 0.0;
    double liquid_capacity_j_k_ = 0.0;
    double latent_energy_j_ = 0.0;
    double minimum_energy_j_ = 0.0;
    double maximum_energy_j_ = 0.0;
};

} // namespace rime::fluids
