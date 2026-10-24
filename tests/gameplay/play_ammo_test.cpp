// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include "play_ammo.hpp"

// The magazine ledger, driven the way the sample drives it: consume per accepted shot, filter
// the trigger per tick, refill when the motion reports the magazine seated.
using namespace rime;
using block_play::PlayAmmo;
using block_play::PlayWeaponMotion;
using Weapon = PlayWeaponMotion::Weapon;

namespace {
constexpr float kTick = 1.0f / 60.0f;

gameplay::CharacterInput fire_input() {
    gameplay::CharacterInput in{};
    in.held = gameplay::kActionFire;
    in.pressed = gameplay::kActionFire;
    return in;
}
} // namespace

TEST_CASE("capacities come from the one table and every gun starts full") {
    PlayAmmo ammo;
    CHECK(ammo.rounds(Weapon::Scar) == 30);
    CHECK(ammo.rounds(Weapon::M4) == 30);
    CHECK(ammo.rounds(Weapon::Sniper) == 5);
    CHECK(ammo.full(Weapon::Sniper));
    CHECK_FALSE(ammo.wants_reload(Weapon::Scar));
}

TEST_CASE("each accepted shot consumes one round, and an empty magazine refuses") {
    PlayAmmo ammo;
    for (int i = 0; i < 5; ++i)
        CHECK(ammo.consume(Weapon::Sniper));
    CHECK(ammo.empty(Weapon::Sniper));
    CHECK_FALSE(ammo.consume(Weapon::Sniper));
    CHECK(ammo.rounds(Weapon::Sniper) == 0);
    // Other guns keep their own counts.
    CHECK(ammo.rounds(Weapon::Scar) == 30);
}

TEST_CASE("empty trigger: fire bits removed, one dry fire reported only when able to act") {
    PlayAmmo ammo;
    const auto live = ammo.filter_trigger(fire_input(), Weapon::M4, false);
    CHECK((live.input.held & gameplay::kActionFire) != 0);
    CHECK_FALSE(live.dry_fire);
    for (int i = 0; i < 30; ++i)
        (void)ammo.consume(Weapon::M4);
    const auto dry = ammo.filter_trigger(fire_input(), Weapon::M4, false);
    CHECK(dry.input.held == 0);
    CHECK(dry.input.pressed == 0);
    CHECK(dry.dry_fire);
    CHECK_FALSE(ammo.filter_trigger(fire_input(), Weapon::M4, true).dry_fire);
    CHECK_FALSE(ammo.filter_trigger(gameplay::CharacterInput{}, Weapon::M4, false).dry_fire);
}

TEST_CASE("the magazine fills at the seat instant, not at the start or by waiting") {
    PlayAmmo ammo;
    PlayWeaponMotion motion;
    (void)ammo.consume(Weapon::Scar);
    REQUIRE(ammo.wants_reload(Weapon::Scar));
    REQUIRE(motion.begin_reload(Weapon::Scar));
    float t = 0.0f;
    const float seat = PlayWeaponMotion::kMagSeatFraction * PlayWeaponMotion::kRifleReloadSeconds;
    bool seated = false;
    while (motion.busy()) {
        motion.advance(kTick, Weapon::Scar, false);
        t += kTick;
        if (motion.take_mag_seated()) {
            seated = true;
            ammo.refill(Weapon::Scar);
            CHECK(t >= seat);
            CHECK(t < seat + 2 * kTick);
        }
        if (t < seat - kTick)
            CHECK(ammo.rounds(Weapon::Scar) == 29);
    }
    CHECK(seated);
    CHECK(ammo.full(Weapon::Scar));
}

TEST_CASE("switching weapon during a reload cancels it and leaves the count alone") {
    PlayAmmo ammo;
    PlayWeaponMotion motion;
    for (int i = 0; i < 10; ++i)
        (void)ammo.consume(Weapon::Scar);
    REQUIRE(motion.begin_reload(Weapon::Scar));
    for (int i = 0; i < 60; ++i) // 1.0 s, before the 1.69 s seat
        motion.advance(kTick, Weapon::Scar, false);
    motion.advance(kTick, Weapon::Sniper, false); // switch: cancels
    for (int i = 0; i < 300; ++i) {
        motion.advance(kTick, Weapon::Sniper, false);
        CHECK_FALSE(motion.take_mag_seated());
    }
    CHECK(ammo.rounds(Weapon::Scar) == 20);
}

TEST_CASE("a huge frame that crosses the seat and the end still reports the seat once") {
    PlayWeaponMotion motion;
    REQUIRE(motion.begin_reload(Weapon::Sniper));
    motion.advance(10.0f, Weapon::Sniper, false);
    CHECK_FALSE(motion.busy());
    CHECK(motion.take_mag_seated());
    CHECK_FALSE(motion.take_mag_seated());
}
