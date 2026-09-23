// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace rime::render {

// Projected coordinates are in framebuffer pixels. Keeping this contract independent of the
// camera and vertex formats lets the CPU oracle and a future GPU classifier share the exact edge
// semantics. A triangle is never silently discarded: even a degenerate one is reported as a
// software candidate so the detailed-shape path owns the decision about how to retain it.
struct ProjectedTriangle {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    // Depth in the target's normalized [0, 1] range. The reference path uses affine
    // interpolation in screen space; a future GPU path may replace this with 1/w interpolation.
    float z0 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;
};

inline constexpr std::uint32_t kInvalidMicroTriangleId = 0u;

struct MicroTriangleRasterTarget {
    std::span<std::uint32_t> identity;
    std::span<float> depth;
    std::size_t width = 0;
    std::size_t height = 0;
};

// Rasterizes one projected triangle into caller-owned identity/depth arrays. Pixel centers are
// (x+0.5,y+0.5); the conventional top-left rule resolves edge ties, and a depth tie retains the
// existing pixel. This is deliberately a small allocation-free CPU oracle for the future GPU
// micro-triangle shader, not a production fallback. Invalid input returns false without touching
// either buffer.
[[nodiscard]] inline bool
rasterize_micro_triangle_reference(const ProjectedTriangle& triangle,
                                   std::uint32_t visibility_id,
                                   MicroTriangleRasterTarget target) noexcept {
    if (visibility_id == kInvalidMicroTriangleId || target.width == 0 || target.height == 0 ||
        target.width > (static_cast<std::size_t>(-1) / target.height) ||
        target.identity.size() != target.width * target.height ||
        target.depth.size() != target.width * target.height) {
        return false;
    }
    const bool finite =
        std::isfinite(triangle.x0) && std::isfinite(triangle.y0) && std::isfinite(triangle.x1) &&
        std::isfinite(triangle.y1) && std::isfinite(triangle.x2) && std::isfinite(triangle.y2) &&
        std::isfinite(triangle.z0) && std::isfinite(triangle.z1) && std::isfinite(triangle.z2);
    if (!finite || triangle.z0 < 0.0f || triangle.z0 > 1.0f || triangle.z1 < 0.0f ||
        triangle.z1 > 1.0f || triangle.z2 < 0.0f || triangle.z2 > 1.0f) {
        return false;
    }
    for (const float existing_depth : target.depth) {
        if (!std::isfinite(existing_depth)) {
            return false;
        }
    }

    struct Vertex {
        float x, y, z;
    } v[3] = {{triangle.x0, triangle.y0, triangle.z0},
              {triangle.x1, triangle.y1, triangle.z1},
              {triangle.x2, triangle.y2, triangle.z2}};

    float area = (v[1].x - v[0].x) * (v[2].y - v[0].y) - (v[1].y - v[0].y) * (v[2].x - v[0].x);
    if (!std::isfinite(area) || area <= 0.0f) {
        if (area < 0.0f) {
            const Vertex tmp = v[1];
            v[1] = v[2];
            v[2] = tmp;
            area = -area;
        }
    }
    if (!(area > 0.0f) || !std::isfinite(area)) {
        return false;
    }

    const float min_x = std::fmin(v[0].x, std::fmin(v[1].x, v[2].x));
    const float max_x = std::fmax(v[0].x, std::fmax(v[1].x, v[2].x));
    const float min_y = std::fmin(v[0].y, std::fmin(v[1].y, v[2].y));
    const float max_y = std::fmax(v[0].y, std::fmax(v[1].y, v[2].y));
    const float viewport_max_x = static_cast<float>(target.width - 1);
    const float viewport_max_y = static_cast<float>(target.height - 1);
    const auto first_x = static_cast<std::ptrdiff_t>(std::ceil(std::fmax(min_x - 0.5f, 0.0f)));
    const auto last_x =
        static_cast<std::ptrdiff_t>(std::floor(std::fmin(max_x - 0.5f, viewport_max_x)));
    const auto first_y = static_cast<std::ptrdiff_t>(std::ceil(std::fmax(min_y - 0.5f, 0.0f)));
    const auto last_y =
        static_cast<std::ptrdiff_t>(std::floor(std::fmin(max_y - 0.5f, viewport_max_y)));
    const auto edge = [](const Vertex& a, const Vertex& b, float x, float y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    };
    const auto top_left = [](const Vertex& a, const Vertex& b) {
        return b.y > a.y || (b.y == a.y && b.x < a.x);
    };
    const bool tl0 = top_left(v[1], v[2]);
    const bool tl1 = top_left(v[2], v[0]);
    const bool tl2 = top_left(v[0], v[1]);
    const float epsilon = 1.0e-6f;
    const auto inside = [epsilon](float value, bool inclusive) {
        return value > epsilon || (inclusive && std::fabs(value) <= epsilon);
    };
    const auto x_begin = first_x;
    const auto x_end = last_x;
    const auto y_begin = first_y;
    const auto y_end = last_y;
    if (x_begin > x_end || y_begin > y_end) {
        return true;
    }
    for (auto y = y_begin; y <= y_end; ++y) {
        for (auto x = x_begin; x <= x_end; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const float e0 = edge(v[1], v[2], px, py);
            const float e1 = edge(v[2], v[0], px, py);
            const float e2 = edge(v[0], v[1], px, py);
            if (!inside(e0, tl0) || !inside(e1, tl1) || !inside(e2, tl2)) {
                continue;
            }
            const float inv_area = 1.0f / area;
            const float depth = (e0 * v[0].z + e1 * v[1].z + e2 * v[2].z) * inv_area;
            if (!std::isfinite(depth)) {
                return false;
            }
            const auto index =
                static_cast<std::size_t>(y) * target.width + static_cast<std::size_t>(x);
            if (depth < target.depth[index]) {
                target.depth[index] = depth;
                target.identity[index] = visibility_id;
            }
        }
    }
    return true;
}

struct MicroTrianglePolicy {
    // A finite, non-negative area in square pixels. The equality boundary is hardware: triangles
    // with area exactly at the threshold retain the conventional raster path.
    float hardware_area_threshold_px2 = 1.0f;
};

enum class MicroTrianglePath {
    Hardware,
    Software,
    InvalidInput,
};

struct MicroTriangleClassification {
    MicroTrianglePath path = MicroTrianglePath::InvalidInput;
    float signed_area_px2 = 0.0f;
    float absolute_area_px2 = 0.0f;
};

[[nodiscard]] inline MicroTriangleClassification
classify_micro_triangle(const ProjectedTriangle& triangle,
                        const MicroTrianglePolicy& policy = {}) noexcept {
    const bool finite = std::isfinite(triangle.x0) && std::isfinite(triangle.y0) &&
                        std::isfinite(triangle.x1) && std::isfinite(triangle.y1) &&
                        std::isfinite(triangle.x2) && std::isfinite(triangle.y2) &&
                        std::isfinite(policy.hardware_area_threshold_px2) &&
                        policy.hardware_area_threshold_px2 >= 0.0f;
    if (!finite) {
        return {};
    }

    const float signed_area_px2 =
        0.5f * ((triangle.x1 - triangle.x0) * (triangle.y2 - triangle.y0) -
                (triangle.y1 - triangle.y0) * (triangle.x2 - triangle.x0));
    if (!std::isfinite(signed_area_px2)) {
        return {};
    }

    const float absolute_area_px2 = std::fabs(signed_area_px2);
    if (!std::isfinite(absolute_area_px2)) {
        return {};
    }

    MicroTriangleClassification result{
        MicroTrianglePath::Software, signed_area_px2, absolute_area_px2};
    // Degenerate triangles stay on the software side even when the configured threshold is zero;
    // this makes the zero-area edge explicit and avoids depending on backend fill rules.
    if (absolute_area_px2 > 0.0f && absolute_area_px2 >= policy.hardware_area_threshold_px2) {
        result.path = MicroTrianglePath::Hardware;
    }
    return result;
}

} // namespace rime::render
