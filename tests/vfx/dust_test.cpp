// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

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
}
