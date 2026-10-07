// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <numbers>

#include "rime/core/math/mat.hpp"
#include "rime/render/temporal_jitter.hpp"

// ADR-0078 step 1b -- camera jitter. CPU only: pure matrix and sequence math, no device.
//
// Every wrong-by-a-factor bug here (a factor of 2, a sign flip, a width/height swap, a bias in the
// sequence) still renders a recognisable image, so the proofs are analytic: they place a point
// exactly, move it by a known number of pixels and compare.

namespace {

using rime::core::Mat4;
using rime::core::Vec2;
using rime::core::Vec4;
using rime::render::halton;
using rime::render::jitter_projection;
using rime::render::TemporalJitter;

constexpr std::uint32_t kW = 1280; // deliberately not square: a width/height swap must be visible
constexpr std::uint32_t kH = 720;

Mat4 test_proj() {
    return rime::core::perspective(
        std::numbers::pi_v<float> / 3.0f, static_cast<float>(kW) / static_cast<float>(kH), 0.1f, 100.0f);
}

// The framebuffer pixel a view-space point lands on. Vulkan: NDC y and framebuffer y both point
// down, so pixel = (ndc * 0.5 + 0.5) * size on both axes.
Vec2 project_to_pixel(const Mat4& proj, Vec4 view_pos) {
    const Vec4 clip = proj * view_pos;
    const float nx = clip.x / clip.w;
    const float ny = clip.y / clip.w;
    return Vec2{(nx * 0.5f + 0.5f) * static_cast<float>(kW),
                (ny * 0.5f + 0.5f) * static_cast<float>(kH)};
}

bool bit_equal(const Mat4& a, const Mat4& b) {
    for (int i = 0; i < 16; ++i) {
        if (!(a.m[i] == b.m[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("halton matches the published radical-inverse values") {
    // External ground truth: base 2 is the van der Corput sequence, base 3 its ternary analogue.
    const float base2[] = {0.5f, 0.25f, 0.75f, 0.125f, 0.625f, 0.375f, 0.875f, 0.0625f};
    for (std::uint32_t i = 0; i < 8; ++i) {
        CHECK(halton(i + 1, 2) == doctest::Approx(base2[i]).epsilon(1e-7));
    }
    CHECK(halton(1, 3) == doctest::Approx(1.0f / 3.0f).epsilon(1e-6));
    CHECK(halton(2, 3) == doctest::Approx(2.0f / 3.0f).epsilon(1e-6));
    CHECK(halton(3, 3) == doctest::Approx(1.0f / 9.0f).epsilon(1e-6));
}

TEST_CASE("every offset lies in [-0.5, 0.5) across and beyond a period") {
    for (std::uint64_t i = 0; i < 10 * TemporalJitter::kPeriod; ++i) {
        const Vec2 o = TemporalJitter::offset_for(i);
        CHECK(o.x >= -0.5f);
        CHECK(o.x < 0.5f);
        CHECK(o.y >= -0.5f);
        CHECK(o.y < 0.5f);
    }
}

TEST_CASE("the sequence is unbiased: mean offset over one period is zero") {
    // A biased sequence shifts the whole image permanently; it would read as "TAA made everything
    // slightly offset", not as a bug. NOTE: a flat "halton - 0.5" is NOT unbiased over 8 frames --
    // that would measure mean (+0.0703, -0.1065) px -- so the sequence is centred on the period's
    // own mean. Measured with this implementation: |mean| < 1e-7 px on both axes.
    double sx = 0.0;
    double sy = 0.0;
    for (std::uint64_t i = 0; i < TemporalJitter::kPeriod; ++i) {
        const Vec2 o = TemporalJitter::offset_for(i);
        sx += o.x;
        sy += o.y;
    }
    CHECK(std::abs(sx / TemporalJitter::kPeriod) < 1e-6);
    CHECK(std::abs(sy / TemporalJitter::kPeriod) < 1e-6);
}

TEST_CASE("a zero offset is a bit-identical no-op") {
    const Mat4 p = test_proj();
    CHECK(bit_equal(jitter_projection(p, Vec2{0.0f, 0.0f}, kW, kH), p));
}

TEST_CASE("the shift moves a point by exactly the offset, in pixels, in the right direction") {
    const Mat4 p = test_proj();
    // Several depths and positions: the shift must be depth-independent (it is a pure image-plane
    // translation), which a shear or a wrongly-placed matrix term would violate.
    const Vec4 points[] = {Vec4{0.3f, -0.2f, -5.0f, 1.0f},
                           Vec4{-1.5f, 0.7f, -20.0f, 1.0f},
                           Vec4{0.0f, 0.0f, -2.0f, 1.0f}};
    const Vec2 offsets[] = {Vec2{0.5f, 0.0f}, Vec2{0.0f, -0.25f}, Vec2{0.375f, 0.125f}};
    for (const Vec4& pt : points) {
        const Vec2 before = project_to_pixel(p, pt);
        for (const Vec2& off : offsets) {
            const Vec2 after = project_to_pixel(jitter_projection(p, off, kW, kH), pt);
            CHECK(std::abs((after.x - before.x) - off.x) < 2e-3f);
            CHECK(std::abs((after.y - before.y) - off.y) < 2e-3f);
        }
    }
}

TEST_CASE("advance wraps within the period and reset returns to index 0") {
    TemporalJitter j;
    CHECK(j.index() == 0);
    for (std::uint32_t i = 0; i < TemporalJitter::kPeriod; ++i) {
        CHECK(j.index() == i);
        j.advance();
    }
    CHECK(j.index() == 0); // wrapped
    const Vec2 first = j.current();
    j.advance();
    j.advance();
    CHECK(j.index() == 2);
    j.reset();
    CHECK(j.index() == 0);
    CHECK(j.current().x == first.x);
    CHECK(j.current().y == first.y);
}

TEST_CASE("two jitter sequences are independent") {
    // Decision 7: two peers in one process must not share a counter.
    TemporalJitter a;
    TemporalJitter b;
    const Vec2 b_before = b.current();
    a.advance();
    a.advance();
    CHECK(b.index() == 0);
    CHECK(b.current().x == b_before.x);
    CHECK(b.current().y == b_before.y);
    CHECK(a.index() == 2);
}
