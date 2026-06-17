// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>

// Scalar constants and helpers shared by the math library. Single-precision float is the
// engine's working type (GPUs are float-native and it halves bandwidth versus double); these
// helpers keep angle units and float comparisons consistent across vec/mat/quat.
namespace rime::core {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kTwoPi = 2.0f * kPi;
inline constexpr float kHalfPi = 0.5f * kPi;

// Default absolute tolerance for approx_eq. Suitable for values near unit scale (where our
// math lives); for very large magnitudes a relative/ULP comparison would be better.
inline constexpr float kEpsilon = 1e-6f;

[[nodiscard]] constexpr float radians(float degrees) noexcept {
    return degrees * (kPi / 180.0f);
}

[[nodiscard]] constexpr float degrees(float radians) noexcept {
    return radians * (180.0f / kPi);
}

[[nodiscard]] inline bool approx_eq(float a, float b, float eps = kEpsilon) noexcept {
    return std::fabs(a - b) <= eps;
}

} // namespace rime::core
