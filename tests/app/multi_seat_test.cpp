// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m20.8b1 (ADR-0108): the per-seat server pump, driven in-process through the exact production
// code (`NetworkInputServer::on_link`) by several scripted clients.
//
// The claim these tests exist for is ATTRIBUTION, not agreement. Every client of one server
// mirrors the same authority, so a pump that hands seat 0's command to seat 1 still produces one
// consistent world, and a digest comparison is green through it (ADR-0108 Consequences: silent
// cross-seat aliasing). So each test sends something only one seat could have sent -- a distinct
// axis value, a distinct sequence range, a distinct body -- and checks it surfaced on THAT seat's
// PlayerInput, result and checkpoint, and nowhere else.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rime/app/network_input.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/link.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/input_v2.hpp"
#include "rime/replication/prediction.hpp"

namespace seat_test {

// One body per seat. The seat's index and generation are written INTO the body, so a checkpoint's
// captured bytes say whose body they are: a checkpoint carrying another seat's pawn is aliasing
// made visible.
struct SeatPawn {
    std::uint32_t seat_index = 0;
    std::uint32_t seat_generation = 0;
    float x = 0.0f;
};

} // namespace seat_test

RIME_REFLECT_BEGIN(seat_test::SeatPawn)
RIME_REFLECT_FIELD(seat_index)
RIME_REFLECT_FIELD(seat_generation)
RIME_REFLECT_FIELD(x)
RIME_REFLECT_END()

using namespace rime;

namespace {

using seat_test::SeatPawn;

constexpr double kDt = 1.0 / 60.0;

ecs::Entity pawn_for(ecs::World& world, app::PlayerSlot slot) {
    ecs::Entity found{};
    world.query<SeatPawn>().for_each([&](ecs::Entity entity, const SeatPawn& pawn) {
        if (pawn.seat_index == slot.index && pawn.seat_generation == slot.generation)
            found = entity;
    });
    return found;
}

// The checkpoint's player bytes: the pawn's seat index and generation, little-endian u32 each.
std::vector<std::byte> encode_pawn(const SeatPawn& pawn) {
    std::vector<std::byte> bytes;
    for (const std::uint32_t value : {pawn.seat_index, pawn.seat_generation})
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    return bytes;
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= std::uint32_t{std::to_integer<std::uint8_t>(bytes[at + i])} << (8 * i);
    return value;
}

replication::PredictionHooks pawn_hooks(ecs::World& world, ecs::Entity pawn) {
    replication::PredictionHooks hooks{};
    auto* bound = &world;
    hooks.capture = [bound, pawn](replication::PredictedEntityId) {
        const auto* state = bound->get<SeatPawn>(pawn);
        return state ? encode_pawn(*state) : std::vector<std::byte>{};
    };
    hooks.restore = [](replication::PredictedEntityId, std::span<const std::byte>) {};
    hooks.capture_group = [] { return std::vector<std::byte>{}; };
    hooks.restore_group = [](std::span<const std::byte>) {};
    hooks.simulate = [](std::span<const replication::PredictedEntityId>,
                        const replication::InputCommand&,
                        const replication::PredictionStep&) {};
    return hooks;
}

// What the game saw: per tick, every PlayerInput it was handed, plus every hook call.
struct Observed {
    std::vector<std::vector<app::PlayerInput>> ticks;
    std::vector<app::ActionState> single_input;
    std::vector<app::PlayerSlot> joined, left;
    bool refuse_joins = false;
};

// A test game with `max_players` seats: a pawn per seat, moved by that seat's axis 0.
class SeatGame final : public app::Game {
public:
    explicit SeatGame(Observed& seen) : seen_(seen) {}

    bool setup(app::SetupContext&) override { return true; }

    bool on_player_joined(app::TickContext& ctx, app::PlayerSlot slot) override {
        seen_.joined.push_back(slot);
        if (seen_.refuse_joins)
            return false;
        core::Transform transform{};
        (void)ctx.world.spawn_with(ecs::LocalTransform{transform},
                                   ecs::WorldTransform{transform},
                                   SeatPawn{slot.index, slot.generation, 0.0f});
        return true;
    }

    void on_player_left(app::TickContext& ctx, app::PlayerSlot slot) override {
        seen_.left.push_back(slot);
        if (const ecs::Entity pawn = pawn_for(ctx.world, slot); ctx.world.is_alive(pawn))
            ctx.world.despawn(pawn);
    }

    void fixed_tick(app::TickContext& ctx) override {
        seen_.ticks.emplace_back(ctx.players.begin(), ctx.players.end());
        seen_.single_input.push_back(ctx.input);
        for (const app::PlayerInput& player : ctx.players)
            if (auto* pawn = ctx.world.get<SeatPawn>(pawn_for(ctx.world, player.slot)))
                pawn->x += player.actions.axis(0);
    }

private:
    Observed& seen_;
};

app::ActionState to_actions(const replication::InputCommand& command) {
    app::ActionState actions{};
    actions.axes[0] = command.move_x;
    return actions;
}

const app::GameDefinition::NetworkInputProfile kProfile{"rime.test.seats", 1, &to_actions};

app::GameDefinition::RemotePlayProfile remote_profile() {
    app::GameDefinition::RemotePlayProfile profile{};
    profile.register_replicated = [](ecs::World& world) {
        ecs::register_transform_components(world);
        (void)world.register_component<SeatPawn>();
    };
    // One-seat form: the first pawn (used only by the one-seat comparison).
    profile.owned_player = [](ecs::World& world) {
        ecs::Entity first{};
        world.query<SeatPawn>().for_each([&](ecs::Entity entity, const SeatPawn&) {
            if (first == ecs::Entity{})
                first = entity;
        });
        return first;
    };
    profile.seat_player = &pawn_for;
    profile.prediction_hooks = &pawn_hooks;
    profile.fixed_dt = kDt;
    return profile;
}

// One scripted client: its own driver on its own endpoint of the shared network.
struct Client {
    net::Endpoint endpoint{};
    net::Link* link = nullptr;
    std::unique_ptr<net::NetDriver> driver;
    std::optional<net::SessionId> id;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;
    std::optional<replication::InputHelloReply> reply;
    std::vector<std::byte> reply_bytes;
    std::vector<replication::OwnerCheckpoint> checkpoints;
    std::vector<replication::InputResult> results;
    bool connected = false;
    bool disconnected = false;
    std::uint32_t next_sequence = 1;

    void pump(std::uint64_t now) {
        events.clear();
        driver->update(now, events);
        inbox.clear();
        for (auto& e : events) {
            connected |= e.kind == net::SessionEvent::Kind::Connected;
            if (e.kind == net::SessionEvent::Kind::Disconnected) {
                disconnected = true;
                for (auto& message : e.undrained)
                    inbox.push_back(std::move(message));
            }
        }
        if (id)
            if (auto* session = driver->session(*id))
                session->drain_received(inbox);
        for (const auto& message : inbox) {
            replication::InputHelloReply decoded{};
            if (replication::decode_input_hello_reply(message.bytes, decoded)) {
                reply = decoded;
                reply_bytes = message.bytes;
                continue;
            }
            replication::InputResult result{};
            if (replication::decode_input_result(message.bytes, result)) {
                results.push_back(result);
                continue;
            }
            replication::OwnerCheckpoint checkpoint{};
            if (!message.bytes.empty() &&
                message.bytes[0] == std::byte{replication::kOwnerCheckpointTag} &&
                replication::decode_owner_checkpoint(message.bytes, checkpoint) ==
                    replication::CheckpointRefusal::None)
                checkpoints.push_back(std::move(checkpoint));
        }
    }

    bool hello(std::uint8_t codec, std::uint64_t identity, std::uint64_t now) {
        const replication::InputHello hello{codec, codec, identity};
        std::array<std::byte, replication::kInputHelloBytes> bytes{};
        const auto size = replication::encode_input_hello(hello, bytes);
        auto* session = driver->session(*id);
        return session && session->send_reliable(std::span(bytes).first(size), now);
    }

    // One fresh command with axis `move_x`, sent with its predecessor for redundancy.
    bool send_axis(float move_x, std::uint64_t now) {
        std::array<replication::InputCommand, 1> window{};
        window[0].sequence = next_sequence++;
        window[0].move_x = move_x;
        std::array<std::byte, replication::kInputV2HeaderBytes + replication::kInputV2CommandBytes>
            bytes{};
        const auto size = replication::encode_input_v2(window, bytes);
        auto* session = driver->session(*id);
        return session && session->send_unreliable(
                              std::span(bytes).first(size), now, app::kRemoteInputCommandsStream);
    }
};

// The deterministic world: a scripted network, one server pump in one world, N clients, and a
// tick that runs exactly what launch.cpp's dedicated loop runs.
struct Harness {
    net::ScriptedNetwork network{0x5EA7u, {0.0f, 0.0f, 0, 0}};
    const net::Endpoint server_endpoint{0x7F000001u, 7900};
    net::Link& server_link = network.add_node(server_endpoint);
    ecs::World world;
    Observed seen;
    SeatGame game{seen};
    app::GameDefinition::RemotePlayProfile profile = remote_profile();
    std::unique_ptr<app::NetworkInputServer> server;
    std::vector<std::unique_ptr<Client>> clients;
    std::uint64_t now = 0;
    std::uint64_t tick = 0;
    app::SeatConfig seats;

    explicit Harness(app::SeatConfig config, bool adaptive = false, bool start_now = true)
        : seats(config) {
        profile.register_replicated(world);
        if (start_now)
            start(adaptive);
    }

    void start(bool adaptive = false) {
        std::string error;
        server = app::NetworkInputServer::on_link(
            server_link, 60.0, kProfile, world, &profile, error, seats, adaptive);
        REQUIRE_MESSAGE(server != nullptr, error);
    }

    [[nodiscard]] std::uint64_t identity(std::uint16_t max_players) const {
        return app::input_schema_identity(kProfile, &profile, max_players);
    }

    Client& add_client(std::uint16_t port) {
        auto client = std::make_unique<Client>();
        client->endpoint = net::Endpoint{0x7F000001u, port};
        client->link = &network.add_node(client->endpoint);
        // A seed per client: two drivers sharing one would draw the same salts (net_driver.hpp).
        net::NetDriver::Config config{};
        config.salt_seed = 0x5EA70000u + port;
        client->driver = std::make_unique<net::NetDriver>(*client->link, config);
        client->id = client->driver->connect(server_endpoint, now);
        REQUIRE(client->id.has_value());
        clients.push_back(std::move(client));
        return *clients.back();
    }

    // One server tick, the production order: pump, seat hooks + inputs, game, publication.
    void server_tick() {
        app::TickContext ctx{world, kDt, tick};
        ctx.input = server->before_tick(tick, now);
        ctx.command = server->command_provenance();
        ctx.refuse_spawn = server->refuse_spawn();
        server->begin_seat_tick(game, ctx);
        game.fixed_tick(ctx);
        server->after_tick(tick);
        ++tick;
    }

    // The same delivery rhythm as remote_play_test: clients pump (and flush) at `now`, the
    // network delivers, then the server ticks; what the server sends goes out on its next pump.
    void step() {
        now += 16;
        network.advance_time(now);
        for (auto& client : clients)
            client->pump(now);
        network.advance_time(now);
        server_tick();
    }

    void step_until(const std::function<bool()>& done, const char* what, int limit = 300) {
        for (int i = 0; i < limit && !done(); ++i)
            step();
        INFO("waiting for: " << std::string(what));
        REQUIRE(done());
    }

    // Connect and send a hello; return once the client has a reply.
    Client& join(std::uint16_t port, std::uint8_t codec, std::uint64_t id_hash) {
        Client& client = add_client(port);
        step_until([&] { return client.connected; }, "connected");
        REQUIRE(client.hello(codec, id_hash, now));
        step_until([&] { return client.reply.has_value(); }, "hello reply");
        return client;
    }

    Client& join_seat(std::uint16_t port) {
        return join(port, replication::kInputCodecV3, identity(seats.max_players));
    }

    // The latest PlayerInput the game saw for `slot`, if any.
    [[nodiscard]] std::optional<app::PlayerInput> last_input(app::PlayerSlot slot) const {
        if (seen.ticks.empty())
            return std::nullopt;
        for (const auto& input : seen.ticks.back())
            if (input.slot == slot)
                return input;
        return std::nullopt;
    }
};

} // namespace

TEST_CASE("seats: two sessions are seated 0 and 1, and the hello reply names each seat") {
    Harness h({.max_players = 2, .max_clients = 2});
    Client& a = h.join_seat(7901);
    Client& b = h.join_seat(7902);
    REQUIRE(a.reply->refusal == replication::InputHelloRefusal::None);
    REQUIRE(b.reply->refusal == replication::InputHelloRefusal::None);
    CHECK(a.reply->codec == replication::kInputCodecV3);
    CHECK(a.reply_bytes.size() == replication::kInputHelloReplyV3Bytes);
    CHECK(a.reply->seated);
    CHECK(a.reply->seat == 0);
    CHECK(a.reply->seat_generation == 0);
    CHECK(b.reply->seated);
    CHECK(b.reply->seat == 1);
    CHECK(b.reply->seat_generation == 0);
    // The game was asked, once each, in seat order, and gave each seat its own body.
    REQUIRE(h.seen.joined.size() == 2);
    CHECK(h.seen.joined[0] == app::PlayerSlot{0, 0});
    CHECK(h.seen.joined[1] == app::PlayerSlot{1, 0});
    CHECK(h.server->counters().clients_admitted == 2);
    // Both seats appear in every later tick, ascending.
    h.step();
    REQUIRE(h.seen.ticks.back().size() == 2);
    CHECK(h.seen.ticks.back()[0].slot == app::PlayerSlot{0, 0});
    CHECK(h.seen.ticks.back()[1].slot == app::PlayerSlot{1, 0});
}

TEST_CASE("seats: each seat's commands reach only its own PlayerInput, result and checkpoint") {
    Harness h({.max_players = 2, .max_clients = 2});
    Client& a = h.join_seat(7901);
    Client& b = h.join_seat(7902);
    // Distinct values only one seat sends, and distinct sequence ranges: seat 1 starts at 500, so
    // a result carrying seat 0's frontier to seat 1 (or the reverse) is visible as a number.
    b.next_sequence = 500;
    std::size_t applied_a = 0, applied_b = 0;
    for (int i = 0; i < 40; ++i) {
        REQUIRE(a.send_axis(1.0f, h.now));
        REQUIRE(b.send_axis(-0.5f, h.now));
        h.step();
        for (const auto& input : h.seen.ticks.back()) {
            if (!input.command || !input.command->applied)
                continue;
            // THE anti-aliasing gate: an applied command on seat 0 is seat 0's value, and the
            // provenance names seat 0's own session and sequence range.
            if (input.slot == app::PlayerSlot{0, 0}) {
                CHECK(input.actions.axis(0) == 1.0f);
                CHECK(input.command->sequence < 500);
                CHECK(input.command->incarnation == a.driver->session(*a.id)->incarnation());
                ++applied_a;
            } else {
                REQUIRE(input.slot == app::PlayerSlot{1, 0});
                CHECK(input.actions.axis(0) == -0.5f);
                CHECK(input.command->sequence >= 500);
                CHECK(input.command->incarnation == b.driver->session(*b.id)->incarnation());
                ++applied_b;
            }
        }
        // A multi-seat game has no "the" player: the single-player fields stay neutral.
        CHECK(h.seen.single_input.back().axis(0) == 0.0f);
    }
    CHECK(applied_a > 30);
    CHECK(applied_b > 30);
    // Each pawn moved by its own seat's axis only.
    CHECK(h.world.get<SeatPawn>(pawn_for(h.world, {0, 0}))->x == doctest::Approx(applied_a * 1.0));
    CHECK(h.world.get<SeatPawn>(pawn_for(h.world, {1, 0}))->x == doctest::Approx(applied_b * -0.5));
    // Each client's results carry its own frontier, and its checkpoints its own body.
    REQUIRE(!a.results.empty());
    REQUIRE(!b.results.empty());
    for (const auto& result : a.results)
        CHECK((!result.any_retired || result.retired_through < 500));
    for (const auto& result : b.results)
        CHECK((!result.any_retired || result.retired_through >= 500));
    REQUIRE(!a.checkpoints.empty());
    REQUIRE(!b.checkpoints.empty());
    for (const auto& checkpoint : a.checkpoints) {
        REQUIRE(!checkpoint.entities.empty());
        CHECK(read_u32(checkpoint.entities[0].state, 0) == 0);
    }
    for (const auto& checkpoint : b.checkpoints) {
        REQUIRE(!checkpoint.entities.empty());
        CHECK(read_u32(checkpoint.entities[0].state, 0) == 1);
    }
    // And the two checkpoints name different replicated bodies.
    CHECK_FALSE(a.checkpoints.back().entities[0].id == b.checkpoints.back().entities[0].id);
    const auto counts = h.server->counters();
    REQUIRE(counts.seats.size() == 2);
    CHECK(counts.seats[0].applied == applied_a);
    CHECK(counts.seats[1].applied == applied_b);
    CHECK(counts.applied == applied_a + applied_b);
}

TEST_CASE("seats: one seat's stall does not move the other seat's schedule") {
    Harness h({.max_players = 2, .max_clients = 2}, /*adaptive=*/true);
    Client& a = h.join_seat(7901);
    Client& b = h.join_seat(7902);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(a.send_axis(1.0f, h.now));
        REQUIRE(b.send_axis(-1.0f, h.now));
        h.step();
    }
    const auto before = h.server->counters();
    // Seat 1 goes silent for 30 ticks while seat 0 keeps its cadence.
    std::size_t a_applied_ticks = 0;
    for (int i = 0; i < 30; ++i) {
        REQUIRE(a.send_axis(1.0f, h.now));
        h.step();
        const auto input = h.last_input({0, 0});
        REQUIRE(input.has_value());
        a_applied_ticks += input->command && input->command->applied ? 1 : 0;
        const auto other = h.last_input({1, 0});
        REQUIRE(other.has_value());
        CHECK_FALSE((other->command && other->command->applied));
    }
    const auto after = h.server->counters();
    // Seat 0 ran one of its own commands on every one of those ticks and never stalled; the stall
    // is entirely seat 1's.
    CHECK(a_applied_ticks == 30);
    CHECK(after.seats[0].applied - before.seats[0].applied == 30);
    CHECK(after.seats[0].stalls == before.seats[0].stalls);
    CHECK(after.seats[1].stalls > before.seats[1].stalls);
    // When seat 1 resumes, its commands are applied on seat 1, still with seat 1's value.
    for (int i = 0; i < 10; ++i) {
        REQUIRE(a.send_axis(1.0f, h.now));
        REQUIRE(b.send_axis(-1.0f, h.now));
        h.step();
    }
    const auto resumed = h.last_input({1, 0});
    REQUIRE(resumed.has_value());
    CHECK(resumed->actions.axis(0) == -1.0f);
}

TEST_CASE("seats: leave and rejoin reuses index 0 with generation + 1; the old slot is dead") {
    Harness h({.max_players = 2, .max_clients = 2});
    Client& a = h.join_seat(7901);
    Client& b = h.join_seat(7902);
    REQUIRE(a.reply->seat == 0);
    a.driver->session(*a.id)->disconnect();
    h.step_until([&] { return !h.seen.left.empty(); }, "on_player_left");
    // The game was told with the slot it keyed the body by, and removed it.
    REQUIRE(h.seen.left.size() == 1);
    CHECK(h.seen.left[0] == app::PlayerSlot{0, 0});
    CHECK(pawn_for(h.world, {0, 0}) == ecs::Entity{});
    CHECK(h.server->counters().clients_left == 1);
    // While the seat is empty, only seat 1 is driven.
    h.step();
    REQUIRE(h.seen.ticks.back().size() == 1);
    CHECK(h.seen.ticks.back()[0].slot == app::PlayerSlot{1, 0});
    // A newcomer takes the lowest free index -- 0 -- on the next generation.
    Client& c = h.join_seat(7903);
    REQUIRE(c.reply->refusal == replication::InputHelloRefusal::None);
    CHECK(c.reply->seat == 0);
    CHECK(c.reply->seat_generation == 1);
    CHECK(h.seen.joined.back() == app::PlayerSlot{0, 1});
    // The old slot addresses nothing: no tick hands the game {0, 0} again, and the newcomer's
    // body is its own (generation 1), not the departed player's.
    REQUIRE(c.send_axis(0.25f, h.now));
    for (int i = 0; i < 5; ++i)
        h.step();
    for (const auto& input : h.seen.ticks.back())
        CHECK_FALSE(input.slot == app::PlayerSlot{0, 0});
    CHECK(h.last_input({0, 1}).has_value());
    CHECK(pawn_for(h.world, {0, 1}) != ecs::Entity{});
    REQUIRE(!c.checkpoints.empty());
    CHECK(read_u32(c.checkpoints.back().entities[0].state, 4) == 1); // the gen-1 body
    (void)b;
}

TEST_CASE("seats: a third session beyond --max-clients 2 is refused as full, and counted") {
    Harness h({.max_players = 4, .max_clients = 2});
    (void)h.join_seat(7901);
    (void)h.join_seat(7902);
    Client& c = h.join_seat(7903);
    CHECK(c.reply->refusal == replication::InputHelloRefusal::Full);
    CHECK_FALSE(c.reply->seated);
    CHECK(c.reply_bytes.size() == replication::kInputHelloReplyBytes); // refusals carry no seat
    const auto counts = h.server->counters();
    CHECK(counts.hello_full == 1);
    CHECK(counts.clients_refused == 1);
    CHECK(counts.clients_admitted == 2);
    CHECK(counts.max_clients == 2);
    CHECK(h.seen.joined.size() == 2); // the game was never asked about the third
}

TEST_CASE("seats: a new session from a seated endpoint replaces it -- never a second seat") {
    // One seat per source endpoint. The transport enforces it first: a fresh connect from the
    // same address:port (a restarted client) REPLACES the old session, whose departure releases
    // the seat before the new hello can arrive. So the table's DuplicateEndpoint refusal is not
    // reachable through NetDriver; this pins the property it guards instead.
    Harness h({.max_players = 2, .max_clients = 2});
    Client& a = h.join_seat(7901);
    REQUIRE(a.reply->seat == 0);
    // The "restarted process": a new driver on the same endpoint, with a fresh salt.
    net::NetDriver::Config restarted{};
    restarted.salt_seed = 0xFEEDu;
    a.driver = std::make_unique<net::NetDriver>(*a.link, restarted);
    a.id = a.driver->connect(h.server_endpoint, h.now);
    a.connected = false;
    a.reply.reset();
    h.step_until([&] { return a.connected; }, "restarted connect");
    REQUIRE(a.hello(replication::kInputCodecV3, h.identity(2), h.now));
    h.step_until([&] { return a.reply.has_value(); }, "restarted reply");
    CHECK(a.reply->refusal == replication::InputHelloRefusal::None);
    CHECK(a.reply->seat == 0);
    CHECK(a.reply->seat_generation == 1);
    const auto counts = h.server->counters();
    CHECK(counts.duplicate_endpoint == 0);
    CHECK(counts.clients_left == 1);
    CHECK(counts.clients_admitted == 2);
    REQUIRE(h.seen.left.size() == 1);
    CHECK(h.seen.left[0] == app::PlayerSlot{0, 0});
    h.step();
    CHECK(h.seen.ticks.back().size() == 1); // one endpoint, one seat
}

TEST_CASE("seats: a codec-2 hello to a multi-seat server is refused as version") {
    Harness h({.max_players = 2, .max_clients = 2});
    // An m20.7 client: codec 2, and the identity it would compute (no seat fold).
    Client& old = h.join(7901, replication::kInputCodecV2, h.identity(1));
    CHECK(old.reply->refusal == replication::InputHelloRefusal::Version);
    CHECK(old.reply_bytes.size() == replication::kInputHelloReplyBytes); // decodable by m20.7
    // Codec 2 with the multi-seat identity is still a version refusal: codec is checked first.
    Client& mixed = h.join(7902, replication::kInputCodecV2, h.identity(2));
    CHECK(mixed.reply->refusal == replication::InputHelloRefusal::Version);
    // Codec 3 with a ONE-seat identity: the seat fold makes it a schema refusal.
    Client& wrong = h.join(7903, replication::kInputCodecV3, h.identity(1));
    CHECK(wrong.reply->refusal == replication::InputHelloRefusal::Schema);
    const auto counts = h.server->counters();
    CHECK(counts.hello_version == 2);
    CHECK(counts.hello_schema == 1);
    CHECK(counts.clients_admitted == 0);
    CHECK(h.seen.joined.empty());
    CHECK(h.identity(2) != h.identity(1));
}

TEST_CASE("seats: a join the game refuses is answered game_refused and frees the seat") {
    Harness h({.max_players = 2, .max_clients = 2});
    h.seen.refuse_joins = true;
    Client& a = h.join_seat(7901);
    CHECK(a.reply->refusal == replication::InputHelloRefusal::GameRefused);
    CHECK(h.seen.joined.size() == 1);
    h.seen.refuse_joins = false;
    Client& b = h.join_seat(7902);
    REQUIRE(b.reply->refusal == replication::InputHelloRefusal::None);
    // The refused join released index 0, so the next player takes it on the next generation.
    CHECK(b.reply->seat == 0);
    CHECK(b.reply->seat_generation == 1);
    const auto counts = h.server->counters();
    CHECK(counts.game_refused_join == 1);
    CHECK(counts.clients_refused == 1);
    CHECK(counts.clients_admitted == 1);
}

TEST_CASE("seats: max_players == 1 keeps the m20.7 hello, input fields and no seat hooks") {
    // The same harness on a ONE-seat server: what every existing game gets. The pinned m20.7
    // behaviour is the 3-byte reply E2 02 00, codec 2, the unchanged identity, no hook calls,
    // and `ctx.input` equal to the one seat's actions. (Byte-level checkpoint and counter
    // identity is held by the untouched remote_play/interaction tests and the process diff.)
    Harness h({.max_players = 1, .max_clients = 16}, false, /*start_now=*/false);
    // The one-seat game spawns its player itself, before anyone connects (hello-game's shape).
    core::Transform transform{};
    (void)h.world.spawn_with(
        ecs::LocalTransform{transform}, ecs::WorldTransform{transform}, SeatPawn{});
    h.start();
    CHECK(h.identity(1) == app::input_schema_identity(kProfile, &h.profile));
    Client& a = h.join(7901, replication::kInputCodecV2, h.identity(1));
    REQUIRE(a.reply->refusal == replication::InputHelloRefusal::None);
    const std::vector<std::byte> m20_7_reply{
        std::byte{replication::kInputHelloReplyTag}, std::byte{2}, std::byte{0}};
    CHECK(a.reply_bytes == m20_7_reply);
    CHECK_FALSE(a.reply->seated);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(a.send_axis(0.75f, h.now));
        h.step();
        REQUIRE(h.seen.ticks.back().size() == 1);
        CHECK(h.seen.ticks.back()[0].actions.axis(0) == h.seen.single_input.back().axis(0));
    }
    CHECK(h.seen.single_input.back().axis(0) == 0.75f);
    CHECK(h.server->command_provenance().has_value());
    CHECK(h.seen.joined.empty());
    CHECK(h.seen.left.empty());
    // A second client is refused as m20.3's `full`, in the 3-byte reply.
    Client& b = h.join(7902, replication::kInputCodecV2, h.identity(1));
    CHECK(b.reply->refusal == replication::InputHelloRefusal::Full);
    CHECK(b.reply_bytes.size() == replication::kInputHelloReplyBytes);
    // One seat means two transport sessions (m20.3's +1 for `full`): the refused peer's slot is
    // held until its deferred close completes, so the next peer connects after that.
    h.step_until([&] { return b.disconnected; }, "refused peer closed");
    for (int i = 0; i < 120; ++i) // the server's side lingers in Closing to resend its goodbye
        h.step();
    // A codec-3 client cannot reach a one-seat server.
    Client& c = h.join(7903, replication::kInputCodecV3, h.identity(1));
    CHECK(c.reply->refusal == replication::InputHelloRefusal::Version);
    const auto counts = h.server->counters();
    CHECK(counts.max_clients == 1);
    CHECK(counts.clients_refused == 1);
    CHECK(counts.hello_full == 1);
}
