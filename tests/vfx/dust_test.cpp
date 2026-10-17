// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <limits>

#include "rime/vfx/dust.hpp"

// M8.4 proofs for the dust STUB: it is GPU-free (the actual additive draw + coverage-delta PIXEL
// proof land with the M8.6 sample), so what we prove here is the SIMULATION — a burst blooms
// coverage, drift/age decays it to nothing, the budget is a hard cap, and the whole thing is a pure
// function of its calls (so the m8.6 GPU pass draws reproducible data).
using namespace rime;

TEST_CASE("M8.4 dust: a burst blooms coverage, then it decays to nothing") {
    vfx::DustField field(200);
    CHECK(field.count() == 0);
    CHECK(field.coverage() == doctest::Approx(0.0f));

    // A break's world AABB (~a 0.4 m part) at intensity 1.
    field.emit_burst({-0.2f, 0.8f, -0.2f}, {0.2f, 1.2f, 0.2f}, 1.0f);
    CHECK(field.count() > 0);
    CHECK(field.count() <= field.capacity());
    const float bloomed = field.coverage();
    CHECK(bloomed > 0.0f);

    // Age it a little without new bursts: coverage only falls (alpha fades, particles retire).
    for (int i = 0; i < 10; ++i) {
        field.simulate(1.0f / 60.0f);
    }
    CHECK(field.coverage() < bloomed);

    // Age past the longest lifetime (1.6 s): the puff is gone.
    for (int i = 0; i < 120; ++i) {
        field.simulate(1.0f / 60.0f);
    }
    CHECK(field.count() == 0);
    CHECK(field.coverage() == doctest::Approx(0.0f));
}

TEST_CASE("M8.4 dust: the budget is a hard cap") {
    vfx::DustField field(200);
    // Twenty violent bursts would want ~20·48·(clamped) particles — far over 200; the field clamps.
    for (int i = 0; i < 20; ++i) {
        field.emit_burst({-1.0f, 0.0f, -1.0f}, {1.0f, 2.0f, 1.0f}, 4.0f);
        CHECK(field.count() <= field.capacity());
    }
    CHECK(field.count() == field.capacity()); // saturated, never exceeded
}

TEST_CASE("M8.4 dust: deterministic — same seed + same calls ⇒ identical particles") {
    const auto run = [] {
        vfx::DustField f(200, 0xABCDEF12u);
        f.emit_burst({-0.2f, 0.0f, -0.2f}, {0.2f, 0.4f, 0.2f}, 1.5f);
        f.simulate(1.0f / 60.0f);
        f.emit_burst({0.8f, 0.0f, 0.0f}, {1.2f, 0.4f, 0.4f}, 0.7f);
        f.simulate(1.0f / 60.0f);
        return f;
    };
    const vfx::DustField a = run();
    const vfx::DustField b = run();
    REQUIRE(a.count() == b.count());
    const auto pa = a.particles();
    const auto pb = b.particles();
    for (std::size_t i = 0; i < pa.size(); ++i) {
        CHECK(pa[i].position.x == doctest::Approx(pb[i].position.x));
        CHECK(pa[i].position.y == doctest::Approx(pb[i].position.y));
        CHECK(pa[i].position.z == doctest::Approx(pb[i].position.z));
        CHECK(pa[i].size == doctest::Approx(pb[i].size));
        CHECK(pa[i].lifetime == doctest::Approx(pb[i].lifetime));
    }
    CHECK(a.coverage() == doctest::Approx(b.coverage()));
}

TEST_CASE("M8.4 dust: degenerate emits are no-ops") {
    vfx::DustField field(200);
    field.emit_burst({-0.2f, 0.0f, -0.2f}, {0.2f, 0.4f, 0.2f}, 0.0f);  // zero intensity
    field.emit_burst({-0.2f, 0.0f, -0.2f}, {0.2f, 0.4f, 0.2f}, -1.0f); // negative intensity
    field.emit_burst({0.2f, 0.4f, 0.2f}, {-0.2f, 0.0f, -0.2f}, 1.0f);  // inverted box
    CHECK(field.count() == 0);
    CHECK(field.stats().rejected_emissions == 3);
}

TEST_CASE("smoke seam: invalid emissions leave the next valid particle sequence untouched") {
    vfx::ParticleField field(100, 71);
    vfx::ParticleField control(100, 71);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const core::Vec3 lo{-0.2f, 0.0f, -0.2f};
    const core::Vec3 hi{0.2f, 0.4f, 0.2f};
    field.emit_burst(lo, hi, nan);
    field.emit_burst(lo, hi, inf);
    field.emit_burst({nan, 0.0f, 0.0f}, hi, 1.0f);
    field.emit_burst(lo, {inf, 0.4f, 0.2f}, 1.0f);
    field.emit_burst(hi, lo, 1.0f);
    auto bad = vfx::impact_dust();
    bad.min_lifetime = bad.max_lifetime + 1.0f;
    field.emit_burst(lo, hi, 1.0f, bad);
    bad = vfx::impact_dust();
    bad.speed = inf;
    field.emit_burst(lo, hi, 1.0f, bad);
    bad = vfx::impact_dust();
    bad.family = static_cast<vfx::Family>(255);
    field.emit_burst(lo, hi, 1.0f, bad);
    CHECK(field.stats().rejected_emissions == 8);
    CHECK(field.stats().requested_particles == 0);

    field.emit_burst(lo, hi, 1.0f);
    control.emit_burst(lo, hi, 1.0f);
    REQUIRE(field.count() == control.count());
    for (std::size_t i = 0; i < field.count(); ++i) {
        const auto& a = field.particles()[i];
        const auto& b = control.particles()[i];
        CHECK(a.position.x == b.position.x);
        CHECK(a.position.y == b.position.y);
        CHECK(a.position.z == b.position.z);
        CHECK(a.velocity.x == b.velocity.x);
        CHECK(a.velocity.y == b.velocity.y);
        CHECK(a.velocity.z == b.velocity.z);
        CHECK(a.size == b.size);
        CHECK(a.lifetime == b.lifetime);
    }
}

TEST_CASE("smoke seam: a zero budget records demand and large finite requests safely") {
    vfx::ParticleField field(0);
    auto huge = vfx::impact_dust();
    huge.count = std::numeric_limits<std::uint32_t>::max();
    field.emit_burst({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 4.0f, huge);
    CHECK(field.count() == 0);
    CHECK(field.stats().requested_particles == 4ull * std::numeric_limits<std::uint32_t>::max());
    CHECK(field.stats().spawned_particles == 0);
    CHECK(field.stats().dropped_capacity == field.stats().requested_particles);
    CHECK(field.stats().rejected_emissions == 0);
}

TEST_CASE("smoke seam: retirement and clear have separate counters") {
    vfx::ParticleField field(100, 23);
    vfx::ParticleField control(100, 23);
    const core::Vec3 point{0.0f, 0.0f, 0.0f};
    field.emit_burst(point, point, 1.0f, vfx::muzzle_flash());
    control.emit_burst(point, point, 1.0f, vfx::muzzle_flash());
    field.simulate(0.2f);
    control.simulate(0.2f);
    CHECK(field.stats().retired_particles == 10);
    CHECK(field.stats().cleared_particles == 0);
    field.emit_burst(point, point, 1.0f, vfx::lingering_smoke());
    control.emit_burst(point, point, 1.0f, vfx::lingering_smoke());
    field.clear();
    control.clear();
    CHECK(field.stats().cleared_particles == 18);
    CHECK(field.stats().retired_particles == 10);
    field.reset_stats();
    CHECK(field.stats().retired_particles == 0);
    CHECK(field.stats().cleared_particles == 0);
    field.emit_burst(point, point, 1.0f);
    control.emit_burst(point, point, 1.0f);
    REQUIRE(field.count() == control.count());
    for (std::size_t i = 0; i < field.count(); ++i) {
        CHECK(field.particles()[i].velocity.x == control.particles()[i].velocity.x);
        CHECK(field.particles()[i].velocity.y == control.particles()[i].velocity.y);
        CHECK(field.particles()[i].velocity.z == control.particles()[i].velocity.z);
    }
    CHECK(field.stats().requested_particles == 48);
    CHECK(field.stats().spawned_particles == 48);
}
