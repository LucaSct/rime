// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/culling.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

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

bool shadow_caster_aabb_in_frustum(const Frustum& frustum,
                                   const core::Mat4& model,
                                   core::Vec3 local_min,
                                   core::Vec3 local_max) noexcept {
    const auto finite = [](float x) { return std::isfinite(x); };
    if (!finite(local_min.x) || !finite(local_min.y) || !finite(local_min.z) ||
        !finite(local_max.x) || !finite(local_max.y) || !finite(local_max.z) ||
        local_min.x > local_max.x || local_min.y > local_max.y || local_min.z > local_max.z)
        return true;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!finite(model.at(r, c)))
                return true;
    if (model.at(3, 0) != 0.0f || model.at(3, 1) != 0.0f || model.at(3, 2) != 0.0f ||
        model.at(3, 3) != 1.0f)
        return true;
    for (const core::Vec4& p : frustum.planes)
        if (!finite(p.x) || !finite(p.y) || !finite(p.z) || !finite(p.w) ||
            !(p.x * p.x + p.y * p.y + p.z * p.z > 0.0f))
            return true;

    core::Vec3 mn{}, mx{};
    transform_aabb(model, local_min, local_max, mn, mx);
    if (!finite(mn.x) || !finite(mn.y) || !finite(mn.z) || !finite(mx.x) || !finite(mx.y) ||
        !finite(mx.z))
        return true;

    // A world-space box formed from float matrix products can round inward by a few ulps.
    // Scale the outward guard with BOTH transformed coordinates and the model/local products:
    // large cancelling translations must not leave a tiny fixed-world epsilon.
    double error_scale = 1.0;
    for (int r = 0; r < 3; ++r) {
        double axis = std::abs(static_cast<double>(model.at(r, 3)));
        for (int c = 0; c < 3; ++c) {
            const float bound = c == 0   ? std::max(std::abs(local_min.x), std::abs(local_max.x))
                                : c == 1 ? std::max(std::abs(local_min.y), std::abs(local_max.y))
                                         : std::max(std::abs(local_min.z), std::abs(local_max.z));
            axis += std::abs(static_cast<double>(model.at(r, c))) * bound;
        }
        error_scale = std::max(error_scale, axis);
    }
    const double world_guard = 64.0 * std::numeric_limits<float>::epsilon() * error_scale;
    if (!std::isfinite(world_guard))
        return true;
    for (const core::Vec4& p : frustum.planes) {
        const double x = p.x >= 0.0f ? mx.x : mn.x;
        const double y = p.y >= 0.0f ? mx.y : mn.y;
        const double z = p.z >= 0.0f ? mx.z : mn.z;
        const double distance = static_cast<double>(p.x) * x + static_cast<double>(p.y) * y +
                                static_cast<double>(p.z) * z + p.w;
        const double plane_guard =
            64.0 * std::numeric_limits<float>::epsilon() *
            (std::abs(static_cast<double>(p.x) * x) + std::abs(static_cast<double>(p.y) * y) +
             std::abs(static_cast<double>(p.z) * z) + std::abs(static_cast<double>(p.w)) + 1.0);
        if (!std::isfinite(distance) || !std::isfinite(plane_guard))
            return true;
        if (distance < -(world_guard + plane_guard))
            return false;
    }
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
