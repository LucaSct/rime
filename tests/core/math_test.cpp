// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.3 (core math I): the vector algebra obeys the identities it claims (dot/cross,
// right-handed basis, normalization), and the matrices implement the conventions documented in
// docs/adr/0004 and derived in docs/math/vectors-matrices.md: column-major storage with the
// v' = M v convention, right-to-left composition, a correct general inverse (M * inv(M) = I),
// and Vulkan-clip-space projections (z in [0,1], y down). We test *properties* — round trips,
// invariants, plane mappings — rather than transcribing magic numbers, so the tests document
// the math instead of merely pinning it.

#include <doctest/doctest.h>

#include <cmath>

#include "rime/core/math.hpp"

using namespace rime::core;

// ----------------------------------------------------------------------------------- scalar
TEST_CASE("radians/degrees round-trip and known angles") {
    CHECK(approx_eq(radians(180.0f), kPi));
    CHECK(approx_eq(radians(90.0f), kHalfPi));
    CHECK(approx_eq(degrees(kPi), 180.0f));
    CHECK(approx_eq(degrees(radians(57.3f)), 57.3f));
}

// ------------------------------------------------------------------------------------- Vec2
TEST_CASE("Vec2 arithmetic, dot, and normalization") {
    const Vec2 a{3.0f, 4.0f};
    CHECK(approx_eq(a + Vec2{1.0f, 1.0f}, Vec2{4.0f, 5.0f}));
    CHECK(approx_eq(a - Vec2{1.0f, 2.0f}, Vec2{2.0f, 2.0f}));
    CHECK(approx_eq(2.0f * a, Vec2{6.0f, 8.0f}));
    CHECK(approx_eq(dot(a, Vec2{1.0f, 0.0f}), 3.0f));
    CHECK(approx_eq(length(a), 5.0f)); // 3-4-5 triangle
    CHECK(approx_eq(length(normalize(a)), 1.0f));
    CHECK(approx_eq(normalize(Vec2{}), Vec2{})); // zero vector guarded
}

// ------------------------------------------------------------------------------------- Vec3
TEST_CASE("Vec3 arithmetic and compound assignment") {
    Vec3 v{1.0f, 2.0f, 3.0f};
    v += Vec3{1.0f, 1.0f, 1.0f};
    CHECK(approx_eq(v, Vec3{2.0f, 3.0f, 4.0f}));
    v -= Vec3{0.0f, 1.0f, 2.0f};
    CHECK(approx_eq(v, Vec3{2.0f, 2.0f, 2.0f}));
    v *= 0.5f;
    CHECK(approx_eq(v, Vec3{1.0f, 1.0f, 1.0f}));
    CHECK(approx_eq(Vec3{2.0f, 4.0f, 6.0f} / 2.0f, Vec3{1.0f, 2.0f, 3.0f}));
}

TEST_CASE("Vec3 dot, length, and normalize") {
    const Vec3 a{1.0f, 2.0f, 2.0f};
    CHECK(approx_eq(dot(a, a), 9.0f));
    CHECK(approx_eq(length_squared(a), 9.0f));
    CHECK(approx_eq(length(a), 3.0f));
    CHECK(approx_eq(length(normalize(a)), 1.0f));
    CHECK(approx_eq(normalize(Vec3{}), Vec3{}));
    // Orthogonal vectors have zero dot product.
    CHECK(approx_eq(dot(Vec3{1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}), 0.0f));
}

TEST_CASE("Vec3 cross product is right-handed and anticommutative") {
    const Vec3 x{1.0f, 0.0f, 0.0f};
    const Vec3 y{0.0f, 1.0f, 0.0f};
    const Vec3 z{0.0f, 0.0f, 1.0f};
    // The defining identity of a right-handed basis: x × y = z (and cyclic permutations).
    CHECK(approx_eq(cross(x, y), z));
    CHECK(approx_eq(cross(y, z), x));
    CHECK(approx_eq(cross(z, x), y));
    // Anticommutativity and self-cross.
    CHECK(approx_eq(cross(x, y), -cross(y, x)));
    CHECK(approx_eq(cross(x, x), Vec3{}));
    // The cross product is perpendicular to both inputs.
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{-2.0f, 0.0f, 1.0f};
    const Vec3 c = cross(a, b);
    CHECK(approx_eq(dot(c, a), 0.0f));
    CHECK(approx_eq(dot(c, b), 0.0f));
}

// ------------------------------------------------------------------------------------- Vec4
TEST_CASE("Vec4 arithmetic and dot") {
    const Vec4 a{1.0f, 2.0f, 3.0f, 4.0f};
    CHECK(approx_eq(a + a, Vec4{2.0f, 4.0f, 6.0f, 8.0f}));
    CHECK(approx_eq(2.0f * a, Vec4{2.0f, 4.0f, 6.0f, 8.0f}));
    CHECK(approx_eq(dot(a, Vec4{1.0f, 1.0f, 1.0f, 1.0f}), 10.0f));
}

TEST_CASE("Vec4 is 16-byte aligned for the SIMD layout") {
    // The alignment is a load-bearing promise to the future intrinsic backend (see simd.hpp).
    CHECK(alignof(Vec4) == 16);
    CHECK(sizeof(Vec4) == 16);
}

// ------------------------------------------------------------------------------------- Mat4
TEST_CASE("Mat4 identity is the multiplicative neutral element") {
    const Mat4 id = identity();
    const Vec4 v{1.0f, 2.0f, 3.0f, 1.0f};
    CHECK(approx_eq(id * v, v));
    const Mat4 m = mat4_translation({1.0f, 2.0f, 3.0f});
    CHECK(approx_eq(id * m, m));
    CHECK(approx_eq(m * id, m));
}

TEST_CASE("Mat4 translation moves points but not directions") {
    const Mat4 t = mat4_translation({10.0f, 20.0f, 30.0f});
    // A point (w = 1) is translated...
    CHECK(approx_eq(transform_point(t, Vec3{1.0f, 1.0f, 1.0f}), Vec3{11.0f, 21.0f, 31.0f}));
    // ...a direction (w = 0) is not. This is the whole reason for homogeneous coordinates.
    CHECK(approx_eq(transform_vector(t, Vec3{1.0f, 1.0f, 1.0f}), Vec3{1.0f, 1.0f, 1.0f}));
}

TEST_CASE("Mat4 scaling scales both points and directions") {
    const Mat4 s = mat4_scaling({2.0f, 3.0f, 4.0f});
    CHECK(approx_eq(transform_point(s, Vec3{1.0f, 1.0f, 1.0f}), Vec3{2.0f, 3.0f, 4.0f}));
    CHECK(approx_eq(transform_vector(s, Vec3{1.0f, 1.0f, 1.0f}), Vec3{2.0f, 3.0f, 4.0f}));
}

TEST_CASE("Mat4 multiplication composes right-to-left") {
    const Mat4 t = mat4_translation({1.0f, 0.0f, 0.0f});
    const Mat4 s = mat4_scaling({2.0f, 2.0f, 2.0f});
    // (T * S) applied to a point scales first, then translates: p -> 2p -> 2p + (1,0,0).
    const Vec3 p{3.0f, 1.0f, 1.0f};
    const Mat4 ts = t * s;
    CHECK(approx_eq(transform_point(ts, p), Vec3{7.0f, 2.0f, 2.0f}));
    // Order matters: (S * T) translates first, then scales: p -> p + (1,0,0) -> 2(p + (1,0,0)).
    const Mat4 st = s * t;
    CHECK(approx_eq(transform_point(st, p), Vec3{8.0f, 2.0f, 2.0f}));
}

TEST_CASE("Mat4 transpose is an involution and swaps off-diagonals") {
    Mat4 m;
    for (int i = 0; i < 16; ++i) {
        m.m[i] = static_cast<float>(i);
    }
    const Mat4 mt = transpose(m);
    CHECK(approx_eq(mt.at(0, 1), m.at(1, 0)));
    CHECK(approx_eq(mt.at(2, 3), m.at(3, 2)));
    CHECK(approx_eq(transpose(mt), m)); // transpose twice == identity
}

TEST_CASE("Mat4 determinant of identity, scaling, and translation") {
    CHECK(approx_eq(determinant(identity()), 1.0f));
    // det of a pure scaling is the product of the scales (the volume scale factor).
    CHECK(approx_eq(determinant(mat4_scaling({2.0f, 3.0f, 4.0f})), 24.0f));
    // A translation preserves volume, so its determinant is 1.
    CHECK(approx_eq(determinant(mat4_translation({5.0f, 6.0f, 7.0f})), 1.0f));
}

TEST_CASE("Mat4 inverse satisfies M * inv(M) = I") {
    // A non-trivial affine transform (scale then translate) exercises the general cofactor path.
    const Mat4 m = mat4_translation({3.0f, -2.0f, 5.0f}) * mat4_scaling({2.0f, 4.0f, 0.5f});
    const Mat4 mi = inverse(m);
    CHECK(approx_eq(m * mi, identity()));
    CHECK(approx_eq(mi * m, identity()));
    // inv(inv(M)) == M.
    CHECK(approx_eq(inverse(mi), m));
    // Inverting then transforming round-trips a point.
    const Vec3 p{7.0f, 1.0f, -3.0f};
    CHECK(approx_eq(transform_point(mi, transform_point(m, p)), p));
}

TEST_CASE("perspective maps the near and far planes to Vulkan depth [0,1]") {
    const float z_near = 0.1f;
    const float z_far = 100.0f;
    const Mat4 p = perspective(radians(60.0f), 16.0f / 9.0f, z_near, z_far);

    // In right-handed view space the camera looks down -z, so a point in front of it has
    // negative z. A point on the near plane must land at NDC z = 0, the far plane at z = 1.
    const Vec4 on_near = p * Vec4{0.0f, 0.0f, -z_near, 1.0f};
    const Vec4 on_far = p * Vec4{0.0f, 0.0f, -z_far, 1.0f};
    CHECK(approx_eq(on_near.z / on_near.w, 0.0f));
    CHECK(approx_eq(on_far.z / on_far.w, 1.0f));
    // The perspective divide w equals the view-space depth (positive in front of the camera).
    CHECK(approx_eq(on_near.w, z_near));
    CHECK(approx_eq(on_far.w, z_far));
    // y is flipped for Vulkan's y-down NDC: the (1,1) entry is negative.
    CHECK(p.at(1, 1) < 0.0f);
}

TEST_CASE("ortho maps the near and far planes to Vulkan depth [0,1]") {
    const float z_near = 1.0f;
    const float z_far = 10.0f;
    const Mat4 o = ortho(-2.0f, 2.0f, -2.0f, 2.0f, z_near, z_far);
    const Vec4 on_near = o * Vec4{0.0f, 0.0f, -z_near, 1.0f};
    const Vec4 on_far = o * Vec4{0.0f, 0.0f, -z_far, 1.0f};
    CHECK(approx_eq(on_near.z, 0.0f)); // ortho has w = 1, no divide needed
    CHECK(approx_eq(on_far.z, 1.0f));
    // Corners of the box map to the edges of NDC xy.
    CHECK(approx_eq((o * Vec4{2.0f, 0.0f, -z_near, 1.0f}).x, 1.0f));
    CHECK(o.at(1, 1) < 0.0f); // y-down flip
}

TEST_CASE("look_at places the eye at the view-space origin and looks down -z") {
    const Vec3 eye{0.0f, 0.0f, 5.0f};
    const Mat4 v = look_at(eye, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
    // The eye maps to the origin of view space.
    CHECK(approx_eq(transform_point(v, eye), Vec3{0.0f, 0.0f, 0.0f}));
    // The target sits in front of the camera, i.e. at negative view-space z (distance 5).
    CHECK(approx_eq(transform_point(v, Vec3{0.0f, 0.0f, 0.0f}), Vec3{0.0f, 0.0f, -5.0f}));
    // A view matrix is rigid (orthonormal rotation + translation), so its determinant is 1.
    CHECK(approx_eq(determinant(v), 1.0f));
}

// ------------------------------------------------------------------------------------- Mat3
TEST_CASE("Mat3 multiplication, transpose, and extraction from Mat4") {
    const Mat3 id; // identity by default
    const Vec3 v{1.0f, 2.0f, 3.0f};
    CHECK(approx_eq(id * v, v));

    // The upper-left 3x3 of a translation is the identity (translation lives in the 4th column).
    const Mat4 t = mat4_translation({9.0f, 9.0f, 9.0f});
    CHECK(approx_eq(mat3_from_mat4(t), Mat3{}));

    // The upper-left 3x3 of a scaling is the scaling — the linear part survives extraction.
    const Mat4 s = mat4_scaling({2.0f, 3.0f, 4.0f});
    const Mat3 s3 = mat3_from_mat4(s);
    CHECK(approx_eq(s3 * v, Vec3{2.0f, 6.0f, 12.0f}));

    // (AB)^T == B^T A^T, checked on the 3x3 path.
    const Mat3 a = mat3_from_mat4(mat4_scaling({1.0f, 2.0f, 3.0f}));
    CHECK(approx_eq(transpose(a * s3), transpose(s3) * transpose(a)));
}
