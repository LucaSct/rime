// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"

// A Transform is a decomposed translate-rotate-scale (TRS): the human-readable form of a rigid-
// plus-scale placement in the world. The renderer/GPU wants a Mat4 (to_matrix), but a scene
// graph wants to STORE and EDIT placement as separate translation/rotation/scale — quaternions
// don't drift, scale stays inspectable, and "move this 2 units right" is one field. So Transform
// is the authoring/runtime representation and Mat4 is what it bakes down to. A point maps as
//     p' = translation + rotation * (scale (component-wise) p),
// i.e. scale first, then rotate, then translate (the order baked into to_matrix as T * R * S).
namespace rime::core {

struct Transform {
    Vec3 translation{0.0f, 0.0f, 0.0f};
    Quat rotation = quat_identity();
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

// Component-wise (Hadamard) product — applying a non-uniform scale to a vector.
[[nodiscard]] inline constexpr Vec3 mul_per_component(Vec3 a, Vec3 b) noexcept {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

// Apply to a point (scale, rotate, translate) and to a direction (scale, rotate — no translate).
[[nodiscard]] inline Vec3 transform_point(const Transform& tf, Vec3 p) noexcept {
    return tf.translation + rotate(tf.rotation, mul_per_component(tf.scale, p));
}

[[nodiscard]] inline Vec3 transform_vector(const Transform& tf, Vec3 d) noexcept {
    return rotate(tf.rotation, mul_per_component(tf.scale, d));
}

// Compose two transforms: (parent * child) is the child expressed in the parent's frame — the
// scene-graph "world = parent_world * local" operation. Derived by substituting the child's map
// into the parent's; it is EXACT for uniform scale and a close approximation under non-uniform
// scale + rotation (which can introduce shear a single TRS cannot represent — bake to Mat4 when
// that matters; see the derivation note). Order matches matrix multiply: child applied first.
[[nodiscard]] inline Transform operator*(const Transform& parent, const Transform& child) noexcept {
    Transform r;
    r.scale = mul_per_component(parent.scale, child.scale);
    r.rotation = parent.rotation * child.rotation;
    r.translation = transform_point(parent, child.translation);
    return r;
}

// Inverse placement: undoes the transform. Exact for uniform scale (for non-uniform scale the
// true inverse is a scale-after-rotation, which is not a TRS — bake to Mat4 and use inverse()).
[[nodiscard]] inline Transform inverse(const Transform& tf) noexcept {
    Transform r;
    r.scale = Vec3{1.0f / tf.scale.x, 1.0f / tf.scale.y, 1.0f / tf.scale.z};
    r.rotation = conjugate(tf.rotation); // unit-quaternion inverse
    r.translation = mul_per_component(r.scale, rotate(r.rotation, -tf.translation));
    return r;
}

[[nodiscard]] inline bool
approx_eq(const Transform& a, const Transform& b, float eps = kEpsilon) noexcept {
    return approx_eq(a.translation, b.translation, eps) && approx_eq(a.rotation, b.rotation, eps) &&
           approx_eq(a.scale, b.scale, eps);
}

// Bake to a Mat4 = T * R * S for upload to the GPU / use with the rest of the matrix pipeline.
// (Defined in transform.cpp, where the Mat4 product lives.)
[[nodiscard]] Mat4 to_matrix(const Transform& tf) noexcept;

} // namespace rime::core
