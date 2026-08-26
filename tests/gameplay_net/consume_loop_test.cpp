// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <vector>

#include "match_fixture.hpp"

// m12.3's consume-loop proofs: the server turns intent into authoritative motion, and does it
// exactly once per command it consumed.
//
// The three cases worth reading first, because each would fail if the DESIGN were wrong rather
// than merely if the code were:
//
//   "a starved tick does not move…"  — the one-step-per-command commitment, which is what makes
//                                      m12.4's predicted-at-q vs. state-at-q comparison
//                                      like-for-like. A server that helpfully repeated the last
//                                      command would pass every other test in this file.
//   "a client cannot outrun…"        — the rate budget, measured against a negative control that
//                                      shares its seed, its geometry and its tick count.
//   "the frontier never goes back…"  — the ordering invariant under real loss, asserted on the
//                                      pairing itself rather than on a counter that could be
//                                      right for the wrong reason.
using namespace rime;
using namespace rime_test;

TEST_CASE("the server moves an avatar only on the commands it consumed") {
    Match match;
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle();

    REQUIRE(match.server_driver->session_count() == 1);
    REQUIRE(match.spawned.size() == 1);
    const ecs::Entity avatar = match.spawned.front();
    REQUIRE(client.local_player().is_valid());

    const gameplay::CharacterState* state = match.server_state(avatar);
    REQUIRE(state != nullptr);
    const core::Vec3 start = state->position;

    // Walk forward (+move_y is -Z) for a fixed number of ticks, one command per tick.
    constexpr int kTicks = 40;
    std::uint32_t last_sequence = 0;
    for (int i = 0; i < kTicks; ++i) {
        last_sequence = client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
    }

    // Every command was consumed exactly once, and none was invented.
    CHECK(match.gameplay.commands_consumed() == static_cast<std::uint64_t>(kTicks));
    CHECK(match.gameplay.commands_dropped_over_rate() == 0);
    CHECK(match.gameplay.players_unbound() == 0);
    // Non-vacuity: the mover really ran, which is what makes "it moved" a statement about the
    // controller rather than about a transform someone wrote by hand.
    CHECK(match.gameplay.step_stats().casts > 0);

    state = match.server_state(avatar);
    REQUIRE(state != nullptr);
    CHECK(state->position.z < start.z - 1.0f); // it went forward, and it went a long way
    CHECK(state->position.x == doctest::Approx(start.x));

    // The pairing names the newest command the server acted on, and the ack agrees with it.
    const gameplay_net::LastProcessedInput* pairing =
        match.world.get<gameplay_net::LastProcessedInput>(avatar);
    REQUIRE(pairing != nullptr);
    CHECK(pairing->sequence == last_sequence);
    CHECK(match.input.consumed_through(match.server_driver->session_ids().front()) ==
          last_sequence);
}

TEST_CASE("the avatar's transform and its physics body follow the controller") {
    // THE REGRESSION THIS EXISTS FOR, and it shipped green in m12.3 before m12.4's client-side work
    // tripped over it. The controller wrote only `WorldTransform`; `propagate_transforms` runs one
    // step later in the canonical tick order and RECOMPUTES WorldTransform from `LocalTransform`,
    // so the write was discarded every tick by a pass doing exactly its job.
    //
    // It was silent because `CharacterState` — the thing every other case in this file asserts on —
    // stayed perfectly correct. Measured before the fix: state at z = -3.29, WorldTransform at 0,
    // LocalTransform at 0, physics body at 0. The consequences are all one layer out: the kinematic
    // capsule never moves, so the player pushes nothing and debris cannot hit them where they are;
    // and the replicated transform is wrong, so every OTHER client mirrors this avatar standing at
    // its spawn point forever.
    //
    // So this case asserts the handoff itself, not the state, and it is the only one here that
    // reaches for the transform components at all.
    Match match;
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle();
    const ecs::Entity avatar = match.spawned.front();

    for (int i = 0; i < 40; ++i) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
    }

    const gameplay::CharacterState* state = match.server_state(avatar);
    REQUIRE(state != nullptr);
    REQUIRE(state->position.z < -1.0f); // non-vacuity: it really walked somewhere

    // Both transforms carry the pose, and LocalTransform is the one that makes it survive step 3.
    const ecs::WorldTransform* world_tf = match.world.get<ecs::WorldTransform>(avatar);
    const ecs::LocalTransform* local_tf = match.world.get<ecs::LocalTransform>(avatar);
    REQUIRE(world_tf != nullptr);
    REQUIRE(local_tf != nullptr);
    CHECK(world_tf->value.translation.z == doctest::Approx(state->position.z));
    CHECK(local_tf->value.translation.z == doctest::Approx(state->position.z));

    // …and push_in drove the kinematic body there, which is what makes the capsule a real
    // participant in the world rather than a number in a component.
    const physics::RigidBodyHandle* handle = match.world.get<physics::RigidBodyHandle>(avatar);
    REQUIRE(handle != nullptr);
    physics::BodyState body{};
    REQUIRE(match.physics.get_body_state(handle->body, body));
    CHECK(body.position.z == doctest::Approx(state->position.z));

    // The client's mirror agrees, which is the half that matters to every other player.
    const ecs::Entity mirror = client.local_player();
    REQUIRE(mirror.is_valid());
    const ecs::LocalTransform* mirrored = client.world.get<ecs::LocalTransform>(mirror);
    REQUIRE(mirrored != nullptr);
    CHECK(mirrored->value.translation.z < -1.0f);
}

TEST_CASE("a starved tick does not move the avatar, and says so") {
    // THE COMMITMENT: `step_character` runs once per CONSUMED command and not at all otherwise.
    // A server that re-ran the last command on a tick with no input would move the player a tick
    // further than the client predicted, and every packet loss would then produce a phantom
    // correction whose cause is the server's own helpfulness (ADR-0035 §4).
    Match match;
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle();
    const ecs::Entity avatar = match.spawned.front();

    // Ten ticks of walking, so the avatar is genuinely in motion when the input stops.
    for (int i = 0; i < 10; ++i) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
    }
    const std::uint64_t consumed_before = match.gameplay.commands_consumed();
    const std::uint64_t starved_before = match.gameplay.ticks_starved();
    const gameplay::CharacterState moving = *match.server_state(avatar);
    const std::uint32_t pairing_before =
        match.world.get<gameplay_net::LastProcessedInput>(avatar)->sequence;
    CHECK(core::length(core::Vec3{moving.velocity.x, 0.0f, moving.velocity.z}) > 0.5f);

    // Now the client sends NOTHING for ten ticks. The avatar must not advance by so much as a
    // rounding — not slide, not coast, not fall to a lower resting height.
    for (int i = 0; i < 10; ++i) {
        match.tick();
    }

    const gameplay::CharacterState frozen = *match.server_state(avatar);
    CHECK(frozen.position.x == moving.position.x); // bit-identical, not approximately equal
    CHECK(frozen.position.y == moving.position.y);
    CHECK(frozen.position.z == moving.position.z);
    CHECK(frozen.velocity.x == moving.velocity.x);
    CHECK(frozen.velocity.z == moving.velocity.z);

    CHECK(match.gameplay.commands_consumed() == consumed_before);
    CHECK(match.gameplay.ticks_starved() == starved_before + 10);
    // And the pairing did NOT advance: a stale sequence beside an unchanged state is the truth.
    CHECK(match.world.get<gameplay_net::LastProcessedInput>(avatar)->sequence == pairing_before);

    // Resuming input resumes motion — the control that proves the freeze was the absent command
    // and not a wedged controller.
    (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
    match.tick();
    CHECK(match.server_state(avatar)->position.z < frozen.position.z);
}

TEST_CASE("a client cannot outrun the tick by sending faster") {
    // The rate budget, measured against a NEGATIVE CONTROL that differs in exactly one thing: how
    // many commands per tick the client sends. Same seed, same geometry, same tick count, same
    // intent. Without the budget the greedy client would travel ~3x as far, which is a speed hack
    // that costs nothing to attempt.
    constexpr int kTicks = 50;

    const auto run = [](int commands_per_tick) {
        Match match;
        add_ground(match.physics);
        ClientPeer& client = match.add_client();
        match.settle();
        const ecs::Entity avatar = match.spawned.front();
        const core::Vec3 start = match.server_state(avatar)->position;

        for (int i = 0; i < kTicks; ++i) {
            for (int c = 0; c < commands_per_tick; ++c) {
                (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
            }
            match.tick();
        }
        struct Result {
            float distance;
            std::uint64_t consumed;
            std::uint64_t dropped;
        };
        return Result{core::length(match.server_state(avatar)->position - start),
                      match.gameplay.commands_consumed(),
                      match.gameplay.commands_dropped_over_rate()};
    };

    const auto honest = run(1);
    const auto greedy = run(3);

    // The honest client is never throttled: one command a tick is exactly what the allowance
    // refills at, so nothing is ever dropped.
    CHECK(honest.dropped == 0);
    CHECK(honest.consumed == static_cast<std::uint64_t>(kTicks));

    // The greedy one is. Non-vacuity first: if nothing was dropped the comparison below would be
    // measuring two identical runs.
    CHECK(greedy.dropped > 0);
    // It is served at the tick rate plus the one-off burst allowance it had banked before it
    // started spending — never at three commands a tick.
    const std::uint64_t burst = gameplay_net::GameplayServer::Config{}.max_command_burst;
    CHECK(greedy.consumed <= static_cast<std::uint64_t>(kTicks) + burst);
    CHECK(greedy.consumed + greedy.dropped == static_cast<std::uint64_t>(kTicks * 3));

    // …and that is what the distance shows. The greedy client gains at most the burst, not a
    // multiplier: it is comfortably under 1.5x rather than near 3x.
    CHECK(greedy.distance > 0.0f);
    CHECK(greedy.distance < honest.distance * 1.5f);
}

TEST_CASE("a hitching client is fully served, because the allowance saves up") {
    // The other half of the budget, and the reason it is an allowance rather than a hard cap: a
    // client whose packets arrive in bursts because of jitter has done nothing wrong, and dropping
    // its catch-up would turn ordinary network weather into lost input.
    Match match;
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle();

    // Four ticks of silence, then four commands at once. Total: four commands over five ticks.
    for (int i = 0; i < 4; ++i) {
        match.tick();
    }
    const std::uint64_t starved = match.gameplay.ticks_starved();
    CHECK(starved >= 4); // the silence really did starve the loop

    for (int c = 0; c < 4; ++c) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
    }
    match.tick();

    // All four applied, none dropped: the allowance banked during the silence paid for the burst.
    CHECK(match.gameplay.commands_consumed() == 4);
    CHECK(match.gameplay.commands_dropped_over_rate() == 0);
}

TEST_CASE("out-of-contract input is clamped by the server, never trusted") {
    // §1 of input.hpp pointed at the mover: an input is a REQUEST, and the server is free to
    // disbelieve it. A client that asks to move at 1e30 m/s, or with a NaN view angle, must not be
    // able to poison a solve or teleport across the level.
    Match match;
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle();
    const ecs::Entity avatar = match.spawned.front();
    const core::Vec3 start = match.server_state(avatar)->position;

    constexpr int kTicks = 20;
    for (int i = 0; i < kTicks; ++i) {
        replication::InputCommand hostile;
        hostile.move_x = 1.0e30f;
        hostile.move_y = -1.0e30f;
        hostile.yaw = std::numeric_limits<float>::quiet_NaN();
        hostile.pitch = 1.0e9f;
        (void)client.send_input(hostile, match.now_ms);
        match.tick();
    }

    // The sanitizer saw it — a client whose commands need clamping is either buggy or lying, and
    // both are worth a number rather than a silent correction.
    CHECK(match.input.commands_sanitized() > 0);

    const gameplay::CharacterState* state = match.server_state(avatar);
    REQUIRE(state != nullptr);
    CHECK(std::isfinite(state->position.x));
    CHECK(std::isfinite(state->position.y));
    CHECK(std::isfinite(state->position.z));
    CHECK(std::isfinite(state->velocity.x));
    CHECK(std::isfinite(state->velocity.z));

    // Bounded by what the CONFIG allows, not by what the client asked for: 20 ticks of walking is
    // at most 20 * dt * max_speed of ground covered, and the unit-disc clamp means a diagonal is
    // no faster than a cardinal.
    const float travelled = core::length(state->position - start);
    const float ceiling =
        static_cast<float>(kTicks) * kDt * match.character_config.max_speed * 1.05f;
    CHECK(travelled <= ceiling);
}

TEST_CASE("the consumption frontier never goes backwards, even under real loss") {
    // The ordering invariant, asserted on the PAIRING rather than on a counter. `consumed_through`
    // steps over permanent gaps by design (input.hpp), so the property under loss is not "every
    // command is applied" — it is that what the server DID apply arrived in order and the number it
    // publishes never rewinds. A frontier that went backwards would resurrect commands already
    // retired and make m12.4's replay set wrong in the one condition it exists for.
    Match match({/*loss_rate=*/0.25f,
                 /*duplicate_rate=*/0.05f,
                 /*min_latency_ms=*/8,
                 /*max_latency_ms=*/48});
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle(400);
    REQUIRE(client.local_player().is_valid());
    const ecs::Entity avatar = match.spawned.front();

    std::uint32_t previous = 0;
    for (int i = 0; i < 200; ++i) {
        (void)client.send_input(walk(0.35f, 0.9f, 0.2f), match.now_ms);
        match.tick();
        const auto* pairing = match.world.get<gameplay_net::LastProcessedInput>(avatar);
        if (pairing != nullptr) {
            CHECK(pairing->sequence >= previous);
            previous = pairing->sequence;
        }
    }

    // NON-VACUITY, three ways. A loss test in which nothing was lost proves nothing; a gap test in
    // which no gap opened proves less; and a redundancy window that never had to cover anything
    // proves nothing about redundancy.
    CHECK(match.network.packets_dropped() > 0);
    CHECK(match.input.gaps_observed() > 0);
    CHECK(match.input.commands_duplicate() > 0);

    // The frontier still got a long way: loss delays commands, it does not stall the game.
    CHECK(previous > 100);
    CHECK(match.gameplay.commands_consumed() > 100);
    // And loss really cost the non-predicting client ticks of motion — the artefact m12.4 hides.
    CHECK(match.gameplay.ticks_starved() > 0);
}

TEST_CASE("an avatar despawned under a live session stops consuming, loudly") {
    // A kill, a team switch — the session lives on while its avatar does not. The commands must
    // still be DRAINED (an undrained buffer fills, and the receiver then evicts the overflow and
    // advances the frontier over it anyway, which only moves the same loss somewhere harder to
    // see) and they must not be applied to a dead entity.
    Match match;
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle();
    const ecs::Entity avatar = match.spawned.front();

    for (int i = 0; i < 5; ++i) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
    }
    CHECK(match.gameplay.commands_consumed() == 5);

    match.replicator->despawn(avatar); // the disciplined path: retract the id, then destroy
    REQUIRE_FALSE(match.world.is_alive(avatar));

    for (int i = 0; i < 10; ++i) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
    }
    CHECK(match.gameplay.commands_consumed() == 5);         // nothing applied to a corpse
    CHECK(match.gameplay.commands_dropped_no_avatar() > 0); // and the loss is counted
    CHECK(match.gameplay.players_unbound() == 0);           // it is not a setup fault
    // The overflow path was never reached, which is what "still drained" buys.
    CHECK(match.input.commands_dropped_overflow() == 0);
    // The backstop stayed quiet: this despawn went through the proper door.
    CHECK(match.replicator->net_ids_orphaned() == 0);
}
