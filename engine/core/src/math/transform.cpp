// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/math/transform.hpp"

// Baking a decomposed TRS down to a single matrix. Kept here (not inline) because it leans on
// the Mat4 product, which lives in mat.cpp.
namespace rime::core {

Mat4 to_matrix(const Transform& tf) noexcept {
    // M = T * R * S. Right-to-left: a vertex is scaled, then rotated, then translated — the same
    // order transform_point applies, so the matrix and the decomposed form agree exactly.
    return mat4_translation(tf.translation) * to_mat4(tf.rotation) * mat4_scaling(tf.scale);
}

} // namespace rime::core
