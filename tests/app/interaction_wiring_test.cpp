// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m20.6b (ADR-0106): the interaction group wired through hello-game -- server-declared membership
// with hysteresis, the crate's full body state in the owner checkpoint, and the client's
// prediction physics world rebuilt on every restore. In-process: the real server pump, a scripted
// network, and the client's SpawnReconciler with the game's decorated hooks.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "game.hpp"
#include "rime/app/application.hpp"
#include "rime/app/network_input.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/contention_context.hpp"
#include "rime/replication/interaction.hpp"
#include "rime/replication/prediction.hpp"
#include "rime/replication/spawn_prediction.hpp"
#include "rime/scene/derive_transforms.hpp"

using namespace rime;

namespace {

using InteractionProfile = app::GameDefinition::RemotePlayProfile::InteractionProfile;

constexpr replication::PredictedEntityId kPlayerId{1, 1};
constexpr replication::PredictedEntityId kCrateId{2, 1};
constexpr double kDt = 1.0 / 60.0;

app::AppConfig server_config() {
    app::AppConfig c{};
    c.gpu = false;
    c.tick_hz = 60.0;
    c.worker_threads = 1;
    return c;
}

const InteractionProfile& profile() {
    static const app::GameDefinition definition = hello_game::definition_with_interaction();
    return *definition.remote_play->interaction;
}

// The authoritative hello-game: the same fixed_tick a dedicated server runs.
struct Server {
    app::Application application{server_config()};
    app::GameDefinition definition;
    hello_game::HelloGame game;
    ecs::Entity player{};
    ecs::Entity crate{};
    replication::PredictionHooks hooks;
    std::uint64_t tick = 0;

    explicit Server(app::GameDefinition def = hello_game::definition_with_interaction())
        : definition(std::move(def)) {
        auto& world = application.world();
        definition.register_components(world);
        hello_game::build_arena(world);
        scene::derive_world_transforms(world, application.jobs());
        app::SetupContext setup{world, application.jobs(), kDt, {}};
        REQUIRE(game.setup(setup));
        player = definition.remote_play->owned_player(world);
        crate = profile().object(world);
        hooks = definition.remote_play->prediction_hooks(world, player);
    }

    ecs::World& world() { return application.world(); }

    // An offline tick: no network, no provenance.
    void step(const replication::InputCommand& command) {
        world().advance_version();
        app::TickContext context{
            world(), kDt, tick++, definition.network_input->to_actions(command)};
        game.fixed_tick(context);
    }

    [[nodiscard]] std::vector<std::byte> crate_bytes() { return profile().capture(world(), crate); }

    [[nodiscard]] const hello_game::CrateBody& crate_body() {
        return *world().get<hello_game::CrateBody>(crate);
    }

    // The sample the server's policy reads, through the game's own profile function.
    [[nodiscard]] replication::InteractionSample sample() {
        return profile().sample(world(), player, crate);
    }
};

// One client prediction world with the game's decorated hooks, as network_client builds it.
struct Client {
    ecs::World world;
    ecs::Entity player{};
    InteractionProfile::ClientHooks hooks;

    Client(const app::GameDefinition& definition,
           InteractionProfile::ClientOptions options,
           replication::PredictedEntityId crate_id = kCrateId) {
        definition.remote_play->register_replicated(world);
        player = world.spawn_with(ecs::WorldTransform{}, hello_game::PlayerState{});
        auto inner = definition.remote_play->spawn_prediction->client_hooks(
            world, player, 1, [](const replication::SpawnKey&, std::span<const std::byte>) {
                return std::optional<replication::PredictedEntityId>{};
            });
        hooks = profile().client_hooks(world, player, crate_id, std::move(inner), options);
    }

    void restore(const std::vector<std::byte>& group,
                 const std::vector<std::byte>& player_bytes,
                 const std::vector<std::byte>& crate_bytes) {
        hooks.hooks.prediction.restore_group(group);
        hooks.hooks.prediction.restore(kPlayerId, player_bytes);
        hooks.hooks.prediction.restore(kCrateId, crate_bytes);
    }

    void simulate(const replication::InputCommand& command, std::uint64_t sim_tick) {
        const std::array members{kPlayerId, kCrateId};
        hooks.hooks.prediction.simulate(members, command, {sim_tick, kDt, true});
    }

    [[nodiscard]] std::vector<std::byte> crate_bytes() {
        return hooks.hooks.prediction.capture(kCrateId);
    }
};

float read_f32(std::span<const std::byte> bytes, std::size_t index) {
    std::uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i)
        bits |= std::to_integer<std::uint32_t>(bytes[index * 4 + i]) << (8 * i);
    return std::bit_cast<float>(bits);
}

core::Vec3 velocity_of(std::span<const std::byte> bytes) {
    REQUIRE(bytes.size() == hello_game::kCrateStateBytes);
    return {read_f32(bytes, 7), read_f32(bytes, 8), read_f32(bytes, 9)};
}

replication::InputCommand walk(std::uint32_t sequence, float move_z) {
    replication::InputCommand command{};
    command.sequence = sequence;
    command.move_y = move_z; // the game's z: -1 walks from the start toward the crate
    return command;
}

// The real server pump on a scripted network, a world-stream mirror, and the client's
// SpawnReconciler with the game's decorated interaction hooks, seeded as network_client seeds it.
struct Rig {
    Server server;
    net::ScriptedNetwork network;
    net::Link& server_link;
    net::Link& client_link;
    net::Endpoint server_endpoint;
    std::unique_ptr<app::NetworkInputServer> pump;
    net::NetDriver client;
    std::optional<net::SessionId> id;
    ecs::World mirror_world;
    std::unique_ptr<replication::ClientReplicator> mirror;
    std::uint64_t now = 0;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;
    std::uint32_t sequence = 0;

    ecs::World predicted_world;
    ecs::Entity predicted_player{};
    replication::PresentationLedger ledger;
    std::optional<InteractionProfile::ClientHooks> hooks;
    std::unique_ptr<replication::SpawnReconciler> reconciler;
    replication::PredictedEntityId crate_id{};

    explicit Rig(std::uint16_t port,
                 app::GameDefinition def = hello_game::definition_with_interaction())
        : server(std::move(def)), network(0x1A7Eu + port, {0.0f, 0.0f, 0, 0}),
          server_link(network.add_node({0x7F000001u, port})),
          client_link(network.add_node({0x7F000001u, static_cast<std::uint16_t>(port + 1)})),
          server_endpoint{0x7F000001u, port}, client(client_link, net::NetDriver::Config{}) {
        server.definition.remote_play->register_replicated(mirror_world);
        mirror = std::make_unique<replication::ClientReplicator>(mirror_world);
        std::string error;
        pump = app::NetworkInputServer::on_link(server_link,
                                                server.definition.tick_hz,
                                                *server.definition.network_input,
                                                server.world(),
                                                &*server.definition.remote_play,
                                                error);
        REQUIRE_MESSAGE(pump != nullptr, error);
        id = client.connect(server_endpoint, 0);
        REQUIRE(id);
    }

    void pump_client() {
        now += 16;
        network.advance_time(now);
        events.clear();
        client.update(now, events);
        network.advance_time(now);
        inbox.clear();
        if (auto* session = client.session(*id))
            session->drain_received(inbox);
        mirror->apply_messages(inbox);
    }

    void connect() {
        bool connected = false;
        for (int i = 0; i < 300 && !connected; ++i) {
            pump_client();
            for (const auto& event : events)
                connected |= event.kind == net::SessionEvent::Kind::Connected;
            (void)pump->before_tick(0, now);
        }
        REQUIRE(connected);
        const replication::InputHello hello{
            2,
            2,
            app::input_schema_identity(*server.definition.network_input,
                                       &*server.definition.remote_play)};
        std::array<std::byte, replication::kInputHelloBytes> bytes{};
        const auto size = replication::encode_input_hello(hello, bytes);
        REQUIRE(client.session(*id)->send_reliable(std::span(bytes).first(size), now));
        bool admitted = false;
        for (int i = 0; i < 300 && !admitted; ++i) {
            pump_client();
            for (const auto& message : inbox) {
                replication::InputHelloReply reply{};
                if (replication::decode_input_hello_reply(message.bytes, reply)) {
                    REQUIRE(reply.refusal == replication::InputHelloRefusal::None);
                    admitted = true;
                }
            }
            (void)pump->before_tick(0, now);
        }
        REQUIRE(admitted);
    }

    // One server tick driven by `command`; returns the checkpoints the client received.
    std::vector<replication::SpawnCheckpoint>
    server_tick(const replication::InputCommand& command) {
        std::array<std::byte, replication::kInputV2HeaderBytes + replication::kInputV2CommandBytes>
            bytes{};
        const auto size = replication::encode_input_v2(std::span(&command, 1), bytes);
        REQUIRE(client.session(*id)->send_unreliable(
            std::span(bytes).first(size), now, app::kRemoteInputCommandsStream));
        pump_client();
        const auto tick = server.tick++;
        const auto actions = pump->before_tick(tick, now);
        std::vector<app::SpawnReport> reports;
        app::TickContext context{server.world(), kDt, tick, actions};
        context.command = pump->command_provenance();
        context.spawns = &reports;
        server.world().advance_version();
        server.game.fixed_tick(context);
        pump->after_tick(tick, reports);
        pump_client();
        std::vector<replication::SpawnCheckpoint> out;
        for (const auto& message : inbox) {
            replication::SpawnCheckpoint checkpoint{};
            if (replication::decode_spawn_checkpoint(message.bytes, checkpoint))
                out.push_back(std::move(checkpoint));
        }
        mirror->send_ack(client, now);
        return out;
    }

    // Seed the client's prediction from one checkpoint, as network_client does.
    void seed(const replication::SpawnCheckpoint& first, bool skip_prepare = false) {
        const auto mirror_player = server.definition.remote_play->owned_player(mirror_world);
        REQUIRE(mirror_player != ecs::kNullEntity);
        predicted_player = server.definition.remote_play->seed_prediction(
            mirror_world, predicted_world, mirror_player);
        const auto net_id = mirror->map().net_id_of(profile().object(mirror_world));
        REQUIRE(net_id);
        crate_id = {net_id->index, net_id->generation};
        auto inner = server.definition.remote_play->spawn_prediction->client_hooks(
            predicted_world,
            predicted_player,
            client.session(*id)->incarnation(),
            [this](const replication::SpawnKey& key, std::span<const std::byte> state) {
                return reconciler->spawn_provisional(key, state);
            });
        hooks = profile().client_hooks(predicted_world, predicted_player, crate_id, inner, {});
        if (skip_prepare)
            hooks->hooks.prediction.prepare_checkpoint = {}; // sensitivity control
        reconciler =
            std::make_unique<replication::SpawnReconciler>(hooks->hooks.prediction,
                                                           hooks->hooks.spawn,
                                                           kDt,
                                                           client.session(*id)->incarnation(),
                                                           &ledger);
        std::vector<replication::PredictedEntityId> members;
        for (const auto& member : first.base.entities)
            members.push_back(member.id);
        reconciler->set_membership(first.base.membership_epoch, std::move(members));
        REQUIRE(reconciler->apply_checkpoint(first.base, *first.spawns).base.applied);
    }

    [[nodiscard]] bool crate_in_roster() const {
        const auto roster = reconciler->roster();
        return std::find(roster.begin(), roster.end(), crate_id) != roster.end();
    }

    [[nodiscard]] std::span<const std::byte>
    crate_bytes_in(const replication::SpawnCheckpoint& checkpoint) const {
        for (const auto& entity : checkpoint.base.entities)
            if (entity.id == crate_id)
                return entity.state;
        return {};
    }
};

} // namespace

TEST_CASE("two-phase v1 restore matches the legacy world after each replay") {
    Server server;
    Client staged(server.definition, {});
    Client legacy(server.definition, {});
    legacy.hooks.hooks.prediction.prepare_checkpoint = {}; // timing control
    replication::Reconciler staged_reconciler(staged.hooks.hooks.prediction, kDt);
    replication::Reconciler legacy_reconciler(legacy.hooks.hooks.prediction, kDt);
    staged_reconciler.set_membership(4, {kPlayerId, kCrateId});
    legacy_reconciler.set_membership(4, {kPlayerId, kCrateId});
    const auto checkpoint = [&](std::uint32_t retired) {
        replication::OwnerCheckpoint value{};
        value.server_tick = server.tick;
        value.membership_epoch = 4;
        value.result.server_tick = server.tick;
        value.result.any_retired = retired != 0;
        value.result.retired_through = retired;
        value.result.applied_mask[0] = retired != 0 ? 1 : 0;
        value.entities.push_back({kPlayerId, server.hooks.capture(kPlayerId)});
        value.entities.push_back({kCrateId, server.crate_bytes()});
        value.group_state = server.hooks.capture_group();
        return value;
    };
    REQUIRE(staged_reconciler.apply_checkpoint(checkpoint(0)).applied);
    REQUIRE(legacy_reconciler.apply_checkpoint(checkpoint(0)).applied);
    std::deque<replication::OwnerCheckpoint> delayed;
    for (std::uint32_t tick = 1; tick <= 160; ++tick) {
        const auto command = walk(tick, tick <= 45 ? -1.0f : (tick <= 80 ? 1.0f : 0.0f));
        staged_reconciler.predict(command);
        legacy_reconciler.predict(command);
        CHECK(staged.crate_bytes() == legacy.crate_bytes());
        server.step(command);
        delayed.push_back(checkpoint(tick));
        if (delayed.size() > 4) {
            const auto answer = std::move(delayed.front());
            delayed.pop_front();
            const auto staged_result = staged_reconciler.apply_checkpoint(answer);
            const auto legacy_result = legacy_reconciler.apply_checkpoint(answer);
            REQUIRE(staged_result.applied);
            REQUIRE(legacy_result.applied);
            CHECK(staged_result.replayed == 4);
            CHECK(legacy_result.replayed == 4);
        }
        CHECK(staged.crate_bytes() == legacy.crate_bytes());
        CHECK(staged.hooks.hooks.prediction.capture(kPlayerId) ==
              legacy.hooks.hooks.prediction.capture(kPlayerId));
    }
}

TEST_CASE("interaction accepts a valid v2 crate context and restores its v1 body state") {
    Rig rig(7895);
    rig.connect();
    auto first = rig.server_tick(walk(++rig.sequence, 0.0f));
    REQUIRE_FALSE(first.empty());
    rig.seed(first.back());
    auto second = rig.server_tick(walk(++rig.sequence, 0.0f));
    REQUIRE_FALSE(second.empty());
    auto checkpoint = second.back();
    REQUIRE(checkpoint.spawns);
    const auto crate = std::find_if(checkpoint.base.entities.begin(),
                                    checkpoint.base.entities.end(),
                                    [&](const auto& entity) { return entity.id == rig.crate_id; });
    REQUIRE(crate != checkpoint.base.entities.end());
    REQUIRE(crate->state.size() == replication::kContentionCrateBytes);
    const auto v1 = crate->state;
    replication::ContentionContext context{};
    std::copy(v1.begin(), v1.end(), context.crate.begin());
    context.owner_seat = 0;
    context.owner_generation = 1;
    context.context_revision = 7;
    for (std::uint16_t seat = 1; seat <= 3; ++seat) {
        replication::ContentionProxy proxy{};
        proxy.seat = seat;
        proxy.seat_generation = 1;
        proxy.net_id = {static_cast<std::uint32_t>(seat + 20), 1};
        context.proxies.push_back(proxy);
    }
    std::array<std::byte, replication::kContentionMaxBytes> encoded{};
    std::size_t written = 0;
    REQUIRE(replication::encode_contention_context(context, encoded, written) ==
            replication::ContentionRefusal::None);
    crate->state.assign(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(written));
    CHECK(rig.reconciler->apply_checkpoint(checkpoint.base, *checkpoint.spawns).base.applied);
    CHECK(rig.hooks->hooks.prediction.capture(rig.crate_id) == v1);
    CHECK(rig.reconciler->checkpoints_refused_invalid() == 0);
}

TEST_CASE("contention: fresh predictor matches every replay tick through stand-in and seat reuse") {
    // Real two-seat authority: HelloGame + NetworkInputServer + world-stream mirror. Seat 0 is the
    // live predictor under test (checkpoints held kDelay ticks so every correction replays); seat 1
    // pushes the crate beside it, drops packets so the server executes synthesized and neutralized
    // stand-ins, then disconnects and is reused at generation 1 with a fresh NetId.
    constexpr int kDelay = 3;
    app::Application application{server_config()};
    const auto def = hello_game::definition_with_interaction();
    auto& server_world = application.world();
    def.register_components(server_world);
    hello_game::build_arena(server_world);
    scene::derive_world_transforms(server_world, application.jobs());
    app::SetupContext setup{server_world, application.jobs(), kDt, {}};
    setup.seats = 2;
    hello_game::HelloGame game;
    REQUIRE(game.setup(setup));
    const ecs::Entity server_crate = profile().object(server_world);
    REQUIRE(server_world.is_alive(server_crate));

    net::ScriptedNetwork network(0xC097u, {0.0f, 0.0f, 0, 0});
    const net::Endpoint server_endpoint{0x7F000001u, 7880};
    net::Link& server_link = network.add_node(server_endpoint);
    std::string error;
    const app::SeatConfig seat_cfg{.max_players = 2, .max_clients = 2};
    auto pump = app::NetworkInputServer::on_link(server_link,
                                                 def.tick_hz,
                                                 *def.network_input,
                                                 server_world,
                                                 &*def.remote_play,
                                                 error,
                                                 seat_cfg);
    REQUIRE_MESSAGE(pump != nullptr, error);
    const std::uint64_t schema_id =
        app::input_schema_identity(*def.network_input, &*def.remote_play, def.max_players);

    struct Peer {
        net::Link* link = nullptr;
        std::unique_ptr<net::NetDriver> driver;
        std::optional<net::SessionId> id;
        ecs::World mirror_world;
        std::unique_ptr<replication::ClientReplicator> mirror;
        std::optional<replication::InputHelloReply> reply;
        std::vector<net::SessionEvent> events;
        std::vector<net::Received> inbox;
        std::uint32_t sequence = 0;
        bool connected = false;

        Peer(net::ScriptedNetwork& net,
             const app::GameDefinition& definition,
             net::Endpoint server_ep,
             std::uint16_t port,
             std::uint64_t now) {
            link = &net.add_node({0x7F000001u, port});
            net::NetDriver::Config cfg{};
            cfg.salt_seed = 0xC0970000u + port;
            driver = std::make_unique<net::NetDriver>(*link, cfg);
            definition.remote_play->register_replicated(mirror_world);
            mirror = std::make_unique<replication::ClientReplicator>(mirror_world);
            id = driver->connect(server_ep, now);
            REQUIRE(id.has_value());
        }

        std::vector<replication::SpawnCheckpoint> pump_io(std::uint64_t now) {
            events.clear();
            driver->update(now, events);
            for (const auto& ev : events)
                connected |= ev.kind == net::SessionEvent::Kind::Connected;
            inbox.clear();
            if (id)
                if (auto* session = driver->session(*id))
                    session->drain_received(inbox);
            mirror->apply_messages(inbox);
            std::vector<replication::SpawnCheckpoint> out;
            for (const auto& msg : inbox) {
                replication::InputHelloReply hello_reply{};
                if (replication::decode_input_hello_reply(msg.bytes, hello_reply)) {
                    reply = hello_reply;
                    continue;
                }
                replication::SpawnCheckpoint cp{};
                if (replication::decode_spawn_checkpoint(msg.bytes, cp))
                    out.push_back(std::move(cp));
            }
            mirror->send_ack(*driver, now);
            return out;
        }

        replication::InputCommand send_move(float mx, float mz, std::uint64_t now) {
            replication::InputCommand cmd{};
            cmd.sequence = ++sequence;
            cmd.move_x = mx;
            cmd.move_y = mz;
            std::array<std::byte,
                       replication::kInputV2HeaderBytes + replication::kInputV2CommandBytes>
                bytes{};
            const auto size = replication::encode_input_v2(std::span(&cmd, 1), bytes);
            REQUIRE(driver->session(*id)->send_unreliable(
                std::span(bytes).first(size), now, app::kRemoteInputCommandsStream));
            return cmd;
        }
    };

    std::uint64_t now = 0;
    std::uint64_t server_tick = 0;
    std::vector<std::unique_ptr<Peer>> peers;
    const auto server_step = [&] {
        std::vector<app::SpawnReport> reports;
        std::vector<ecs::Entity> despawns;
        app::TickContext ctx{server_world, kDt, server_tick};
        ctx.spawns = &reports;
        ctx.despawns = &despawns;
        ctx.input = pump->before_tick(server_tick, now);
        ctx.command = pump->command_provenance();
        ctx.refuse_spawn = pump->refuse_spawn();
        pump->begin_seat_tick(game, ctx);
        server_world.advance_version();
        game.fixed_tick(ctx);
        pump->after_tick(server_tick, reports, despawns);
        ++server_tick;
    };
    const auto advance_handshake = [&] {
        now += 16;
        network.advance_time(now);
        for (auto& peer : peers)
            (void)peer->pump_io(now);
        network.advance_time(now);
        server_step();
    };
    const auto admit_peer = [&](std::uint16_t port) -> Peer& {
        peers.push_back(std::make_unique<Peer>(network, def, server_endpoint, port, now));
        Peer& peer = *peers.back();
        for (int i = 0; i < 60 && !peer.connected; ++i)
            advance_handshake();
        REQUIRE(peer.connected);
        const replication::InputHello hello{
            replication::kInputCodecV3, replication::kInputCodecV3, schema_id};
        std::array<std::byte, replication::kInputHelloBytes> bytes{};
        const auto size = replication::encode_input_hello(hello, bytes);
        REQUIRE(peer.driver->session(*peer.id)->send_reliable(std::span(bytes).first(size), now));
        for (int i = 0; i < 60 && !peer.reply.has_value(); ++i)
            advance_handshake();
        REQUIRE(peer.reply.has_value());
        REQUIRE(peer.reply->refusal == replication::InputHelloRefusal::None);
        return peer;
    };

    Peer& seat0 = admit_peer(7881);
    Peer& seat1_first = admit_peer(7882);
    REQUIRE(seat0.reply->seat == 0);
    REQUIRE(seat0.reply->seat_generation == 0);
    REQUIRE(seat1_first.reply->seat == 1);
    REQUIRE(seat1_first.reply->seat_generation == 0);

    // Drive one initial tick so seat 0 receives its baseline world stream and seed checkpoint.
    std::vector<replication::SpawnCheckpoint> seed_cps;
    while (seed_cps.empty() || seat0.mirror->deltas_applied() == 0) {
        now += 16;
        (void)seat0.send_move(0.0f, 0.0f, now);
        (void)seat1_first.send_move(0.0f, 0.0f, now);
        network.advance_time(now);
        (void)seat0.pump_io(now);
        (void)seat1_first.pump_io(now);
        network.advance_time(now);
        server_step();
        now += 16;
        network.advance_time(now);
        for (auto& cp : seat0.pump_io(now))
            seed_cps.push_back(std::move(cp));
        (void)seat1_first.pump_io(now);
    }
    const auto& seed_cp = seed_cps.back();
    REQUIRE(seed_cp.base.entities.size() == 2);
    const auto crate_net_id = seat0.mirror->map().net_id_of(profile().object(seat0.mirror_world));
    REQUIRE(crate_net_id.has_value());
    const replication::PredictedEntityId crate_id{crate_net_id->index, crate_net_id->generation};
    const replication::PredictedEntityId player_id = seed_cp.base.entities[0].id;
    REQUIRE(seed_cp.base.entities[1].id == crate_id);

    InteractionProfile::ClientOptions options{};
    options.owner = {seat0.reply->seat, seat0.reply->seat_generation};
    options.owner_known = true;
    Client live(def, options, crate_id);
    auto live_hooks = live.hooks.hooks.prediction;
    std::vector<std::vector<std::byte>> replay_states;
    const auto live_simulate = live_hooks.simulate;
    live_hooks.simulate = [&](std::span<const replication::PredictedEntityId> members,
                              const replication::InputCommand& command,
                              const replication::PredictionStep& step) {
        live_simulate(members, command, step);
        if (step.replaying)
            replay_states.push_back(live.hooks.context_snapshot());
    };
    replication::Reconciler reconciler(live_hooks, kDt);
    reconciler.set_membership(seed_cp.base.membership_epoch, {player_id, crate_id});
    REQUIRE(reconciler.apply_checkpoint(seed_cp.base).applied);

    auto no_rebuild_options = options;
    no_rebuild_options.rebuild_on_restore = false;
    Client no_rebuild(def, no_rebuild_options, crate_id);
    auto no_rebuild_hooks = no_rebuild.hooks.hooks.prediction;
    std::vector<std::vector<std::byte>> no_rebuild_replay_states;
    const auto no_rebuild_simulate = no_rebuild_hooks.simulate;
    no_rebuild_hooks.simulate = [&](std::span<const replication::PredictedEntityId> members,
                                    const replication::InputCommand& command,
                                    const replication::PredictionStep& step) {
        no_rebuild_simulate(members, command, step);
        if (step.replaying)
            no_rebuild_replay_states.push_back(no_rebuild.hooks.context_snapshot());
    };
    replication::Reconciler no_rebuild_predictor(no_rebuild_hooks, kDt);
    no_rebuild_predictor.set_membership(seed_cp.base.membership_epoch, {player_id, crate_id});
    REQUIRE(no_rebuild_predictor.apply_checkpoint(seed_cp.base).applied);

    struct HeldCheckpoint {
        int release_step = 0;
        replication::OwnerCheckpoint cp;
    };

    std::deque<HeldCheckpoint> held;
    std::vector<replication::InputCommand> commands;
    const std::uint32_t initial_epoch = seed_cp.base.membership_epoch;
    std::optional<replication::PredictedEntityId> first_proxy_net_id;
    std::uint32_t max_context_revision = 0;
    bool saw_synthesized_stand_in = false;
    bool saw_neutralized_stand_in = false;
    bool saw_empty_proxy_on_leave = false;
    bool saw_reused_seat = false;
    std::size_t mirror_differed_ticks = 0;
    std::size_t rebuild_differed_ticks = 0;
    std::size_t total_replay_ticks_checked = 0;

    Peer* active_seat1 = &seat1_first;
    std::unique_ptr<Peer> replacement_connecting;

    // Seat 0 (-1.8, 0) and Seat 1 (-0.6, 0) converge onto the left and right halves of the crate
    // at (0, 0.5, -2.5), keeping > 0.70 m lateral separation so both capsules contact the crate.
    for (int step = 1; step <= 95; ++step) {
        now += 16;
        const auto cmd0 = seat0.send_move(0.494f, -0.870f, now);
        commands.push_back(cmd0);
        reconciler.predict(cmd0);
        no_rebuild_predictor.predict(cmd0);

        if (step == 44 && active_seat1 != nullptr) {
            active_seat1->driver->session(*active_seat1->id)->disconnect();
            active_seat1 = nullptr;
        } else if (step == 48) {
            replacement_connecting =
                std::make_unique<Peer>(network, def, server_endpoint, 7883, now);
        }
        if (replacement_connecting && replacement_connecting->connected &&
            !replacement_connecting->reply.has_value()) {
            // Send hello once connected; harmless if re-sent before admission.
            if (step == 50) {
                const replication::InputHello hello{
                    replication::kInputCodecV3, replication::kInputCodecV3, schema_id};
                std::array<std::byte, replication::kInputHelloBytes> bytes{};
                const auto size = replication::encode_input_hello(hello, bytes);
                REQUIRE(replacement_connecting->driver->session(*replacement_connecting->id)
                            ->send_reliable(std::span(bytes).first(size), now));
            }
        }
        // Steps 32..39: seat 1 sends nothing, forcing 6 synthesized repeat stand-ins followed by
        // 2 neutralized stand-ins while both bodies are actively pushing the crate.
        const bool silent_stand_in = (step >= 32 && step <= 39);
        if (active_seat1 != nullptr && !silent_stand_in)
            (void)active_seat1->send_move(0.365f, -0.931f, now);

        network.advance_time(now);
        for (auto& peer : peers)
            for (auto& cp : peer->pump_io(now))
                if (peer.get() == &seat0)
                    held.push_back({step + kDelay, std::move(cp.base)});
        if (replacement_connecting) {
            (void)replacement_connecting->pump_io(now);
            if (replacement_connecting->reply.has_value()) {
                REQUIRE(replacement_connecting->reply->refusal ==
                        replication::InputHelloRefusal::None);
                REQUIRE(replacement_connecting->reply->seat == 1);
                REQUIRE(replacement_connecting->reply->seat_generation == 1);
                peers.push_back(std::move(replacement_connecting));
                active_seat1 = peers.back().get();
            }
        }
        network.advance_time(now);
        server_step();

        while (!held.empty() && held.front().release_step <= step) {
            const auto cp = std::move(held.front().cp);
            held.pop_front();
            REQUIRE(cp.entities.size() == 2);
            CHECK(cp.membership_epoch == initial_epoch);

            replication::ContentionContext decoded{};
            REQUIRE(replication::decode_contention_context(cp.entities[1].state, decoded) ==
                    replication::ContentionRefusal::None);
            max_context_revision = std::max(max_context_revision, decoded.context_revision);
            if (decoded.proxies.empty()) {
                saw_empty_proxy_on_leave = true;
            } else {
                const auto& proxy = decoded.proxies[0];
                CHECK(proxy.seat == 1);
                if (!first_proxy_net_id)
                    first_proxy_net_id = proxy.net_id;
                if (proxy.seat_generation == 0 && cp.server_tick >= 34) {
                    if (std::abs(proxy.axes[1] - (-0.931f)) < 1e-4f)
                        saw_synthesized_stand_in = true;
                    if (proxy.axes[0] == 0.0f && proxy.axes[1] == 0.0f)
                        saw_neutralized_stand_in = true;
                }
                if (proxy.seat_generation == 1) {
                    CHECK(proxy.net_id != *first_proxy_net_id);
                    saw_reused_seat = true;
                }
            }

            replay_states.clear();
            no_rebuild_replay_states.clear();
            const auto result = reconciler.apply_checkpoint(cp);
            REQUIRE(result.applied);
            const auto no_rebuild_result = no_rebuild_predictor.apply_checkpoint(cp);
            REQUIRE(no_rebuild_result.applied);
            REQUIRE(result.replayed == replay_states.size());
            REQUIRE(no_rebuild_result.replayed == result.replayed);
            if (result.replayed == 0)
                continue;

            // Fresh-predictor oracle: construct a brand-new predictor from ONLY this checkpoint
            // and the unretired command suffix; every replay tick must be byte-identical.
            Client fresh(def, options, crate_id);
            replication::Reconciler oracle(fresh.hooks.hooks.prediction, kDt);
            oracle.set_membership(cp.membership_epoch, {player_id, crate_id});
            REQUIRE(oracle.apply_checkpoint(cp).applied);
            const std::size_t suffix_start = commands.size() - result.replayed;
            for (std::size_t i = suffix_start, replay_tick = 0; i < commands.size();
                 ++i, ++replay_tick) {
                oracle.predict(commands[i]);
                const auto expected = fresh.hooks.context_snapshot();
                CHECK(replay_states[replay_tick] == expected);
                if (no_rebuild_replay_states[replay_tick] != expected)
                    ++rebuild_differed_ticks;
                ++total_replay_ticks_checked;
            }

            // Sensitivity control (a): seed the proxy from seat 0's world-stream mirror instead of
            // the checkpoint's proxy record. Because the mirror leads the held checkpoint (and
            // carries no executed command), the coupled replay diverges from the byte oracle.
            if (!decoded.proxies.empty()) {
                const auto& proxy = decoded.proxies[0];
                const ecs::Entity mirror_peer = def.remote_play->seat_player(
                    seat0.mirror_world, {proxy.seat, proxy.seat_generation});
                if (const auto* mirror_state =
                        seat0.mirror_world.get<hello_game::PlayerState>(mirror_peer)) {
                    auto tainted_ctx = decoded;
                    tainted_ctx.proxies[0].position = {mirror_state->position.x,
                                                       mirror_state->position.y,
                                                       mirror_state->position.z};
                    tainted_ctx.proxies[0].velocity = {mirror_state->velocity.x,
                                                       mirror_state->velocity.y,
                                                       mirror_state->velocity.z};
                    std::array<std::byte, replication::kContentionMaxBytes> buffer{};
                    std::size_t written = 0;
                    REQUIRE(replication::encode_contention_context(tainted_ctx, buffer, written) ==
                            replication::ContentionRefusal::None);
                    auto mirror_cp = cp;
                    mirror_cp.entities[1].state.assign(
                        buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(written));
                    Client mirror_seeded(def, options, crate_id);
                    replication::Reconciler mirror_oracle(mirror_seeded.hooks.hooks.prediction,
                                                          kDt);
                    mirror_oracle.set_membership(mirror_cp.membership_epoch, {player_id, crate_id});
                    REQUIRE(mirror_oracle.apply_checkpoint(mirror_cp).applied);
                    for (std::size_t i = suffix_start, replay_tick = 0; i < commands.size();
                         ++i, ++replay_tick) {
                        mirror_oracle.predict(commands[i]);
                        if (mirror_seeded.hooks.context_snapshot() != replay_states[replay_tick])
                            ++mirror_differed_ticks;
                    }
                }
            }
        }
    }

    const auto* final_crate = server_world.get<hello_game::CrateBody>(server_crate);
    REQUIRE(final_crate != nullptr);
    CHECK(final_crate->position.z < -2.8f); // genuinely pushed under two-seat contention
    CHECK(pump->counters().synthesized > 0);
    CHECK(pump->counters().neutralized > 0);
    CHECK(saw_synthesized_stand_in);
    CHECK(saw_neutralized_stand_in);
    CHECK(saw_empty_proxy_on_leave);
    CHECK(saw_reused_seat);
    CHECK(max_context_revision >= 3);
    CHECK(total_replay_ticks_checked >= 150);
    CHECK(mirror_differed_ticks > 0);
    CHECK(rebuild_differed_ticks > 0);
    std::printf("contention oracle: replay_ticks=%zu crate_z=%.4f max_rev=%u "
                "mirror_differed=%zu no_rebuild_differed=%zu\n",
                total_replay_ticks_checked,
                static_cast<double>(final_crate->position.z),
                max_context_revision,
                mirror_differed_ticks,
                rebuild_differed_ticks);
}

TEST_CASE("a malformed final context record refuses the entire checkpoint before any mutation") {
    const auto corrupt_last_proxy = [](std::vector<std::byte>& crate_state) {
        REQUIRE(crate_state.size() == replication::kContentionCrateBytes);
        replication::ContentionContext context{};
        std::copy(crate_state.begin(), crate_state.end(), context.crate.begin());
        context.owner_seat = 0;
        context.owner_generation = 1;
        context.context_revision = 1;
        for (std::uint16_t seat = 1; seat <= 3; ++seat) {
            replication::ContentionProxy proxy{};
            proxy.seat = seat;
            proxy.seat_generation = 1;
            proxy.net_id = {static_cast<std::uint32_t>(seat + 20), 1};
            context.proxies.push_back(proxy);
        }
        std::array<std::byte, replication::kContentionMaxBytes> encoded{};
        std::size_t written = 0;
        REQUIRE(replication::encode_contention_context(context, encoded, written) ==
                replication::ContentionRefusal::None);
        crate_state.assign(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(written));
        // The LAST record's velocity x is NaN. Earlier records and the crate are valid.
        crate_state[64 + 2 * 48 + 24 + 2] = std::byte{0xC0};
        crate_state[64 + 2 * 48 + 24 + 3] = std::byte{0x7F};
    };
    for (int fault = 0; fault < 4; ++fault) {
        Rig rig(static_cast<std::uint16_t>(7890 + fault));
        rig.connect();
        auto first = rig.server_tick(walk(++rig.sequence, 0.0f));
        REQUIRE_FALSE(first.empty());
        rig.seed(first.back());
        const auto command = walk(++rig.sequence, -1.0f);
        rig.reconciler->predict(command);
        auto second = rig.server_tick(command);
        REQUIRE_FALSE(second.empty());
        auto checkpoint = second.back();
        REQUIRE(checkpoint.spawns);
        const replication::SpawnKey verdict_key{999, 999, 0};
        checkpoint.spawns->outcomes.push_back(
            {verdict_key, replication::SpawnOutcomeKind::Rejected, {}, 1});
        REQUIRE(rig.ledger.should_play(verdict_key, replication::PresentationEventKind::Launch, 0));
        auto& entities = checkpoint.base.entities;
        const auto crate = std::find_if(entities.begin(), entities.end(), [&](const auto& entity) {
            return entity.id == rig.crate_id;
        });
        REQUIRE(crate != entities.end());
        if (fault == 0) {
            corrupt_last_proxy(crate->state);
        } else if (fault == 1) {
            crate->state[28 + 2] = std::byte{0xC0}; // v1 crate linear velocity x = NaN
            crate->state[28 + 3] = std::byte{0x7F};
        } else if (fault == 2) {
            crate->state.pop_back();
        } else {
            crate->state.clear();
        }
        const auto player_before = rig.hooks->hooks.prediction.capture(kPlayerId);
        const auto crate_before = rig.hooks->hooks.prediction.capture(rig.crate_id);
        const std::vector roster_before(rig.reconciler->roster().begin(),
                                        rig.reconciler->roster().end());
        const auto history_before = rig.reconciler->base().history_size();
        const auto tick_before = rig.reconciler->base().last_checkpoint_tick();
        const auto ledger_played = rig.ledger.played();
        const auto ledger_adoptions = rig.ledger.adoptions();
        const auto ledger_cancels = rig.ledger.cancels();
        const auto held_before = rig.reconciler->outcomes_held_through();
        const auto tombstone_before = rig.reconciler->has_tombstone(verdict_key);
        CHECK_FALSE(
            rig.reconciler->apply_checkpoint(checkpoint.base, *checkpoint.spawns).base.applied);
        CHECK(rig.reconciler->checkpoints_refused_invalid() == 1);
        CHECK(rig.reconciler->base().history_size() == history_before);
        CHECK(rig.reconciler->base().last_checkpoint_tick() == tick_before);
        CHECK(std::vector(rig.reconciler->roster().begin(), rig.reconciler->roster().end()) ==
              roster_before);
        CHECK(rig.ledger.played() == ledger_played);
        CHECK(rig.ledger.adoptions() == ledger_adoptions);
        CHECK(rig.ledger.cancels() == ledger_cancels);
        CHECK_FALSE(rig.ledger.cancelled(verdict_key));
        CHECK(rig.reconciler->outcomes_held_through() == held_before);
        CHECK(rig.reconciler->has_tombstone(verdict_key) == tombstone_before);
        CHECK(rig.hooks->hooks.prediction.capture(kPlayerId) == player_before);
        CHECK(rig.hooks->hooks.prediction.capture(rig.crate_id) == crate_before);
        if (fault == 0)
            CHECK(rig.hooks->context_refusals(replication::ContentionRefusal::NonFinite) == 1);
        if (fault == 1)
            CHECK(rig.hooks->context_refusals(replication::ContentionRefusal::NonFinite) == 1);
        if (fault >= 2)
            CHECK(rig.hooks->context_refusals(replication::ContentionRefusal::BadLength) == 1);
    }

    // With prepare deliberately skipped, the same malformed checkpoint reaches the old restore
    // path and retires history. This proves the immutable observations above are sensitive.
    Rig control(7894);
    control.connect();
    auto first = control.server_tick(walk(++control.sequence, 0.0f));
    REQUIRE_FALSE(first.empty());
    control.seed(first.back(), true);
    const auto command = walk(++control.sequence, -1.0f);
    control.reconciler->predict(command);
    auto second = control.server_tick(command);
    REQUIRE_FALSE(second.empty());
    auto checkpoint = second.back();
    REQUIRE(checkpoint.spawns);
    const replication::SpawnKey verdict_key{999, 999, 0};
    checkpoint.spawns->outcomes.push_back(
        {verdict_key, replication::SpawnOutcomeKind::Rejected, {}, 1});
    REQUIRE(control.ledger.should_play(verdict_key, replication::PresentationEventKind::Launch, 0));
    for (auto& entity : checkpoint.base.entities)
        if (entity.id == control.crate_id)
            corrupt_last_proxy(entity.state);
    const auto tick_before = control.reconciler->base().last_checkpoint_tick();
    CHECK(control.reconciler->apply_checkpoint(checkpoint.base, *checkpoint.spawns).base.applied);
    CHECK(control.reconciler->base().last_checkpoint_tick() != tick_before);
    CHECK(control.ledger.cancelled(verdict_key));
    CHECK(control.ledger.cancels() == 1);
}

TEST_CASE("interaction: the server declares membership with hysteresis and the client follows") {
    Rig rig(7851);
    rig.connect();
    const auto& params = profile().params;
    const float join_radius = params.join_radius();
    const float leave_radius = params.leave_radius();
    CHECK(join_radius == doctest::Approx(0.85f + 4.0f * 64.0f / 60.0f));

    // The first tick: the crate starts 2.5 m away, inside the join radius, so it joins at once.
    auto first = rig.server_tick(walk(++rig.sequence, 0.0f));
    REQUIRE_FALSE(first.empty());
    rig.seed(first.back());
    CHECK_FALSE(rig.crate_bytes_in(first.back()).empty());
    CHECK(rig.crate_in_roster());
    CHECK(rig.hooks->object_pose().has_value());
    std::uint32_t epoch = first.back().base.membership_epoch;

    struct Tick {
        float distance = 0.0f;
        float speed = 0.0f;
        bool member = false;
    };

    std::vector<Tick> trace;
    bool saw_velocity = false;
    std::uint64_t epoch_changes = 0, roster_changes = 0;
    bool member = true;
    const auto one_tick = [&](float move_z) {
        const auto command = walk(++rig.sequence, move_z);
        rig.reconciler->predict(command);
        const auto checkpoints = rig.server_tick(command);
        const auto sample = rig.server.sample();
        for (const auto& checkpoint : checkpoints) {
            const auto result =
                rig.reconciler->apply_checkpoint(checkpoint.base, *checkpoint.spawns);
            REQUIRE(result.base.applied);
            if (checkpoint.base.membership_epoch != epoch) {
                ++epoch_changes;
                epoch = checkpoint.base.membership_epoch;
            }
            // Bytes carry velocity as well as pose: what the client seeds its physics from.
            const auto bytes = rig.crate_bytes_in(checkpoint);
            if (!bytes.empty()) {
                REQUIRE(bytes.size() == hello_game::kCrateStateBytes);
                if (core::length(velocity_of(bytes)) > 0.5f) {
                    saw_velocity = true;
                    CHECK(velocity_of(bytes).z == rig.server.crate_body().linear_velocity.z);
                }
            }
        }
        CHECK(rig.reconciler->epoch() == epoch);
        CHECK(rig.crate_in_roster() == rig.hooks->object_pose().has_value());
        if (rig.crate_in_roster() != member)
            ++roster_changes;
        member = rig.crate_in_roster();
        trace.push_back({sample.distance, sample.speed, member});
    };
    const auto until = [&](float move_z, bool want_member) {
        for (int i = 0; i < 400; ++i) {
            one_tick(move_z);
            if (member == want_member)
                return true;
        }
        return false;
    };

    // 1. Push the crate while it is awake (it sleeps ~0.5 s after spawn, and a sleeping crate is
    //    walked through -- see the report), then retreat at once: it is still sliding when the
    //    player crosses the leave radius, so the leave must wait for it to rest.
    for (int i = 0; i < 40; ++i)
        one_tick(-1.0f);
    CHECK(rig.server.crate_body().position.z < -2.6f); // it really was pushed
    const auto before_leave = trace.size();
    REQUIRE(until(1.0f, false));
    const auto leave_tick = trace.size() - 1;
    CHECK(trace[leave_tick].distance > leave_radius);
    // It left only after `rest_ticks` quiet ticks, and some ticks beyond the radius were deferred.
    REQUIRE(leave_tick >= params.rest_ticks);
    for (std::size_t t = leave_tick - params.rest_ticks + 1; t <= leave_tick; ++t)
        CHECK(trace[t].speed < params.rest_speed);
    bool deferred_beyond_radius = false;
    for (std::size_t t = before_leave; t < leave_tick; ++t)
        deferred_beyond_radius |= trace[t].distance > leave_radius;
    CHECK(deferred_beyond_radius);
    // Measured: these deferred ticks are NOT counted by the m20.6a leaf. It counts a deferral only
    // on a tick that is itself moving or touching; a crate that has stopped but has not yet been
    // quiet for `rest_ticks` defers the leave silently (reported, not changed here).
    CHECK_FALSE(rig.hooks->object_pose().has_value());

    // 2. Walk back: it joins the first tick the distance drops under the join radius.
    REQUIRE(until(-1.0f, true));
    const auto join_tick = trace.size() - 1;
    CHECK(trace[join_tick].distance < join_radius);
    CHECK(trace[join_tick - 1].distance >= join_radius);
    CHECK(rig.hooks->object_pose().has_value());

    // 3. Away again: the crate has long been at rest, so it leaves at the leave radius.
    REQUIRE(until(1.0f, false));
    const auto second_leave = trace.size() - 1;
    CHECK(trace[second_leave].distance > leave_radius);
    CHECK(trace[second_leave - 1].distance <= leave_radius);

    const auto counters = rig.pump->counters();
    CHECK(counters.interaction_joins == 2);
    CHECK(counters.interaction_leave_deferred_touching == 0);
    CHECK(counters.interaction_leaves == 2);
    CHECK(counters.interaction_joined_in_contact == 0);
    CHECK(counters.interaction_epoch_advances == 4);
    CHECK(epoch_changes == 3); // the first join was already in the seeding checkpoint
    CHECK(roster_changes == 3);
    CHECK(rig.reconciler->epoch_roster_mismatch() == 0);
    CHECK(saw_velocity);
    std::printf("interaction membership: join_radius=%.4f leave_radius=%.4f leave@%zu (d=%.3f) "
                "join@%zu (d=%.3f) leave@%zu (d=%.3f) deferred_moving=%llu\n",
                static_cast<double>(join_radius),
                static_cast<double>(leave_radius),
                leave_tick,
                static_cast<double>(trace[leave_tick].distance),
                join_tick,
                static_cast<double>(trace[join_tick].distance),
                second_leave,
                static_cast<double>(trace[second_leave].distance),
                static_cast<unsigned long long>(counters.interaction_leave_deferred_moving));
}

TEST_CASE("interaction: a predicted push moves the crate on the push tick; corrections smooth") {
    Rig rig(7861);
    rig.connect();
    auto first = rig.server_tick(walk(++rig.sequence, 0.0f));
    REQUIRE_FALSE(first.empty());
    rig.seed(first.back());
    replication::CorrectionSmoother smoother;

    struct Held {
        int release = 0;
        replication::SpawnCheckpoint checkpoint;
    };

    std::deque<Held> held;
    constexpr int kDelay = 3;
    std::optional<int> push_tick;
    float push_step = 0.0f;
    float authoritative_z_at_push = 0.0f;
    double max_sim_correction = 0.0;
    std::uint64_t member_corrections = 0;
    bool tampered = false, presented_continuous = false;
    float tamper_raw = 0.0f, tamper_presented_jump = 0.0f;
    const auto apply = [&](replication::SpawnCheckpoint checkpoint, bool tamper) {
        const auto before = rig.hooks->object_pose();
        const auto shown =
            before ? smoother.presented(rig.crate_id, before->translation) : core::Vec3{};
        if (tamper)
            for (auto& entity : checkpoint.base.entities)
                if (entity.id == rig.crate_id) {
                    // A server-only disturbance the client could not predict: 0.3 m sideways.
                    const auto bits =
                        std::bit_cast<std::uint32_t>(read_f32(entity.state, 0) + 0.3f);
                    for (unsigned i = 0; i < 4; ++i)
                        entity.state[i] = std::byte{static_cast<std::uint8_t>(bits >> (8 * i))};
                }
        const auto result = rig.reconciler->apply_checkpoint(checkpoint.base, *checkpoint.spawns);
        if (!result.base.applied)
            return;
        const auto after = rig.hooks->object_pose();
        if (!after || !before)
            return;
        smoother.on_correction(rig.crate_id, shown, after->translation);
        if (tamper) {
            tamper_raw = core::length(shown - after->translation);
            presented_continuous =
                core::length(smoother.presented(rig.crate_id, after->translation) - shown) < 1e-5f;
            return;
        }
        if (tampered)
            return; // the next checkpoints undo the tamper: a disturbance, not the residual
        max_sim_correction =
            std::max(max_sim_correction,
                     static_cast<double>(core::length(before->translation - after->translation)));
        ++member_corrections;
    };
    float presented_jump_before_tamper = 0.0f;
    for (int tick = 0; tick < 70; ++tick) {
        // Hold delivery kDelay ticks so the client predicts ahead of authority.
        const auto command = walk(++rig.sequence, tick < 40 ? -1.0f : 0.0f);
        const auto before = rig.hooks->object_pose();
        rig.reconciler->predict(command);
        const auto after = rig.hooks->object_pose();
        if (!push_tick && rig.hooks->touching() && before && after) {
            push_tick = tick;
            push_step = core::length(after->translation - before->translation);
            authoritative_z_at_push =
                rig.mirror_world.get<hello_game::CrateBody>(profile().object(rig.mirror_world))
                    ->position.z;
        }
        for (auto& checkpoint : rig.server_tick(command))
            held.push_back({tick + kDelay, std::move(checkpoint)});
        while (!held.empty() && held.front().release <= tick) {
            const bool tamper = tick >= 60 && !tampered;
            if (tamper) {
                presented_jump_before_tamper = smoother.counters().max_presented_jump;
                tampered = true;
            }
            apply(std::move(held.front().checkpoint), tamper);
            held.pop_front();
            if (tamper)
                break; // decay the tampered offset for one tick before the next checkpoint
        }
        smoother.advance(kDt);
        if (tampered && tamper_presented_jump == 0.0f)
            tamper_presented_jump = smoother.counters().max_presented_jump;
    }
    REQUIRE(push_tick);
    // The PUSH TICK: the predicted crate moved in the very step whose command first touched it,
    // while the newest authority the client held still had it at its spawn position.
    CHECK(push_step > 1e-3f);
    CHECK(authoritative_z_at_push == doctest::Approx(-2.5f));
    // A correction is SMOOTHED: what is shown does not jump at the correction, and the offset's
    // largest one-tick change is smaller than the raw jump it hides.
    REQUIRE(tampered);
    CHECK(presented_continuous);
    CHECK(tamper_raw > 0.25f);
    CHECK(tamper_presented_jump > presented_jump_before_tamper);
    CHECK(tamper_presented_jump < tamper_raw);
    CHECK(smoother.counters().snaps == 0);
    std::printf("interaction push: push_tick=%d step=%.4f m; undisturbed max sim correction "
                "%.6f m over %llu member checkpoints (delivery held %d ticks); tamper raw=%.4f "
                "max presented step=%.4f\n",
                *push_tick,
                static_cast<double>(push_step),
                max_sim_correction,
                static_cast<unsigned long long>(member_corrections),
                kDelay,
                static_cast<double>(tamper_raw),
                static_cast<double>(tamper_presented_jump));
}

TEST_CASE("interaction: restore and replay twice gives identical crate bytes; no-rebuild differs") {
    // Server history while the crate is awake and being pushed: per tick, the checkpoint state
    // (group, player, crate) and the command that produced the next tick.
    Server server;

    struct Frame {
        std::vector<std::byte> group, player, crate;
        replication::InputCommand next{};
    };

    std::vector<Frame> frames;
    for (std::uint32_t t = 1; t <= 60; ++t) {
        Frame frame{server.hooks.capture_group(),
                    server.hooks.capture(kPlayerId),
                    server.crate_bytes(),
                    walk(t, t <= 45 ? -1.0f : 0.0f)};
        server.step(frame.next);
        frames.push_back(std::move(frame));
    }
    REQUIRE(server.crate_body().position.z < -3.0f); // pushed, so contacts were live

    const auto replay = [&](Client& client, std::size_t from, std::size_t count) {
        client.restore(frames[from].group, frames[from].player, frames[from].crate);
        std::vector<std::byte> trace;
        for (std::size_t i = 0; i < count; ++i) {
            client.simulate(frames[from + i].next, from + i + 1);
            const auto bytes = client.crate_bytes();
            trace.insert(trace.end(), bytes.begin(), bytes.end());
        }
        return trace;
    };
    const auto twice = [&](InteractionProfile::ClientOptions options) {
        Client client(server.definition, options);
        // A DISCARDED timeline first, as a real correction discards one: other commands from
        // another checkpoint, leaving contacts and solver state behind.
        (void)replay(client, 20, 8);
        auto a = replay(client, 30, 12);
        auto b = replay(client, 30, 12);
        return std::pair{std::move(a), std::move(b)};
    };
    const auto rebuilt = twice({});
    CHECK(rebuilt.first.size() == 12 * hello_game::kCrateStateBytes);
    CHECK(rebuilt.first == rebuilt.second);
    // SENSITIVITY CONTROL: the same test with the rebuild skipped must be able to see the leak.
    const auto kept = twice({.rebuild_on_restore = false});
    CHECK(kept.first != kept.second);
    std::size_t differing = 0;
    for (std::size_t i = 0; i < kept.first.size(); ++i)
        differing += kept.first[i] != kept.second[i] ? 1 : 0;
    std::printf("interaction replay-twice: rebuild identical=%d; no-rebuild differing bytes=%zu "
                "of %zu\n",
                rebuilt.first == rebuilt.second ? 1 : 0,
                differing,
                kept.first.size());
}

TEST_CASE("interaction: without the profile, or without spawns beside it, nothing changes") {
    const auto plain = hello_game::definition();
    REQUIRE(plain.remote_play);
    CHECK_FALSE(plain.remote_play->interaction.has_value());
    CHECK(plain.remote_play->active_interaction() == nullptr);
    const auto opted = hello_game::definition_with_interaction();
    REQUIRE(opted.remote_play->active_interaction() != nullptr);
    const auto m20_5 = app::input_schema_identity(*plain.network_input, &*plain.remote_play);
    const auto m20_6 = app::input_schema_identity(*opted.network_input, &*opted.remote_play);
    CHECK(m20_6 != m20_5);
    auto bumped = opted;
    bumped.remote_play->interaction->version = 3;
    CHECK(app::input_schema_identity(*bumped.network_input, &*bumped.remote_play) != m20_6);
    // Inert without a spawn profile: the identity is m20.4's, bit for bit.
    auto inert = opted;
    inert.remote_play->spawn_prediction.reset();
    CHECK(inert.remote_play->active_interaction() == nullptr);
    CHECK(app::input_schema_identity(*inert.network_input, &*inert.remote_play) ==
          replication::input_schema_hash(inert.network_input->profile_id,
                                         inert.network_input->profile_version,
                                         inert.remote_play->fixed_dt));
    std::printf("interaction identity: m20.5 0x%016llx -> m20.6 0x%016llx\n",
                static_cast<unsigned long long>(m20_5),
                static_cast<unsigned long long>(m20_6));

    // The server pump with the plain definition: no crate in the roster, every counter zero.
    Rig rig(7871, hello_game::definition());
    rig.connect();
    std::size_t checkpoints = 0;
    for (int i = 0; i < 20; ++i)
        for (const auto& checkpoint : rig.server_tick(walk(++rig.sequence, -1.0f))) {
            ++checkpoints;
            CHECK(checkpoint.base.entities.size() == 1); // the player alone, as in m20.5
        }
    CHECK(checkpoints > 0);
    const auto counters = rig.pump->counters();
    CHECK(counters.interaction_joins == 0);
    CHECK(counters.interaction_epoch_advances == 0);
    CHECK(counters.interaction_member_ticks == 0);
}

TEST_CASE("interaction: measured resting-crate speed against the 0.05 m/s rest bound") {
    // A MEASUREMENT, printed for the report; it asserts only that the run happened. The server
    // sleeps bodies, so a resting crate's speed reaches exactly 0 once it sleeps; what matters for
    // `rest_speed` is the solver's jitter in the ticks BEFORE sleep, and in the client's
    // prediction world, which never sleeps.
    const float rest_speed = profile().params.rest_speed;
    Server idle;
    std::uint64_t idle_sleep_tick = 0;
    float idle_max = 0.0f;
    for (std::uint32_t t = 1; t <= 120; ++t) {
        idle.step(walk(t, 0.0f));
        const float speed = core::length(idle.crate_body().linear_velocity);
        idle_max = std::max(idle_max, speed);
        if (speed == 0.0f && idle_sleep_tick == 0)
            idle_sleep_tick = idle.tick;
    }
    // After a push: the speeds of the last 30 ticks before the crate stops for good.
    Server pushed;
    std::vector<float> speeds;
    for (std::uint32_t t = 1; t <= 240; ++t) {
        pushed.step(walk(t, t <= 40 ? -1.0f : (t <= 50 ? 1.0f : 0.0f)));
        speeds.push_back(core::length(pushed.crate_body().linear_velocity));
    }
    // `stopped`: the last tick with any speed (sleep zeroes it after); `settled`: the first tick
    // after the push from which the speed stays under rest_speed until then. The JITTER is the
    // largest speed in [settled, stopped): the resting solver's own motion before sleep.
    std::size_t stopped = speeds.size();
    while (stopped > 0 && speeds[stopped - 1] == 0.0f)
        --stopped;
    std::size_t settled = stopped;
    while (settled > 50 && speeds[settled - 1] < rest_speed)
        --settled;
    float settle_max = 0.0f;
    for (std::size_t t = settled; t < stopped; ++t)
        settle_max = std::max(settle_max, speeds[t]);
    const float approach = settled > 0 ? speeds[settled - 1] : 0.0f;
    // The client's prediction world (sleep off): a crate at rest on the floor for 120 steps.
    Client client(idle.definition, {});
    client.restore(idle.hooks.capture_group(), idle.hooks.capture(kPlayerId), idle.crate_bytes());
    float client_max = 0.0f;
    for (std::uint32_t t = 1; t <= 120; ++t) {
        client.simulate(walk(t, 0.0f), t);
        client_max = std::max(client_max, core::length(velocity_of(client.crate_bytes())));
    }
    // And the pre-existing sleep finding: a crate walked into after it has slept does not move.
    Server late;
    for (std::uint32_t t = 1; t <= 120; ++t)
        late.step(walk(t, 0.0f));
    for (std::uint32_t t = 1; t <= 90; ++t)
        late.step(walk(t, -1.0f));
    std::printf("interaction rest: idle max %.6f m/s, asleep (speed 0) from tick %llu; after a "
                "push, speed stays under %.2f from tick %zu (previous tick %.6f m/s) and the "
                "resting jitter until sleep at tick %zu is max %.6f m/s; client world (no sleep) "
                "resting max %.6f m/s; a crate "
                "walked into after sleeping ends at z=%.4f with the player at z=%.4f\n",
                static_cast<double>(idle_max),
                static_cast<unsigned long long>(idle_sleep_tick),
                static_cast<double>(rest_speed),
                settled + 1,
                static_cast<double>(approach),
                stopped + 1,
                static_cast<double>(settle_max),
                static_cast<double>(client_max),
                static_cast<double>(late.crate_body().position.z),
                static_cast<double>(late.game.player_position().z));
    CHECK(idle_sleep_tick > 0);
}

TEST_CASE("crate contact skin bounds overlap for 120 held ticks and still pushes") {
    const auto run = [](bool clip) {
        Server server;
        server.game.set_crate_clip_enabled(clip);
        float maximum = 0.0f;
        std::uint32_t touching_ticks = 0;
        for (std::uint32_t tick = 1; tick <= 120; ++tick) {
            server.step(walk(tick, -1.0f));
            maximum = std::max(maximum, server.game.player_crate_overlap_before_step());
            touching_ticks += server.crate_body().touching_player != 0 ? 1u : 0u;
        }
        return std::tuple{maximum, server.crate_body().position.z, touching_ticks};
    };
    const auto [clipped_overlap, clipped_z, clipped_contacts] = run(true);
    const auto [control_overlap, control_z, control_contacts] = run(false);
    CHECK(clipped_overlap <= 0.015f); // 1 cm skin + 5 mm solver/rounding allowance
    CHECK(clipped_z < -2.6f);
    CHECK(clipped_contacts > 0);
    CHECK(control_overlap > 0.015f);
    std::printf("crate skin: clipped max %.6f m, crate z %.6f m, contacts %u; control max %.6f m, "
                "crate z %.6f m, contacts %u\n",
                static_cast<double>(clipped_overlap),
                static_cast<double>(clipped_z),
                clipped_contacts,
                static_cast<double>(control_overlap),
                static_cast<double>(control_z),
                control_contacts);
}

TEST_CASE("crate move and slide preserves motion along its side") {
    Server server;
    auto* state = server.world().get<hello_game::PlayerState>(server.player);
    auto* transform = server.world().get<ecs::WorldTransform>(server.player);
    auto* local = server.world().get<ecs::LocalTransform>(server.player);
    REQUIRE(state != nullptr);
    REQUIRE(transform != nullptr);
    REQUIRE(local != nullptr);
    state->position = {0.80f, state->position.y, -1.0f};
    transform->value.translation = state->position;
    local->value.translation = state->position;
    float maximum = 0.0f;
    for (std::uint32_t tick = 1; tick <= 36; ++tick) {
        server.step(walk(tick, -1.0f));
        maximum = std::max(maximum, server.game.player_crate_overlap_before_step());
    }
    const auto* after = server.world().get<hello_game::PlayerState>(server.player);
    REQUIRE(after != nullptr);
    CHECK(after->position.x > 0.81f); // the side normal actually clipped the x overlap
    CHECK(after->position.z < -2.0f);
    CHECK(maximum <= 0.015f);
    std::printf("crate slide: z %.6f -> %.6f m, max overlap %.6f m\n",
                -1.0,
                static_cast<double>(after->position.z),
                static_cast<double>(maximum));
}

TEST_CASE("crate footprint follows yaw rather than an axis aligned square") {
    Server server;
    auto* player = server.world().get<hello_game::PlayerState>(server.player);
    auto* player_world = server.world().get<ecs::WorldTransform>(server.player);
    auto* player_local = server.world().get<ecs::LocalTransform>(server.player);
    auto* crate = server.world().get<hello_game::CrateBody>(server.crate);
    auto* crate_world = server.world().get<ecs::WorldTransform>(server.crate);
    auto* crate_local = server.world().get<ecs::LocalTransform>(server.crate);
    REQUIRE(player != nullptr);
    REQUIRE(player_world != nullptr);
    REQUIRE(player_local != nullptr);
    REQUIRE(crate != nullptr);
    REQUIRE(crate_world != nullptr);
    REQUIRE(crate_local != nullptr);
    player->position = {1.0f, player->position.y, -2.5f};
    player_world->value.translation = player->position;
    player_local->value.translation = player->position;
    const auto yaw = core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.785398163f);
    crate->orientation = yaw;
    crate_world->value.rotation = yaw;
    crate_local->value.rotation = yaw;
    server.step(walk(1, 0.0f));
    const auto* moved = server.world().get<hello_game::PlayerState>(server.player);
    REQUIRE(moved != nullptr);
    CHECK(moved->position.x > 1.04f);
    CHECK(server.game.player_crate_overlap_before_step() <= 0.015f);
}

TEST_CASE("two kinematic players stop at contact and neither passes through") {
    app::Application application{server_config()};
    auto& world = application.world();
    hello_game::register_components(world);
    hello_game::build_arena(world);
    scene::derive_world_transforms(world, application.jobs());
    hello_game::HelloGame game;
    app::SetupContext setup{world, application.jobs(), kDt, {}};
    setup.seats = 2;
    REQUIRE(game.setup(setup));
    const app::PlayerSlot left{0, 0};
    const app::PlayerSlot right{1, 0};
    app::TickContext joined{world, kDt, 0};
    REQUIRE(game.on_player_joined(joined, left));
    REQUIRE(game.on_player_joined(joined, right));
    const auto a = hello_game::seat_player(world, left);
    const auto b = hello_game::seat_player(world, right);
    REQUIRE(world.is_alive(a));
    REQUIRE(world.is_alive(b));
    float minimum = 100.0f;
    for (std::uint64_t tick = 0; tick < 60; ++tick) {
        std::array<app::PlayerInput, 2> inputs{};
        inputs[0].slot = left;
        inputs[0].actions.axes[hello_game::kAxisMoveX] = 1.0f;
        inputs[1].slot = right;
        inputs[1].actions.axes[hello_game::kAxisMoveX] = -1.0f;
        app::TickContext context{world, kDt, tick};
        context.players = inputs;
        game.fixed_tick(context);
        const auto* first = world.get<hello_game::PlayerState>(a);
        const auto* second = world.get<hello_game::PlayerState>(b);
        REQUIRE(first != nullptr);
        REQUIRE(second != nullptr);
        const float distance = std::abs(second->position.x - first->position.x);
        minimum = std::min(minimum, distance);
        CHECK(first->position.x < second->position.x);
    }
    CHECK(minimum >= 0.699f); // 2r - 1 mm floating-point allowance
    std::printf("players contact: minimum distance %.6f m, required %.6f m\n",
                static_cast<double>(minimum),
                0.699);
}
