// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/math/mat.hpp"

#include <cmath>

#include "rime/core/diagnostics/assert.hpp"

namespace rime::core {

Mat4 operator*(const Mat4& a, const Mat4& b) noexcept {
    // (AB)(r,c) = sum_k A(r,k) B(k,c). Composition reads right-to-left: (A*B) applied to a
    // vector is A(B v), so B happens first. Column-major storage keeps each column contiguous.
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(row, k) * b.at(k, c);
            }
            r.at(row, c) = sum;
        }
    }
    return r;
}

namespace {
// Determinant of a 3x3 given row-major scalars [[a b c], [d e f], [g h i]].
float det3(float a, float b, float c, float d, float e, float f, float g, float h, float i) {
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}
} // namespace

float determinant(const Mat4& m) noexcept {
    // Laplace (cofactor) expansion along row 0: det = sum_c (-1)^c m(0,c) * minor(0,c), where
    // minor(0,c) is the 3x3 determinant left after deleting row 0 and column c.
    const float minor0 = det3(m.at(1, 1),
                              m.at(1, 2),
                              m.at(1, 3),
                              m.at(2, 1),
                              m.at(2, 2),
                              m.at(2, 3),
                              m.at(3, 1),
                              m.at(3, 2),
                              m.at(3, 3));
    const float minor1 = det3(m.at(1, 0),
                              m.at(1, 2),
                              m.at(1, 3),
                              m.at(2, 0),
                              m.at(2, 2),
                              m.at(2, 3),
                              m.at(3, 0),
                              m.at(3, 2),
                              m.at(3, 3));
    const float minor2 = det3(m.at(1, 0),
                              m.at(1, 1),
                              m.at(1, 3),
                              m.at(2, 0),
                              m.at(2, 1),
                              m.at(2, 3),
                              m.at(3, 0),
                              m.at(3, 1),
                              m.at(3, 3));
    const float minor3 = det3(m.at(1, 0),
                              m.at(1, 1),
                              m.at(1, 2),
                              m.at(2, 0),
                              m.at(2, 1),
                              m.at(2, 2),
                              m.at(3, 0),
                              m.at(3, 1),
                              m.at(3, 2));
    return m.at(0, 0) * minor0 - m.at(0, 1) * minor1 + m.at(0, 2) * minor2 - m.at(0, 3) * minor3;
}

Mat4 inverse(const Mat4& mat) noexcept {
    // Adjugate / cofactor inverse: inv = adjugate(M) / det(M). The 16 cofactor expressions are
    // the classic (MESA) formulation; our column-major storage is the same layout OpenGL uses,
    // so the flat indices m[0..15] line up directly. An affine transform has a cheaper inverse
    // ([[R^-1, -R^-1 t], [0, 1]]); this general form is the rarely-hot fallback, so we keep it
    // readable. (See docs/math/vectors-matrices.md.)
    const float* m = mat.m;
    float inv[16];

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    // det expanded along row 0 reuses the first cofactor of each column.
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    RIME_ASSERT(std::fabs(det) > kEpsilon); // a singular matrix has no inverse
    if (std::fabs(det) <= kEpsilon) {
        return Mat4{}; // safe-ish fallback (identity); asserted above in debug builds
    }

    const float inv_det = 1.0f / det;
    Mat4 result;
    for (int i = 0; i < 16; ++i) {
        result.m[i] = inv[i] * inv_det;
    }
    return result;
}

Mat4 perspective(float fovy_radians, float aspect, float z_near, float z_far) noexcept {
    // Right-handed, Vulkan clip space (z in [0,1]). The (1,1) entry is negated so NDC y points
    // down, matching Vulkan's framebuffer origin. Full derivation in
    // docs/math/vectors-matrices.md (the projection section).
    const float tan_half = std::tan(fovy_radians * 0.5f);
    Mat4 r;
    for (float& e : r.m) {
        e = 0.0f;
    }
    r.at(0, 0) = 1.0f / (aspect * tan_half);
    r.at(1, 1) = -1.0f / tan_half;
    r.at(2, 2) = z_far / (z_near - z_far);
    r.at(3, 2) = -1.0f;
    r.at(2, 3) = (z_far * z_near) / (z_near - z_far);
    return r;
}

Mat4 ortho(float left, float right, float bottom, float top, float z_near, float z_far) noexcept {
    // Right-handed, Vulkan clip space (z in [0,1]); row 1 is negated for Vulkan's y-down NDC.
    Mat4 r;
    for (float& e : r.m) {
        e = 0.0f;
    }
    r.at(0, 0) = 2.0f / (right - left);
    r.at(1, 1) = -2.0f / (top - bottom);
    r.at(2, 2) = -1.0f / (z_far - z_near);
    r.at(0, 3) = -(right + left) / (right - left);
    r.at(1, 3) = (top + bottom) / (top - bottom);
    r.at(2, 3) = -z_near / (z_far - z_near);
    r.at(3, 3) = 1.0f;
    return r;
}

Mat4 look_at(Vec3 eye, Vec3 center, Vec3 up) noexcept {
    // Right-handed view matrix. `forward` points from the eye toward the target; in RH view
    // space the camera looks down -forward, so row 2 stores -forward. The basis (right, up,
    // -forward) is orthonormal; the last column re-expresses the world origin in that basis.
    const Vec3 forward = normalize(center - eye);
    const Vec3 right = normalize(cross(forward, up));
    const Vec3 true_up = cross(right, forward);

    Mat4 r;
    r.at(0, 0) = right.x;
    r.at(0, 1) = right.y;
    r.at(0, 2) = right.z;
    r.at(0, 3) = -dot(right, eye);
    r.at(1, 0) = true_up.x;
    r.at(1, 1) = true_up.y;
    r.at(1, 2) = true_up.z;
    r.at(1, 3) = -dot(true_up, eye);
    r.at(2, 0) = -forward.x;
    r.at(2, 1) = -forward.y;
    r.at(2, 2) = -forward.z;
    r.at(2, 3) = dot(forward, eye);
    r.at(3, 0) = 0.0f;
    r.at(3, 1) = 0.0f;
    r.at(3, 2) = 0.0f;
    r.at(3, 3) = 1.0f;
    return r;
}

} // namespace rime::core
