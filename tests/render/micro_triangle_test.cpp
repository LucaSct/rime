// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <doctest/doctest.h>

#include "rime/render/micro_triangle.hpp"

using namespace rime;

TEST_CASE("micro triangle classification preserves explicit hardware/software boundaries (M18)") {
    const render::MicroTrianglePolicy policy{.hardware_area_threshold_px2 = 1.0f};

    SUBCASE("threshold equality is hardware") {
        const auto result = render::classify_micro_triangle(
            {.x0 = 0.0f, .y0 = 0.0f, .x1 = 2.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 1.0f}, policy);
        CHECK(result.path == render::MicroTrianglePath::Hardware);
        CHECK(result.signed_area_px2 == doctest::Approx(1.0f));
        CHECK(result.absolute_area_px2 == doctest::Approx(1.0f));
    }

    SUBCASE("winding changes the signed area but not the path") {
        const auto result = render::classify_micro_triangle(
            {.x0 = 0.0f, .y0 = 0.0f, .x1 = 0.0f, .y1 = 1.0f, .x2 = 2.0f, .y2 = 0.0f}, policy);
        CHECK(result.path == render::MicroTrianglePath::Hardware);
        CHECK(result.signed_area_px2 == doctest::Approx(-1.0f));
        CHECK(result.absolute_area_px2 == doctest::Approx(1.0f));
    }

    SUBCASE("near-subpixel area uses the software detail path") {
        const auto result = render::classify_micro_triangle(
            {.x0 = 0.0f, .y0 = 0.0f, .x1 = 0.5f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 0.5f}, policy);
        CHECK(result.path == render::MicroTrianglePath::Software);
        CHECK(result.absolute_area_px2 == doctest::Approx(0.125f));
    }

    SUBCASE("degenerate triangles remain explicit software candidates") {
        const auto result = render::classify_micro_triangle(
            {.x0 = 1.0f, .y0 = 1.0f, .x1 = 2.0f, .y1 = 2.0f, .x2 = 3.0f, .y2 = 3.0f},
            {.hardware_area_threshold_px2 = 0.0f});
        CHECK(result.path == render::MicroTrianglePath::Software);
        CHECK(result.absolute_area_px2 == 0.0f);
    }

    SUBCASE("nonfinite coordinates and policy are invalid") {
        CHECK(render::classify_micro_triangle(
                  {.x0 = NAN, .y0 = 0.0f, .x1 = 1.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 1.0f}, policy)
                  .path == render::MicroTrianglePath::InvalidInput);
        CHECK(render::classify_micro_triangle(
                  {.x0 = 0.0f, .y0 = 0.0f, .x1 = INFINITY, .y1 = 0.0f, .x2 = 0.0f, .y2 = 1.0f},
                  policy)
                  .path == render::MicroTrianglePath::InvalidInput);
        CHECK(render::classify_micro_triangle(
                  {.x0 = 0.0f, .y0 = 0.0f, .x1 = 1.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 1.0f},
                  {.hardware_area_threshold_px2 = -1.0f})
                  .path == render::MicroTrianglePath::InvalidInput);
    }
}
