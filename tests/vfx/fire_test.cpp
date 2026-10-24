// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <limits>

#include "rime/vfx/fire.hpp"

namespace {
rime::vfx::IgnitionSnapshot snapshot(std::uint64_t key, std::uint64_t tick, float intensity) {
    return {key, tick, {0.0f, 0.0f, 0.0f}, {2.0f, 2.0f, 2.0f}, intensity};
}
} // namespace

TEST_CASE("fire ignites, sustains, extinguishes and reignites on later canonical ticks") {
    rime::vfx::ParticleField field(100);
    rime::vfx::FirePresentationSource fire(2.0f);
    CHECK(fire.consume(field, snapshot(7, 10, 1.0f), 0.5f));
    REQUIRE(fire.light().has_value());
    CHECK(fire.light()->position.x == 1.0f);
    CHECK(fire.light()->radius == 2.5f);
    CHECK(field.count() == 1);
    CHECK(fire.consume(field, snapshot(7, 11, 1.0f), 0.5f));
    CHECK(field.count() == 2);
    CHECK(fire.consume(field, snapshot(7, 12, 0.0f), 0.5f));
    CHECK_FALSE(fire.light().has_value());
    CHECK(field.count() == 2); // smoke already emitted lives in the pool
    CHECK(fire.consume(field, snapshot(7, 13, 1.0f), 0.5f));
    CHECK(fire.burning());
    CHECK(fire.stats().extinguishes == 1);
}

TEST_CASE("fire rejects duplicate, stale, mismatched and invalid snapshots without losing light") {
    rime::vfx::ParticleField field(100);
    rime::vfx::FirePresentationSource fire(2.0f);
    CHECK(fire.consume(field, snapshot(9, 4, 1.0f), 0.5f));
    const auto count = field.count();
    CHECK_FALSE(fire.consume(field, snapshot(9, 4, 0.0f), 0.5f));
    CHECK_FALSE(fire.consume(field, snapshot(9, 3, 0.0f), 0.5f));
    CHECK_FALSE(fire.consume(field, snapshot(10, 5, 1.0f), 0.5f));
    auto invalid = snapshot(9, 5, std::numeric_limits<float>::quiet_NaN());
    CHECK_FALSE(fire.consume(field, invalid, 0.5f));
    invalid = snapshot(9, 5, 1.0f);
    invalid.bounds_max.x = -1.0f;
    CHECK_FALSE(fire.consume(field, invalid, 0.5f));
    CHECK(fire.burning());
    CHECK(field.count() == count);
    CHECK(fire.stats().rejected_snapshots == 5);
    CHECK(fire.stats().stale_ticks == 2);
    CHECK(fire.stats().source_cap_rejections == 1);
    CHECK(fire.consume(field, snapshot(9, 5, 1.0f), 0.5f));
    CHECK(field.count() == count + 1);
}

TEST_CASE("fire stop retires a source and reset permits deliberate key replacement") {
    rime::vfx::ParticleField field(100);
    rime::vfx::FirePresentationSource fire(2.0f);
    CHECK(fire.consume(field, snapshot(1, 1, 1.0f), 0.5f));
    CHECK_FALSE(fire.stop(1, 1));
    CHECK(fire.stop(1, 2));
    CHECK_FALSE(fire.burning());
    CHECK_FALSE(fire.consume(field, snapshot(1, 2, 1.0f), 0.5f));
    CHECK(fire.consume(field, snapshot(1, 3, 1.0f), 0.5f));
    fire.reset();
    CHECK_FALSE(fire.bound());
    CHECK(fire.consume(field, snapshot(2, 1, 1.0f), 0.5f));
    CHECK(fire.burning());
}

TEST_CASE("fire clamps presentation intensity and bounds smoke work") {
    rime::vfx::ParticleField field(100);
    rime::vfx::FirePresentationSource fire(20.0f, 2, 1);
    CHECK(fire.consume(field, snapshot(1, 1, 100.0f), 1.0f));
    REQUIRE(fire.light().has_value());
    CHECK(fire.light()->intensity == 4.0f);
    CHECK(fire.stats().intensity_clamps == 1);
    CHECK(fire.smoke_stats().emitted_particles == 2);
    CHECK(fire.smoke_stats().deferred_particles == 1);
    CHECK(fire.smoke_stats().dropped_particles == 77);
}
