// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math/scalar.hpp"
#include "rime/core/math/vec.hpp"

// Matrices for the engine. Storage is COLUMN-MAJOR (element (row r, col c) is m[c*N + r]) with
// the column-vector convention v' = M v, a right-handed world, and Vulkan clip space (z in
// [0,1], y down). This matches GLSL/Vulkan, so a matrix uploads to a uniform buffer with no
// transpose. Rationale: docs/adr/0004. Full derivation: docs/math/vectors-matrices.md.
namespace rime::core {

struct Mat3 {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // column-major; identity by default

    [[nodiscard]] float& at(int row, int col) noexcept { return m[col * 3 + row]; }

    [[nodiscard]] float at(int row, int col) const noexcept { return m[col * 3 + row]; }
};

struct alignas(16) Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // column-major; identity

    [[nodiscard]] float& at(int row, int col) noexcept { return m[col * 4 + row]; }

    [[nodiscard]] float at(int row, int col) const noexcept { return m[col * 4 + row]; }
};

// ---- Mat4 construction --------------------------------------------------------------------
[[nodiscard]] inline Mat4 identity() noexcept {
    return Mat4{};
}

[[nodiscard]] inline Mat4 mat4_translation(Vec3 t) noexcept {
    Mat4 r; // starts as identity
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}

[[nodiscard]] inline Mat4 mat4_scaling(Vec3 s) noexcept {
    Mat4 r;
    r.at(0, 0) = s.x;
    r.at(1, 1) = s.y;
    r.at(2, 2) = s.z;
    return r;
}

// ---- Mat4 operations ----------------------------------------------------------------------
// v' = M v (column-vector convention): result_r = sum_c M(r,c) v_c.
[[nodiscard]] inline Vec4 operator*(const Mat4& mtx, const Vec4& v) noexcept {
    return Vec4{mtx.at(0, 0) * v.x + mtx.at(0, 1) * v.y + mtx.at(0, 2) * v.z + mtx.at(0, 3) * v.w,
                mtx.at(1, 0) * v.x + mtx.at(1, 1) * v.y + mtx.at(1, 2) * v.z + mtx.at(1, 3) * v.w,
                mtx.at(2, 0) * v.x + mtx.at(2, 1) * v.y + mtx.at(2, 2) * v.z + mtx.at(2, 3) * v.w,
                mtx.at(3, 0) * v.x + mtx.at(3, 1) * v.y + mtx.at(3, 2) * v.z + mtx.at(3, 3) * v.w};
}

// Apply M to a point (w = 1): rotation, scale, and translation.
[[nodiscard]] inline Vec3 transform_point(const Mat4& m, Vec3 p) noexcept {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0f};
    return Vec3{r.x, r.y, r.z};
}

// Apply M to a direction (w = 0): rotation and scale only, never translation.
[[nodiscard]] inline Vec3 transform_vector(const Mat4& m, Vec3 d) noexcept {
    const Vec4 r = m * Vec4{d.x, d.y, d.z, 0.0f};
    return Vec3{r.x, r.y, r.z};
}

[[nodiscard]] inline Mat4 transpose(const Mat4& m) noexcept {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            r.at(row, c) = m.at(c, row);
        }
    }
    return r;
}

[[nodiscard]] inline bool approx_eq(const Mat4& a, const Mat4& b, float eps = kEpsilon) noexcept {
    for (int i = 0; i < 16; ++i) {
        if (!approx_eq(a.m[i], b.m[i], eps)) {
            return false;
        }
    }
    return true;
}

// Heavier operations live in mat.cpp.
[[nodiscard]] Mat4 operator*(const Mat4& a, const Mat4& b) noexcept;
[[nodiscard]] float determinant(const Mat4& m) noexcept;
[[nodiscard]] Mat4 inverse(const Mat4& m) noexcept;

// Right-handed projections targeting Vulkan clip space: depth z in [0,1], and y flipped so NDC
// y points down (the flip lives here, so the rest of the engine can ignore it).
[[nodiscard]] Mat4
perspective(float fovy_radians, float aspect, float z_near, float z_far) noexcept;
[[nodiscard]] Mat4
ortho(float left, float right, float bottom, float top, float z_near, float z_far) noexcept;
[[nodiscard]] Mat4 look_at(Vec3 eye, Vec3 center, Vec3 up) noexcept;

// ---- Mat3 ---------------------------------------------------------------------------------
[[nodiscard]] inline Mat3 mat3_from_mat4(const Mat4& m) noexcept {
    Mat3 r;
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) {
            r.at(row, c) = m.at(row, c);
        }
    }
    return r;
}

[[nodiscard]] inline Vec3 operator*(const Mat3& mtx, Vec3 v) noexcept {
    return Vec3{mtx.at(0, 0) * v.x + mtx.at(0, 1) * v.y + mtx.at(0, 2) * v.z,
                mtx.at(1, 0) * v.x + mtx.at(1, 1) * v.y + mtx.at(1, 2) * v.z,
                mtx.at(2, 0) * v.x + mtx.at(2, 1) * v.y + mtx.at(2, 2) * v.z};
}

[[nodiscard]] inline Mat3 operator*(const Mat3& a, const Mat3& b) noexcept {
    Mat3 r;
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += a.at(row, k) * b.at(k, c);
            }
            r.at(row, c) = sum;
        }
    }
    return r;
}

[[nodiscard]] inline Mat3 transpose(const Mat3& m) noexcept {
    Mat3 r;
    for (int c = 0; c < 3; ++c) {
        for (int row = 0; row < 3; ++row) {
            r.at(row, c) = m.at(c, row);
        }
    }
    return r;
}

[[nodiscard]] inline bool approx_eq(const Mat3& a, const Mat3& b, float eps = kEpsilon) noexcept {
    for (int i = 0; i < 9; ++i) {
        if (!approx_eq(a.m[i], b.m[i], eps)) {
            return false;
        }
    }
    return true;
}

} // namespace rime::core
