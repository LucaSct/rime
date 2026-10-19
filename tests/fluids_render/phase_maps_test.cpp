// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
// Pure CPU tests of the cosmetic phase maps: no device, so they run everywhere.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "rime/fluids_render/phase_maps.hpp"
using namespace rime::fluids_render::detail;

namespace {
const FrostField& frost() {
    static const FrostField f = make_frost_field();
    return f;
}

double decode(std::uint8_t b) {
    return 2.0 * b / 255.0 - 1.0;
}

// Largest per-channel byte difference between two maps.
int max_diff(const Pixels& a, const Pixels& b) {
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}
} // namespace

TEST_CASE("phase maps: every normal is finite, unit length and faces outward") {
    Pixels px;
    for (const std::size_t level : {0u, 1u, 4u, 8u})
        for (const std::size_t frame : {0u, 13u, 31u}) {
            fill_normal(px, frost(), level, frame);
            for (std::size_t i = 0; i < px.size(); i += 4) {
                const double x = decode(px[i]), y = decode(px[i + 1]), z = decode(px[i + 2]);
                REQUIRE(z > .4);
                REQUIRE(std::abs(std::sqrt(x * x + y * y + z * z) - 1) < .02);
                REQUIRE(px[i + 3] == 255);
            }
        }
}

TEST_CASE("phase maps: water slope tiles in UV and loops in time exactly") {
    for (const float u : {0.f, .17f, .5f})
        for (const float v : {0.f, .31f, .9f})
            for (const float t : {0.f, .25f, .7f}) {
                const auto a = water_slope(u, v, t);
                const auto b = water_slope(u + 1, v - 1, t + 1);
                CHECK(a.x == doctest::Approx(b.x).epsilon(1e-4));
                CHECK(a.y == doctest::Approx(b.y).epsilon(1e-4));
            }
    // The loop point is as smooth as any other step: last->first frame delta is the same order as
    // an interior step (no animation jump).
    const auto step = [&](unsigned f) {
        const auto a = water_slope(.3f, .6f, float(f) / time_frames);
        const auto b = water_slope(.3f, .6f, float(f + 1) / time_frames);
        return std::hypot(a.x - b.x, a.y - b.y);
    };
    float worst = 0;
    for (unsigned f = 0; f + 1 < time_frames; ++f)
        worst = std::max(worst, step(f));
    CHECK(step(time_frames - 1) <= worst * 1.0001f);
}

TEST_CASE("phase maps: texture seams are no sharper than interior neighbours") {
    Pixels px;
    fill_normal(px, frost(), 4, 7);
    int interior = 0, seam = 0;
    for (unsigned y = 0; y < map_side; ++y) {
        const auto at = [&](unsigned x, int c) { return int(px[(y * map_side + x) * 4 + c]); };
        for (int c = 0; c < 3; ++c) {
            seam = std::max(seam, std::abs(at(map_side - 1, c) - at(0, c)));
            for (unsigned x = 0; x + 1 < map_side; ++x)
                interior = std::max(interior, std::abs(at(x, c) - at(x + 1, c)));
        }
    }
    CHECK(seam <= interior);
}

TEST_CASE("phase maps: frozen endpoint is static; water moves; no large white blobs") {
    Pixels a, b;
    fill_normal(a, frost(), 0, 0);
    fill_normal(b, frost(), 0, 19);
    CHECK(a == b); // static ice, whatever the phase
    fill_normal(a, frost(), 8, 0);
    fill_normal(b, frost(), 8, 9);
    CHECK(max_diff(a, b) > 8); // sensitivity: animated water really differs between frames
    fill_tint(a, frost(), 0);
    std::size_t bright = 0;
    for (std::size_t i = 0; i < a.size(); i += 4)
        bright += a[i] > 235 && a[i + 1] > 245;
    CHECK(bright < map_side * map_side / 10); // veins/glints only
    fill_tint(b, frost(), tint_levels - 1);
    for (std::size_t i = 0; i < b.size(); i += 4)
        REQUIRE((b[i] == 255 && b[i + 1] == 255 && b[i + 2] == 255)); // liquid: plain white
}

TEST_CASE("phase maps: epsilon-to-zero liquid selects the same maps, steps stay small") {
    const auto frozen = select_detail(.9, 0);
    for (const double eps : {1e-12, 1e-6, 1e-3, .0624}) {
        const auto near = select_detail(.9, eps);
        CHECK(near.normal_index == frozen.normal_index);
        CHECK(near.tint_index == frozen.tint_index);
    }
    CHECK(frozen.normal_index == 0); // and that one map is frame independent (above)
    CHECK(select_detail(0, 1).normal_index == 1 + 7 * time_frames);
    CHECK(select_detail(loop_seconds, 1).normal_index == select_detail(0, 1).normal_index);
    // Adjacent liquid levels differ by a bounded amount (one eighth of the water share).
    Pixels a, b;
    for (std::size_t k = 0; k < liquid_steps; ++k) {
        fill_normal(a, frost(), k, 5);
        fill_normal(b, frost(), k + 1, 5);
        CHECK(max_diff(a, b) < 60);
    }
    // Non-finite / out-of-range input degrades safely.
    CHECK(select_detail(std::nan(""), 1).normal_index < normal_count);
    CHECK(select_detail(-3, 2).tint_index == tint_levels - 1);
    CHECK(select_detail(1e300, std::nan("")).normal_index < normal_count);
}

TEST_CASE("phase maps: resource budget is bounded and documented exactly") {
    CHECK(normal_count == 257);
    CHECK(total_bytes == 266 * 87380);
    CHECK(total_bytes <= 32u * 1024 * 1024);
    std::size_t mip = 0;
    for (unsigned s = map_side, l = 0; l < map_mips; ++l, s >>= 1)
        mip += std::size_t(s) * s * 4;
    CHECK(mip == bytes_per_map);
}
