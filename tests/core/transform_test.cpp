// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.4 (TRS transforms). The decomposed transform applies scale-then-rotate-then-
// translate; baking to a Mat4 produces the *same* mapping (to_matrix == the decomposed apply);
// composition nests like the scene graph (parent * child); and inverse undoes the transform.
// Derivation: docs/math/quaternions-transforms.md.

#include <doctest/doctest.h>

#include "rime/core/math.hpp"

using namespace rime::core;

namespace {
constexpr float kLoose = 1e-5f;
}

TEST_CASE("identity transform is a no-op") {
    const Transform id;
    CHECK(approx_eq(transform_point(id, Vec3{3.0f, -1.0f, 2.0f}), Vec3{3.0f, -1.0f, 2.0f}));
}

TEST_CASE("TRS applies scale, then rotation, then translation") {
    Transform tf;
    tf.scale = Vec3{2.0f, 2.0f, 2.0f};
    tf.rotation = quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi); // +90 deg about z
    tf.translation = Vec3{10.0f, 0.0f, 0.0f};
    // (1,0,0): scale -> (2,0,0); rotate 90 about z -> (0,2,0); translate -> (10,2,0).
    CHECK(approx_eq(transform_point(tf, Vec3{1.0f, 0.0f, 0.0f}), Vec3{10.0f, 2.0f, 0.0f}, kLoose));
    // A direction ignores translation: (1,0,0) -> scale -> rotate -> (0,2,0).
    CHECK(approx_eq(transform_vector(tf, Vec3{1.0f, 0.0f, 0.0f}), Vec3{0.0f, 2.0f, 0.0f}, kLoose));
}

TEST_CASE("to_matrix bakes the same mapping as the decomposed transform") {
    Transform tf;
    tf.scale = Vec3{1.5f, 0.5f, 3.0f}; // non-uniform: still exact for a single transform
    tf.rotation = quat_from_axis_angle(normalize(Vec3{1.0f, 2.0f, 3.0f}), 0.8f);
    tf.translation = Vec3{-4.0f, 7.0f, 1.0f};
    const Mat4 m = to_matrix(tf);
    const Vec3 p{2.0f, -1.0f, 0.5f};
    // The decomposed apply and the baked matrix must agree on points...
    CHECK(approx_eq(transform_point(tf, p), transform_point(m, p), kLoose));
    // ...and on directions (w = 0: rotation + scale only).
    CHECK(approx_eq(transform_vector(tf, p), transform_vector(m, p), kLoose));
}

TEST_CASE("composition nests like a scene graph") {
    Transform parent;
    parent.rotation = quat_from_axis_angle(Vec3{0.0f, 0.0f, 1.0f}, kHalfPi);
    parent.translation = Vec3{5.0f, 0.0f, 0.0f};

    Transform child;
    child.translation = Vec3{0.0f, 1.0f, 0.0f};

    const Transform world = parent * child;
    const Vec3 p{1.0f, 0.0f, 0.0f};
    // world(p) must equal applying child first, then parent.
    CHECK(approx_eq(
        transform_point(world, p), transform_point(parent, transform_point(child, p)), kLoose));
    // And it must match the baked matrix product (uniform scale here, so exact).
    CHECK(approx_eq(to_matrix(world), to_matrix(parent) * to_matrix(child), kLoose));
}

TEST_CASE("inverse undoes a transform (exact for uniform scale)") {
    Transform tf;
    tf.scale = Vec3{2.0f, 2.0f, 2.0f}; // uniform: Transform::inverse is exact
    tf.rotation = quat_from_axis_angle(normalize(Vec3{1.0f, -1.0f, 2.0f}), 1.3f);
    tf.translation = Vec3{3.0f, -5.0f, 8.0f};

    const Transform inv = inverse(tf);
    const Vec3 p{4.0f, 1.0f, -2.0f};
    CHECK(approx_eq(transform_point(inv, transform_point(tf, p)), p, kLoose));
    CHECK(approx_eq(transform_point(tf, transform_point(inv, p)), p, kLoose));
    // Composing a transform with its inverse yields the identity.
    CHECK(approx_eq(tf * inv, Transform{}, kLoose));
}

TEST_CASE("non-uniform-scale inverse is exact through the baked matrix") {
    // Transform::inverse approximates under non-uniform scale, but the Mat4 inverse is exact —
    // this documents the recommended path when scale is non-uniform.
    Transform tf;
    tf.scale = Vec3{1.0f, 2.0f, 4.0f};
    tf.rotation = quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.9f);
    tf.translation = Vec3{1.0f, 2.0f, 3.0f};
    const Mat4 m = to_matrix(tf);
    const Vec3 p{-3.0f, 5.0f, 2.0f};
    CHECK(approx_eq(transform_point(inverse(m), transform_point(m, p)), p, kLoose));
}
