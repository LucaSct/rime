// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>

#include "rime/vfx/dust.hpp"

// m13.1b — the three effect families ADR-0035 §5 names for fx1a.
//
// THE POINT OF THESE CASES IS THAT THE FAMILIES DIFFER. "Three families" is only a feature if the
// three behave differently; three identically-parameterised puffs that all work would prove one
// family three times over and read as full coverage. So each case names the property that makes a
// family that family — smoke rises and grows, a flash is over before dust has started settling,
// dust does what it always did — and asserts it against the others rather than against a constant.
//
// Pure CPU: no device, no render, no destruction. What a family LOOKS like (colour, one day an
// atlas frame) is the consumer's, and this module deliberately has no notion of it.
using namespace rime;

namespace {

constexpr float kDt = 1.0f / 60.0f;

// Emit one burst of `params` into a fresh field and run it for `seconds`.
[[nodiscard]] vfx::ParticleField run(const vfx::EmitParams& params, float seconds) {
    vfx::ParticleField field;
    field.emit_burst({-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}, 1.0f, params);
    const int steps = static_cast<int>(seconds / kDt);
    for (int i = 0; i < steps; ++i) {
        field.simulate(kDt);
    }
    return field;
}

[[nodiscard]] float mean_y(const vfx::ParticleField& field) {
    if (field.count() == 0) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (const vfx::Particle& p : field.particles()) {
        sum += p.position.y;
    }
    return sum / static_cast<float>(field.count());
}

[[nodiscard]] float mean_velocity_y(const vfx::ParticleField& field) {
    if (field.count() == 0) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (const vfx::Particle& p : field.particles()) {
        sum += p.velocity.y;
    }
    return sum / static_cast<float>(field.count());
}

[[nodiscard]] float mean_size(const vfx::ParticleField& field) {
    if (field.count() == 0) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (const vfx::Particle& p : field.particles()) {
        sum += p.size;
    }
    return sum / static_cast<float>(field.count());
}

} // namespace

TEST_CASE("m13.1b: a burst carries its family, and the default spelling is still dust") {
    vfx::ParticleField field;

    // The M8.4 spelling, untouched: callers written before families existed still mean dust.
    field.emit_burst({-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}, 1.0f);
    REQUIRE(field.count() > 0);
    for (const vfx::Particle& p : field.particles()) {
        CHECK(p.family == vfx::Family::ImpactDust);
    }

    // …and one field holds all three at once. That is the reason gravity and growth are per
    // particle rather than per field: smoke rising while dust settles in the same puff is the
    // common case, and a per-field constant would force a field per family and a draw per field.
    field.emit_burst({-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}, 1.0f, vfx::lingering_smoke());
    field.emit_burst({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f, vfx::muzzle_flash());

    int dust = 0;
    int smoke = 0;
    int flash = 0;
    for (const vfx::Particle& p : field.particles()) {
        dust += p.family == vfx::Family::ImpactDust ? 1 : 0;
        smoke += p.family == vfx::Family::LingeringSmoke ? 1 : 0;
        flash += p.family == vfx::Family::MuzzleFlash ? 1 : 0;
    }
    CHECK(dust > 0);
    CHECK(smoke > 0);
    CHECK(flash > 0);
}

TEST_CASE("m13.1b: smoke rises where dust settles") {
    // The whole visual difference between the two, and the reason `gravity` is signed. Measured
    // against EACH OTHER at the same moment rather than against an absolute height, because what
    // matters is the relationship — a change that made both fall would still satisfy any absolute
    // bound generous enough to be stable.
    const vfx::ParticleField dust = run(vfx::impact_dust(), 0.7f);
    const vfx::ParticleField smoke = run(vfx::lingering_smoke(), 0.7f);
    REQUIRE(dust.count() > 0);
    REQUIRE(smoke.count() > 0);

    const float dust_y = mean_y(dust);
    const float smoke_y = mean_y(smoke);
    MESSAGE("m13.1b after 0.7 s: mean height dust " << dust_y << " m, smoke " << smoke_y << " m");
    CHECK(smoke_y > dust_y);

    // Dust is FALLING by now — it lifts first, then settles, which is what makes it read as debris
    // haze rather than as a balloon.
    //
    // Asserted on vertical VELOCITY rather than on height, and the difference is not cosmetic. Dust
    // lifts at ~0.7 m/s under 1.5 m/s², so it apexes around t = 0.47 s and by 0.7 s has come back
    // only to about where it was at 0.25 s — a height comparison there is a coin flip between two
    // numbers that differ in the third decimal (measured: 0.128 vs 0.1225). Velocity has already
    // changed sign and says the same thing without the knife edge.
    CHECK(mean_velocity_y(dust) < 0.0f);
    CHECK(mean_velocity_y(smoke) > 0.0f);
}

TEST_CASE("m13.1b: smoke expands as it dissipates; dust does not") {
    // `growth`. Without it a long-lived puff just fades in place, which reads as a bug rather than
    // as smoke — a dissipating cloud gets larger and fainter at once.
    const vfx::ParticleField smoke_early = run(vfx::lingering_smoke(), 0.2f);
    const vfx::ParticleField smoke_late = run(vfx::lingering_smoke(), 3.0f);
    REQUIRE(smoke_late.count() > 0);
    CHECK(mean_size(smoke_late) > mean_size(smoke_early) * 1.5f);

    const vfx::ParticleField dust_early = run(vfx::impact_dust(), 0.2f);
    const vfx::ParticleField dust_late = run(vfx::impact_dust(), 1.0f);
    REQUIRE(dust_late.count() > 0);
    // Dust holds its size exactly — growth is 0, so this is an equality, not a bound.
    CHECK(mean_size(dust_late) == doctest::Approx(mean_size(dust_early)).epsilon(0.001));
}

TEST_CASE("m13.1b: a flash is over before dust has finished lifting") {
    // "Lingering" and "flash" are claims about TIME, so they are asserted against the clock.
    const vfx::ParticleField flash = run(vfx::muzzle_flash(), 0.15f);
    CHECK(flash.count() == 0); // gone in under a sixth of a second

    const vfx::ParticleField dust = run(vfx::impact_dust(), 0.15f);
    CHECK(dust.count() > 0); // …while dust is still going strong

    // And smoke outlives dust by a wide margin — it is still there long after the dust retires.
    const vfx::ParticleField dust_gone = run(vfx::impact_dust(), 2.0f);
    const vfx::ParticleField smoke_still = run(vfx::lingering_smoke(), 2.0f);
    CHECK(dust_gone.count() == 0);
    CHECK(smoke_still.count() > 0);
}

TEST_CASE("m13.1b: a flash emitted at a point stays a point") {
    // The degenerate box the header names: a muzzle is a POINT, so the flash is emitted with zero
    // extent and every spark starts at the same place. What spreads them is velocity, not the
    // spawn box — and the outward-from-centre term is exactly zero when the box has no centre to
    // be outward from, so the jitter is what has to carry it.
    vfx::ParticleField field;
    const core::Vec3 muzzle{1.0f, 1.5f, -2.0f};
    field.emit_burst(muzzle, muzzle, 1.0f, vfx::muzzle_flash());
    REQUIRE(field.count() > 0);

    for (const vfx::Particle& p : field.particles()) {
        CHECK(p.position.x == doctest::Approx(muzzle.x));
        CHECK(p.position.y == doctest::Approx(muzzle.y));
        CHECK(p.position.z == doctest::Approx(muzzle.z));
    }

    // They do move apart, or a "flash" would be one billboard's worth of light.
    for (int i = 0; i < 2; ++i) {
        field.simulate(kDt);
    }
    float spread = 0.0f;
    for (const vfx::Particle& p : field.particles()) {
        spread = std::max(spread, core::length(p.position - muzzle));
    }
    CHECK(spread > 0.0f);
}

TEST_CASE("m13.1b: families share one budget, and the cap still holds") {
    // The m8.5 discipline, unchanged by families: three effects firing at once must not be able to
    // grow the pool. A demolition storm with muzzle flashes in it is exactly the moment the cap
    // matters, and a per-family pool would have made the total unbounded again.
    vfx::ParticleField field(32);
    for (int i = 0; i < 20; ++i) {
        field.emit_burst({-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}, 4.0f, vfx::impact_dust());
        field.emit_burst({-0.2f, -0.2f, -0.2f}, {0.2f, 0.2f, 0.2f}, 4.0f, vfx::lingering_smoke());
        field.emit_burst({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 4.0f, vfx::muzzle_flash());
    }
    CHECK(field.count() <= field.capacity());
    CHECK(field.count() == 32); // …and it really filled, so the cap was exercised
}

TEST_CASE("m13.1b: every family is still deterministic") {
    // The property the whole module rests on (M8.4): two fields fed the same calls hold identical
    // particles. Families multiply the call space, so the claim is re-checked across all three
    // rather than assumed to survive.
    const auto tape = [](vfx::ParticleField& f) {
        f.emit_burst({-0.3f, -0.3f, -0.3f}, {0.3f, 0.3f, 0.3f}, 1.0f, vfx::impact_dust());
        f.simulate(kDt);
        f.emit_burst({-0.1f, 0.4f, -0.1f}, {0.1f, 0.6f, 0.1f}, 0.7f, vfx::lingering_smoke());
        f.simulate(kDt);
        f.emit_burst({2.0f, 1.0f, 0.0f}, {2.0f, 1.0f, 0.0f}, 1.0f, vfx::muzzle_flash());
        for (int i = 0; i < 30; ++i) {
            f.simulate(kDt);
        }
    };

    vfx::ParticleField a;
    vfx::ParticleField b;
    tape(a);
    tape(b);

    REQUIRE(a.count() == b.count());
    REQUIRE(a.count() > 0);
    const auto pa = a.particles();
    const auto pb = b.particles();
    for (std::size_t i = 0; i < pa.size(); ++i) {
        // BIT-identical, not approximately equal: an epsilon would pass on a field that had
        // genuinely diverged and merely stayed close, which is the failure determinism exists to
        // rule out.
        CHECK(pa[i].position.x == pb[i].position.x);
        CHECK(pa[i].position.y == pb[i].position.y);
        CHECK(pa[i].position.z == pb[i].position.z);
        CHECK(pa[i].size == pb[i].size);
        CHECK(pa[i].age == pb[i].age);
        CHECK(pa[i].family == pb[i].family);
    }
    CHECK(a.coverage() == b.coverage());
}
