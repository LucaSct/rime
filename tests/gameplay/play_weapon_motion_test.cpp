// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <limits>

#include "play_weapon_motion.hpp"

using rime::block_play::PlayWeaponMotion;

TEST_CASE("weapon reload uses elapsed time and ignores a held reload retrigger") {
    using Weapon = PlayWeaponMotion::Weapon;
    for (const float step : std::array{0.01f, 0.37f, 1.5f}) {
        PlayWeaponMotion motion;
        REQUIRE(motion.begin_reload(Weapon::Scar));
        float elapsed = 0.0f;
        while (elapsed + step < PlayWeaponMotion::kRifleReloadSeconds) {
            motion.advance(step, Weapon::Scar, false);
            elapsed += step;
            CHECK(motion.reloading());
            CHECK_FALSE(motion.begin_reload(Weapon::Scar));
        }
        motion.advance(PlayWeaponMotion::kRifleReloadSeconds - elapsed, Weapon::Scar, false);
        CHECK_FALSE(motion.busy());
        CHECK(motion.elapsed() == 0.0f);
        CHECK(motion.pose().lower == 0.0f);
    }
    PlayWeaponMotion slow;
    REQUIRE(slow.begin_reload(Weapon::Sniper));
    slow.advance(1.4f, Weapon::Sniper, false);
    CHECK(slow.reloading());
    CHECK(slow.pose().lower > 0.5f);
    CHECK(slow.pose().left_magazine > 0.5f);
    slow.advance(1.4f, Weapon::Sniper, false);
    CHECK_FALSE(slow.busy());
}

TEST_CASE("only accepted sniper shots cycle and emit one extraction event") {
    using Weapon = PlayWeaponMotion::Weapon;
    PlayWeaponMotion motion;
    motion.advance(0.5f, Weapon::Scar, true);
    CHECK_FALSE(motion.busy());
    CHECK_FALSE(motion.take_case_ejection());

    motion.advance(0.1f, Weapon::Sniper, true);
    REQUIRE(motion.cycling());
    CHECK_FALSE(motion.take_case_ejection());
    motion.advance(0.21f, Weapon::Sniper, true); // A second shot cannot restart a busy cycle.
    CHECK(motion.elapsed() == doctest::Approx(0.31f));
    CHECK(motion.take_case_ejection());
    CHECK_FALSE(motion.take_case_ejection());
    motion.advance(0.79f, Weapon::Sniper, false);
    CHECK_FALSE(motion.busy());
    CHECK_FALSE(motion.take_case_ejection());

    motion.advance(PlayWeaponMotion::kBoltSeconds + 1.0f, Weapon::Sniper, true);
    CHECK_FALSE(motion.busy());
    CHECK(motion.take_case_ejection());
    CHECK_FALSE(motion.take_case_ejection());
}

TEST_CASE("switch cancels motion and queued extraction without changing new weapon") {
    using Weapon = PlayWeaponMotion::Weapon;
    PlayWeaponMotion motion;
    motion.advance(0.4f, Weapon::Sniper, true);
    REQUIRE(motion.cycling());
    motion.cancel(Weapon::M4);
    CHECK_FALSE(motion.busy());
    CHECK_FALSE(motion.take_case_ejection());
    CHECK(motion.begin_reload(Weapon::M4));
    motion.advance(0.1f, Weapon::Scar, false); // Defensive switch also cancels.
    CHECK_FALSE(motion.busy());
    CHECK_FALSE(motion.take_case_ejection());
}

TEST_CASE("invalid time leaves presentation and ejection unchanged") {
    using Weapon = PlayWeaponMotion::Weapon;
    PlayWeaponMotion motion;
    REQUIRE(motion.begin_reload(Weapon::Scar));
    motion.advance(0.4f, Weapon::Scar, false);
    const auto elapsed = motion.elapsed();
    motion.advance(-1.0f, Weapon::Scar, false);
    motion.advance(std::numeric_limits<float>::infinity(), Weapon::Scar, false);
    motion.advance(std::numeric_limits<float>::quiet_NaN(), Weapon::Scar, true);
    CHECK(motion.reloading());
    CHECK(motion.elapsed() == elapsed);
    CHECK_FALSE(motion.take_case_ejection());
}
