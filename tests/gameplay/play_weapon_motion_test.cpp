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

TEST_CASE("invalid time still honours a weapon change and an accepted sniper shot") {
    using Weapon = PlayWeaponMotion::Weapon;
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    PlayWeaponMotion motion;
    REQUIRE(motion.begin_reload(Weapon::Scar));
    motion.advance(0.4f, Weapon::Scar, false);
    // A switch on a bad-clock frame still cancels: the old gun's reload must not survive onto
    // the new one just because the frame time was unusable.
    motion.advance(kNaN, Weapon::M4, false);
    CHECK_FALSE(motion.busy());
    CHECK(motion.elapsed() == 0.0f);
    CHECK(motion.begin_reload(Weapon::M4));
    motion.advance(-0.5f, Weapon::Sniper, false);
    CHECK_FALSE(motion.busy());

    // The shot itself is authoritative, so the bolt starts; time does not advance until valid.
    motion.advance(kNaN, Weapon::Sniper, true);
    REQUIRE(motion.cycling());
    CHECK(motion.elapsed() == 0.0f);
    motion.advance(std::numeric_limits<float>::infinity(), Weapon::Sniper, false);
    CHECK(motion.elapsed() == 0.0f);
    CHECK_FALSE(motion.take_case_ejection());
    motion.advance(PlayWeaponMotion::kExtractionSeconds, Weapon::Sniper, false);
    CHECK(motion.take_case_ejection());
    CHECK_FALSE(motion.take_case_ejection());

    // A rifle's accepted shot on a bad frame never starts a cycle.
    motion.cancel(Weapon::Scar);
    motion.advance(kNaN, Weapon::Scar, true);
    CHECK_FALSE(motion.busy());
}

TEST_CASE("scoped sniper holds the shot until aim release and cycles exactly once") {
    using Weapon = PlayWeaponMotion::Weapon;
    PlayWeaponMotion motion;
    motion.advance(0.1f, Weapon::Sniper, true, true);
    REQUIRE(motion.pending_bolt());
    CHECK(motion.busy());
    CHECK_FALSE(motion.begin_reload(Weapon::Sniper));
    CHECK(motion.pose().lower == 0.0f);
    motion.advance(3.0f, Weapon::Sniper, false, true);
    motion.advance(3.0f, Weapon::Sniper, true, true);
    CHECK(motion.pending_bolt());
    CHECK(motion.elapsed() == 0.0f);
    CHECK_FALSE(motion.take_case_ejection());

    motion.advance(0.0f, Weapon::Sniper, false, false);
    REQUIRE(motion.cycling());
    motion.advance(PlayWeaponMotion::kExtractionSeconds, Weapon::Sniper, false, false);
    CHECK(motion.take_case_ejection());
    CHECK_FALSE(motion.take_case_ejection());
    motion.advance(10.0f, Weapon::Sniper, false, false);
    CHECK_FALSE(motion.busy());
    CHECK_FALSE(motion.take_case_ejection());
}

TEST_CASE("pending scoped shot survives invalid time and selection cancels it") {
    using Weapon = PlayWeaponMotion::Weapon;
    constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
    PlayWeaponMotion motion;
    motion.advance(kNaN, Weapon::Sniper, true, true);
    REQUIRE(motion.pending_bolt());
    motion.advance(kNaN, Weapon::Sniper, false, false);
    REQUIRE(motion.cycling());
    CHECK(motion.elapsed() == 0.0f);
    CHECK_FALSE(motion.take_case_ejection());
    motion.cancel(Weapon::M4);
    CHECK_FALSE(motion.busy());
    CHECK_FALSE(motion.take_case_ejection());

    motion.advance(-1.0f, Weapon::Sniper, true, true);
    REQUIRE(motion.pending_bolt());
    motion.cancel(Weapon::Scar);
    CHECK_FALSE(motion.busy());
    CHECK(motion.begin_reload(Weapon::Scar));
    motion.cancel(Weapon::Sniper);
    motion.advance(PlayWeaponMotion::kBoltSeconds + 1.0f, Weapon::Sniper, true);
    CHECK_FALSE(motion.busy());
    CHECK(motion.take_case_ejection());
    CHECK_FALSE(motion.take_case_ejection());
}
