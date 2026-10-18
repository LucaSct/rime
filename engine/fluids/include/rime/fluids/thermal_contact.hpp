// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/fluids/phase_change.hpp"

namespace rime::fluids {

enum class ThermalContactStatus {
    Exchanged,
    NoExchange,
    InvalidInput,
    TemperatureLimit,
    Unrepresentable,
    UnsupportedEnvironment,
    IterationLimit
};

struct ThermalContactResult {
    ThermalContactStatus status = ThermalContactStatus::InvalidInput;
    HeatExchange first{};
    HeatExchange second{};
    // On Exchanged: positive transfer magnitudes bracket a sign change of the rounded-state
    // backward-Euler residual. The lower endpoint was applied, without reversing temperature
    // order. Width / upper <= relative_heat_tolerance (or the rounded residual is exactly zero).
    double lower_joules = 0.0;
    double upper_joules = 0.0;
    unsigned iterations = 0;

    static constexpr double relative_heat_tolerance = 0x1p-36;
    static constexpr unsigned maximum_iterations = 128;
};

// One isolated conductive contact between distinct, fixed-mass, well-mixed samples.
// Conductance G is in W/K (NOT material conductivity W/(m K)); caller owns geometry and time.
// Backward-Euler conduction: q = (G*dt) * (T_hot(E_hot-q) - T_cold(E_cold+q)). The implemented
// binary64 equation is q/Q0 = gap(q)/gap(0), Q0 = round(round(G*dt)*gap(0)); temperatures are
// ThermalSample's rounded caches. Solve using copied samples, then commit both.
// Different melting temperatures/configs are supported. Equal temperatures never redistribute
// phase fractions. Zero G/dt also returns NoExchange with bitwise unchanged states.
//
// Exchanged receipts request exactly opposite heat. As exact real values of returned doubles:
// first.applied + first.applied_error + second.applied + second.applied_error
//     + first.roundoff + second.roundoff == 0.
// Stored total energy alone need NOT be exactly conserved. Keep both residuals; there is no
// hidden reservoir or automatic residual reapplication. G*dt and its initial temperature-gap
// product are rounded binary64 inputs to the solve, outside this update ledger.
//
// Negative/nonfinite inputs and aliasing refuse. Any numerical/range refusal leaves BOTH
// originals unchanged and returns empty receipts. Limits refuse rather than clip the law.
// Finite nonzero products, representable trial updates and convergence within the fixed budget
// are required: this reference deliberately refuses unsupported scales, rather than silently
// treating positive demand as no flow. Requires ThermalSample's floating-point environment.
// First-order time discretization; not an exact continuous-time solution or an N-body solver.
[[nodiscard]] ThermalContactResult exchange_heat(ThermalSample& first,
                                                 ThermalSample& second,
                                                 double conductance_w_k,
                                                 double dt_seconds) noexcept;

} // namespace rime::fluids
