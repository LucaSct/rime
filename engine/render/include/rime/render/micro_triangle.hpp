// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cmath>

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
};

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
