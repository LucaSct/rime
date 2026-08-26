// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cmath>
#include <numbers>

#include "character_fixture.hpp"
#include "rime/gameplay/weapon.hpp"

// m12.3's weapon proofs, in ISOLATION — one shooter, static box geometry, no network anywhere.
//
// The split is deliberate and it is the same one m12.2 made for the mover: the weapon is proven
// here as a pure function over physics queries, so tests/gameplay_net debugs the CONSUME LOOP and
// never the gun. Every assertion below is closed-form (a direction from trigonometry, a tick count
// from the cooldown contract, a hit or a miss from a distance you can compute), never a golden
// value recorded from a previous run.
using namespace rime;
using namespace rime_test;

namespace {

// A shooter standing at the origin with a body of its own — because the interesting failure is
// hitting yourself, and a test without a self body could never catch it.
struct Shooter {
    physics::PhysicsWorld* world = nullptr;
    gameplay::WeaponConfig config{};
    gameplay::WeaponState state{};
    gameplay::CharacterConfig character{};
    gameplay::CharacterState pose{};
    physics::BodyId body{};
    gameplay::FireStats stats{};

    void spawn(physics::PhysicsWorld& w, core::Vec3 position) {
        world = &w;
        (void)gameplay::validate(character);
        (void)gameplay::validate(config);
        pose = gameplay::CharacterState{};
        pose.position = position;

        physics::BodyDesc d;
        d.motion = physics::MotionType::Kinematic;
        d.shape = gameplay::character_shape(character);
        d.position = position;
        body = w.create_body(d);
    }

    // One tick of trigger, from the character's own aim.
    gameplay::ShotResult tick(const gameplay::CharacterInput& input) {
        const gameplay::Aim aim = gameplay::character_aim(pose, input, config);
        gameplay::ShotResult result;
        state = gameplay::step_weapon(state, input, config, aim, *world, body, &result, &stats);
        return result;
    }
};

[[nodiscard]] gameplay::CharacterInput
trigger(bool pressed, bool held, float yaw = 0.0f, float pitch = 0.0f) {
    gameplay::CharacterInput in;
    in.yaw = yaw;
    in.pitch = pitch;
    in.pressed = pressed ? gameplay::kActionFire : 0u;
    in.held = held ? gameplay::kActionFire : 0u;
    return in;
}

} // namespace

TEST_CASE("the aim basis is the engine's, and the mover's, and the camera's") {
    // ONE convention, checked against its own definition rather than against a recorded vector.
    // Getting this wrong is not a subtle bug: it is "the gun shoots behind the player", and the
    // reason it has a test at all is that character.cpp derives the same basis independently for
    // movement, so the two agreeing is a property worth pinning.
    const core::Vec3 neutral = gameplay::aim_direction(0.0f, 0.0f);
    CHECK(neutral.x == doctest::Approx(0.0f));
    CHECK(neutral.y == doctest::Approx(0.0f));
    CHECK(neutral.z == doctest::Approx(-1.0f)); // forward at yaw 0 is -Z

    // Yaw is right-handed about +Y: a quarter turn takes -Z to -X.
    const core::Vec3 quarter = gameplay::aim_direction(std::numbers::pi_v<float> / 2.0f, 0.0f);
    CHECK(quarter.x == doctest::Approx(-1.0f));
    CHECK(quarter.z == doctest::Approx(0.0f).epsilon(1e-6));

    // Pitch is POSITIVE LOOKING UP.
    const core::Vec3 up45 = gameplay::aim_direction(0.0f, std::numbers::pi_v<float> / 4.0f);
    CHECK(up45.y == doctest::Approx(std::sqrt(0.5f)));
    CHECK(up45.z == doctest::Approx(-std::sqrt(0.5f)));

    // Unit by construction at every angle tried — no normalize is done, so this is a claim about
    // the algebra rather than about a cleanup step.
    for (int i = -8; i <= 8; ++i) {
        const float a = static_cast<float>(i) * 0.37f;
        for (int j = -4; j <= 4; ++j) {
            const float b = static_cast<float>(j) * 0.31f;
            CHECK(core::length(gameplay::aim_direction(a, b)) == doctest::Approx(1.0f));
        }
    }

    // A non-finite angle collapses to the neutral pose rather than poisoning a raycast.
    const core::Vec3 nan_yaw =
        gameplay::aim_direction(std::numeric_limits<float>::quiet_NaN(), 0.0f);
    CHECK(finite(nan_yaw));
    CHECK(core::length(nan_yaw) == doctest::Approx(1.0f));
}

TEST_CASE("a shooter does not shoot itself") {
    // The reason QueryFilter::exclude exists, arriving for the second time (m12.2's mover was the
    // first). The shooter's own capsule is a KINEMATIC body — it has to be, so debris can hit the
    // player — and it sits exactly where the ray starts. Without the exclusion every shot reports a
    // hit at distance ~0 on the shooter's own body, and nothing downstream could tell that from a
    // point-blank wall.
    physics::PhysicsWorld world;
    (void)add_ground(world);
    Shooter shooter;
    shooter.spawn(world, {0.0f, rest_y(shooter.character), 0.0f});

    const gameplay::ShotResult shot = shooter.tick(trigger(true, true));
    REQUIRE(shot.fired);
    // Nothing is in front of the shooter, so this must be a clean miss — not a hit on itself.
    CHECK_FALSE(shot.did_hit);
    CHECK(shooter.stats.hits == 0);
    CHECK(shooter.stats.misses == 1);

    // The control that proves the ray was live rather than merely absent: the same shot, aimed
    // down, finds the floor.
    shooter.state = gameplay::WeaponState{}; // clear the cooldown
    const gameplay::ShotResult down =
        shooter.tick(trigger(true, true, 0.0f, -std::numbers::pi_v<float> / 2.0f));
    REQUIRE(down.fired);
    REQUIRE(down.did_hit);
    CHECK(down.hit.body != shooter.body);
}

TEST_CASE("a miss is still a shot") {
    // `fired` and `did_hit` are separate booleans because "fired and missed" is a real outcome with
    // a muzzle flash, a tracer and a report. A ShotResult that only reported hits would make
    // missing silent — the opposite of ADR-0035 §1's "the shot feels connected".
    physics::PhysicsWorld world;
    Shooter shooter;
    shooter.spawn(world, {0.0f, 1.0f, 0.0f});

    const gameplay::ShotResult shot = shooter.tick(trigger(true, true));
    CHECK(shot.fired);
    CHECK_FALSE(shot.did_hit);
    CHECK(core::length(shot.shot.direction) == doctest::Approx(1.0f));
    CHECK(shot.shot.range == doctest::Approx(shooter.config.range));
    CHECK(shooter.stats.shots == 1);
}

TEST_CASE("range bounds the shot, and the boundary is where the config says") {
    physics::PhysicsWorld world;
    Shooter shooter;
    shooter.spawn(world, {0.0f, 1.0f, 0.0f});
    shooter.config.range = 5.0f;
    shooter.config.cooldown_ticks = 0;

    // A wall 8 m ahead (forward is -Z), well outside a 5 m range.
    (void)add_static_box(world, {2.0f, 2.0f, 0.5f}, {0.0f, 1.0f, -8.0f});
    CHECK_FALSE(shooter.tick(trigger(true, true)).did_hit);

    // Widen the range past it and the same geometry is hit — so the miss above was the RANGE, not
    // a mis-aimed ray.
    shooter.config.range = 20.0f;
    const gameplay::ShotResult hit = shooter.tick(trigger(true, true));
    REQUIRE(hit.did_hit);
    // The wall's near face is at z = -7.5, so the hit is 7.5 m from an origin at z = 0.
    CHECK(hit.hit.distance == doctest::Approx(7.5f).epsilon(0.01));
}

TEST_CASE("the eye is above the origin, so a shot clears cover the player can see over") {
    // Why WeaponConfig::eye_height is not decoration. The capsule's ORIGIN is its centre, so a ray
    // fired from `state.position` leaves at navel height — and a chest-high wall the player is
    // plainly looking over then stops half their shots dead in the cover.
    physics::PhysicsWorld world;
    (void)add_ground(world);
    Shooter shooter;
    shooter.spawn(world, {0.0f, rest_y(shooter.character), 0.0f});
    shooter.config.cooldown_ticks = 0;

    // Cover whose top sits just below the eye and well above the navel. The default capsule is
    // half_height 0.5 + radius 0.4, so the origin rests at ~0.93 and the eye at ~1.43.
    const float eye_y = shooter.pose.position.y + shooter.config.eye_height;
    const float navel_y = shooter.pose.position.y;
    const float cover_top = 0.5f * (eye_y + navel_y); // between the two, by construction
    (void)add_static_box(world, {2.0f, cover_top * 0.5f, 0.25f}, {0.0f, cover_top * 0.5f, -2.0f});
    // A target further away, at eye level.
    (void)add_static_box(world, {2.0f, 0.5f, 0.25f}, {0.0f, eye_y, -6.0f});

    const gameplay::ShotResult over = shooter.tick(trigger(true, true));
    REQUIRE(over.did_hit);
    CHECK(over.hit.distance > 3.0f); // it reached the far target, not the cover at 1.75 m

    // The negative control, and the whole point: drop the eye to the origin and the SAME shot,
    // same geometry, same everything, stops in the cover.
    shooter.config.eye_height = 0.0f;
    const gameplay::ShotResult through = shooter.tick(trigger(true, true));
    REQUIRE(through.did_hit);
    CHECK(through.hit.distance < 2.5f);
}

TEST_CASE("semi-automatic fires on the edge: one trigger pull, one shot") {
    // The held/pressed split input.hpp was built for, arriving at its first real consumer. A weapon
    // that read `held` while semi-automatic would fire once per tick for as long as the finger is
    // down — thirty shots a second from one pull.
    physics::PhysicsWorld world;
    Shooter shooter;
    shooter.spawn(world, {0.0f, 1.0f, 0.0f});
    shooter.config.automatic = false;
    shooter.config.cooldown_ticks = 0; // isolate the edge rule from the rate limit

    CHECK(shooter.tick(trigger(/*pressed=*/true, /*held=*/true)).fired);
    for (int i = 0; i < 20; ++i) {
        CHECK_FALSE(shooter.tick(trigger(/*pressed=*/false, /*held=*/true)).fired);
    }
    CHECK(shooter.stats.shots == 1);
    // A second pull: release, then press again.
    CHECK_FALSE(shooter.tick(trigger(false, false)).fired);
    CHECK(shooter.tick(trigger(true, true)).fired);
    CHECK(shooter.stats.shots == 2);
}

TEST_CASE("automatic fires on the level, gated only by the cooldown") {
    physics::PhysicsWorld world;
    Shooter shooter;
    shooter.spawn(world, {0.0f, 1.0f, 0.0f});
    shooter.config.automatic = true;
    shooter.config.cooldown_ticks = 0;

    int shots = 0;
    for (int i = 0; i < 10; ++i) {
        shots += shooter.tick(trigger(i == 0, true)).fired ? 1 : 0;
    }
    CHECK(shots == 10); // no cooldown: every tick the finger is down
    CHECK(shooter.stats.refused_cooldown == 0);

    // Releasing stops it, immediately and with no trailing shot.
    CHECK_FALSE(shooter.tick(trigger(false, false)).fired);
}

TEST_CASE("a weapon with cooldown_ticks = N fires at most once every max(N, 1) ticks") {
    // The contract stated in step_weapon, checked at four values so an off-by-one in the ageing
    // ORDER cannot pass: ageing after the trigger instead of before would give every row a period
    // of N + 1, which the N = 2 and N = 5 rows below both catch.
    physics::PhysicsWorld world;
    Shooter shooter;
    shooter.spawn(world, {0.0f, 1.0f, 0.0f});
    shooter.config.automatic = true;

    for (const std::uint32_t cooldown : {0u, 1u, 2u, 5u}) {
        shooter.state = gameplay::WeaponState{};
        shooter.stats = gameplay::FireStats{};
        shooter.config.cooldown_ticks = cooldown;

        constexpr int kTicks = 60;
        int shots = 0;
        for (int i = 0; i < kTicks; ++i) {
            shots += shooter.tick(trigger(i == 0, true)).fired ? 1 : 0;
        }
        // Every tick either fires or is refused — nothing is silently neither.
        CHECK(shooter.stats.intents == kTicks);
        CHECK(shooter.stats.shots + shooter.stats.refused_cooldown ==
              static_cast<std::uint32_t>(kTicks));

        const int period = std::max(static_cast<int>(cooldown), 1);
        const int expected = (kTicks + period - 1) / period;
        CHECK(shots == expected);
    }
}

TEST_CASE("a degenerate aim refuses the shot and says so") {
    // Firing along a normalized NaN would put a garbage ray through the broadphase and the answer
    // would be acted on as if it meant something. Refusing costs a shot; the counter is what keeps
    // "the gun is broken" from looking like "nobody fired".
    physics::PhysicsWorld world;
    Shooter shooter;
    shooter.spawn(world, {0.0f, 1.0f, 0.0f});

    gameplay::Aim aim;
    aim.origin = {0.0f, 1.0f, 0.0f};
    aim.direction = {0.0f, 0.0f, 0.0f}; // points nowhere
    gameplay::ShotResult result;
    shooter.state = gameplay::step_weapon(shooter.state,
                                          trigger(true, true),
                                          shooter.config,
                                          aim,
                                          world,
                                          shooter.body,
                                          &result,
                                          &shooter.stats);
    CHECK_FALSE(result.fired);
    CHECK(shooter.stats.refused_aim == 1);
    // The cooldown was NOT spent on a shot that never happened.
    CHECK(shooter.state.cooldown == 0);
}

TEST_CASE("validate replaces non-finite fields rather than clamping them") {
    // A NaN compares false against every bound, so a clamp alone passes it straight through — the
    // trap that makes "we validate our inputs" untrue in code that looks like it does.
    gameplay::WeaponConfig config;
    config.range = std::numeric_limits<float>::quiet_NaN();
    config.damage = -std::numeric_limits<float>::infinity();
    config.damage_radius = 0.0f; // in range as a float, but deposits no damage at all
    config.fire_bit = 0;         // a trigger no input could ever pull
    CHECK(gameplay::validate(config));

    const gameplay::WeaponConfig defaults;
    CHECK(config.range == doctest::Approx(defaults.range));
    CHECK(config.damage == doctest::Approx(defaults.damage));
    CHECK(config.damage_radius > 0.0f);
    CHECK(config.fire_bit == defaults.fire_bit);

    // Idempotent: validating a validated config changes nothing.
    gameplay::WeaponConfig again = config;
    CHECK_FALSE(gameplay::validate(again));
}

TEST_CASE("the weapon replays bit-identically") {
    // The purity rule the header states, held to the same standard as the mover's replay proof:
    // anything the weapon remembered outside WeaponState would make the second run differ, and it
    // would differ as a rare desync rather than as a failing test. m12.4 replays these ticks.
    // A FRESH world per run, not one shared between them. Sharing looks tidier and is wrong: the
    // shooter's own capsule is a real kinematic body, so a second run in the same world stands
    // beside the first run's abandoned capsule — which `QueryFilter::exclude` does not hide,
    // because it excludes the CURRENT shooter's body and not every capsule that ever existed. The
    // symptom is a replay that "diverges" by hitting a ghost. (Caught by this very test on its
    // first run, which is the argument for building the world inside the closure.)
    const auto run = [] {
        physics::PhysicsWorld world;
        (void)add_ground(world);
        (void)add_static_box(world, {3.0f, 2.0f, 0.5f}, {0.0f, 2.0f, -6.0f});

        Shooter shooter;
        shooter.spawn(world, {0.0f, rest_y(shooter.character), 0.0f});
        shooter.config.automatic = true;
        shooter.config.cooldown_ticks = 3;
        std::vector<gameplay::ShotResult> tape;
        for (int i = 0; i < 40; ++i) {
            const float yaw = static_cast<float>(i) * 0.05f;
            tape.push_back(shooter.tick(trigger(i == 0, true, yaw, 0.1f)));
        }
        return std::pair{tape, shooter.stats};
    };

    const auto [first, first_stats] = run();
    const auto [second, second_stats] = run();

    REQUIRE(first.size() == second.size());
    // Non-vacuity: the tape has to contain both hits and misses, or "identical" is a claim about
    // forty identical no-ops.
    int fired = 0;
    int hit = 0;
    for (std::size_t i = 0; i < first.size(); ++i) {
        fired += first[i].fired ? 1 : 0;
        hit += first[i].did_hit ? 1 : 0;
        CHECK(first[i].fired == second[i].fired);
        CHECK(first[i].did_hit == second[i].did_hit);
        // BIT-identical, not approximately equal: a float compare with an epsilon would pass on a
        // replay that had genuinely drifted.
        CHECK(first[i].shot.direction.x == second[i].shot.direction.x);
        CHECK(first[i].shot.direction.y == second[i].shot.direction.y);
        CHECK(first[i].shot.direction.z == second[i].shot.direction.z);
        CHECK(first[i].hit.distance == second[i].hit.distance);
        CHECK(first[i].hit.child == second[i].hit.child);
    }
    CHECK(fired > 5);
    CHECK(hit > 0);
    CHECK(hit < fired); // it swept off the wall, so some shots missed
    CHECK(first_stats.shots == second_stats.shots);
    CHECK(first_stats.hits == second_stats.hits);
    CHECK(first_stats.misses == second_stats.misses);
}
