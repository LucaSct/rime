// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.4 (quaternions). We test the rotation *semantics*, not the stored components:
// axis-angle rotations send the basis vectors where the right-hand rule says; composition is
// right-to-left and agrees with applying rotations in sequence; conjugate is the inverse;
// the quaternion->matrix conversion agrees with rotate(); and slerp is the constant-speed
// shortest arc. Derivation: docs/math/quaternions-transforms.md.

#include <doctest/doctest.h>

#include <cmath>

#include "rime/core/math.hpp"

using namespace rime::core;

namespace {
constexpr float kLoose = 1e-5f; // trig round-off accumulates; loosen from the 1e-6 default
const Vec3 kX{1.0f, 0.0f, 0.0f};
const Vec3 kY{0.0f, 1.0f, 0.0f};
const Vec3 kZ{0.0f, 0.0f, 1.0f};
} // namespace

TEST_CASE("identity quaternion rotates nothing") {
    const Quat id = quat_identity();
    CHECK(approx_eq(rotate(id, Vec3{3.0f, -2.0f, 1.0f}), Vec3{3.0f, -2.0f, 1.0f}));
    CHECK(approx_eq(length(id), 1.0f));
}

TEST_CASE("axis-angle rotations follow the right-hand rule") {
    const Quat rz = quat_from_axis_angle(kZ, kHalfPi); // +90 deg about z
    const Quat rx = quat_from_axis_angle(kX, kHalfPi);
    const Quat ry = quat_from_axis_angle(kY, kHalfPi);
    // Right-handed 90-degree turns cycle the axes: x->y->z->x about z, x, y respectively.
    CHECK(approx_eq(rotate(rz, kX), kY, kLoose));
    CHECK(approx_eq(rotate(rx, kY), kZ, kLoose));
    CHECK(approx_eq(rotate(ry, kZ), kX, kLoose));
    // Rotating about an axis leaves that axis fixed.
    CHECK(approx_eq(rotate(rz, kZ), kZ, kLoose));
    // A unit quaternion preserves length.
    CHECK(approx_eq(
        length(rotate(rz, Vec3{2.0f, 5.0f, -1.0f})), length(Vec3{2.0f, 5.0f, -1.0f}), kLoose));
}

TEST_CASE("quaternion product composes rotations right-to-left") {
    const Quat a = quat_from_axis_angle(kZ, 0.7f);
    const Quat b = quat_from_axis_angle(kX, -1.3f);
    const Vec3 v{1.0f, 2.0f, 3.0f};
    // (a * b) applies b first, then a.
    CHECK(approx_eq(rotate(a * b, v), rotate(a, rotate(b, v)), kLoose));
    // Two 90-degree z-turns make a 180-degree turn: x -> -x.
    const Quat rz = quat_from_axis_angle(kZ, kHalfPi);
    CHECK(approx_eq(rotate(rz * rz, kX), -kX, kLoose));
}

TEST_CASE("conjugate is the inverse rotation for unit quaternions") {
    const Quat q = quat_from_axis_angle(Vec3{1.0f, 2.0f, 3.0f}, 1.1f);
    CHECK(approx_eq(q * conjugate(q), quat_identity(), kLoose));
    const Vec3 v{4.0f, -1.0f, 2.0f};
    // Rotating then un-rotating returns the original vector.
    CHECK(approx_eq(rotate(conjugate(q), rotate(q, v)), v, kLoose));
    CHECK(approx_eq(rotate(inverse(q), rotate(q, v)), v, kLoose));
}

TEST_CASE("to_mat3 / to_mat4 agree with rotate()") {
    const Quat q = quat_from_axis_angle(normalize(Vec3{1.0f, 1.0f, 0.0f}), 1.0f);
    const Mat3 r3 = to_mat3(q);
    const Mat4 r4 = to_mat4(q);
    const Vec3 v{0.5f, -2.0f, 3.0f};
    CHECK(approx_eq(r3 * v, rotate(q, v), kLoose));
    // to_mat4 is a pure rotation: no translation, so points and the quaternion agree.
    CHECK(approx_eq(transform_point(r4, v), rotate(q, v), kLoose));
    // A rotation matrix is orthonormal with determinant +1.
    CHECK(approx_eq(determinant(r4), 1.0f, kLoose));
}

TEST_CASE("axis-angle round-trips through to_axis_angle") {
    const Vec3 axis = normalize(Vec3{2.0f, -1.0f, 0.5f});
    const float angle = 1.2f;
    const Quat q = quat_from_axis_angle(axis, angle);
    Vec3 out_axis;
    float out_angle = 0.0f;
    to_axis_angle(q, out_axis, out_angle);
    CHECK(approx_eq(out_angle, angle, kLoose));
    CHECK(approx_eq(out_axis, axis, kLoose));
}

TEST_CASE("euler construction matches axis-angle and sequential rotation") {
    // A single-axis Euler angle is just that axis-angle rotation.
    CHECK(same_rotation(quat_from_euler(0.9f, 0.0f, 0.0f), quat_from_axis_angle(kX, 0.9f), kLoose));
    CHECK(same_rotation(quat_from_euler(0.0f, 0.0f, 0.5f), quat_from_axis_angle(kZ, 0.5f), kLoose));
    // The XYZ convention means q = qz * qy * qx (x applied first).
    const float ax = 0.3f, ay = -0.6f, az = 1.1f;
    const Quat composed =
        quat_from_axis_angle(kZ, az) * quat_from_axis_angle(kY, ay) * quat_from_axis_angle(kX, ax);
    CHECK(same_rotation(quat_from_euler(ax, ay, az), composed, kLoose));
}

TEST_CASE("slerp endpoints, midpoint, and unit length") {
    const Quat a = quat_identity();
    const Quat b = quat_from_axis_angle(kZ, kHalfPi); // 90 deg about z
    CHECK(same_rotation(slerp(a, b, 0.0f), a, kLoose));
    CHECK(same_rotation(slerp(a, b, 1.0f), b, kLoose));
    // Halfway is a 45-degree rotation: x -> (cos45, sin45, 0).
    const Vec3 mid = rotate(slerp(a, b, 0.5f), kX);
    const float s = std::sqrt(0.5f);
    CHECK(approx_eq(mid, Vec3{s, s, 0.0f}, kLoose));
    // slerp stays on the unit sphere throughout.
    for (float t = 0.0f; t <= 1.0f; t += 0.25f) {
        CHECK(approx_eq(length(slerp(a, b, t)), 1.0f, kLoose));
    }
    // q and -q are the same rotation, so slerp takes the short way regardless of sign.
    CHECK(same_rotation(slerp(a, -b, 0.5f), slerp(a, b, 0.5f), kLoose));
}

TEST_CASE("Quat is 16-byte aligned for the SIMD layout") {
    CHECK(alignof(Quat) == 16);
    CHECK(sizeof(Quat) == 16);
}
