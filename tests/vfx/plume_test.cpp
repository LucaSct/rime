// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <limits>

#include "rime/vfx/plume.hpp"

namespace {
const rime::core::Vec3 kLo{0.0f, 0.0f, 0.0f};
const rime::core::Vec3 kHi{1.0f, 1.0f, 1.0f};
} // namespace

TEST_CASE("plume carries fractional rate across valid calls") {
    rime::vfx::ParticleField field(100);
    rime::vfx::PlumeEmitter plume;
    for (int i = 0; i < 4; ++i) {
        CHECK(plume.update(field, kLo, kHi, 0.25f, 3.0f, 1.0f));
    }
    CHECK(field.count() == 3);
    CHECK(plume.stats().requested_particles == 3);
    CHECK(plume.stats().emitted_particles == 3);
    CHECK(plume.stats().deferred_particles == 0);
}

TEST_CASE("invalid plume inputs preserve valid scatter and accumulated debt") {
    rime::vfx::ParticleField actual(100, 123), reference(100, 123);
    rime::vfx::PlumeEmitter plume, clean;
    CHECK(plume.update(actual, kLo, kHi, 0.5f, 3.0f, 1.0f));
    CHECK(clean.update(reference, kLo, kHi, 0.5f, 3.0f, 1.0f));
    CHECK_FALSE(plume.update(actual, kHi, kLo, 1.0f, 3.0f, 1.0f));
    CHECK_FALSE(plume.update(actual, kLo, kHi, 0.0f, 3.0f, 1.0f));
    CHECK_FALSE(plume.update(actual, kLo, kHi, 1.0f, 0.0f, 1.0f));
    CHECK_FALSE(plume.update(actual, kLo, kHi, 1.0f, 3.0f, std::numeric_limits<float>::infinity()));
    CHECK(plume.update(actual, kLo, kHi, 0.5f, 3.0f, 1.0f));
    CHECK(clean.update(reference, kLo, kHi, 0.5f, 3.0f, 1.0f));
    REQUIRE(actual.count() == reference.count());
    for (std::size_t i = 0; i < actual.count(); ++i) {
        CHECK(actual.particles()[i].position.x == reference.particles()[i].position.x);
        CHECK(actual.particles()[i].position.y == reference.particles()[i].position.y);
        CHECK(actual.particles()[i].position.z == reference.particles()[i].position.z);
    }
    CHECK(plume.stats().rejected_updates == 4);
}

TEST_CASE("plume cap conserves bounded work and reset discards debt") {
    rime::vfx::ParticleField field(100);
    rime::vfx::PlumeEmitter plume(rime::vfx::lingering_smoke(), 2, 3);
    CHECK(plume.update(field, kLo, kHi, 1.0f, 10.0f, 1.0f));
    CHECK(plume.stats().requested_particles == 10);
    CHECK(plume.stats().emitted_particles == 2);
    CHECK(plume.stats().deferred_particles == 3);
    CHECK(plume.stats().dropped_particles == 5);
    CHECK(plume.stats().capped_updates == 1);
    plume.reset();
    CHECK(plume.stats().deferred_particles == 0);
    CHECK(plume.update(field, kLo, kHi, 0.5f, 2.0f, 1.0f));
    CHECK(field.count() == 3);
    CHECK(plume.stats().requested_particles ==
          plume.stats().emitted_particles + plume.stats().dropped_particles +
              plume.stats().deferred_particles + plume.stats().discarded_on_reset);
    CHECK(plume.stats().discarded_on_reset == 3);
}

TEST_CASE("plume pool capacity is accounted by the field") {
    rime::vfx::ParticleField field(1);
    rime::vfx::PlumeEmitter plume;
    CHECK(plume.update(field, kLo, kHi, 1.0f, 4.0f, 1.0f));
    CHECK(plume.stats().emitted_particles == 4);
    CHECK(field.stats().requested_particles == 4);
    CHECK(field.stats().spawned_particles == 1);
    CHECK(field.stats().dropped_capacity == 3);
}
