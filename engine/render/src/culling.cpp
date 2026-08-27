// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/culling.hpp"

#include <algorithm>
#include <cmath>

namespace rime::render {
namespace {

// Row `r` of a clip-from-world matrix, as a plane-shaped Vec4. `Mat4::at(row, col)` hides the
// column-major storage, so this reads the way the derivation does.
[[nodiscard]] core::Vec4 row_of(const core::Mat4& m, int r) noexcept {
    return core::Vec4{m.at(r, 0), m.at(r, 1), m.at(r, 2), m.at(r, 3)};
}

[[nodiscard]] core::Vec4 add(core::Vec4 a, core::Vec4 b) noexcept {
    return core::Vec4{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}

[[nodiscard]] core::Vec4 sub(core::Vec4 a, core::Vec4 b) noexcept {
    return core::Vec4{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}

// Scale a plane so its normal is unit length, making `d` a true signed distance in world units.
// A degenerate plane (zero normal — a singular matrix) is left alone rather than divided by zero;
// it then admits everything, which is the safe direction for a cull.
[[nodiscard]] core::Vec4 normalize_plane(core::Vec4 p) noexcept {
    const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (!(len > 0.0f)) {
        return p;
    }
    const float inv = 1.0f / len;
    return core::Vec4{p.x * inv, p.y * inv, p.z * inv, p.w * inv};
}

} // namespace

Frustum frustum_from_view_proj(const core::Mat4& view_proj) noexcept {
    // Gribb–Hartmann. A point is inside clip space iff, in homogeneous coordinates,
    //     −w ≤ x ≤ w,  −w ≤ y ≤ w,  0 ≤ z ≤ w
    // and each of those six inequalities, with x = row0·v, y = row1·v, z = row2·v, w = row3·v
    // substituted in, IS a plane equation in world space. So the planes are sums and differences
    // of rows — no matrix inverse, no corner unprojection.
    const core::Vec4 r0 = row_of(view_proj, 0);
    const core::Vec4 r1 = row_of(view_proj, 1);
    const core::Vec4 r2 = row_of(view_proj, 2);
    const core::Vec4 r3 = row_of(view_proj, 3);

    Frustum f{};
    f.planes[0] = normalize_plane(add(r3, r0)); // left:   x ≥ −w
    f.planes[1] = normalize_plane(sub(r3, r0)); // right:  x ≤  w
    f.planes[2] = normalize_plane(add(r3, r1)); // bottom: y ≥ −w
    f.planes[3] = normalize_plane(sub(r3, r1)); // top:    y ≤  w
    // NEAR is r2 ALONE, not r3 + r2. Vulkan clip space runs z from 0 to w, not −w to w (ADR-0004),
    // so the inequality is `z ≥ 0` rather than `z ≥ −w`. Using the OpenGL form here does not break
    // the picture in any obvious way — it puts the near plane in the wrong place, which culls
    // correctly everywhere except directly in front of the camera, which is the one place a player
    // is guaranteed to be looking.
    f.planes[4] = normalize_plane(r2);          // near:   z ≥ 0
    f.planes[5] = normalize_plane(sub(r3, r2)); // far:    z ≤ w
    return f;
}

bool aabb_in_frustum(const Frustum& frustum, core::Vec3 min, core::Vec3 max) noexcept {
    for (const core::Vec4& p : frustum.planes) {
        // The POSITIVE VERTEX: the box corner furthest along this plane's normal. Choosing it per
        // axis by the sign of the normal is branch-light and exact — if this corner is behind the
        // plane then all eight are, and the box is entirely outside.
        const core::Vec3 positive{
            p.x >= 0.0f ? max.x : min.x, p.y >= 0.0f ? max.y : min.y, p.z >= 0.0f ? max.z : min.z};
        if (p.x * positive.x + p.y * positive.y + p.z * positive.z + p.w < 0.0f) {
            return false;
        }
    }
    // Not proven outside. That is deliberately weaker than "proven inside": a box straddling two
    // planes' outer regions without touching the frustum survives this test. It costs a draw and
    // never costs a pixel (culling.hpp on why that is the only acceptable direction to be wrong).
    return true;
}

void transform_aabb(const core::Mat4& model,
                    core::Vec3 local_min,
                    core::Vec3 local_max,
                    core::Vec3& out_min,
                    core::Vec3& out_max) noexcept {
    bool first = true;
    for (int corner = 0; corner < 8; ++corner) {
        // The eight corners, enumerated by the bits of `corner`: bit 0 picks x, bit 1 y, bit 2 z.
        const core::Vec3 local{(corner & 1) != 0 ? local_max.x : local_min.x,
                               (corner & 2) != 0 ? local_max.y : local_min.y,
                               (corner & 4) != 0 ? local_max.z : local_min.z};
        const core::Vec4 world = model * core::Vec4{local.x, local.y, local.z, 1.0f};
        const core::Vec3 p{world.x, world.y, world.z};
        if (first) {
            out_min = p;
            out_max = p;
            first = false;
            continue;
        }
        out_min.x = std::min(out_min.x, p.x);
        out_min.y = std::min(out_min.y, p.y);
        out_min.z = std::min(out_min.z, p.z);
        out_max.x = std::max(out_max.x, p.x);
        out_max.y = std::max(out_max.y, p.y);
        out_max.z = std::max(out_max.z, p.z);
    }
}

} // namespace rime::render
