// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <vector>

#include "play_weapon_motion.hpp"
#include "play_weapon_policy.hpp"
#include "rime/gameplay/weapon.hpp"
#include "rime/physics/world.hpp"

// The play sample's weapon selection, proven through the PUBLIC engine weapon rather than by
// re-deriving cadence arithmetic. Each tick runs exactly what the sample's PreSim stage runs:
// filter the command by the presentation's busy flag, step the authoritative weapon, then feed
// an accepted shot back into the presentation timeline. An empty physics world and a valid aim
// mean every accepted trigger is a miss that still FIRED, so `fired` is the whole observable.
using namespace rime;
using block_play::PlayWeaponMotion;
using Weapon = PlayWeaponMotion::Weapon;

namespace {

constexpr float kTick = 1.0f / static_cast<float>(block_play::kPlaySimHz);

struct Rig {
    physics::PhysicsWorld world;
    gameplay::WeaponConfig config{};
    gameplay::WeaponState state{};
    gameplay::FireStats stats{};
    PlayWeaponMotion motion;
    Weapon weapon;

    explicit Rig(Weapon w) : weapon(w) {
        block_play::configure_selected_weapon(config, weapon);
        motion.cancel(weapon);
    }

    // Returns whether the authoritative weapon accepted a shot this tick.
    bool tick(gameplay::CharacterInput input) {
        input = block_play::filter_busy_fire(input, motion.busy(), config.fire_bit);
        gameplay::ShotResult result;
        state = gameplay::step_weapon(
            state, input, config, gameplay::Aim{}, world, physics::BodyId{}, &result, &stats);
        motion.advance(kTick, weapon, result.fired);
        return result.fired;
    }
};

gameplay::CharacterInput trigger(bool held, bool pressed) {
    gameplay::CharacterInput in;
    in.held = held ? gameplay::kActionFire : 0u;
    in.pressed = pressed ? gameplay::kActionFire : 0u;
    return in;
}

} // namespace

TEST_CASE("play weapon policy: selection rewrites only trigger rule and cadence") {
    gameplay::WeaponConfig config{};
    config.range = 77.0f;
    config.damage = 0.25f;
    config.fire_bit = 1u << 5;
    block_play::configure_selected_weapon(config, Weapon::Sniper);
    CHECK_FALSE(config.automatic);
    CHECK(config.cooldown_ticks == 72u);
    CHECK(config.range == 77.0f);
    CHECK(config.damage == 0.25f);
    CHECK(config.fire_bit == 1u << 5);
    block_play::configure_selected_weapon(config, Weapon::Scar);
    CHECK(config.automatic);
    CHECK(config.cooldown_ticks == 8u);
    block_play::configure_selected_weapon(config, Weapon::M4);
    CHECK(config.automatic);
    CHECK(config.cooldown_ticks == 7u);

    CHECK(block_play::rounds_per_minute(block_play::kScarCooldownTicks) == doctest::Approx(450));
    CHECK(block_play::rounds_per_minute(block_play::kM4CooldownTicks) ==
          doctest::Approx(514.2857f));
    CHECK(block_play::rounds_per_minute(block_play::kSniperCooldownTicks) == doctest::Approx(50));
    CHECK(block_play::rounds_per_minute(0) == block_play::rounds_per_minute(1));
}

TEST_CASE("play weapon policy: a held rifle fires at exactly its authored cadence") {
    for (const Weapon weapon : {Weapon::Scar, Weapon::M4}) {
        Rig rig(weapon);
        std::vector<int> shots;
        for (int t = 0; t < 120; ++t)
            if (rig.tick(trigger(true, t == 0)))
                shots.push_back(t);
        const int period = static_cast<int>(rig.config.cooldown_ticks);
        REQUIRE(shots.size() == static_cast<std::size_t>((120 + period - 1) / period));
        for (std::size_t i = 0; i < shots.size(); ++i)
            CHECK(shots[i] == static_cast<int>(i) * period);
        CHECK(rig.stats.refused_cooldown == 120u - shots.size());
        CHECK_FALSE(rig.motion.busy()); // a rifle shot never starts a bolt cycle
    }
}

TEST_CASE("play weapon policy: a held sniper fires once and needs a fresh press after cooldown") {
    Rig rig(Weapon::Sniper);
    REQUIRE(rig.tick(trigger(true, true)));
    REQUIRE(rig.motion.cycling());
    // Holding the trigger through the whole cycle is a level, and a semi-automatic weapon reads
    // only the edge: no second shot however long it is held.
    for (int t = 1; t < 200; ++t)
        CHECK_FALSE(rig.tick(trigger(true, false)));
    CHECK(rig.stats.shots == 1u);
    CHECK(rig.stats.intents == 1u);

    // Fresh presses: one while the bolt is on screen (filtered, not even an intent), one after
    // the bolt but inside the cooldown (refused and LOST), then nothing pressed on the tick the
    // cooldown expires -- a queued shot would fire there.
    Rig cycle(Weapon::Sniper);
    REQUIRE(cycle.tick(trigger(true, true)));
    for (int t = 1; t < 100; ++t) {
        const bool press = t == 40 || t == 69 || t == 90;
        if (t == 40)
            REQUIRE(cycle.motion.cycling());
        if (t == 69)
            REQUIRE_FALSE(cycle.motion.busy());
        const bool fired = cycle.tick(trigger(press, press));
        CHECK(fired == (t == 90));
    }
    CHECK(cycle.stats.shots == 2u);
    CHECK(cycle.stats.intents == 3u);          // t = 0, 69, 90; the busy press never arrived
    CHECK(cycle.stats.refused_cooldown == 1u); // t = 69
    CHECK(cycle.motion.cycling());             // the second accepted shot cycles again
    CHECK(cycle.motion.take_case_ejection());  // the first cycle's case
    CHECK_FALSE(cycle.motion.take_case_ejection());
}

TEST_CASE("play weapon policy: a reload blocks fire but keeps movement, look and jump") {
    gameplay::CharacterInput in = trigger(true, true);
    in.move_x = 0.25f;
    in.move_y = -1.0f;
    in.yaw = 1.5f;
    in.pitch = -0.2f;
    in.held |= gameplay::kActionJump | (1u << 7);
    in.pressed |= gameplay::kActionJump;
    const auto out = block_play::filter_busy_fire(in, true);
    CHECK(out.held == (gameplay::kActionJump | (1u << 7)));
    CHECK(out.pressed == gameplay::kActionJump);
    CHECK(out.move_x == in.move_x);
    CHECK(out.move_y == in.move_y);
    CHECK(out.yaw == in.yaw);
    CHECK(out.pitch == in.pitch);
    const auto idle = block_play::filter_busy_fire(in, false);
    CHECK(idle.held == in.held);
    CHECK(idle.pressed == in.pressed);
    // A game that moved the trigger to another bit keeps bit 1 for itself.
    const auto other = block_play::filter_busy_fire(in, true, 1u << 7);
    CHECK(other.held == (gameplay::kActionFire | gameplay::kActionJump));

    // Through the real weapon: press-and-release during a reload is lost, and holding through
    // the end of a reload fires on the restated level only once the presentation is idle.
    for (const bool hold_through : {false, true}) {
        Rig rig(Weapon::Scar);
        REQUIRE(rig.motion.begin_reload(Weapon::Scar));
        const int reload_ticks =
            static_cast<int>(PlayWeaponMotion::kRifleReloadSeconds * block_play::kPlaySimHz);
        int first_shot = -1;
        for (int t = 0; t < reload_ticks + 20; ++t) {
            const bool held = hold_through ? true : t < 30;
            const bool was_busy = rig.motion.busy();
            if (rig.tick(trigger(held, t == 0)) && first_shot < 0) {
                CHECK_FALSE(was_busy);
                CHECK(rig.stats.intents == 1u); // nothing reached the trigger while busy
                first_shot = t;
            }
        }
        if (hold_through) {
            CHECK(first_shot >= reload_ticks - 1);
            CHECK(first_shot <= reload_ticks + 1);
        } else {
            CHECK(first_shot == -1);
            CHECK(rig.stats.intents == 0u);
        }
    }
}
