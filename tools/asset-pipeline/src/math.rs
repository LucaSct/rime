// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

//! Just enough matrix math to flatten a glTF node hierarchy into world-space vertices (M6.2 does not
//! keep a runtime scene graph from glTF yet). Matrices are 4x4, stored **column-major** in a
//! `[f32; 16]` — the same convention glTF uses for `node.transform().matrix()`, so flattening one is
//! a direct copy. Kept tiny and dependency-free (VISION: teach from the code) rather than pulling a
//! linear-algebra crate for a dozen operations.

/// A 4x4 matrix, column-major: element (row, col) is at index `col * 4 + row`.
pub type Mat4 = [f32; 16];

/// The identity transform.
pub const IDENTITY: Mat4 = [
    1.0, 0.0, 0.0, 0.0, //
    0.0, 1.0, 0.0, 0.0, //
    0.0, 0.0, 1.0, 0.0, //
    0.0, 0.0, 0.0, 1.0,
];

/// Flatten glTF's `[[f32; 4]; 4]` (an array of columns) into our column-major `Mat4`.
pub fn from_columns(cols: [[f32; 4]; 4]) -> Mat4 {
    let mut m = [0.0f32; 16];
    for (col, column) in cols.iter().enumerate() {
        for (row, &value) in column.iter().enumerate() {
            m[col * 4 + row] = value;
        }
    }
    m
}

/// Matrix product `a * b` (both column-major), i.e. apply `b` then `a`.
pub fn mul(a: &Mat4, b: &Mat4) -> Mat4 {
    let mut r = [0.0f32; 16];
    for col in 0..4 {
        for row in 0..4 {
            let mut sum = 0.0;
            for k in 0..4 {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            r[col * 4 + row] = sum;
        }
    }
    r
}

/// Transform a position by `m` (treated as a point: the translation column applies).
pub fn transform_point(m: &Mat4, p: [f32; 3]) -> [f32; 3] {
    [
        m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
        m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
        m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14],
    ]
}

/// Transform a normal by `m`, returning a unit vector.
///
/// Normals are covectors, so they transform by the **inverse-transpose** of the upper-left 3x3, not
/// the matrix itself — otherwise a non-uniform scale would shear them off the surface. We build that
/// from the cofactor matrix (`cofactor(A) = det(A) * (A^-1)^T`) and divide by the determinant, which
/// also flips the normal correctly for a mirroring (negative-determinant) transform. A singular 3x3
/// falls back to the plain linear part. The result is renormalized because scaling changes length.
pub fn transform_normal(m: &Mat4, n: [f32; 3]) -> [f32; 3] {
    // Upper-left 3x3 as scalars a..i (row-major for readability).
    let (a, b, c) = (m[0], m[4], m[8]);
    let (d, e, f) = (m[1], m[5], m[9]);
    let (g, h, i) = (m[2], m[6], m[10]);

    let cofactor = [
        e * i - f * h,
        -(d * i - f * g),
        d * h - e * g, // row 0
        -(b * i - c * h),
        a * i - c * g,
        -(a * h - b * g), // row 1
        b * f - c * e,
        -(a * f - c * d),
        a * e - b * d, // row 2
    ];
    let det = a * cofactor[0] + b * cofactor[1] + c * cofactor[2];

    let transformed = if det.abs() > 1.0e-12 {
        let inv_det = 1.0 / det;
        [
            (cofactor[0] * n[0] + cofactor[1] * n[1] + cofactor[2] * n[2]) * inv_det,
            (cofactor[3] * n[0] + cofactor[4] * n[1] + cofactor[5] * n[2]) * inv_det,
            (cofactor[6] * n[0] + cofactor[7] * n[1] + cofactor[8] * n[2]) * inv_det,
        ]
    } else {
        [
            a * n[0] + b * n[1] + c * n[2],
            d * n[0] + e * n[1] + f * n[2],
            g * n[0] + h * n[1] + i * n[2],
        ]
    };
    normalize(transformed)
}

/// Normalize a 3-vector; returns it unchanged if it is (near) zero-length.
pub fn normalize(v: [f32; 3]) -> [f32; 3] {
    let len = (v[0] * v[0] + v[1] * v[1] + v[2] * v[2]).sqrt();
    if len > 0.0 {
        [v[0] / len, v[1] / len, v[2] / len]
    } else {
        v
    }
}

/// Cross product, used to derive a triangle's geometric normal.
pub fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identity_is_a_no_op() {
        assert_eq!(transform_point(&IDENTITY, [1.0, 2.0, 3.0]), [1.0, 2.0, 3.0]);
        assert_eq!(
            transform_normal(&IDENTITY, [0.0, 0.0, 1.0]),
            [0.0, 0.0, 1.0]
        );
    }

    #[test]
    fn translation_moves_points_but_not_normals() {
        let mut t = IDENTITY;
        t[12] = 1.0;
        t[13] = 2.0;
        t[14] = 3.0;
        assert_eq!(transform_point(&t, [0.0, 0.0, 0.0]), [1.0, 2.0, 3.0]);
        assert_eq!(transform_normal(&t, [0.0, 0.0, 1.0]), [0.0, 0.0, 1.0]);
    }

    #[test]
    fn non_uniform_scale_uses_the_inverse_transpose_for_normals() {
        // Scale x by 2. A normal along +x must stay unit +x (inverse-transpose = diag(1/2,1,1),
        // then renormalized), NOT be stretched — the classic bug the inverse-transpose prevents.
        let mut s = IDENTITY;
        s[0] = 2.0;
        assert_eq!(transform_point(&s, [1.0, 0.0, 0.0]), [2.0, 0.0, 0.0]);
        let n = transform_normal(&s, [1.0, 0.0, 0.0]);
        assert!((n[0] - 1.0).abs() < 1.0e-6 && n[1].abs() < 1.0e-6 && n[2].abs() < 1.0e-6);
    }
}
