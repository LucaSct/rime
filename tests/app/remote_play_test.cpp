// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <memory>
#include <vector>

#include "game.hpp"
#include "rime/app/application.hpp"
#include "rime/app/network_input.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/prediction.hpp"
#include "rime/replication/spawn_prediction.hpp"
#include "rime/scene/derive_transforms.hpp"

using namespace rime;

namespace {

struct DelayedBatch {
    int release_tick = 0;
    std::vector<net::Received> messages;
};

// Retain m20.4's exact digest recipe as a measurement of the deliberate Throw-state change.
// This hashes the same authoritative fields before charges, cooldown, target score and facing
// were appended; it never asks a second executable to run a different simulation.
std::uint64_t legacy_hello_digest(ecs::World& world) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto bytes = [&](const void* data, std::size_t size) {
        const auto* first = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i)
            hash = (hash ^ first[i]) * 0x100000001b3ull;
    };
    const auto u64 = [&](std::uint64_t value) { bytes(&value, sizeof value); };
    const auto f32 = [&](float value) { u64(std::bit_cast<std::uint32_t>(value)); };
    const auto vec3 = [&](core::Vec3 value) {
        f32(value.x);
        f32(value.y);
        f32(value.z);
    };
    world.query<hello_game::ReplicatedGameState>().for_each(
        [&](ecs::Entity, const hello_game::ReplicatedGameState& state) {
            u64(state.ticks);
            u64(state.score);
            u64(state.collected0);
            u64(state.collected1);
            u64(state.collected2);
            u64(state.collected3);
            u64(state.collected4);
            vec3(state.player_position);
            vec3(state.crate_position);
            f32(state.crate_rotation.x);
            f32(state.crate_rotation.y);
            f32(state.crate_rotation.z);
            f32(state.crate_rotation.w);
        });
    return hash;
}

} // namespace

TEST_CASE("hello-game remote commands round-trip its action slots") {
    const auto definition = hello_game::definition();
    REQUIRE(definition.network_input.has_value());
    REQUIRE(definition.remote_play.has_value());
    app::ActionState actions{};
    actions.axes[hello_game::kAxisMoveX] = -0.5f;
    actions.axes[hello_game::kAxisMoveZ] = 0.75f;
    actions.held = 1u << hello_game::kButtonQuit;
    actions.pressed = actions.held;
    const auto command = definition.remote_play->to_command(actions);
    const auto again = definition.network_input->to_actions(command);
    CHECK(again.axis(hello_game::kAxisMoveX) == actions.axis(hello_game::kAxisMoveX));
    CHECK(again.axis(hello_game::kAxisMoveZ) == actions.axis(hello_game::kAxisMoveZ));
    CHECK(again.held == actions.held);
    CHECK(again.pressed == actions.pressed);
}

TEST_CASE("owner checkpoints refuse a wrong membership and a truncated wire message") {
    const replication::PredictedEntityId player{7, 3};
    std::vector<std::byte> state{std::byte{4}};
    replication::PredictionHooks hooks{};
    hooks.capture = [&](replication::PredictedEntityId) { return state; };
    hooks.restore = [&](replication::PredictedEntityId, std::span<const std::byte> bytes) {
        state.assign(bytes.begin(), bytes.end());
    };
    hooks.capture_group = [] { return std::vector<std::byte>{}; };
    hooks.restore_group = [](std::span<const std::byte>) {};
    hooks.simulate = [](std::span<const replication::PredictedEntityId>,
                        const replication::InputCommand&,
                        const replication::PredictionStep&) {};
    replication::Reconciler reconciler{std::move(hooks), 1.0 / 60.0};
    reconciler.set_membership(9, {player});

    replication::OwnerCheckpoint checkpoint{};
    checkpoint.server_tick = 4;
    checkpoint.result.server_tick = 4;
    checkpoint.membership_epoch = 10;
    checkpoint.entities.push_back({player, state});
    CHECK_FALSE(reconciler.apply_checkpoint(checkpoint).applied);
    CHECK(reconciler.epoch_mismatch() == 1);

    std::array<std::byte, replication::kCheckpointMaxMessageBytes> bytes{};
    const auto encoded = replication::encode_owner_checkpoint(checkpoint, bytes);
    REQUIRE(encoded);
    replication::OwnerCheckpoint decoded{};
    CHECK(replication::decode_owner_checkpoint(std::span(bytes).first(encoded.bytes_written - 1),
                                               decoded) ==
          replication::CheckpointRefusal::Truncated);
    std::printf("remote_play refused_epoch=%llu refused_truncated=1 corrections=%llu\n",
                static_cast<unsigned long long>(reconciler.epoch_mismatch()),
                static_cast<unsigned long long>(reconciler.corrections()));
}

TEST_CASE("hello-game publishes a convergent mirror and a leading predicted player") {
    app::AppConfig config{};
    config.gpu = false;
    config.tick_hz = 60.0;
    config.worker_threads = 1;
    app::Application application(config);
    auto definition = hello_game::definition();
    REQUIRE(definition.remote_play.has_value());
    REQUIRE(definition.network_input.has_value());
    // This m20.4 reconciler reads base checkpoints only. Disable the optional extension here so
    // the existing proof also guards byte-for-byte legacy publication after the opt-in exists.
    definition.remote_play->spawn_prediction.reset();
    auto& server_world = application.world();
    definition.register_components(server_world);
    definition.remote_play->register_replicated(server_world);
    hello_game::build_arena(server_world);
    scene::derive_world_transforms(server_world, application.jobs());
    hello_game::HelloGame game;
    app::SetupContext setup{server_world, application.jobs(), application.fixed_dt(), {}};
    REQUIRE(game.setup(setup));

    net::ScriptedNetwork network(0xC04Bu, {0.0f, 0.0f, 0, 0});
    const net::Endpoint server_endpoint{0x7F000001u, 7813};
    const net::Endpoint client_endpoint{0x7F000001u, 7814};
    net::Link& server_link = network.add_node(server_endpoint);
    net::Link& client_link = network.add_node(client_endpoint);
    std::uint64_t virtual_now = 0;
    const auto advance = [&] {
        virtual_now += 16;
        network.advance_time(virtual_now);
    };
    std::string error;
    auto server = app::NetworkInputServer::on_link(server_link,
                                                   definition.tick_hz,
                                                   *definition.network_input,
                                                   server_world,
                                                   &*definition.remote_play,
                                                   error);
    REQUIRE_MESSAGE(server != nullptr, error);

    ecs::World mirror_world;
    definition.remote_play->register_replicated(mirror_world);
    replication::ClientReplicator mirror{mirror_world};
    net::NetDriver client{client_link, net::NetDriver::Config{}};
    const auto client_id = client.connect(server_endpoint, virtual_now);
    REQUIRE(client_id.has_value());

    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;
    const auto pump_client = [&] {
        events.clear();
        client.update(virtual_now, events);
        network.advance_time(virtual_now);
        inbox.clear();
        if (auto* session = client.session(*client_id))
            session->drain_received(inbox);
    };
    bool connected = false;
    for (int attempt = 0; attempt < 300 && !connected; ++attempt) {
        advance();
        pump_client();
        for (const auto& event : events)
            connected |= event.kind == net::SessionEvent::Kind::Connected;
        (void)server->before_tick(0, virtual_now);
    }
    REQUIRE(connected);

    const auto& input_profile = *definition.network_input;
    const replication::InputHello hello{
        2,
        2,
        replication::input_schema_hash(input_profile.profile_id,
                                       input_profile.profile_version,
                                       definition.remote_play->fixed_dt)};
    std::array<std::byte, replication::kInputHelloBytes> hello_bytes{};
    const auto hello_size = replication::encode_input_hello(hello, hello_bytes);
    REQUIRE(client.session(*client_id)
                ->send_reliable(std::span(hello_bytes).first(hello_size), virtual_now));
    bool admitted = false;
    for (int attempt = 0; attempt < 300 && !admitted; ++attempt) {
        advance();
        pump_client();
        for (const auto& message : inbox) {
            replication::InputHelloReply reply{};
            if (replication::decode_input_hello_reply(message.bytes, reply)) {
                REQUIRE(reply.refusal == replication::InputHelloRefusal::None);
                admitted = true;
            }
        }
        (void)server->before_tick(0, virtual_now);
    }
    REQUIRE(admitted);

    ecs::World predicted_world;
    definition.remote_play->register_replicated(predicted_world);
    const auto predicted_player = predicted_world.spawn_with(ecs::WorldTransform{});
    const auto predicted_state_entity =
        predicted_world.spawn_with(hello_game::ReplicatedGameState{});
    replication::Reconciler reconciler{
        definition.remote_play->prediction_hooks(predicted_world, predicted_player),
        definition.remote_play->fixed_dt};
    std::deque<DelayedBatch> delayed;
    bool has_membership = false;
    bool checkpoint_has_player = false;
    bool predicted_lead = false;
    std::uint64_t checkpoints = 0;
    std::uint64_t refused_epoch = 0;
    std::uint64_t refused_stale_incarnation = 0;
    std::uint64_t refused_truncated = 0;
    std::uint64_t standalone_results = 0;
    std::vector<float> authoritative_x;
    // Feed one drained batch to every reader. A checkpoint may share the batch with a Spawn, so
    // replication must bind the NetId before the owner lookup tests what the checkpoint named.
    const auto apply_batch = [&](const std::vector<net::Received>& batch) {
        mirror.apply_messages(batch);
        for (const auto& message : batch) {
            replication::InputResult standalone{};
            if (replication::decode_input_result(message.bytes, standalone))
                ++standalone_results;
            replication::OwnerCheckpoint checkpoint{};
            if (message.bytes.empty() ||
                message.bytes[0] != std::byte{replication::kOwnerCheckpointTag})
                continue;
            if (replication::decode_owner_checkpoint(message.bytes, checkpoint) !=
                replication::CheckpointRefusal::None)
                continue;
            REQUIRE(checkpoint.entities.size() == 1);
            const auto id = checkpoint.entities.front().id;
            REQUIRE(checkpoint.server_tick < authoritative_x.size());
            REQUIRE(checkpoint.entities.front().state.size() == 24);
            std::uint32_t x_bits = 0;
            for (unsigned byte = 0; byte < 4; ++byte)
                x_bits |= std::to_integer<std::uint32_t>(checkpoint.entities.front().state[byte])
                          << (byte * 8);
            CHECK(std::bit_cast<float>(x_bits) == authoritative_x[checkpoint.server_tick]);
            const replication::NetId net_id{id.index, id.generation};
            const auto local = mirror.map().resolve(net_id);
            checkpoint_has_player |= local != ecs::kNullEntity &&
                                     local == definition.remote_play->owned_player(mirror_world) &&
                                     checkpoint.entities.front().state.size() == 24;
            if (!has_membership) {
                reconciler.set_membership(checkpoint.membership_epoch, {id});
                has_membership = true;
            }
            const auto outcome = reconciler.apply_checkpoint(checkpoint);
            CHECK(outcome.applied);
            ++checkpoints;
            if (checkpoints == 1) {
                auto wrong = checkpoint;
                ++wrong.membership_epoch;
                ++wrong.server_tick;
                ++wrong.result.server_tick;
                CHECK_FALSE(reconciler.apply_checkpoint(wrong).applied);
                refused_epoch = reconciler.epoch_mismatch();
                auto stale_incarnation = checkpoint;
                ++stale_incarnation.entities.front().id.generation;
                ++stale_incarnation.server_tick;
                ++stale_incarnation.result.server_tick;
                // A reused network index belongs to a different entity. Restoring its bytes
                // would teleport the owner even though the entity count and epoch still match.
                CHECK_FALSE(reconciler.apply_checkpoint(stale_incarnation).applied);
                refused_stale_incarnation = reconciler.incomplete_checkpoints();
                std::array<std::byte, replication::kCheckpointMaxMessageBytes> encoded_bytes{};
                const auto encoded =
                    replication::encode_owner_checkpoint(checkpoint, encoded_bytes);
                REQUIRE(encoded);
                replication::OwnerCheckpoint unused{};
                refused_truncated = replication::decode_owner_checkpoint(
                                        std::span(encoded_bytes).first(encoded.bytes_written - 1),
                                        unused) == replication::CheckpointRefusal::Truncated;
            }
        }
    };

    constexpr int kTicks = 60;
    for (int tick = 0; tick < kTicks; ++tick) {
        advance();
        replication::InputCommand command{};
        command.sequence = static_cast<std::uint32_t>(tick + 1);
        command.move_x = 1.0f;
        if (has_membership)
            reconciler.predict(command);
        std::array<std::byte, 4 + replication::kInputV2CommandBytes> command_bytes{};
        const auto encoded = replication::encode_input_v2(std::span(&command, 1), command_bytes);
        REQUIRE(encoded > 0);
        REQUIRE(client.session(*client_id)
                    ->send_unreliable(std::span(command_bytes).first(encoded),
                                      virtual_now,
                                      app::kRemoteInputCommandsStream));
        pump_client();
        std::vector<net::Received> arrivals = inbox;
        const auto actions = server->before_tick(static_cast<std::uint64_t>(tick), virtual_now);
        server_world.advance_version();
        app::TickContext context{
            server_world, application.fixed_dt(), static_cast<std::uint64_t>(tick), actions};
        game.fixed_tick(context);
        authoritative_x.push_back(game.player_position().x);
        server->after_tick(static_cast<std::uint64_t>(tick));
        network.advance_time(virtual_now);
        pump_client();
        arrivals.insert(arrivals.end(), inbox.begin(), inbox.end());
        // The transport remains live for acks, while application delivery waits exactly three
        // simulation ticks. That makes a predicted lead measurable without wall-clock luck.
        if (!arrivals.empty())
            delayed.push_back({tick + 3, std::move(arrivals)});
        while (!delayed.empty() && delayed.front().release_tick <= tick) {
            apply_batch(delayed.front().messages);
            delayed.pop_front();
        }
        mirror.send_ack(client, virtual_now);
        if (has_membership) {
            const auto* mirror_state = [&]() -> const hello_game::ReplicatedGameState* {
                const hello_game::ReplicatedGameState* result = nullptr;
                mirror_world.query<hello_game::ReplicatedGameState>().for_each(
                    [&](ecs::Entity, const hello_game::ReplicatedGameState& state) {
                        result = &state;
                    });
                return result;
            }();
            const auto* predicted_state =
                predicted_world.get<hello_game::ReplicatedGameState>(predicted_state_entity);
            if (mirror_state && predicted_state)
                predicted_lead |=
                    predicted_state->player_position.x > mirror_state->player_position.x;
        }
    }
    for (int drain = 0; drain < 40; ++drain) {
        advance();
        (void)server->before_tick(kTicks, virtual_now);
        pump_client();
        if (!inbox.empty())
            delayed.push_back({kTicks + drain + 3, inbox});
        while (!delayed.empty() && delayed.front().release_tick <= kTicks + drain) {
            apply_batch(delayed.front().messages);
            delayed.pop_front();
        }
        mirror.send_ack(client, virtual_now);
    }
    CHECK(checkpoints > 0);
    CHECK(standalone_results > 0);
    CHECK(mirror.deltas_applied() > 0);
    CHECK(server->counters().replication_acks > 0);
    CHECK(checkpoint_has_player);
    CHECK(predicted_lead);
    CHECK(refused_epoch == 1);
    CHECK(refused_stale_incarnation == 1);
    CHECK(refused_truncated == 1);
    CHECK(definition.remote_play->digest(mirror_world) ==
          definition.remote_play->digest(server_world));
    CHECK(definition.remote_play->digest(server_world) != 0);
    CHECK(legacy_hello_digest(server_world) != definition.remote_play->digest(server_world));
    std::printf("remote_play checkpoints=%llu corrections=%llu mirror_digest=%016llx "
                "server_digest=%016llx legacy_digest=%016llx\n",
                static_cast<unsigned long long>(checkpoints),
                static_cast<unsigned long long>(reconciler.corrections()),
                static_cast<unsigned long long>(definition.remote_play->digest(mirror_world)),
                static_cast<unsigned long long>(definition.remote_play->digest(server_world)),
                static_cast<unsigned long long>(legacy_hello_digest(server_world)));
    if (auto* session = client.session(*client_id))
        session->disconnect();
    for (int attempt = 0; attempt < 100 && server->counters().clients_left == 0; ++attempt) {
        advance();
        pump_client();
        (void)server->before_tick(kTicks, virtual_now);
    }
    CHECK(server->counters().clients_left == 1);
    CHECK(server->counters().replication_clients_released == 1);
}

TEST_CASE("Throw spends three charges, sweeps a target, and never fires a stand-in") {
    const auto exercise = [](core::Vec3 target) {
        app::AppConfig config{};
        config.gpu = false;
        config.worker_threads = 1;
        app::Application application(config);
        auto& world = application.world();
        hello_game::register_components(world);
        hello_game::build_arena(world);
        scene::derive_world_transforms(world, application.jobs());
        hello_game::HelloGame game;
        game.set_throw_target(target);
        app::SetupContext setup{world, application.jobs(), 1.0 / 60.0, {}};
        REQUIRE(game.setup(setup));
        std::uint32_t sequence = 1;
        std::uint32_t launches = 0, refusals = 0, ends = 0;
        const auto step = [&](std::uint64_t tick, bool press, bool applied) {
            app::ActionState input{};
            if (press)
                input.pressed = 1u << hello_game::kButtonThrow;
            input.held = 1u << hello_game::kButtonThrow;
            std::vector<app::SpawnReport> reports;
            app::TickContext context{world, 1.0 / 60.0, tick, input};
            context.command = app::CommandProvenance{91, sequence++, applied};
            context.spawns = &reports;
            world.advance_version();
            game.fixed_tick(context);
            for (const auto& report : reports) {
                launches += report.kind == app::SpawnReport::Kind::Launched;
                refusals += report.kind == app::SpawnReport::Kind::Refused;
                ends += report.kind == app::SpawnReport::Kind::Ended;
                if (report.kind == app::SpawnReport::Kind::Ended)
                    world.despawn(report.entity); // stand in for the server's replicator
            }
            return reports;
        };
        const auto first = step(0, true, true);
        REQUIRE(first.size() == 1);
        CHECK(first[0].kind == app::SpawnReport::Kind::Launched);
        const auto live = step(1, true, true);
        REQUIRE(live.size() == 1);
        CHECK(live[0].reason == 3);
        const auto stand_in = step(2, true, false);
        CHECK(stand_in.empty());
        for (std::uint64_t tick = 3; tick < 45; ++tick)
            (void)step(tick, false, true);
        const auto cooling = step(45, true, true);
        REQUIRE(cooling.size() == 1);
        CHECK(cooling[0].reason == 2);
        for (std::uint64_t tick = 46; tick < 60; ++tick)
            (void)step(tick, false, true);
        const auto second = step(60, true, true);
        REQUIRE(second.size() == 1);
        CHECK(second[0].kind == app::SpawnReport::Kind::Launched);
        for (std::uint64_t tick = 61; tick < 120; ++tick)
            (void)step(tick, false, true);
        const auto third = step(120, true, true);
        REQUIRE(third.size() == 1);
        CHECK(third[0].kind == app::SpawnReport::Kind::Launched);
        for (std::uint64_t tick = 121; tick < 180; ++tick)
            (void)step(tick, false, true);
        const auto empty = step(180, true, true);
        REQUIRE(empty.size() == 1);
        CHECK(empty[0].reason == 1);
        std::uint32_t score = 0, charges = 99;
        world.query<hello_game::ReplicatedGameState>().for_each(
            [&](ecs::Entity, const hello_game::ReplicatedGameState& state) {
                score = state.target_score;
                charges = state.throw_charges;
            });
        CHECK(launches == 3);
        CHECK(refusals == 3);
        CHECK(ends == 3);
        CHECK(charges == 0);
        return score;
    };
    // The target centre sits between two fixed-step endpoints, with a small radius. Moving it
    // sideways is a control that proves a point came from the segment and not from every launch.
    CHECK(exercise({0.0f, 0.9f, -2.55f}) == 3);
    CHECK(exercise({2.0f, 0.9f, -2.55f}) == 0);
}

TEST_CASE("spawn outcomes stay in checkpoints until an applied ack") {
    app::AppConfig config{};
    config.gpu = false;
    config.worker_threads = 1;
    app::Application application(config);
    auto definition = hello_game::definition();
    REQUIRE(definition.remote_play.has_value());
    REQUIRE(definition.remote_play->spawn_prediction.has_value());
    auto& world = application.world();
    definition.register_components(world);
    definition.remote_play->register_replicated(world);
    hello_game::build_arena(world);
    scene::derive_world_transforms(world, application.jobs());
    hello_game::HelloGame game;
    app::SetupContext setup{world, application.jobs(), 1.0 / 60.0, {}};
    REQUIRE(game.setup(setup));
    net::ScriptedNetwork network(0xE507u, {0.0f, 0.0f, 0, 0});
    const net::Endpoint server_endpoint{0x7F000001u, 7823};
    const net::Endpoint client_endpoint{0x7F000001u, 7824};
    net::Link& server_link = network.add_node(server_endpoint);
    net::Link& client_link = network.add_node(client_endpoint);
    std::string error;
    auto server = app::NetworkInputServer::on_link(server_link,
                                                   definition.tick_hz,
                                                   *definition.network_input,
                                                   world,
                                                   &*definition.remote_play,
                                                   error);
    REQUIRE_MESSAGE(server != nullptr, error);
    net::NetDriver client{client_link, net::NetDriver::Config{}};
    std::uint64_t now = 0;
    const auto id = client.connect(server_endpoint, now);
    REQUIRE(id.has_value());
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;
    const auto client_pump = [&] {
        events.clear();
        client.update(now, events);
        network.advance_time(now);
        inbox.clear();
        if (auto* session = client.session(*id))
            session->drain_received(inbox);
    };
    const auto advance = [&] {
        now += 16;
        network.advance_time(now);
    };
    bool connected = false;
    for (int attempt = 0; attempt < 300 && !connected; ++attempt) {
        advance();
        client_pump();
        for (const auto& e : events)
            connected |= e.kind == net::SessionEvent::Kind::Connected;
        (void)server->before_tick(0, now);
    }
    REQUIRE(connected);
    const replication::InputHello hello{
        2, 2, app::input_schema_identity(*definition.network_input, &*definition.remote_play)};
    std::array<std::byte, replication::kInputHelloBytes> hello_bytes{};
    const auto hello_size = replication::encode_input_hello(hello, hello_bytes);
    REQUIRE(client.session(*id)->send_reliable(std::span(hello_bytes).first(hello_size), now));
    bool admitted = false;
    for (int attempt = 0; attempt < 300 && !admitted; ++attempt) {
        advance();
        client_pump();
        for (const auto& message : inbox) {
            replication::InputHelloReply reply{};
            if (replication::decode_input_hello_reply(message.bytes, reply)) {
                CHECK(reply.refusal == replication::InputHelloRefusal::None);
                admitted = true;
            }
        }
        (void)server->before_tick(0, now);
    }
    REQUIRE(admitted);
    const auto incarnation = client.session(*id)->incarnation();
    replication::InputCommand input{};
    input.sequence = 1;
    input.held = 1u << hello_game::kButtonThrow;
    input.pressed = input.held;
    std::array<std::byte, replication::kInputV2HeaderBytes + replication::kInputV2CommandBytes>
        input_bytes{};
    const auto input_size = replication::encode_input_v2(std::span(&input, 1), input_bytes);
    REQUIRE(client.session(*id)->send_unreliable(
        std::span(input_bytes).first(input_size), now, app::kRemoteInputCommandsStream));
    advance();
    client_pump();
    const auto selected = server->before_tick(0, now);
    REQUIRE(server->command_provenance().has_value());
    CHECK(server->command_provenance()->incarnation == incarnation);
    CHECK(server->command_provenance()->sequence == 1);
    CHECK(server->command_provenance()->applied);
    CHECK(selected.was_pressed(hello_game::kButtonThrow));
    (void)server->before_tick(0, now);
    REQUIRE(server->command_provenance().has_value());
    CHECK_FALSE(server->command_provenance()->applied);
    CHECK_FALSE(server->before_tick(0, now).was_pressed(hello_game::kButtonThrow));
    const replication::SpawnKey key1{incarnation, 1, 0};
    const replication::SpawnKey key2{incarnation, 2, 0};
    core::Transform transform{};
    const ecs::Entity projectile =
        world.spawn_with(ecs::LocalTransform{transform},
                         ecs::WorldTransform{transform},
                         hello_game::ThrownProjectile{{0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -18.0f}, 0});
    const auto publish = [&](std::uint64_t tick, std::vector<app::SpawnReport> reports) {
        advance();
        client_pump();
        (void)server->before_tick(tick, now);
        server->after_tick(tick, reports);
        // after_tick queues the checkpoint; the next transport pump flushes it to the link.
        advance();
        (void)server->before_tick(tick + 1, now);
        client_pump();
        replication::SpawnCheckpoint checkpoint{};
        bool found = false;
        for (const auto& message : inbox) {
            if (message.bytes.empty() ||
                message.bytes[0] != std::byte{replication::kOwnerCheckpointTag})
                continue;
            CHECK(replication::decode_spawn_checkpoint(message.bytes, checkpoint));
            found = true;
        }
        REQUIRE(found);
        REQUIRE(checkpoint.spawns.has_value());
        CHECK(checkpoint.base.server_tick == tick);
        return checkpoint;
    };
    const auto launched = publish(1, {{app::SpawnReport::Kind::Launched, key1, projectile, 0}});
    CHECK(launched.base.entities.size() == 2);
    CHECK(launched.base.membership_epoch == 2);
    REQUIRE(launched.spawns->bindings.size() == 1);
    CHECK(launched.spawns->bindings[0].key == key1);
    REQUIRE(launched.spawns->outcomes.size() == 1);
    CHECK(launched.spawns->outcomes[0].kind == replication::SpawnOutcomeKind::Accepted);
    const auto rejected = publish(2, {{app::SpawnReport::Kind::Refused, key2, {}, 3}});
    REQUIRE(rejected.spawns->outcomes.size() == 2);
    CHECK(rejected.spawns->outcomes[1].kind == replication::SpawnOutcomeKind::Rejected);
    CHECK(rejected.spawns->outcomes[1].reason == 3);
    const auto ended = publish(3, {{app::SpawnReport::Kind::Ended, key1, projectile, 0}});
    CHECK(ended.base.entities.size() == 1);
    CHECK(ended.base.membership_epoch == 3);
    CHECK(ended.spawns->bindings.empty());
    CHECK(ended.spawns->outcomes[0].kind == replication::SpawnOutcomeKind::AcceptedEnded);
    std::array<std::byte, replication::kCheckpointMaxMessageBytes> encoded{};
    const auto size = replication::encode_spawn_checkpoint(ended.base, *ended.spawns, encoded);
    REQUIRE(size);
    CHECK(size.bytes_written <= 1200);
    std::array<std::byte, replication::kSpawnOutcomeAckBytes> ack_bytes{};
    const auto ack_size = replication::encode_spawn_outcome_ack({key2}, ack_bytes);
    REQUIRE(client.session(*id)->send_reliable(std::span(ack_bytes).first(ack_size), now));
    const auto acked = publish(4, {});
    CHECK(acked.spawns->outcomes.empty());
    CHECK(acked.spawns->acknowledged_through == key2);
    CHECK(server->counters().spawn_acks_applied == 1);
    // Bad frontiers cannot strengthen what the peer is known to hold.
    const replication::SpawnKey ahead{incarnation, 3, 0};
    (void)replication::encode_spawn_outcome_ack({ahead}, ack_bytes);
    REQUIRE(client.session(*id)->send_reliable(ack_bytes, now));
    (void)publish(5, {});
    CHECK(server->counters().spawn_ack_ahead == 1);
    const replication::SpawnKey wrong{incarnation + 1, 2, 0};
    (void)replication::encode_spawn_outcome_ack({wrong}, ack_bytes);
    REQUIRE(client.session(*id)->send_reliable(ack_bytes, now));
    (void)publish(6, {});
    CHECK(server->counters().spawn_ack_wrong_incarnation == 1);
    (void)replication::encode_spawn_outcome_ack({key2}, ack_bytes);
    REQUIRE(client.session(*id)->send_reliable(ack_bytes, now));
    (void)publish(7, {});
    CHECK(server->counters().spawn_ack_stale == 1);
    REQUIRE(client.session(*id)->send_reliable(std::span(ack_bytes).first(ack_size - 1), now));
    (void)publish(8, {});
    CHECK(server->counters().spawn_ack_malformed == 1);
    std::vector<app::SpawnReport> burst;
    for (std::uint32_t sequence = 3; sequence <= 19; ++sequence)
        burst.push_back({app::SpawnReport::Kind::Refused, {incarnation, sequence, 0}, {}, 2});
    const auto deferred = publish(9, burst);
    REQUIRE(deferred.spawns->outcomes.size() == replication::kSpawnExtensionMaxOutcomes);
    CHECK(deferred.spawns->outcomes.front().key.sequence == 3);
    CHECK(deferred.spawns->outcomes.back().key.sequence == 18);
    CHECK(server->counters().spawn_outcomes_deferred > 0);
    (void)replication::encode_spawn_outcome_ack({{incarnation, 18, 0}}, ack_bytes);
    REQUIRE(client.session(*id)->send_reliable(ack_bytes, now));
    const auto remainder = publish(10, {});
    REQUIRE(remainder.spawns->outcomes.size() == 1);
    CHECK(remainder.spawns->outcomes[0].key.sequence == 19);
    CHECK(server->counters().spawn_accepted == 1);
    CHECK(server->counters().spawn_rejected == 18);
    CHECK(server->counters().spawn_accepted_ended == 1);
}

TEST_CASE("spawn negotiation leaves the absent profile identity and checkpoint bytes unchanged") {
    auto definition = hello_game::definition();
    REQUIRE(definition.network_input.has_value());
    REQUIRE(definition.remote_play.has_value());
    const auto extended =
        app::input_schema_identity(*definition.network_input, &*definition.remote_play);
    definition.remote_play->spawn_prediction.reset();
    const auto legacy =
        app::input_schema_identity(*definition.network_input, &*definition.remote_play);
    CHECK(legacy == replication::input_schema_hash(definition.network_input->profile_id,
                                                   definition.network_input->profile_version,
                                                   definition.remote_play->fixed_dt));
    CHECK(extended != legacy);
    definition.remote_play->spawn_prediction =
        app::GameDefinition::RemotePlayProfile::SpawnPredictionProfile{2, nullptr};
    CHECK(app::input_schema_identity(*definition.network_input, &*definition.remote_play) !=
          extended);
    replication::OwnerCheckpoint base{};
    base.server_tick = 8;
    base.result.server_tick = 8;
    base.membership_epoch = 1;
    base.entities.push_back({{1, 1}, {std::byte{1}}});
    std::array<std::byte, replication::kCheckpointMaxMessageBytes> original{};
    std::array<std::byte, replication::kCheckpointMaxMessageBytes> again{};
    const auto old_size = replication::encode_owner_checkpoint(base, original);
    const auto absent_size = replication::encode_owner_checkpoint(base, again);
    REQUIRE(old_size);
    REQUIRE(absent_size);
    CHECK(old_size.bytes_written == absent_size.bytes_written);
    CHECK(std::equal(original.begin(), original.begin() + old_size.bytes_written, again.begin()));
    replication::SpawnCheckpointExtension worst{};
    for (std::uint32_t sequence = 1; sequence <= 16; ++sequence)
        worst.outcomes.push_back(
            {{1, sequence, 0}, replication::SpawnOutcomeKind::Rejected, {}, 1});
    const auto bounded = replication::encode_spawn_checkpoint(base, worst, again);
    REQUIRE(bounded);
    CHECK(bounded.bytes_written <= 1200);
}
