// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "rime/assets/cooked_reader.hpp"
#include "rime/destruction/bind.hpp"
#include "rime/destruction/components.hpp"
#include "rime/destruction/world.hpp"
#include "rime/destruction_net/components.hpp"
#include "rime/destruction_net/composition.hpp"
#include "rime/destruction_net/destruction_client.hpp"
#include "rime/destruction_net/destruction_server.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/physics/physics.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/server_replicator.hpp"

// m11.4a's proof: a wall broken on the server breaks the SAME WAY on a client, over a link that
// really loses and really reorders, with the client never converting a contact impulse of its own.
//
// The harness discipline is m11.1's through m11.3's: a ScriptedNetwork on a virtual clock (loss and
// latency are INPUTS, never environment luck), every wait is a bounded tick loop so a failure is a
// deadline rather than a hang, and every proof asserts its own non-vacuousness — a loss test that
// dropped nothing, or a multi-packet test whose ticks all fit one packet, proves nothing.
//
// WHAT IS HASHED, AND WHY NOT state_hash(). DestructionWorld::state_hash() is the M8 replay witness
// and folds physics BODY IDS. Those are allocation-order artifacts of one process: two peers build
// their physics worlds independently, so their body ids agree about nothing even when the
// destruction is identical. It is the right witness for "this process replayed its own tick" and
// the wrong one for "these two processes agree". The cross-peer witness has to fold only what both
// sides can be expected to share — per-part alive bits and health, and debris COMPOSITION — walked
// in NetId order, which is the only name the two worlds have in common.
using namespace rime;

namespace {

constexpr std::uint16_t kServerPort = 7777;
constexpr std::uint16_t kClientPort = 7778;
constexpr std::uint64_t kTickMs = 16;
constexpr float kDt = 1.0f / 60.0f;

std::vector<std::byte> read_fixture(const std::string& name) {
    const std::string path = std::string(RIME_DESTRUCTION_FIXTURE_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE_MESSAGE(file.good(), "cannot open fixture: " << path);
    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    return bytes;
}

assets::DestructibleAsset load_asset(const std::string& name) {
    const std::vector<std::byte> file = read_fixture(name);
    assets::AssetError err = assets::AssetError::Truncated;
    auto asset = assets::read_destructible(file, err);
    REQUIRE_MESSAGE(asset.has_value(), name << " failed to decode: " << assets::to_string(err));
    return std::move(*asset);
}

// The cross-peer witness now lives in the ENGINE (`destruction_net::shared_state_hash`), not here.
// It was test-local when m11.4a first needed it, which was the wrong home: m11.7 hash-verifies a
// scripted match in CI, a dedicated server compares it to spot a diverged client, and a sample
// prints it to show two windows agree — three callers each re-deriving it privately would be three
// subtly different answers to one question. This alias just keeps the call sites short.
std::uint64_t shared_destruction_hash(const ecs::World& world,
                                      const replication::NetIdMap& map,
                                      const destruction::DestructionWorld& destruction) {
    return destruction_net::shared_state_hash(world, map, destruction);
}

// One peer: an ECS world, a physics world, a destruction world, and the pattern registration both
// sides do independently from the same cooked asset.
struct Peer {
    ecs::World world;
    physics::PhysicsWorld physics;
    destruction::DestructionWorld destruction;
    destruction::PatternId pattern{};
    std::uint64_t asset_id = 0;

    void register_components() {
        ecs::register_transform_components(world);
        destruction::register_destruction_components(world);
        destruction_net::register_destruction_net_components(world);
    }

    void register_pattern(const assets::DestructibleAsset& asset, std::uint64_t id) {
        pattern = destruction.register_pattern(asset, physics);
        REQUIRE(pattern.is_valid());
        asset_id = id;
    }

    // The resolver the bind pass uses: this peer holds exactly one pattern.
    [[nodiscard]] destruction::PatternResolver resolver() {
        return [this](std::uint64_t id) {
            return id == asset_id ? pattern : destruction::PatternId{};
        };
    }
};

} // namespace

TEST_CASE("a remote instance ignores locally-authored damage") {
    // The suppression half of ADR-0033 A1, isolated from the network: an instance the local peer
    // does not own must not be eroded by anything the local peer decides.
    const assets::DestructibleAsset asset = load_asset("wall.rdest");
    Peer peer;
    peer.register_components();
    peer.register_pattern(asset, 0xABCDull);

    const destruction::InstanceId instance =
        peer.destruction.spawn(peer.pattern, core::Transform{}, peer.physics);
    REQUIRE(instance.is_valid());
    const float before = peer.destruction.part_health(instance, 0);
    REQUIRE(before > 0.0f);

    // While Local, damage lands — this is the control that proves the damage would have worked.
    peer.destruction.apply_damage(
        instance, core::Vec3{0.0f, 0.0f, 0.0f}, 100.0f, 0.1f, core::Vec3{});
    peer.destruction.update(peer.physics);
    const float after_local = peer.destruction.part_health(instance, 0);
    CHECK(after_local < before);

    // Flipped to Remote, the identical call does nothing.
    peer.destruction.set_authority(instance, destruction::Authority::Remote);
    CHECK(peer.destruction.authority_of(instance) == destruction::Authority::Remote);
    peer.destruction.apply_damage(
        instance, core::Vec3{0.0f, 0.0f, 0.0f}, 100.0f, 0.1f, core::Vec3{});
    peer.destruction.update(peer.physics);
    CHECK(peer.destruction.part_health(instance, 0) == doctest::Approx(after_local));

    // And its committed op list is empty, so a server publishing from a mirror would send nothing.
    CHECK(peer.destruction.committed_ops().empty());
}

TEST_CASE("committed_ops reports the expanded per-part list a radius call fanned out to") {
    // ADR-0033 A1's central claim, checked directly: what is replicated is the EXPANDED op list,
    // not the apply_damage call. A single radius call must appear as one op per overlapped part,
    // with the falloff already resolved into `amount` — that is what makes the arithmetic happen on
    // exactly one machine.
    const assets::DestructibleAsset asset = load_asset("wall.rdest");
    Peer peer;
    peer.register_components();
    peer.register_pattern(asset, 0xABCDull);

    const destruction::InstanceId instance =
        peer.destruction.spawn(peer.pattern, core::Transform{}, peer.physics);
    REQUIRE(instance.is_valid());

    peer.destruction.apply_damage(
        instance, core::Vec3{0.0f, 0.0f, 0.0f}, 100.0f, 0.2f, core::Vec3{});
    peer.destruction.update(peer.physics);

    const std::span<const destruction::DamageOp> ops = peer.destruction.committed_ops();
    REQUIRE(ops.size() > 1); // a 100m radius over a 16-part wall reaches many parts
    for (const destruction::DamageOp& op : ops) {
        CHECK(op.instance.index == instance.index);
        CHECK(op.amount > 0.0f);
        CHECK(op.central); // an explicit blast pushes through the COM, not at a contact point
    }
    // Distinct parts, and the falloff really varied across them rather than every op carrying the
    // caller's raw `amount`.
    bool amounts_differ = false;
    for (std::size_t i = 1; i < ops.size(); ++i) {
        CHECK(ops[i].part != ops[i - 1].part);
        amounts_differ = amounts_differ || ops[i].amount != ops[0].amount;
    }
    CHECK(amounts_differ);
}

TEST_CASE("a wall broken on the server breaks identically on a client across a lossy link") {
    // The headline proof. 20% loss with jitter wide enough to reorder: the damage-op stream rides
    // the RELIABLE channel, so it must survive that intact, while the entity structure it is
    // addressed by rides m11.3's mix of reliable and unreliable.
    net::ScriptedNetwork network{0xC0FFEEull,
                                 {/*loss_rate=*/0.20f,
                                  /*duplicate_rate=*/0.0f,
                                  /*min_latency_ms=*/5,
                                  /*max_latency_ms=*/40}};
    const net::Endpoint server_endpoint{0x7F000001u, kServerPort};
    const net::Endpoint client_endpoint{0x7F000001u, kClientPort};

    const assets::DestructibleAsset asset = load_asset("wall.rdest");
    constexpr std::uint64_t kAssetId = 0xABCDull;

    Peer server_peer;
    Peer client_peer;
    server_peer.register_components();
    client_peer.register_components();
    server_peer.register_pattern(asset, kAssetId);
    client_peer.register_pattern(asset, kAssetId);

    net::Link& server_link = network.add_node(server_endpoint);
    net::Link& client_link = network.add_node(client_endpoint);

    net::NetDriver::Config server_config;
    server_config.app_id = 0x52494D45u; // 'RIME'
    server_config.schema_hash = ecs::component_schema_hash(server_peer.world);
    server_config.salt_seed = 0x1111ull;
    net::NetDriver::Config client_config = server_config;
    client_config.schema_hash = ecs::component_schema_hash(client_peer.world);
    client_config.salt_seed = 0x2222ull;
    REQUIRE(server_config.schema_hash == client_config.schema_hash);

    net::NetDriver server_driver{server_link, server_config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator state_server{server_peer.world};
    replication::ClientReplicator state_client{client_peer.world};
    destruction_net::DestructionServer destruction_server;
    destruction_net::DestructionClient destruction_client;

    // Three walls, so the proof covers naming the RIGHT one rather than "the only one".
    constexpr int kWalls = 3;
    for (int i = 0; i < kWalls; ++i) {
        core::Transform placement;
        placement.translation.x = static_cast<float>(i) * 50.0f;
        const ecs::Entity e = server_peer.world.spawn_with(ecs::LocalTransform{placement},
                                                           destruction::Destructible{kAssetId});
        (void)state_server.replicate(e);
    }

    REQUIRE(client_driver.connect(server_endpoint, kTickMs).has_value());

    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;

    // One tick of the whole system, in the order Application's sim stage prescribes (ADR-0033 A5):
    // PreSim = poll, apply remote ops, bind; sim; Publish = send.
    const auto tick = [&](bool damage) {
        now_ms += kTickMs;
        ++tick_index;
        network.advance_time(now_ms);

        // ── PreSim: server ──
        events.clear();
        server_driver.update(now_ms, events);
        state_server.on_session_events(events);
        (void)state_server.apply_inbound(server_driver);

        // ── PreSim: client. ONE drain, fanned out to both subsystems — the shared tag registry in
        // replication/snapshot.hpp is what lets them coexist, and drain_received moving messages
        // out is what makes a single drain mandatory rather than merely tidy.
        events.clear();
        client_driver.update(now_ms, events);
        for (const net::SessionId id : client_driver.session_ids()) {
            net::Session* session = client_driver.session(id);
            if (session == nullptr) {
                continue;
            }
            inbox.clear();
            (void)session->drain_received(inbox);
            state_client.apply_messages(inbox);
            destruction_client.apply_messages(inbox, state_client.map(), client_peer.world);
        }

        // Bind on both sides: an entity that says it is a destructible becomes one. The server owns
        // its instances; the client's are mirrors, which is what suppresses its contact conversion.
        (void)destruction::bind_destructibles(server_peer.world,
                                              server_peer.destruction,
                                              server_peer.physics,
                                              server_peer.resolver(),
                                              destruction::Authority::Local);
        (void)destruction::bind_destructibles(client_peer.world,
                                              client_peer.destruction,
                                              client_peer.physics,
                                              client_peer.resolver(),
                                              destruction::Authority::Remote);

        // ── the simulation ──
        server_peer.world.advance_version();
        if (damage) {
            // Scripted, server-side, deterministic — the m11.7 shooter in miniature (A6).
            destruction::PatternId dummy{};
            (void)dummy;
            std::vector<ecs::Entity> bound;
            destruction::build_instance_entity_table(server_peer.world, bound);
            for (std::uint32_t i = 0; i < bound.size(); ++i) {
                if (!bound[i].is_valid()) {
                    continue;
                }
                server_peer.destruction.apply_damage(
                    destruction::InstanceId{i, 0},
                    core::Vec3{static_cast<float>(i) * 50.0f, 0.4f, 0.0f},
                    0.30f,
                    0.34f,
                    core::Vec3{0.0f, 0.0f, 4.0f});
            }
        }
        server_peer.physics.step(kDt);
        server_peer.destruction.update(server_peer.physics);

        // ONE queued batch per destruction update — never two merged into one, which would skip the
        // fracture boundary between them (see destruction_client.hpp). A backlog is drained by
        // running extra whole update cycles, so the authority's ticks are replayed one at a time.
        do {
            (void)destruction_client.apply_next_batch(
                client_peer.world, state_client.map(), client_peer.destruction);
            client_peer.physics.step(kDt);
            client_peer.destruction.update(client_peer.physics);
        } while (destruction_client.pending_batches() > 0);

        // ── Publish ──
        destruction_server.publish(server_driver,
                                   state_server.map(),
                                   server_peer.world,
                                   server_peer.destruction,
                                   tick_index,
                                   now_ms);
        state_server.publish(server_driver, now_ms);
        state_client.send_ack(client_driver, now_ms);
    };

    // Settle the handshake and let the three walls be spawned, replicated and bound on both ends.
    for (int i = 0; i < 40; ++i) {
        tick(false);
    }
    REQUIRE(server_peer.destruction.instance_count() == kWalls);
    REQUIRE(client_peer.destruction.instance_count() == kWalls);

    const std::uint64_t hash_before_damage =
        shared_destruction_hash(client_peer.world, state_client.map(), client_peer.destruction);

    // Break them PARTIALLY: enough to detach islands and make debris, not enough to level the wall.
    // A partly-standing wall is the discriminating case — it leaves surviving parts whose HEALTH
    // must match bit for bit, which a fully-collapsed wall (every part at zero) would not test.
    for (int i = 0; i < 6; ++i) {
        tick(true);
    }
    for (int i = 0; i < 60; ++i) {
        tick(false);
    }

    // ── Non-vacuousness: the mechanism actually fired, and the link actually misbehaved. ──
    CHECK(destruction_server.ops_sent() > 0);
    CHECK(destruction_client.ops_applied() > 0);
    CHECK(destruction_server.ops_unaddressable() == 0); // every damaged wall was nameable
    CHECK(destruction_client.malformed_messages() == 0);
    CHECK(destruction_client.pending_parts() == 0);   // no tick left half-accumulated
    CHECK(destruction_client.pending_batches() == 0); // every batch got its own fracture boundary
    CHECK(destruction_client.ops_applied() == destruction_server.ops_sent());
    CHECK(network.packets_dropped() > 0); // the link really lost packets
    // The two streams really did share one session, and replication really did step over the
    // destruction traffic rather than eating it.
    CHECK(state_client.foreign_messages() > 0);

    // Something actually broke — a proof that both sides are still intact proves nothing.
    bool any_part_down = false;
    for (std::uint32_t i = 0; i < kWalls; ++i) {
        const destruction::InstanceId inst{i, 0};
        for (std::uint32_t p = 0; p < server_peer.destruction.instance_part_count(inst); ++p) {
            any_part_down = any_part_down || !server_peer.destruction.part_alive(inst, p);
        }
    }
    CHECK(any_part_down);

    // ── The proof. ──
    const std::uint64_t server_hash =
        shared_destruction_hash(server_peer.world, state_server.map(), server_peer.destruction);
    const std::uint64_t client_hash =
        shared_destruction_hash(client_peer.world, state_client.map(), client_peer.destruction);
    CHECK(server_hash == client_hash);

    // ── The negative control: the hash must be able to TELL STATES APART, or the equality above is
    // an accident of a function that returns the same number for everything. The intact world is
    // the perturbation — it needs no extra mutation and cannot be defeated by damage tuning.
    CHECK(client_hash != hash_before_damage);
}

TEST_CASE("a tick that splits across packets is applied whole or not at all") {
    // The atomicity rule from destruction_client.hpp. A tick's op list is ONE canonical sequence;
    // applying half of it, running a support solve, then applying the rest lands the second half on
    // a wall of a different shape than the authority applied it to. Here the batch is fed part by
    // part, and nothing may reach the world until the last part arrives.
    const assets::DestructibleAsset asset = load_asset("wall.rdest");
    constexpr std::uint64_t kAssetId = 0xABCDull;

    Peer server_peer;
    Peer client_peer;
    server_peer.register_components();
    client_peer.register_components();
    server_peer.register_pattern(asset, kAssetId);
    client_peer.register_pattern(asset, kAssetId);

    // A loopback pair with a perfect link: this case is about FRAMING, not delivery.
    net::ScriptedNetwork network{0x1234ull,
                                 {/*loss_rate=*/0.0f,
                                  /*duplicate_rate=*/0.0f,
                                  /*min_latency_ms=*/1,
                                  /*max_latency_ms=*/1}};
    const net::Endpoint server_endpoint{0x7F000001u, kServerPort};
    const net::Endpoint client_endpoint{0x7F000001u, kClientPort};
    net::Link& server_link = network.add_node(server_endpoint);
    net::Link& client_link = network.add_node(client_endpoint);

    net::NetDriver::Config config;
    config.app_id = 0x52494D45u;
    config.schema_hash = ecs::component_schema_hash(server_peer.world);
    config.salt_seed = 0x1111ull;
    net::NetDriver::Config client_config = config;
    client_config.salt_seed = 0x2222ull;

    net::NetDriver server_driver{server_link, config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator state_server{server_peer.world};
    replication::ClientReplicator state_client{client_peer.world};
    destruction_net::DestructionServer destruction_server;
    destruction_net::DestructionClient destruction_client;

    // Enough walls that one tick's op list cannot fit a single 1150-byte packet: an op is 41 bytes,
    // so 27 fit, and a 16-part wall under a wide blast contributes up to 16 ops.
    constexpr int kWalls = 6;
    for (int i = 0; i < kWalls; ++i) {
        core::Transform placement;
        placement.translation.x = static_cast<float>(i) * 50.0f;
        const ecs::Entity e = server_peer.world.spawn_with(ecs::LocalTransform{placement},
                                                           destruction::Destructible{kAssetId});
        (void)state_server.replicate(e);
    }

    REQUIRE(client_driver.connect(server_endpoint, kTickMs).has_value());

    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;

    const auto tick = [&](bool damage) {
        now_ms += kTickMs;
        ++tick_index;
        network.advance_time(now_ms);

        events.clear();
        server_driver.update(now_ms, events);
        state_server.on_session_events(events);
        (void)state_server.apply_inbound(server_driver);

        events.clear();
        client_driver.update(now_ms, events);
        for (const net::SessionId id : client_driver.session_ids()) {
            net::Session* session = client_driver.session(id);
            if (session == nullptr) {
                continue;
            }
            inbox.clear();
            (void)session->drain_received(inbox);
            state_client.apply_messages(inbox);
            destruction_client.apply_messages(inbox, state_client.map(), client_peer.world);
        }

        (void)destruction::bind_destructibles(server_peer.world,
                                              server_peer.destruction,
                                              server_peer.physics,
                                              server_peer.resolver(),
                                              destruction::Authority::Local);
        (void)destruction::bind_destructibles(client_peer.world,
                                              client_peer.destruction,
                                              client_peer.physics,
                                              client_peer.resolver(),
                                              destruction::Authority::Remote);

        server_peer.world.advance_version();
        if (damage) {
            std::vector<ecs::Entity> bound;
            destruction::build_instance_entity_table(server_peer.world, bound);
            for (std::uint32_t i = 0; i < bound.size(); ++i) {
                if (bound[i].is_valid()) {
                    // A very wide, very weak blast: it touches every part of every wall (many ops)
                    // without killing anything, so one tick's list is long by construction.
                    server_peer.destruction.apply_damage(
                        destruction::InstanceId{i, 0},
                        core::Vec3{static_cast<float>(i) * 50.0f, 0.0f, 0.0f},
                        100.0f,
                        0.001f,
                        core::Vec3{});
                }
            }
        }
        server_peer.physics.step(kDt);
        server_peer.destruction.update(server_peer.physics);
        // ONE queued batch per destruction update — never two merged into one, which would skip the
        // fracture boundary between them (see destruction_client.hpp). A backlog is drained by
        // running extra whole update cycles, so the authority's ticks are replayed one at a time.
        do {
            (void)destruction_client.apply_next_batch(
                client_peer.world, state_client.map(), client_peer.destruction);
            client_peer.physics.step(kDt);
            client_peer.destruction.update(client_peer.physics);
        } while (destruction_client.pending_batches() > 0);

        destruction_server.publish(server_driver,
                                   state_server.map(),
                                   server_peer.world,
                                   server_peer.destruction,
                                   tick_index,
                                   now_ms);
        state_server.publish(server_driver, now_ms);
        state_client.send_ack(client_driver, now_ms);
    };

    for (int i = 0; i < 40; ++i) {
        tick(false);
    }
    REQUIRE(client_peer.destruction.instance_count() == kWalls);

    for (int i = 0; i < 10; ++i) {
        tick(true);
    }
    for (int i = 0; i < 20; ++i) {
        tick(false);
    }

    // The split really happened on both ends — otherwise this test proves nothing about multi-part
    // ticks, only that single-part ticks still work.
    CHECK(destruction_server.multipart_ticks() > 0);
    CHECK(destruction_client.multipart_ticks() > 0);
    CHECK(destruction_client.pending_parts() == 0);
    CHECK(destruction_client.pending_batches() == 0);
    CHECK(destruction_client.malformed_messages() == 0);

    // Every op the server addressed reached the client's world: nothing was dropped between the
    // parts, and no partial tick was ever handed over.
    CHECK(destruction_client.ops_applied() == destruction_server.ops_sent());

    const std::uint64_t server_hash =
        shared_destruction_hash(server_peer.world, state_server.map(), server_peer.destruction);
    const std::uint64_t client_hash =
        shared_destruction_hash(client_peer.world, state_client.map(), client_peer.destruction);
    CHECK(server_hash == client_hash);
}

TEST_CASE("the state-application seam corrects a mirror that missed the op stream") {
    // ADR-0033 A3: a client that joined late, or that drifted, must be able to be handed STATE
    // rather than only the events that produced it. The correction has to replay the ordinary body
    // swap — so it must reach the same alive bits AND the same debris roster as a peer that watched
    // the whole thing happen.
    const assets::DestructibleAsset asset = load_asset("wall.rdest");

    Peer authority;
    Peer latecomer;
    authority.register_components();
    latecomer.register_components();
    authority.register_pattern(asset, 0xABCDull);
    latecomer.register_pattern(asset, 0xABCDull);

    const destruction::InstanceId a_inst =
        authority.destruction.spawn(authority.pattern, core::Transform{}, authority.physics);
    const destruction::InstanceId l_inst =
        latecomer.destruction.spawn(latecomer.pattern, core::Transform{}, latecomer.physics);
    REQUIRE(a_inst.is_valid());
    REQUIRE(l_inst.is_valid());
    latecomer.destruction.set_authority(l_inst, destruction::Authority::Remote);

    // The authority breaks its wall properly, over several ticks.
    for (int i = 0; i < 8; ++i) {
        authority.destruction.apply_damage(
            a_inst, core::Vec3{0.0f, 0.4f, 0.0f}, 1.5f, 0.35f, core::Vec3{0.0f, 0.0f, 4.0f});
        authority.physics.step(kDt);
        authority.destruction.update(authority.physics);
    }

    // Collect what the authority ended up with — the dead set and the healths it froze at.
    std::vector<std::uint32_t> dead;
    std::vector<float> healths;
    const std::uint32_t parts = authority.destruction.instance_part_count(a_inst);
    for (std::uint32_t p = 0; p < parts; ++p) {
        if (!authority.destruction.part_alive(a_inst, p)) {
            dead.push_back(p);
            healths.push_back(authority.destruction.part_health(a_inst, p));
        }
    }
    REQUIRE(!dead.empty()); // the scripted damage really did take parts out

    // The latecomer never saw an op. Hand it the state.
    CHECK(latecomer.destruction.debris_count() == 0);
    latecomer.destruction.apply_detach_set(l_inst, dead, healths, latecomer.physics);

    // Alive bits and healths now match, part for part.
    for (std::uint32_t p = 0; p < parts; ++p) {
        CHECK(latecomer.destruction.part_alive(l_inst, p) ==
              authority.destruction.part_alive(a_inst, p));
        CHECK(latecomer.destruction.part_health(l_inst, p) ==
              doctest::Approx(authority.destruction.part_health(a_inst, p)));
    }

    // And the body swap really ran: the correction produced debris, not just flipped bits. This is
    // the difference between agreeing about what you can see and agreeing about the tables
    // underneath it.
    CHECK(latecomer.destruction.debris_count() > 0);

    // Idempotent: applying the same detach set again changes nothing.
    const std::size_t debris_after_first = latecomer.destruction.debris_count();
    latecomer.destruction.apply_detach_set(l_inst, dead, healths, latecomer.physics);
    CHECK(latecomer.destruction.debris_count() == debris_after_first);

    // Corrections are silent — a late-join must not fire the dust and audio of a wall that fell
    // before it connected.
    CHECK(latecomer.destruction.events().empty());
}

TEST_CASE("the debris half of the state-application seam moves a body and wakes it") {
    // The other half of ADR-0033 A3, and the physics mutator it needed (`set_body_state`, which A3
    // correctly predicted did not exist). m11.4b puts replicated transforms through here; this
    // proves the mechanism now, because a teleport is more than four field writes — a body moved
    // while asleep would otherwise keep a broadphase proxy describing where it used to be, and that
    // failure surfaces much later and far from its cause.
    const assets::DestructibleAsset asset = load_asset("wall.rdest");
    Peer peer;
    peer.register_components();
    peer.register_pattern(asset, 0xABCDull);

    const destruction::InstanceId instance =
        peer.destruction.spawn(peer.pattern, core::Transform{}, peer.physics);
    REQUIRE(instance.is_valid());

    // Break it hard enough to make debris, then let the pieces come to rest.
    for (int i = 0; i < 6; ++i) {
        peer.destruction.apply_damage(
            instance, core::Vec3{0.0f, 0.4f, 0.0f}, 0.30f, 0.34f, core::Vec3{0.0f, 0.0f, 4.0f});
        peer.physics.step(kDt);
        peer.destruction.update(peer.physics);
    }
    REQUIRE(peer.destruction.debris_count() > 0);
    for (int i = 0; i < 400; ++i) {
        peer.physics.step(kDt);
        peer.destruction.update(peer.physics);
    }

    const physics::BodyId body = peer.destruction.debris_body(0);
    REQUIRE(peer.physics.is_alive(body));

    physics::BodyState before{};
    REQUIRE(peer.physics.get_body_state(body, before));

    // Teleport it somewhere it has certainly never been.
    physics::BodyState target{};
    target.position = core::Vec3{123.0f, 45.0f, -67.0f};
    target.orientation = core::quat_identity();
    target.linear_velocity = core::Vec3{1.0f, 2.0f, 3.0f};
    target.angular_velocity = core::Vec3{0.0f, 0.5f, 0.0f};
    peer.destruction.set_debris_state(0, target, peer.physics);

    physics::BodyState after{};
    REQUIRE(peer.physics.get_body_state(body, after));
    CHECK(after.position.x == doctest::Approx(target.position.x));
    CHECK(after.position.y == doctest::Approx(target.position.y));
    CHECK(after.position.z == doctest::Approx(target.position.z));
    CHECK(after.linear_velocity.z == doctest::Approx(target.linear_velocity.z));
    CHECK(after.position.x != doctest::Approx(before.position.x)); // it really moved

    // Woken, not left asserting it is at rest somewhere it has never been: one step of real gravity
    // must now change its state, which a body still asleep would not do.
    peer.physics.step(kDt);
    physics::BodyState stepped{};
    REQUIRE(peer.physics.get_body_state(body, stepped));
    CHECK(stepped.position.y != doctest::Approx(after.position.y));

    // Out-of-range and frozen debris are safe no-ops, not crashes.
    peer.destruction.set_debris_state(peer.destruction.debris_count() + 99, target, peer.physics);
}

// ── m11.4b: debris ──────────────────────────────────────────────────────────────────────────────

TEST_CASE("the composition hash discriminates the divergence it exists to catch") {
    // A unit check on the fingerprint itself, before leaning on it across a link. The failure it
    // guards is ADR-0033 A12's: the same ten parts leaving as one island rather than as two. If the
    // hash cannot tell those apart it is decoration, and every proof built on it is vacuous.
    const assets::DestructibleAsset asset = load_asset("wall.rdest");

    Peer two_waves;
    Peer one_wave;
    two_waves.register_components();
    one_wave.register_components();
    two_waves.register_pattern(asset, 0xABCDull);
    one_wave.register_pattern(asset, 0xABCDull);

    const destruction::InstanceId a =
        two_waves.destruction.spawn(two_waves.pattern, core::Transform{}, two_waves.physics);
    const destruction::InstanceId b =
        one_wave.destruction.spawn(one_wave.pattern, core::Transform{}, one_wave.physics);
    REQUIRE(a.is_valid());
    REQUIRE(b.is_valid());

    // Identical damage, delivered as two separate ticks on one peer and merged into a single tick
    // on the other — precisely the merge A12 forbids.
    const auto blast = [](destruction::DestructionWorld& d, destruction::InstanceId i, float y) {
        d.apply_damage(i, core::Vec3{0.0f, y, 0.0f}, 0.45f, 3.0f, core::Vec3{0.0f, 0.0f, 4.0f});
    };

    blast(two_waves.destruction, a, 0.4f);
    two_waves.physics.step(kDt);
    two_waves.destruction.update(two_waves.physics);
    blast(two_waves.destruction, a, -0.4f);
    two_waves.physics.step(kDt);
    two_waves.destruction.update(two_waves.physics);

    blast(one_wave.destruction, b, 0.4f);
    blast(one_wave.destruction, b, -0.4f);
    one_wave.physics.step(kDt);
    one_wave.destruction.update(one_wave.physics);

    const std::uint64_t h_two = destruction_net::debris_composition_hash(two_waves.destruction, a);
    const std::uint64_t h_one = destruction_net::debris_composition_hash(one_wave.destruction, b);

    // Non-vacuousness: both really did produce rubble, and it really was grouped differently.
    REQUIRE(two_waves.destruction.debris_count() > 0);
    REQUIRE(one_wave.destruction.debris_count() > 0);
    REQUIRE(two_waves.destruction.debris_count() != one_wave.destruction.debris_count());
    CHECK(h_two != h_one);

    // And it is stable: the same state hashes the same way twice.
    CHECK(h_two == destruction_net::debris_composition_hash(two_waves.destruction, a));
}

TEST_CASE("debris bind to the chunks the client derived, and corrections pull drift back") {
    // The m11.4b proof. Two halves, because they test opposite things:
    //   1. With both peers simulating faithfully, the mirrors must BIND — every chunk's
    //   DebrisOrigin
    //      ordinal must resolve to the same chunk on the client — and the composition fingerprints
    //      must match. This is the addressing proof.
    //   2. With the client's physics deliberately wrong, corrections must FIRE and pull the rubble
    //      back to the authority. This is the correction proof, and it needs a genuinely divergent
    //      client — two identical simulations staying identical would exercise nothing.
    net::ScriptedNetwork network{0xBEEFull,
                                 {/*loss_rate=*/0.10f,
                                  /*duplicate_rate=*/0.0f,
                                  /*min_latency_ms=*/5,
                                  /*max_latency_ms=*/25}};
    const net::Endpoint server_endpoint{0x7F000001u, kServerPort};
    const net::Endpoint client_endpoint{0x7F000001u, kClientPort};

    const assets::DestructibleAsset asset = load_asset("wall.rdest");
    constexpr std::uint64_t kAssetId = 0xABCDull;

    Peer server_peer;
    Peer client_peer;
    server_peer.register_components();
    client_peer.register_components();
    server_peer.register_pattern(asset, kAssetId);
    client_peer.register_pattern(asset, kAssetId);

    net::Link& server_link = network.add_node(server_endpoint);
    net::Link& client_link = network.add_node(client_endpoint);

    net::NetDriver::Config server_config;
    server_config.app_id = 0x52494D45u;
    server_config.schema_hash = ecs::component_schema_hash(server_peer.world);
    server_config.salt_seed = 0x1111ull;
    net::NetDriver::Config client_config = server_config;
    client_config.schema_hash = ecs::component_schema_hash(client_peer.world);
    client_config.salt_seed = 0x2222ull;
    REQUIRE(server_config.schema_hash == client_config.schema_hash);

    net::NetDriver server_driver{server_link, server_config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator state_server{server_peer.world};
    replication::ClientReplicator state_client{client_peer.world};
    destruction_net::DestructionServer destruction_server;
    destruction_net::DestructionClient destruction_client;

    const ecs::Entity wall =
        server_peer.world.spawn_with(ecs::LocalTransform{}, destruction::Destructible{kAssetId});
    (void)state_server.replicate(wall);

    REQUIRE(client_driver.connect(server_endpoint, kTickMs).has_value());

    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;

    const auto tick = [&](bool damage) {
        now_ms += kTickMs;
        ++tick_index;
        network.advance_time(now_ms);

        events.clear();
        server_driver.update(now_ms, events);
        state_server.on_session_events(events);
        (void)state_server.apply_inbound(server_driver);

        events.clear();
        client_driver.update(now_ms, events);
        for (const net::SessionId id : client_driver.session_ids()) {
            net::Session* session = client_driver.session(id);
            if (session == nullptr) {
                continue;
            }
            inbox.clear();
            (void)session->drain_received(inbox);
            state_client.apply_messages(inbox);
            destruction_client.apply_messages(inbox, state_client.map(), client_peer.world);
        }

        (void)destruction::bind_destructibles(server_peer.world,
                                              server_peer.destruction,
                                              server_peer.physics,
                                              server_peer.resolver(),
                                              destruction::Authority::Local);
        (void)destruction::bind_destructibles(client_peer.world,
                                              client_peer.destruction,
                                              client_peer.physics,
                                              client_peer.resolver(),
                                              destruction::Authority::Remote);

        server_peer.world.advance_version();
        if (damage) {
            std::vector<ecs::Entity> bound;
            destruction::build_instance_entity_table(server_peer.world, bound);
            for (std::uint32_t i = 0; i < bound.size(); ++i) {
                if (bound[i].is_valid()) {
                    server_peer.destruction.apply_damage(destruction::InstanceId{i, 0},
                                                         core::Vec3{0.0f, 0.4f, 0.0f},
                                                         0.45f,
                                                         3.0f,
                                                         core::Vec3{0.0f, 0.0f, 6.0f});
                }
            }
        }
        server_peer.physics.step(kDt);
        server_peer.destruction.update(server_peer.physics);

        do {
            (void)destruction_client.apply_next_batch(
                client_peer.world, state_client.map(), client_peer.destruction);
            client_peer.physics.step(kDt);
            client_peer.destruction.update(client_peer.physics);
        } while (destruction_client.pending_batches() > 0);

        // PostSim: keep the debris↔entity bridge in step, then bind/correct on the client.
        destruction_server.sync_debris(
            server_peer.world,
            server_peer.destruction,
            server_peer.physics,
            state_server.map(),
            [&](ecs::Entity e) { (void)state_server.replicate(e); },
            [&](ecs::Entity e) { state_server.despawn(e); });
        destruction_client.sync_debris(
            client_peer.world, state_client.map(), client_peer.destruction, client_peer.physics);

        destruction_server.publish(server_driver,
                                   state_server.map(),
                                   server_peer.world,
                                   server_peer.destruction,
                                   tick_index,
                                   now_ms);
        state_server.publish(server_driver, now_ms);
        state_client.send_ack(client_driver, now_ms);
    };

    for (int i = 0; i < 40; ++i) {
        tick(false);
    }
    REQUIRE(server_peer.destruction.instance_count() == 1);
    REQUIRE(client_peer.destruction.instance_count() == 1);

    for (int i = 0; i < 3; ++i) {
        tick(true);
    }
    for (int i = 0; i < 60; ++i) {
        tick(false);
    }

    // ── Half 1: the addressing proof. ──
    REQUIRE(server_peer.destruction.debris_count() > 0);
    CHECK(server_peer.destruction.debris_count() == client_peer.destruction.debris_count());
    CHECK(destruction_server.debris_entities_spawned() > 0);
    CHECK(destruction_server.debris_unaddressable() == 0);
    CHECK(destruction_server.composition_checks_sent() > 0);
    CHECK(destruction_client.debris_bound() > 0);

    // Every chunk the authority described was the chunk the client had. This is the assertion the
    // whole ordinal-addressing scheme rests on.
    // Every fingerprint the authority sent was actually compared — matches + mismatches + orphans
    // must account for all of them. Without this the proof could pass while verifying a handful of
    // ticks and silently skipping the rest, which is exactly what an orphaned check causes.
    CHECK(destruction_client.composition_matches() > 0);
    CHECK(destruction_client.composition_mismatches() == 0);
    CHECK(destruction_client.composition_matches() + destruction_client.composition_mismatches() +
              destruction_client.composition_checks_unverified() ==
          destruction_server.composition_checks_sent());

    // ── Half 2: the correction proof. Make the client's simulation genuinely wrong, so its rubble
    // parts company with the authority's and the correction path has something to do. ──
    const std::uint64_t corrections_before = destruction_client.debris_corrections();
    client_peer.physics.set_gravity(core::Vec3{4.0f, -3.0f, 0.0f}); // sideways, and weaker

    // The largest gap between the authority's rubble and ours, chunk by chunk in roster order —
    // an order half 1 just proved both peers agree on.
    const auto worst_drift = [&]() {
        float worst = 0.0f;
        std::size_t compared = 0;
        for (std::size_t d = 0; d < server_peer.destruction.debris_count(); ++d) {
            physics::BodyState s{};
            physics::BodyState c{};
            if (!server_peer.physics.get_body_state(server_peer.destruction.debris_body(d), s) ||
                !client_peer.physics.get_body_state(client_peer.destruction.debris_body(d), c)) {
                continue;
            }
            worst = std::max(worst, core::length(s.position - c.position));
            ++compared;
        }
        CHECK(compared > 0); // a measurement over nothing measures nothing
        return worst;
    };

    for (int i = 0; i < 90; ++i) {
        tick(false);
    }
    const float drift_early = worst_drift();

    for (int i = 0; i < 180; ++i) {
        tick(false);
    }
    const float drift_late = worst_drift();

    CHECK(destruction_client.debris_corrections() > corrections_before); // the path really fired
    CHECK(destruction_client.composition_mismatches() == 0); // wrong physics is not wrong shape

    // THE ASSERTION THAT MATTERS, and why it is not "drift < some constant". A client simulating at
    // the wrong gravity diverges without bound: at 4 m/s² sideways, three seconds of unchecked
    // drift is already ~18 m and growing quadratically. What the correction buys is not a small
    // number, it is a BOUNDED one — the rubble is pinned to the authority's version however long
    // the wrong simulation runs. So the test triples the elapsed time and asserts the gap did not
    // grow with it, which is a property no magic constant can express and no runaway can
    // accidentally satisfy.
    CHECK(drift_late < drift_early * 1.5f + 0.5f);
    CHECK(drift_late < 3.0f); // and bounded in absolute terms, not merely non-increasing
}

TEST_CASE("destruction events survive a client whose relevancy and budget withhold everything") {
    // THE ROADMAP'S m11.5 RULE, PROVEN: "destruction events are never culled, debris transforms are
    // distance-budgeted per client." You can budget a CORRECTION; you can never budget an EVENT.
    //
    // The two travel by different roads on purpose. Damage ops go straight to `send_reliable` on
    // every connected session, untouched by ServerReplicator's relevancy filter or byte budget.
    // Transforms ride m11.3's snapshot delta, which both of those throttle. So a wall on the far
    // side of the map still BREAKS correctly for a client that is being sent none of its rubble's
    // motion — the client derives the break itself from the op stream, and only the trajectory
    // afterwards is a thing the server pays to correct.
    //
    // This is the structural argument turned into a test: the relevancy policy here scores
    // EVERYTHING irrelevant and the budget is one byte, which is as hostile as the snapshot path
    // can be made, and the cross-peer destruction witness must still match bit for bit.
    net::ScriptedNetwork network{0xC0FFEEull,
                                 {/*loss_rate=*/0.10f,
                                  /*duplicate_rate=*/0.0f,
                                  /*min_latency_ms=*/5,
                                  /*max_latency_ms=*/40}};
    const net::Endpoint server_endpoint{0x7F000001u, kServerPort};
    const net::Endpoint client_endpoint{0x7F000001u, kClientPort};

    const assets::DestructibleAsset asset = load_asset("wall.rdest");
    constexpr std::uint64_t kAssetId = 0xABCDull;

    Peer server_peer;
    Peer client_peer;
    server_peer.register_components();
    client_peer.register_components();
    server_peer.register_pattern(asset, kAssetId);
    client_peer.register_pattern(asset, kAssetId);

    net::Link& server_link = network.add_node(server_endpoint);
    net::Link& client_link = network.add_node(client_endpoint);

    net::NetDriver::Config server_config;
    server_config.app_id = 0x52494D45u;
    server_config.schema_hash = ecs::component_schema_hash(server_peer.world);
    server_config.salt_seed = 0x1111ull;
    net::NetDriver::Config client_config = server_config;
    client_config.schema_hash = ecs::component_schema_hash(client_peer.world);
    client_config.salt_seed = 0x2222ull;

    net::NetDriver server_driver{server_link, server_config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator state_server{server_peer.world};
    replication::ClientReplicator state_client{client_peer.world};
    destruction_net::DestructionServer destruction_server;
    destruction_net::DestructionClient destruction_client;

    constexpr int kWalls = 3;
    for (int i = 0; i < kWalls; ++i) {
        core::Transform placement;
        placement.translation.x = static_cast<float>(i) * 50.0f;
        const ecs::Entity e = server_peer.world.spawn_with(ecs::LocalTransform{placement},
                                                           destruction::Destructible{kAssetId});
        (void)state_server.replicate(e);
    }

    REQUIRE(client_driver.connect(server_endpoint, kTickMs).has_value());

    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0;
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;

    const auto tick = [&](bool damage) {
        now_ms += kTickMs;
        ++tick_index;
        network.advance_time(now_ms);

        events.clear();
        server_driver.update(now_ms, events);
        state_server.on_session_events(events);
        (void)state_server.apply_inbound(server_driver);

        events.clear();
        client_driver.update(now_ms, events);
        for (const net::SessionId id : client_driver.session_ids()) {
            net::Session* session = client_driver.session(id);
            if (session == nullptr) {
                continue;
            }
            inbox.clear();
            (void)session->drain_received(inbox);
            state_client.apply_messages(inbox);
            destruction_client.apply_messages(inbox, state_client.map(), client_peer.world);
        }

        (void)destruction::bind_destructibles(server_peer.world,
                                              server_peer.destruction,
                                              server_peer.physics,
                                              server_peer.resolver(),
                                              destruction::Authority::Local);
        (void)destruction::bind_destructibles(client_peer.world,
                                              client_peer.destruction,
                                              client_peer.physics,
                                              client_peer.resolver(),
                                              destruction::Authority::Remote);

        server_peer.world.advance_version();
        if (damage) {
            std::vector<ecs::Entity> bound;
            destruction::build_instance_entity_table(server_peer.world, bound);
            for (std::uint32_t i = 0; i < bound.size(); ++i) {
                if (!bound[i].is_valid()) {
                    continue;
                }
                server_peer.destruction.apply_damage(
                    destruction::InstanceId{i, 0},
                    core::Vec3{static_cast<float>(i) * 50.0f, 0.4f, 0.0f},
                    0.30f,
                    0.34f,
                    core::Vec3{0.0f, 0.0f, 4.0f});
            }
        }
        server_peer.physics.step(kDt);
        server_peer.destruction.update(server_peer.physics);

        do {
            (void)destruction_client.apply_next_batch(
                client_peer.world, state_client.map(), client_peer.destruction);
            client_peer.physics.step(kDt);
            client_peer.destruction.update(client_peer.physics);
        } while (destruction_client.pending_batches() > 0);

        // PostSim: the debris↔entity bridge. Without it no debris entity ever exists, and a proof
        // that transforms were withheld would be withholding nothing.
        destruction_server.sync_debris(
            server_peer.world,
            server_peer.destruction,
            server_peer.physics,
            state_server.map(),
            [&](ecs::Entity e) { (void)state_server.replicate(e); },
            [&](ecs::Entity e) { state_server.despawn(e); });
        destruction_client.sync_debris(
            client_peer.world, state_client.map(), client_peer.destruction, client_peer.physics);

        destruction_server.publish(server_driver,
                                   state_server.map(),
                                   server_peer.world,
                                   server_peer.destruction,
                                   tick_index,
                                   now_ms);
        state_server.publish(server_driver, now_ms);
        state_client.send_ack(client_driver, now_ms);
    };

    // Settle with the snapshot path OPEN. The walls' `Destructible{asset}` component is ordinary
    // replicated state, so it has to arrive before the client can bind them — an op addressed to an
    // entity the client never learned about is unaddressable, which would prove the wrong thing.
    // Relevancy throttles the correction stream, not the client's ability to exist.
    for (int i = 0; i < 40; ++i) {
        tick(false);
    }
    REQUIRE(server_peer.destruction.instance_count() == kWalls);
    REQUIRE(client_peer.destruction.instance_count() == kWalls);

    const std::uint64_t hash_before_damage =
        shared_destruction_hash(client_peer.world, state_client.map(), client_peer.destruction);

    // NOW slam the snapshot path shut: nothing is relevant to anyone, and the budget is one byte.
    state_server.set_relevancy(
        [](net::SessionId, std::span<const ecs::Entity> candidates, std::span<float> priorities) {
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                (void)candidates;
                priorities[i] = 0.0f;
            }
        });
    replication::Budget starved;
    starved.max_bytes_per_tick = 1;
    state_server.set_budget(starved);

    const std::uint64_t culled_before = state_server.entities_culled_irrelevant();

    for (int i = 0; i < 6; ++i) {
        tick(true);
    }
    for (int i = 0; i < 60; ++i) {
        tick(false);
    }

    // ── Non-vacuousness: the throttle really was throttling, and the event path really fired. ──
    CHECK(state_server.entities_culled_irrelevant() > culled_before);
    CHECK(destruction_server.ops_sent() > 0);
    CHECK(destruction_client.ops_applied() == destruction_server.ops_sent());
    CHECK(destruction_server.ops_unaddressable() == 0);
    CHECK(destruction_client.malformed_messages() == 0);
    CHECK(destruction_client.pending_batches() == 0);
    CHECK(network.packets_dropped() > 0);

    // Debris really were created, so "transforms were withheld" is a claim about something that
    // existed to be withheld.
    CHECK(destruction_server.debris_entities_spawned() > 0);

    bool any_part_down = false;
    for (std::uint32_t i = 0; i < kWalls; ++i) {
        const destruction::InstanceId inst{i, 0};
        for (std::uint32_t p = 0; p < server_peer.destruction.instance_part_count(inst); ++p) {
            any_part_down = any_part_down || !server_peer.destruction.part_alive(inst, p);
        }
    }
    CHECK(any_part_down);

    // ── The proof: alive bits, health and debris COMPOSITION agree bit for bit, with the entire
    // snapshot path closed. Every one of those facts was derived by the client from the op stream
    // rather than sent to it as state — which is the whole reason destruction can be budgeted at
    // all. (Transforms are deliberately outside this witness; see composition.hpp.)
    const std::uint64_t server_hash =
        shared_destruction_hash(server_peer.world, state_server.map(), server_peer.destruction);
    const std::uint64_t client_hash =
        shared_destruction_hash(client_peer.world, state_client.map(), client_peer.destruction);
    CHECK(server_hash == client_hash);
    CHECK(client_hash != hash_before_damage);
}
