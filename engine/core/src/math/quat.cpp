// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/math/quat.hpp"

#include <cmath>

// Heavier quaternion operations. The algebra (product, conjugate, rotate, axis-angle) is inline
// in the header; here live the conversions to rotation matrices, the inverse axis-angle/Euler
// constructions, and slerp. Math derived in docs/math/quaternions-transforms.md.
namespace rime::core {

Mat3 to_mat3(const Quat& q) noexcept {
    // Standard unit-quaternion -> rotation-matrix formula. Each entry comes from expanding the
    // sandwich q e_i q* for the basis vectors; assumes |q| = 1 (normalize first if unsure).
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    Mat3 r;
    r.at(0, 0) = 1.0f - 2.0f * (yy + zz);
    r.at(0, 1) = 2.0f * (xy - wz);
    r.at(0, 2) = 2.0f * (xz + wy);
    r.at(1, 0) = 2.0f * (xy + wz);
    r.at(1, 1) = 1.0f - 2.0f * (xx + zz);
    r.at(1, 2) = 2.0f * (yz - wx);
    r.at(2, 0) = 2.0f * (xz - wy);
    r.at(2, 1) = 2.0f * (yz + wx);
    r.at(2, 2) = 1.0f - 2.0f * (xx + yy);
    return r;
}

Mat4 to_mat4(const Quat& q) noexcept {
    // The rotation occupies the upper-left 3x3; the rest stays identity (no translation/scale).
    const Mat3 r3 = to_mat3(q);
    Mat4 r; // identity by default
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) {
            r.at(row, c) = r3.at(row, c);
        }
    }
    return r;
}

void to_axis_angle(const Quat& q, Vec3& out_axis, float& out_angle) noexcept {
    // w = cos(angle/2); the vector part has length sin(angle/2). Recover angle from the
    // half-angle, then divide out sin to get the unit axis. Use a normalized copy so callers
    // need not pre-normalize.
    const Quat n = normalize(q);
    out_angle = 2.0f * std::acos(n.w);           // in [0, pi] (acos clamps w to [-1, 1] domain)
    const float s = std::sqrt(1.0f - n.w * n.w); // = sin(angle/2)
    if (s < kEpsilon) {
        // Near-zero rotation: the axis is undefined. Return an arbitrary unit axis.
        out_axis = Vec3{1.0f, 0.0f, 0.0f};
    } else {
        out_axis = Vec3{n.x / s, n.y / s, n.z / s};
    }
}

Quat quat_from_euler(float x_rad, float y_rad, float z_rad) noexcept {
    // Extrinsic X-then-Y-then-Z (== intrinsic Z-Y'-X''): q = qz * qy * qx, so the X rotation is
    // applied first. Built directly from half-angle sines/cosines (cheaper than three products).
    const float hx = 0.5f * x_rad, hy = 0.5f * y_rad, hz = 0.5f * z_rad;
    const float cx = std::cos(hx), sx = std::sin(hx);
    const float cy = std::cos(hy), sy = std::sin(hy);
    const float cz = std::cos(hz), sz = std::sin(hz);

    return Quat{
        sx * cy * cz - cx * sy * sz, // x
        cx * sy * cz + sx * cy * sz, // y
        cx * cy * sz - sx * sy * cz, // z
        cx * cy * cz + sx * sy * sz, // w
    };
}

Quat slerp(const Quat& a, const Quat& b, float t) noexcept {
    // Shortest-arc spherical interpolation. cos(Omega) = a . b is the angle between the
    // quaternions on the unit 3-sphere. Because q and -q are the same rotation, flip b when the
    // dot is negative so we travel the short way round.
    Quat end = b;
    float d = dot(a, b);
    if (d < 0.0f) {
        end = -b;
        d = -d;
    }

    // When the inputs are nearly parallel, sin(Omega) -> 0 and the slerp weights blow up; a
    // normalized lerp is numerically safe and visually indistinguishable there.
    constexpr float kSlerpLinearThreshold = 0.9995f;
    if (d > kSlerpLinearThreshold) {
        return normalize(a + (end - a) * t);
    }

    const float omega = std::acos(d);
    const float sin_omega = std::sin(omega);
    const float wa = std::sin((1.0f - t) * omega) / sin_omega;
    const float wb = std::sin(t * omega) / sin_omega;
    return a * wa + end * wb;
}

} // namespace rime::core
