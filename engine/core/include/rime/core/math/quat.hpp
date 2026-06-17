// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/scalar.hpp"
#include "rime/core/math/vec.hpp"

// Quaternions for representing 3-D rotations. A unit quaternion q = (x, y, z, w) encodes a
// rotation as the scalar w = cos(theta/2) and the vector (x, y, z) = sin(theta/2) * axis (the
// half-angle is intrinsic to the q v q* sandwich — see docs/math/quaternions-transforms.md).
// We use the HAMILTON product with a right-handed, active (local->world) convention; rotations
// compose right-to-left like matrices (ADR-0004, ADR-0005). Quaternions beat matrices for
// *storing and interpolating* orientation: 4 floats instead of 9, no drift-prone
// re-orthonormalization (just renormalize), and a well-defined shortest-arc blend (slerp).
namespace rime::core {

// Stored vector-part-first (x, y, z, w) to match Vec4 and GLSL. 16-byte aligned for the same
// SIMD-layout reasons as Vec4 (math/simd.hpp). Identity = no rotation.
struct alignas(16) Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

[[nodiscard]] inline constexpr Quat quat_identity() noexcept {
    return Quat{0.0f, 0.0f, 0.0f, 1.0f};
}

// ---- Linear-space helpers (treat a quaternion as a 4-vector) ------------------------------
// Used by slerp/normalize; a quaternion is a point on the unit 3-sphere, so these are the
// ambient R^4 operations.
[[nodiscard]] constexpr Quat operator+(const Quat& a, const Quat& b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

[[nodiscard]] constexpr Quat operator-(const Quat& a, const Quat& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

[[nodiscard]] constexpr Quat operator-(const Quat& q) noexcept {
    return {-q.x, -q.y, -q.z, -q.w};
}

[[nodiscard]] constexpr Quat operator*(const Quat& q, float s) noexcept {
    return {q.x * s, q.y * s, q.z * s, q.w * s};
}

[[nodiscard]] constexpr Quat operator*(float s, const Quat& q) noexcept {
    return q * s;
}

[[nodiscard]] constexpr float dot(const Quat& a, const Quat& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

[[nodiscard]] inline float length(const Quat& q) noexcept {
    return std::sqrt(dot(q, q));
}

[[nodiscard]] inline Quat normalize(const Quat& q) noexcept {
    const float len = length(q);
    return len > kEpsilon ? q * (1.0f / len) : quat_identity();
}

// ---- Rotation algebra ---------------------------------------------------------------------
// Hamilton product: composition of rotations. (a * b) applies b first, then a — right-to-left,
// matching matrix multiplication, so a TRS chain reads the same whichever type you use.
// Derived from i^2 = j^2 = k^2 = ijk = -1 (see the derivation note).
[[nodiscard]] constexpr Quat operator*(const Quat& a, const Quat& b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Conjugate negates the vector part. For a UNIT quaternion this is the inverse rotation, because
// q q* = |q|^2 = 1; we expose both names so intent is clear at the call site.
[[nodiscard]] constexpr Quat conjugate(const Quat& q) noexcept {
    return {-q.x, -q.y, -q.z, q.w};
}

[[nodiscard]] inline Quat inverse(const Quat& q) noexcept {
    const float n2 = dot(q, q);
    return n2 > kEpsilon ? conjugate(q) * (1.0f / n2) : quat_identity();
}

// Rotate a vector by a unit quaternion. Mathematically v' = q v q* (v as a pure quaternion),
// but we use the algebraically equivalent, cheaper form
//     t = 2 (q_xyz x v),   v' = v + q_w t + q_xyz x t,
// which avoids building the two-product sandwich. (Derivation note, §"rotating a vector".)
[[nodiscard]] inline Vec3 rotate(const Quat& q, Vec3 v) noexcept {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = 2.0f * cross(u, v);
    return v + q.w * t + cross(u, t);
}

// Unit quaternion for a rotation of `angle` radians about `axis` (right-handed). The half-angle
// is the quaternion's defining feature: w = cos(angle/2), xyz = sin(angle/2) * axis_hat.
[[nodiscard]] inline Quat quat_from_axis_angle(Vec3 axis, float angle) noexcept {
    const Vec3 a = normalize(axis);
    const float h = 0.5f * angle;
    const float s = std::sin(h);
    return {a.x * s, a.y * s, a.z * s, std::cos(h)};
}

[[nodiscard]] inline bool approx_eq(const Quat& a, const Quat& b, float eps = kEpsilon) noexcept {
    return approx_eq(a.x, b.x, eps) && approx_eq(a.y, b.y, eps) && approx_eq(a.z, b.z, eps) &&
           approx_eq(a.w, b.w, eps);
}

// True if two unit quaternions represent the SAME rotation. q and -q are the same orientation
// (the double cover SU(2) -> SO(3)), so we compare |dot| against 1.
[[nodiscard]] inline bool
same_rotation(const Quat& a, const Quat& b, float eps = kEpsilon) noexcept {
    return approx_eq(std::fabs(dot(a, b)), 1.0f, eps);
}

// ---- Heavier conversions (defined in quat.cpp) --------------------------------------------
[[nodiscard]] Mat3 to_mat3(const Quat& q) noexcept;
[[nodiscard]] Mat4 to_mat4(const Quat& q) noexcept;

// Recover the (axis, angle) of a unit quaternion. angle in [0, pi]; axis is unit (or an
// arbitrary unit vector when angle ~ 0, where the axis is undefined).
void to_axis_angle(const Quat& q, Vec3& out_axis, float& out_angle) noexcept;

// Euler convenience. Convention: extrinsic rotations about the world X, then Y, then Z axes
// (equivalently intrinsic Z-Y'-X''), i.e. q = qz * qy * qx. Angles in radians. Euler input is a
// convenience; quaternions/axis-angle are the engine's preferred, gimbal-lock-free form.
[[nodiscard]] Quat quat_from_euler(float x_rad, float y_rad, float z_rad) noexcept;

// Spherical linear interpolation along the shortest arc: constant angular velocity, unit-length
// output. Falls back to normalized lerp when the quaternions are nearly parallel (sin -> 0).
[[nodiscard]] Quat slerp(const Quat& a, const Quat& b, float t) noexcept;

} // namespace rime::core
