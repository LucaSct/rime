// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

#include "rime/core/byte_cursor.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/input.hpp"
#include "rime/replication/server_replicator.hpp"

// m11.6c's proofs: the client→server input path.
//
// The harness discipline is m11.1's, unchanged: a ScriptedNetwork on a virtual clock so loss is an
// INPUT rather than environment luck, every wait a bounded tick loop so a failure is a deadline
// rather than a hang, and every proof asserting its own non-vacuousness — a loss test that dropped
// nothing proves nothing.
//
// The three cases worth reading first, because they are the ones that would fail if the design were
// wrong rather than merely if the code were:
//
//   "the redundancy window..."      — a negative control that measures the window against itself
//                                     with redundancy 1, on the same seed and the same losses.
//   "the acknowledgement follows..." — the replication invariant pointed upstream: the frontier
//                                     must move on the game DRAINING, never on bytes arriving.
//   "a client cannot grow..."       — the buffer bound, and the honest cost of reaching it.
using namespace rime;

namespace {

constexpr std::uint16_t kServerPort = 7801;
constexpr std::uint16_t kClientPort = 7802;
constexpr std::uint64_t kTickMs = 16;

// A connected server/client pair with nothing replicated — the input path is the only tenant, so
// counters are unambiguous. Sessions still need the schema hash to agree, so both worlds register
// the same components.
struct Peers {
    net::ScriptedNetwork network;
    net::Endpoint server_endpoint{0x7F000001u, kServerPort};
    net::Endpoint client_endpoint{0x7F000001u, kClientPort};
    ecs::World server_world;
    ecs::World client_world;
    net::Link& server_link;
    net::Link& client_link;
    net::NetDriver server_driver;
    net::NetDriver client_driver;
    std::vector<net::SessionEvent> events;
    std::uint64_t now_ms = 0;

    static net::NetDriver::Config config_for(ecs::World& world, std::uint64_t salt_seed) {
        net::NetDriver::Config config;
        config.app_id = 0x52494D45u; // 'RIME'
        config.schema_hash = ecs::component_schema_hash(world);
        config.salt_seed = salt_seed;
        return config;
    }

    explicit Peers(net::ScriptedNetwork::Config net_config, std::uint64_t seed = 0xC0FFEEull)
        : network(seed, net_config), server_link(network.add_node(server_endpoint)),
          client_link(network.add_node(client_endpoint)),
          server_driver(server_link,
                        [this] {
                            ecs::register_transform_components(server_world);
                            return config_for(server_world, 0x1111ull);
                        }()),
          client_driver(client_link, [this] {
              ecs::register_transform_components(client_world);
              return config_for(client_world, 0x2222ull);
          }()) {
        server_driver.listen();
        REQUIRE(client_driver.connect(server_endpoint, now_ms).has_value());
    }

    // Advance both drivers one tick. Callers layer their own PreSim/Publish work around it.
    void pump() {
        now_ms += kTickMs;
        network.advance_time(now_ms);
        events.clear();
        server_driver.update(now_ms, events);
        events.clear();
        client_driver.update(now_ms, events);
    }

    // Run ticks until the handshake completes, bounded.
    void connect() {
        for (int i = 0; i < 200 && server_driver.session_count() == 0; ++i) {
            pump();
        }
        REQUIRE(server_driver.session_count() == 1);
    }

    [[nodiscard]] net::SessionId server_session() const {
        return server_driver.session_ids().front();
    }

    // Drain the client's one session into `out` (appended). The client end has several readers of
    // the same span, so draining is deliberately something the caller does once and shares.
    void drain_client(std::vector<net::Received>& out) {
        for (const net::SessionId id : client_driver.session_ids()) {
            net::Session* session = client_driver.session(id);
            REQUIRE(session != nullptr);
            (void)session->drain_received(out);
        }
    }
};

platform::Event key_event(platform::EventType type, platform::Key key, bool repeat = false) {
    platform::Event e{};
    e.type = type;
    e.key = {key, platform::KeyMods::None, repeat};
    return e;
}

platform::Event mouse_move(float dx, float dy) {
    platform::Event e{};
    e.type = platform::EventType::MouseMove;
    e.mouse_move = {0.0f, 0.0f, dx, dy};
    return e;
}

platform::Event focus_event(bool focused) {
    platform::Event e{};
    e.type = platform::EventType::WindowFocus;
    e.focus = {focused};
    return e;
}

} // namespace

// ── The sampler: events are edges, the wire wants levels ────────────────────────────────────────

TEST_CASE("held state survives frames that carry no events") {
    // The whole reason InputSampler exists. `frame_input()` reports "W went down" on ONE frame; the
    // player is still holding W on the ninety after it. An implementation that read only the
    // current frame's events would send held=0 on every frame but the first — the character would
    // twitch forward once and stop.
    replication::InputSampler sampler;
    sampler.set_bindings(replication::default_action_bindings(),
                         replication::default_axis_bindings());

    const platform::Event down = key_event(platform::EventType::KeyDown, platform::Key::W);
    sampler.accumulate(std::span{&down, 1});
    CHECK(sampler.build(1).move_y == doctest::Approx(1.0f));

    for (int i = 0; i < 10; ++i) {
        sampler.accumulate({}); // a frame in which nothing happened at all
    }
    CHECK(sampler.build(2).move_y == doctest::Approx(1.0f));

    const platform::Event up = key_event(platform::EventType::KeyUp, platform::Key::W);
    sampler.accumulate(std::span{&up, 1});
    CHECK(sampler.build(3).move_y == doctest::Approx(0.0f));
}

TEST_CASE("a press is an edge, reported by exactly one command") {
    replication::InputSampler sampler;
    sampler.set_bindings(replication::default_action_bindings(),
                         replication::default_axis_bindings());

    const std::uint32_t primary = replication::default_action_bindings()[0].bit;
    const platform::Key primary_key = replication::default_action_bindings()[0].key;

    const platform::Event down = key_event(platform::EventType::KeyDown, primary_key);
    sampler.accumulate(std::span{&down, 1});

    const replication::InputCommand first = sampler.build(1);
    CHECK((first.pressed & primary) != 0);
    CHECK((first.held & primary) != 0);

    // The edge is consumed; the level is not. Getting this backwards is one trigger pull becoming
    // one per tick for as long as the finger is down.
    const replication::InputCommand second = sampler.build(2);
    CHECK((second.pressed & primary) == 0);
    CHECK((second.held & primary) != 0);
}

TEST_CASE("OS auto-repeat is not a press") {
    // Holding a key makes the OS deliver a stream of KeyDowns. Counting them as presses turns one
    // trigger pull into thirty a second — and on this path each one is a shot the server fires.
    replication::InputSampler sampler;
    sampler.set_bindings(replication::default_action_bindings(),
                         replication::default_axis_bindings());
    const std::uint32_t primary = replication::default_action_bindings()[0].bit;
    const platform::Key primary_key = replication::default_action_bindings()[0].key;

    const platform::Event down = key_event(platform::EventType::KeyDown, primary_key);
    sampler.accumulate(std::span{&down, 1});
    CHECK((sampler.build(1).pressed & primary) != 0);

    const platform::Event repeat = key_event(platform::EventType::KeyDown, primary_key, true);
    for (int i = 0; i < 20; ++i) {
        sampler.accumulate(std::span{&repeat, 1});
    }
    CHECK((sampler.build(2).pressed & primary) == 0);
    CHECK((sampler.build(2).held & primary) != 0);
}

TEST_CASE("losing focus releases every held key") {
    // The OS delivers the KeyUp to whoever has focus now, which is not us. Without this,
    // alt-tabbing mid-stride tells the server the player is walking forward forever.
    replication::InputSampler sampler;
    sampler.set_bindings(replication::default_action_bindings(),
                         replication::default_axis_bindings());

    const platform::Event down = key_event(platform::EventType::KeyDown, platform::Key::W);
    sampler.accumulate(std::span{&down, 1});
    REQUIRE(sampler.is_held(platform::Key::W));

    const platform::Event lost = focus_event(false);
    sampler.accumulate(std::span{&lost, 1});
    CHECK_FALSE(sampler.is_held(platform::Key::W));
    CHECK(sampler.build(1).move_y == doctest::Approx(0.0f));
}

TEST_CASE("view angles accumulate relative motion and stay off the poles") {
    replication::InputSampler sampler;
    sampler.set_mouse_sensitivity(0.01f);

    for (int i = 0; i < 1000; ++i) {
        const platform::Event move = mouse_move(0.0f, 100.0f); // straight up, for a long time
        sampler.accumulate(std::span{&move, 1});
    }
    const replication::InputCommand command = sampler.build(1);

    // Clamped short of exactly π/2: at the pole the forward vector is parallel to world up and
    // every look-at basis built from the pair degenerates.
    CHECK(command.pitch < std::numbers::pi_v<float> / 2.0f);
    CHECK(command.pitch > std::numbers::pi_v<float> / 2.0f - 0.01f);
}

// ── sanitize: the server trusts nothing ─────────────────────────────────────────────────────────

TEST_CASE("sanitize replaces non-finite fields rather than clamping them") {
    // The trap this exists for: NaN compares false against every bound, so `std::clamp(nan, lo,
    // hi)` returns the NaN. Code that looks like it validates its inputs frequently does not, and a
    // NaN reaching the solver poisons a whole physics step.
    replication::InputCommand command{};
    command.move_x = std::numeric_limits<float>::quiet_NaN();
    command.yaw = std::numeric_limits<float>::infinity();

    CHECK(replication::sanitize(command));
    CHECK(std::isfinite(command.move_x));
    CHECK(std::isfinite(command.yaw));
    CHECK(command.move_x == doctest::Approx(0.0f));
}

TEST_CASE("sanitize normalizes the movement disc rather than each axis") {
    // Clamping the axes independently leaves a diagonal at length √2 — the strafe-running bug. Here
    // it would not be a quirk but a client-chosen 41% speed bonus, which is the difference between
    // a physics oddity and a cheat.
    replication::InputCommand command{};
    command.move_x = 1.0f;
    command.move_y = 1.0f;

    CHECK(replication::sanitize(command));
    const float length =
        std::sqrt(command.move_x * command.move_x + command.move_y * command.move_y);
    CHECK(length == doctest::Approx(1.0f));

    // A client that simply lies is reduced to the same unit disc as an honest one.
    replication::InputCommand liar{};
    liar.move_y = 1e6f;
    CHECK(replication::sanitize(liar));
    CHECK(liar.move_y == doctest::Approx(1.0f));
}

TEST_CASE("an honest sampler never produces a command the server has to correct") {
    // Why this matters beyond tidiness: commands_sanitized() is meant to be a signal about the
    // PEER. If our own sampler routinely emitted out-of-contract commands, the counter would read
    // non-zero for every honest client and tell us nothing about a dishonest one.
    replication::InputSampler sampler;
    sampler.set_bindings(replication::default_action_bindings(),
                         replication::default_axis_bindings());

    const platform::Event w = key_event(platform::EventType::KeyDown, platform::Key::W);
    const platform::Event d = key_event(platform::EventType::KeyDown, platform::Key::D);
    sampler.accumulate(std::span{&w, 1});
    sampler.accumulate(std::span{&d, 1});

    replication::InputCommand diagonal = sampler.build(1);
    CHECK_FALSE(replication::sanitize(diagonal));
}

// ── The wire, over a link that really loses packets ─────────────────────────────────────────────

TEST_CASE("commands arrive in order, exactly once, over a lossy link") {
    Peers peers({/*loss_rate=*/0.20f,
                 /*duplicate_rate=*/0.0f,
                 /*min_latency_ms=*/5,
                 /*max_latency_ms=*/40});
    peers.connect();

    replication::ClientInputSender sender;
    replication::ServerInputReceiver receiver;
    std::vector<replication::InputCommand> delivered;

    constexpr int kTicks = 400;
    for (int i = 0; i < kTicks; ++i) {
        peers.pump();
        (void)receiver.apply_inbound(peers.server_driver);
        (void)receiver.drain(peers.server_session(), delivered);

        replication::InputCommand command{};
        command.move_y = 1.0f;
        (void)sender.record(command);
        sender.send(peers.client_driver, peers.now_ms);
        receiver.send_acks(peers.server_driver, peers.now_ms);
    }

    // Non-vacuousness first: a "survives loss" proof over a link that lost nothing proves nothing.
    REQUIRE(peers.network.packets_dropped() > 0);
    REQUIRE(receiver.commands_duplicate() > 0); // the redundancy window really did repeat commands
    REQUIRE(delivered.size() > 0);

    // Strictly ascending, so nothing was delivered twice and nothing arrived out of order. The
    // channel is unreliable-SEQUENCED, so old packets are dropped by the channel — but a packet
    // carrying commands 5,6,7 can still land after one carrying 6,7,8, and the receive frontier is
    // what makes that harmless.
    for (std::size_t i = 1; i < delivered.size(); ++i) {
        CHECK(delivered[i].sequence > delivered[i - 1].sequence);
    }
    CHECK(delivered.back().sequence <= static_cast<std::uint32_t>(kTicks));

    // The gap count and the delivered count must together account for every command the client
    // recorded — a receiver that silently ate commands would satisfy the ordering check above.
    CHECK(delivered.size() + receiver.gaps_observed() == delivered.back().sequence);
}

TEST_CASE("the redundancy window is what survives the loss — a negative control") {
    // The claim under test is not "few commands are lost" (a threshold, which some other design
    // could accidentally meet) but "the window is the reason". So: the same scenario twice, same
    // seed, same scripted losses, differing only in how many copies each packet carries.
    const auto run = [](std::size_t redundancy) {
        Peers peers({/*loss_rate=*/0.30f,
                     /*duplicate_rate=*/0.0f,
                     /*min_latency_ms=*/5,
                     /*max_latency_ms=*/40},
                    /*seed=*/0xABCDEFull);
        peers.connect();

        replication::ClientInputSender sender;
        sender.set_redundancy(redundancy);
        replication::ServerInputReceiver receiver;
        std::vector<replication::InputCommand> delivered;

        for (int i = 0; i < 400; ++i) {
            peers.pump();
            (void)receiver.apply_inbound(peers.server_driver);
            (void)receiver.drain(peers.server_session(), delivered);
            (void)sender.record(replication::InputCommand{});
            sender.send(peers.client_driver, peers.now_ms);
            receiver.send_acks(peers.server_driver, peers.now_ms);
        }
        return receiver.gaps_observed();
    };

    const std::uint64_t without = run(1);
    const std::uint64_t with = run(replication::kInputRedundancy);

    REQUIRE(without > 0); // 30% loss and one copy per command: of course input is lost
    CHECK(with < without);

    // And by a margin no accident produces. Measured on this seed: 146 commands lost with one copy
    // each, 23 with three — about 6×, short of the ~11× independent losses would predict, because
    // the windows of consecutive packets overlap and the link's drops are not independent across
    // them. The assertion is set at 5× rather than at the measured value so it tests the PROPERTY
    // (redundancy is what carries the input through) instead of pinning this seed's arithmetic; a
    // future "optimization" collapsing the window back to one copy fails here loudly either way.
    CHECK(with * 5 < without);
}

// ── The acknowledgement frontier: the replication invariant, pointed upstream ────────────────────

TEST_CASE("the acknowledgement follows consumption, not arrival") {
    // docs/design/replication.md, corollary 1, in the one direction the document does not itself
    // cover: a claim about what a peer holds needs evidence of HOLDING. Here the peer is the SERVER
    // and the claim is "I have your input". Bytes landing in the receiver's buffer are not that
    // evidence — the game has not seen them, and a server that stalls or overflows may never hand
    // them over. If the ack advanced on arrival, the client would retire commands a predictor still
    // needs to replay.
    Peers peers({0.0f, 0.0f, 0, 0}); // clean link: this is about bookkeeping, not loss
    peers.connect();

    replication::ClientInputSender sender;
    replication::ServerInputReceiver receiver;
    std::vector<net::Received> inbox;

    constexpr int kCommands = 20;
    for (int i = 0; i < kCommands; ++i) {
        peers.pump();
        (void)receiver.apply_inbound(peers.server_driver); // arrives...
        //  ...and is deliberately NOT drained
        inbox.clear();
        peers.drain_client(inbox); // the client IS listening, so a premature ack would be caught
        sender.apply_messages(inbox);
        (void)sender.record(replication::InputCommand{});
        sender.send(peers.client_driver, peers.now_ms);
        receiver.send_acks(peers.server_driver, peers.now_ms);
    }
    // Let the last packets land, so "nothing was acknowledged" is a statement about bookkeeping
    // rather than about bytes still being in flight.
    for (int i = 0; i < 5; ++i) {
        peers.pump();
        (void)receiver.apply_inbound(peers.server_driver);
        inbox.clear();
        peers.drain_client(inbox);
        sender.apply_messages(inbox);
        receiver.send_acks(peers.server_driver, peers.now_ms);
    }

    REQUIRE(receiver.commands_accepted() == kCommands); // every byte arrived and parsed
    CHECK(receiver.consumed_through(peers.server_session()) == 0);
    CHECK(sender.acked_through() == 0);
    CHECK(sender.unacked().size() == kCommands); // nothing retired on the strength of arrival

    // Now let the game take them, and the frontier moves — with the client's history retiring one
    // round trip later, on evidence that is finally the right evidence.
    std::vector<replication::InputCommand> delivered;
    (void)receiver.drain(peers.server_session(), delivered);
    CHECK(delivered.size() == kCommands);
    CHECK(receiver.consumed_through(peers.server_session()) == kCommands);

    bool retired = false;
    for (int i = 0; i < 50 && !retired; ++i) {
        receiver.send_acks(peers.server_driver, peers.now_ms);
        peers.pump();
        inbox.clear();
        peers.drain_client(inbox);
        sender.apply_messages(inbox);
        retired = sender.acked_through() == kCommands;
    }
    REQUIRE_MESSAGE(retired, "the ack never reached the client before the deadline");
    CHECK(sender.unacked().empty());
    CHECK(sender.acks_received() > 0);
}

TEST_CASE("a stale acknowledgement cannot resurrect retired commands") {
    // Acks ride the unreliable channel, so a stale one can land behind a fresher one. A frontier
    // that took "whatever arrived last" would move backwards and hand a predictor commands the
    // server finished with.
    replication::ClientInputSender sender;
    for (int i = 0; i < 10; ++i) {
        (void)sender.record(replication::InputCommand{});
    }

    const auto ack_bytes = [](std::uint32_t consumed) {
        std::vector<std::byte> bytes;
        core::ByteWriter writer{bytes};
        writer.u8(static_cast<std::uint8_t>(replication::MessageTag::InputAck));
        writer.u32(consumed);
        return bytes;
    };

    const std::vector<std::byte> fresh = ack_bytes(8);
    const std::vector<std::byte> stale = ack_bytes(3);

    std::vector<net::Received> inbox;
    inbox.push_back(net::Received{net::Channel::UnreliableSequenced, fresh});
    sender.apply_messages(inbox);
    CHECK(sender.acked_through() == 8);
    CHECK(sender.unacked().size() == 2);

    inbox.clear();
    inbox.push_back(net::Received{net::Channel::UnreliableSequenced, stale});
    sender.apply_messages(inbox);
    CHECK(sender.acked_through() == 8);
    CHECK(sender.unacked().size() == 2);
}

// ── Bounds, and what a hostile peer can and cannot do ───────────────────────────────────────────

TEST_CASE("a client cannot grow the server's memory without bound") {
    // The peer chooses how fast it sends. An unbounded per-client buffer is a denial of service
    // wearing a resilience feature's clothes — the same rule kMaxDeferredRecords answers on the
    // downstream side.
    Peers peers({0.0f, 0.0f, 0, 0});
    peers.connect();

    replication::ClientInputSender sender;
    sender.set_redundancy(1); // one command per packet keeps the arithmetic below exact
    replication::ServerInputReceiver receiver;

    // The game never drains. The client keeps talking.
    const std::size_t flood = replication::kMaxBufferedCommands * 3;
    for (std::size_t i = 0; i < flood; ++i) {
        peers.pump();
        (void)receiver.apply_inbound(peers.server_driver);
        (void)sender.record(replication::InputCommand{});
        sender.send(peers.client_driver, peers.now_ms);
    }
    for (int i = 0; i < 10; ++i) { // let the last packets land
        peers.pump();
        (void)receiver.apply_inbound(peers.server_driver);
    }

    REQUIRE(receiver.commands_dropped_overflow() > 0);

    std::vector<replication::InputCommand> delivered;
    const std::size_t drained = receiver.drain(peers.server_session(), delivered);
    CHECK(drained <= replication::kMaxBufferedCommands);

    // Drop-OLDEST, not drop-newest: after a stall, the intent worth having is the recent intent.
    // With no loss and one command per packet, the newest sequence accepted is the last one sent,
    // and it is still here — a drop-newest buffer would have discarded exactly this one.
    CHECK(delivered.back().sequence == receiver.commands_accepted());

    // And the documented consequence, asserted so nobody "fixes" it quietly: the frontier steps
    // OVER the dropped commands, so the client will retire them believing they were acted on.
    // Reconciliation therefore has to compare resulting state, never a diff of command lists.
    CHECK(receiver.consumed_through(peers.server_session()) > receiver.commands_dropped_overflow());
}

TEST_CASE("a malformed packet is counted, not obeyed") {
    replication::ServerInputReceiver receiver;
    const net::SessionId id{1, 1};

    const auto message = [](std::vector<std::byte> bytes) {
        std::vector<net::Received> inbox;
        inbox.push_back(net::Received{net::Channel::UnreliableSequenced, std::move(bytes)});
        return inbox;
    };

    // A count field larger than any packet could carry. The bound must be the constant, never the
    // peer's number, or this is a loop the peer controls.
    std::vector<std::byte> hostile;
    core::ByteWriter writer{hostile};
    writer.u8(static_cast<std::uint8_t>(replication::MessageTag::InputCommands));
    writer.u8(255);
    CHECK(receiver.apply_messages(id, message(hostile)) == 0);
    CHECK(receiver.malformed_messages() == 1);

    // A truncated command: the header promises two, the bytes carry one and a half.
    std::vector<std::byte> truncated;
    core::ByteWriter short_writer{truncated};
    short_writer.u8(static_cast<std::uint8_t>(replication::MessageTag::InputCommands));
    short_writer.u8(2);
    for (std::size_t i = 0; i < replication::kInputCommandBytes + 4; ++i) {
        short_writer.u8(0xAB);
    }
    CHECK(receiver.apply_messages(id, message(truncated)) == 1); // the first command was whole
    CHECK(receiver.malformed_messages() == 2);
}

TEST_CASE("a disconnected client's input state does not outlive it") {
    // Corollary 2's third bullet in docs/design/replication.md: a record keyed by a recyclable slot
    // must not outlive its subject. Instance six of that bug lived in this very module.
    replication::ServerInputReceiver receiver;
    const net::SessionId id{4, 1};

    std::vector<std::byte> bytes;
    core::ByteWriter writer{bytes};
    writer.u8(static_cast<std::uint8_t>(replication::MessageTag::InputCommands));
    writer.u8(1);
    writer.u32(7);
    for (int i = 0; i < 6; ++i) {
        writer.u32(0);
    }
    std::vector<net::Received> inbox;
    inbox.push_back(net::Received{net::Channel::UnreliableSequenced, bytes});
    REQUIRE(receiver.apply_messages(id, inbox) == 1);

    std::vector<replication::InputCommand> delivered;
    (void)receiver.drain(id, delivered);
    REQUIRE(receiver.consumed_through(id) == 7);

    // Through the event batch, which is how a server actually reaches this — `forget` on its own
    // would be a correct function nobody calls, and the slot would never be freed.
    net::SessionEvent ended{};
    ended.kind = net::SessionEvent::Kind::Disconnected;
    ended.id = id;
    receiver.on_session_events(std::span{&ended, 1});
    CHECK(receiver.consumed_through(id) == 0);

    // The recycled slot must start clean, or the next tenant's first drain hands the game its
    // predecessor's intent.
    const net::SessionId reincarnation{4, 2};
    std::vector<replication::InputCommand> after;
    CHECK(receiver.drain(reincarnation, after) == 0);
    CHECK(after.empty());
}

// ── Sharing one session with the rest of replication ────────────────────────────────────────────

TEST_CASE("input and state replication share a session without eating each other's mail") {
    // The m11.4a lesson, now applying INSIDE replication's own tag block: drain_received MOVES
    // messages out, so a reader that consumes a tag it does not own makes the real owner's mail
    // vanish. Two readers on the client, two message kinds in both directions, one session.
    Peers peers({0.0f, 0.0f, 0, 0});
    peers.connect();

    replication::ServerReplicator server{peers.server_world};
    replication::ClientReplicator client{peers.client_world};
    replication::ClientInputSender sender;
    replication::ServerInputReceiver receiver;

    const ecs::Entity moving = peers.server_world.spawn_with(ecs::LocalTransform{});
    (void)server.replicate(moving);

    std::vector<net::Received> client_inbox;
    std::vector<replication::InputCommand> delivered;

    for (int i = 0; i < 60; ++i) {
        peers.pump();

        // ── server PreSim: ONE drain, both readers ──
        //
        // NOT ServerReplicator::apply_inbound: that convenience form drains the sessions itself and
        // so takes sole ownership of the mail. With a second reader present it is exactly the bug
        // the shared tag space was documented against — whoever drains first consumes everything
        // and the other subsystem silently never receives.
        for (const net::SessionId id : peers.server_driver.session_ids()) {
            net::Session* session = peers.server_driver.session(id);
            REQUIRE(session != nullptr);
            std::vector<net::Received> server_inbox;
            (void)session->drain_received(server_inbox);
            (void)server.apply_messages(id, server_inbox);
            (void)receiver.apply_messages(id, server_inbox);
        }
        (void)receiver.drain(peers.server_session(), delivered);

        // ── client PreSim: ONE drain, both readers ──
        client_inbox.clear();
        peers.drain_client(client_inbox);
        client.apply_messages(client_inbox);
        sender.apply_messages(client_inbox);

        // ── sim ──
        peers.server_world.advance_version();
        auto* transform = peers.server_world.get<ecs::LocalTransform>(moving);
        REQUIRE(transform != nullptr);
        transform->value.translation.x += 1.0f;
        peers.server_world.mark_changed<ecs::LocalTransform>(moving);

        // ── Publish ──
        server.publish(peers.server_driver, peers.now_ms);
        receiver.send_acks(peers.server_driver, peers.now_ms);
        (void)sender.record(replication::InputCommand{});
        sender.send(peers.client_driver, peers.now_ms);
        client.send_ack(peers.client_driver, peers.now_ms);
    }

    // Each reader got its own mail...
    CHECK(client.deltas_applied() > 0);
    CHECK(sender.acks_received() > 0);
    CHECK(delivered.size() > 0);

    // ...and the replicator SAW the input acks and passed over them rather than counting them
    // malformed. A tag from our own block that belongs to another reader is not an error, and this
    // counter is where that distinction is visible.
    CHECK(client.foreign_messages() > 0);
    CHECK(client.malformed_messages() == 0);
}
