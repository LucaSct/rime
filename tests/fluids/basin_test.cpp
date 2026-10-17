// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "rime/fluids/basin.hpp"

using namespace rime::fluids;

namespace {

BasinConfig config(std::size_t width = 8, std::size_t height = 8) {
    BasinConfig c{};
    c.width = width;
    c.height = height;
    c.initial_depth_metres = 1.0;
    c.bed_height_metres = -2.0;
    c.origin_x_metres = 3.0;
    c.origin_z_metres = -4.0;
    return c;
}

} // namespace

TEST_CASE("closed flat lake remains at rest") {
    auto basin = Basin::create(config());
    REQUIRE(basin);
    const double initial = basin->total_volume_m3();
    for (int i = 0; i < 100; ++i)
        REQUIRE(basin->advance(0.01) == BasinStepStatus::Accepted);
    CHECK(basin->tick() == 100);
    CHECK(basin->total_volume_m3() == initial);
    for (std::size_t z = 0; z < 8; ++z) {
        for (std::size_t x = 0; x < 8; ++x) {
            const auto cell = basin->cell(x, z);
            REQUIRE(cell);
            CHECK(cell->depth_metres == 1.0);
            CHECK(cell->momentum_x == 0.0);
            CHECK(cell->momentum_z == 0.0);
        }
    }
}

TEST_CASE("pulse evolves while zero dt does not; closed mass survives 10000 steps") {
    auto basin = Basin::create(config());
    REQUIRE(basin);
    REQUIRE(basin->set_cell(4, 4, {1.25, 0.0, 0.0}));
    const double initial_volume = basin->total_volume_m3();
    REQUIRE(basin->advance(0.0) == BasinStepStatus::Accepted);
    CHECK(basin->tick() == 0);
    CHECK(basin->cell(4, 4)->depth_metres == 1.25);
    REQUIRE(basin->advance(0.01) == BasinStepStatus::Accepted);
    CHECK(basin->cell(4, 4)->depth_metres < 1.25);
    CHECK(basin->cell(3, 4)->depth_metres > 1.0);
    for (int i = 1; i < 10000; ++i)
        REQUIRE(basin->advance(0.01) == BasinStepStatus::Accepted);
    CHECK(std::abs(basin->total_volume_m3() - initial_volume) < 1.0e-10 * initial_volume);
}

TEST_CASE("dry shore wets without negative depth or lost volume") {
    auto c = config(7, 1);
    c.initial_depth_metres = 0.0;
    auto basin = Basin::create(c);
    REQUIRE(basin);
    REQUIRE(basin->set_cell(1, 0, {1.0, 0.0, 0.0}));
    const double initial_volume = basin->total_volume_m3();
    for (int i = 0; i < 300; ++i) {
        REQUIRE(basin->advance(0.005) == BasinStepStatus::Accepted);
        for (std::size_t x = 0; x < 7; ++x) {
            const auto cell = basin->cell(x, 0);
            REQUIRE(cell);
            CHECK(std::isfinite(cell->depth_metres));
            CHECK(cell->depth_metres >= 0.0);
        }
    }
    CHECK(basin->cell(0, 0)->depth_metres > 0.0);
    CHECK(std::abs(basin->total_volume_m3() - initial_volume) < 1.0e-11);
}

TEST_CASE("configuration, state, and dt refusals preserve snapshots") {
    auto c = config();
    c.width = 257;
    c.height = 257;
    CHECK_FALSE(Basin::create(c));
    c = config();
    c.cell_spacing_metres = 0.0;
    CHECK_FALSE(Basin::create(c));
    c = config();
    c.cfl = 0.75;
    CHECK_FALSE(Basin::create(c));
    c = config();
    c.max_substeps = 0;
    CHECK_FALSE(Basin::create(c));
    c = config();
    c.max_substeps = 1;
    auto basin = Basin::create(c);
    REQUIRE(basin);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(basin->set_cell(0, 0, {-1.0, 0.0, 0.0}));
    CHECK_FALSE(basin->set_cell(0, 0, {0.0, 1.0, 0.0}));
    CHECK_FALSE(basin->set_cell(0, 0, {nan, 0.0, 0.0}));
    CHECK_FALSE(basin->set_cell(8, 0, {1.0, 0.0, 0.0}));
    REQUIRE(basin->set_cell(4, 4, {1.5, 0.0, 0.0}));
    const auto before = basin->cell(4, 4);
    const auto neighbor = basin->cell(3, 4);
    const double volume = basin->total_volume_m3();
    CHECK(basin->advance(nan) == BasinStepStatus::InvalidDt);
    CHECK(basin->advance(-0.01) == BasinStepStatus::InvalidDt);
    CHECK(basin->advance(1.0) == BasinStepStatus::SubstepBudgetExceeded);
    CHECK(basin->refused_advances() == 3);
    CHECK(basin->tick() == 0);
    CHECK(basin->last_substeps() == 0);
    CHECK(basin->total_volume_m3() == volume);
    CHECK(basin->cell(4, 4)->depth_metres == before->depth_metres);
    CHECK(basin->cell(3, 4)->depth_metres == neighbor->depth_metres);
}

TEST_CASE("surface sample carries cell height, flow, density, and accepted tick") {
    auto basin = Basin::create(config(2, 1));
    REQUIRE(basin);
    REQUIRE(basin->set_cell(0, 0, {2.0, 1.0, -0.5}));
    REQUIRE(basin->set_cell(1, 0, {0.0, 0.0, 0.0}));
    const auto wet = basin->sample(3.5, -3.5);
    REQUIRE(wet);
    CHECK(wet->height_metres == 0.0f);
    CHECK(wet->velocity_m_s.x == 0.5f);
    CHECK(wet->velocity_m_s.z == -0.25f);
    CHECK(wet->density_kg_m3 == 1000.0f);
    CHECK(wet->authoritative_tick == 0);
    const auto dry = basin->sample(4.5, -3.5);
    REQUIRE(dry);
    CHECK(dry->height_metres == -2.0f);
    CHECK(dry->velocity_m_s.x == 0.0f);
    CHECK_FALSE(basin->sample(5.0, -3.5));
    CHECK_FALSE(basin->sample(3.5, -4.1));
    REQUIRE(basin->advance(0.01) == BasinStepStatus::Accepted);
    CHECK(basin->sample(3.5, -3.5)->authoritative_tick == 1);
}
