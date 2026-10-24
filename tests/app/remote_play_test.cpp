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
#include "rime/scene/derive_transforms.hpp"

using namespace rime;

namespace {

struct DelayedBatch {
    int release_tick = 0;
    std::vector<net::Received> messages;
};

} // namespace

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
        replication::input_schema_hash(input_profile.profile_id, input_profile.profile_version)};
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
    CHECK(refused_truncated == 1);
    CHECK(definition.remote_play->digest(mirror_world) ==
          definition.remote_play->digest(server_world));
    CHECK(definition.remote_play->digest(server_world) != 0);
    std::printf("remote_play checkpoints=%llu corrections=%llu mirror_digest=%016llx "
                "server_digest=%016llx\n",
                static_cast<unsigned long long>(checkpoints),
                static_cast<unsigned long long>(reconciler.corrections()),
                static_cast<unsigned long long>(definition.remote_play->digest(mirror_world)),
                static_cast<unsigned long long>(definition.remote_play->digest(server_world)));
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
