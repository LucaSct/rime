// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

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

TEST_CASE("micro triangle reference rasterizer is a deterministic visibility oracle (M18)") {
    std::array<std::uint32_t, 16> ids{};
    std::array<float, 16> depths{};
    depths.fill(1.0f);
    render::MicroTriangleRasterTarget target{ids, depths, 4, 4};

    // M18.4: the legs were 1.0, which put the (0.5, 0.5) centre EXACTLY on the hypotenuse — a
    // bottom-right edge that the corrected top-left rule does not own (the pre-M18.4 mirrored rule
    // did, which is what this case silently pinned). 1.25 keeps the sample strictly inside, so the
    // case tests what its name says; the edge itself is pinned by the fill-rule test below.
    SUBCASE("subpixel coverage uses pixel centers") {
        CHECK(render::rasterize_micro_triangle_reference({.x0 = 0.0f,
                                                          .y0 = 0.0f,
                                                          .x1 = 1.25f,
                                                          .y1 = 0.0f,
                                                          .x2 = 0.0f,
                                                          .y2 = 1.25f,
                                                          .z0 = 0.25f,
                                                          .z1 = 0.25f,
                                                          .z2 = 0.25f},
                                                         7,
                                                         target));
        CHECK(ids[0] == 7);
        CHECK(ids[1] == 0);
    }

    SUBCASE("nearer depth wins and an equal depth retains the existing id") {
        const render::ProjectedTriangle tri{.x0 = 0.0f,
                                            .y0 = 0.0f,
                                            .x1 = 4.0f,
                                            .y1 = 0.0f,
                                            .x2 = 0.0f,
                                            .y2 = 4.0f,
                                            .z0 = 0.5f,
                                            .z1 = 0.5f,
                                            .z2 = 0.5f};
        CHECK(render::rasterize_micro_triangle_reference(tri, 9, target));
        CHECK(render::rasterize_micro_triangle_reference(tri, 8, target));
        CHECK(ids[0] == 9);
    }

    SUBCASE("viewport boundary is clipped") {
        CHECK(render::rasterize_micro_triangle_reference({.x0 = -2.0f,
                                                          .y0 = -2.0f,
                                                          .x1 = 4.0f,
                                                          .y1 = -2.0f,
                                                          .x2 = -2.0f,
                                                          .y2 = 4.0f,
                                                          .z0 = 0.2f,
                                                          .z1 = 0.2f,
                                                          .z2 = 0.2f},
                                                         3,
                                                         target));
        CHECK(ids[0] == 3);
    }

    SUBCASE("invalid input preserves both buffers") {
        ids[0] = 11;
        depths[0] = 0.2f;
        const auto before_ids = ids;
        const auto before_depths = depths;
        CHECK_FALSE(render::rasterize_micro_triangle_reference(
            {.x0 = NAN, .y0 = 0.0f, .x1 = 1.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 1.0f}, 4, target));
        CHECK(ids == before_ids);
        CHECK(depths == before_depths);
        CHECK_FALSE(render::rasterize_micro_triangle_reference({}, 0, target));
        CHECK(ids == before_ids);
        CHECK(depths == before_depths);
    }
}

// M18.4: the fill rule's DIRECTION, pinned. y points down (row 0 is the top), and the owned edges
// are the left ones and a flat top — the convention the hardware rasterizer measurably uses (see
// virtual_geometry_micro_raster_test.cpp (b)). Samples sit exactly on the edges, so any other
// convention moves at least one of these assertions.
TEST_CASE("micro triangle reference rasterizer owns left and top edges, y down (M18.4)") {
    std::array<std::uint32_t, 25> ids{};
    std::array<float, 25> depths{};
    depths.fill(1.0f);
    const render::MicroTriangleRasterTarget target{ids, depths, 5, 5};
    const auto id_at = [&](std::size_t x, std::size_t y) { return ids[y * 5 + x]; };

    SUBCASE("the upper-left half owns its left and top edges, not its diagonal") {
        // Left edge x = 1.5, top edge y = 1.5, diagonal through the sample (2.5, 2.5).
        CHECK(render::rasterize_micro_triangle_reference(
            {1.5f, 1.5f, 1.5f, 3.5f, 3.5f, 1.5f, 0.5f, 0.5f, 0.5f}, 7, target));
        CHECK(id_at(1, 1) == 7); // the corner: on both owned edges
        CHECK(id_at(1, 2) == 7); // on the left edge
        CHECK(id_at(2, 1) == 7); // on the top edge
        CHECK(id_at(2, 2) == 0); // on the diagonal: its bottom-right side
    }

    SUBCASE("the lower-right half owns the diagonal, not its right and bottom edges") {
        CHECK(render::rasterize_micro_triangle_reference(
            {3.5f, 3.5f, 3.5f, 1.5f, 1.5f, 3.5f, 0.5f, 0.5f, 0.5f}, 9, target));
        CHECK(id_at(2, 2) == 9); // the diagonal is this triangle's top-left side
        CHECK(id_at(3, 2) == 0); // on the right edge
        CHECK(id_at(2, 3) == 0); // on the bottom edge
        CHECK(id_at(3, 3) == 0);
    }

    SUBCASE("the two halves tile the square: every sample exactly once") {
        CHECK(render::rasterize_micro_triangle_reference(
            {1.5f, 1.5f, 1.5f, 3.5f, 3.5f, 1.5f, 0.5f, 0.5f, 0.5f}, 7, target));
        CHECK(render::rasterize_micro_triangle_reference(
            {3.5f, 3.5f, 3.5f, 1.5f, 1.5f, 3.5f, 0.5f, 0.5f, 0.5f}, 9, target));
        std::size_t sevens = 0, nines = 0;
        for (const std::uint32_t id : ids) {
            sevens += id == 7 ? 1u : 0u;
            nines += id == 9 ? 1u : 0u;
        }
        CHECK(sevens == 3);
        CHECK(nines == 1);
    }
}

TEST_CASE("micro triangle routing sends only walkable software triangles to software (M18.4)") {
    SUBCASE("a small sub-pixel triangle is software") {
        const auto r = render::route_micro_triangle(
            {.x0 = 0.0f, .y0 = 0.0f, .x1 = 0.5f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 0.5f});
        CHECK(r.path == render::MicroTrianglePath::Software);
        CHECK_FALSE(r.rerouted_oversized);
    }
    SUBCASE("a long sub-pixel-area sliver is rerouted to hardware, not dropped") {
        const auto r = render::route_micro_triangle(
            {.x0 = 0.0f, .y0 = 0.0f, .x1 = 40.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 0.02f});
        CHECK(render::classify_micro_triangle(
                  {.x0 = 0.0f, .y0 = 0.0f, .x1 = 40.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 0.02f})
                  .path == render::MicroTrianglePath::Software);
        CHECK(r.path == render::MicroTrianglePath::Hardware);
        CHECK(r.rerouted_oversized);
    }
    SUBCASE("the cap is inclusive: an extent exactly at it stays software") {
        const auto r = render::route_micro_triangle(
            {.x0 = 0.0f, .y0 = 0.0f, .x1 = 16.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 0.03125f});
        CHECK(r.path == render::MicroTrianglePath::Software);
    }
    SUBCASE("large triangles are hardware and invalid ones stay invalid") {
        CHECK(render::route_micro_triangle(
                  {.x0 = 0.0f, .y0 = 0.0f, .x1 = 4.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 4.0f})
                  .path == render::MicroTrianglePath::Hardware);
        CHECK(render::route_micro_triangle(
                  {.x0 = NAN, .y0 = 0.0f, .x1 = 4.0f, .y1 = 0.0f, .x2 = 0.0f, .y2 = 4.0f})
                  .path == render::MicroTrianglePath::InvalidInput);
    }
}

// The GPU twin spells the oracle's epsilon as a bit pattern (vg_micro_raster.comp kEpsilonBits);
// this is what keeps the two spellings of 1.0e-6f the same number.
TEST_CASE("micro triangle epsilon bit pattern matches the shader's (M18.4)") {
    CHECK(std::bit_cast<std::uint32_t>(1.0e-6f) == 0x358637BDu);
}
