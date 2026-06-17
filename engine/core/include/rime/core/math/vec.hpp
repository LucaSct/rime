// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>

#include "rime/core/math/scalar.hpp"

// Vectors for the engine. Plain aggregates (no hidden state) operated on by free functions and
// operators: data-oriented, trivially copyable, and easy to vectorize later. Conventions live
// in docs/adr/0004; the geometry behind dot/cross/normalize is derived in
// docs/math/vectors-matrices.md.
namespace rime::core {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 16-byte aligned: one Vec4 maps onto a single SIMD register, and arrays of Vec4 stay aligned
// for a future intrinsic backend. As a homogeneous coordinate, w = 1 marks a point (affected
// by translation) and w = 0 a direction (not).
struct alignas(16) Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// --------------------------------------------------------------------------------- Vec2
[[nodiscard]] constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept {
    return {a.x + b.x, a.y + b.y};
}

[[nodiscard]] constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept {
    return {a.x - b.x, a.y - b.y};
}

[[nodiscard]] constexpr Vec2 operator-(Vec2 v) noexcept {
    return {-v.x, -v.y};
}

[[nodiscard]] constexpr Vec2 operator*(Vec2 v, float s) noexcept {
    return {v.x * s, v.y * s};
}

[[nodiscard]] constexpr Vec2 operator*(float s, Vec2 v) noexcept {
    return v * s;
}

[[nodiscard]] constexpr float dot(Vec2 a, Vec2 b) noexcept {
    return a.x * b.x + a.y * b.y;
}

[[nodiscard]] inline float length(Vec2 v) noexcept {
    return std::sqrt(dot(v, v));
}

[[nodiscard]] inline Vec2 normalize(Vec2 v) noexcept {
    const float len = length(v);
    return len > kEpsilon ? Vec2{v.x / len, v.y / len} : Vec2{};
}

[[nodiscard]] inline bool approx_eq(Vec2 a, Vec2 b, float eps = kEpsilon) noexcept {
    return approx_eq(a.x, b.x, eps) && approx_eq(a.y, b.y, eps);
}

// --------------------------------------------------------------------------------- Vec3
[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] constexpr Vec3 operator-(Vec3 v) noexcept {
    return {-v.x, -v.y, -v.z};
}

[[nodiscard]] constexpr Vec3 operator*(Vec3 v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s};
}

[[nodiscard]] constexpr Vec3 operator*(float s, Vec3 v) noexcept {
    return v * s;
}

[[nodiscard]] constexpr Vec3 operator/(Vec3 v, float s) noexcept {
    return {v.x / s, v.y / s, v.z / s};
}

constexpr Vec3& operator+=(Vec3& a, Vec3 b) noexcept {
    a = a + b;
    return a;
}

constexpr Vec3& operator-=(Vec3& a, Vec3 b) noexcept {
    a = a - b;
    return a;
}

constexpr Vec3& operator*=(Vec3& a, float s) noexcept {
    a = a * s;
    return a;
}

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Right-handed cross product: a x b is perpendicular to both, with magnitude |a||b|sin(theta)
// and orientation given by the right-hand rule. In our right-handed basis, cross(x, y) == z.
[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] constexpr float length_squared(Vec3 v) noexcept {
    return dot(v, v);
}

[[nodiscard]] inline float length(Vec3 v) noexcept {
    return std::sqrt(dot(v, v));
}

[[nodiscard]] inline Vec3 normalize(Vec3 v) noexcept {
    const float len = length(v);
    // Guard the zero vector: normalizing it returns zero instead of dividing by zero.
    return len > kEpsilon ? v * (1.0f / len) : Vec3{};
}

[[nodiscard]] inline bool approx_eq(Vec3 a, Vec3 b, float eps = kEpsilon) noexcept {
    return approx_eq(a.x, b.x, eps) && approx_eq(a.y, b.y, eps) && approx_eq(a.z, b.z, eps);
}

// --------------------------------------------------------------------------------- Vec4
[[nodiscard]] constexpr Vec4 operator+(const Vec4& a, const Vec4& b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

[[nodiscard]] constexpr Vec4 operator-(const Vec4& a, const Vec4& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

[[nodiscard]] constexpr Vec4 operator*(const Vec4& v, float s) noexcept {
    return {v.x * s, v.y * s, v.z * s, v.w * s};
}

[[nodiscard]] constexpr Vec4 operator*(float s, const Vec4& v) noexcept {
    return v * s;
}

[[nodiscard]] constexpr float dot(const Vec4& a, const Vec4& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

[[nodiscard]] inline bool approx_eq(const Vec4& a, const Vec4& b, float eps = kEpsilon) noexcept {
    return approx_eq(a.x, b.x, eps) && approx_eq(a.y, b.y, eps) && approx_eq(a.z, b.z, eps) &&
           approx_eq(a.w, b.w, eps);
}

} // namespace rime::core
