// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/core/reflect/serialize.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/server_replicator.hpp"

// m11.3's proof: a moving ECS world replicated to a second peer converges to a bit-identical state,
// over a link that really loses and really reorders packets.
//
// The harness discipline is m11.1's and m11.2's: a ScriptedNetwork on a virtual clock (loss and
// latency are INPUTS, never environment luck), every wait is a bounded tick loop so a failure is a
// deadline rather than a hang, and every proof asserts its own non-vacuousness — a loss test that
// dropped nothing proves nothing.
using namespace rime;

namespace {

constexpr std::uint16_t kServerPort = 7777;
constexpr std::uint16_t kClientPort = 7778;
constexpr std::uint64_t kTickMs = 16;

// Register exactly the components the proof replicates. LocalTransform is the payload; Parent comes
// along because register_transform_components registers both, and its presence is itself worth
// asserting on — it contains an ecs::Entity, so it must be EXCLUDED from the wire schema.
void register_components(ecs::World& world) {
    ecs::register_transform_components(world);
}

// Fold every replicated entity's LocalTransform into one number, walked in NetId order.
//
// NetId order, not local Entity order, is the whole trick: the two worlds allocate entity slots
// independently, so their Entity ids agree about nothing. NetId is the only name both sides share,
// and NetIdMap::for_each is specified to walk it ascending on both.
std::uint64_t replicated_state_hash(const ecs::World& world, const replication::NetIdMap& map) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto fold = [&hash](std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8) {
            hash ^= (value >> shift) & 0xFFull;
            hash *= 0x100000001b3ull;
        }
    };
    map.for_each([&](replication::NetId net_id, ecs::Entity entity) {
        fold(net_id.index);
        const auto* transform = world.get<ecs::LocalTransform>(entity);
        if (transform == nullptr) {
            fold(0xDEADull); // "bound but carries nothing" — a real difference, so hash it
            return;
        }
        const std::vector<std::byte> bytes = core::serialize(*transform);
        for (const std::byte b : bytes) {
            hash ^= std::to_integer<std::uint64_t>(b);
            hash *= 0x100000001b3ull;
        }
    });
    return hash;
}

// Everything a two-peer replication scenario needs, on one virtual clock.
struct Fixture {
    net::ScriptedNetwork network;
    net::Endpoint server_endpoint{0x7F000001u, kServerPort};
    net::Endpoint client_endpoint{0x7F000001u, kClientPort};

    ecs::World server_world;
    ecs::World client_world;

    std::vector<net::SessionEvent> events;
    std::uint64_t now_ms = 0;

    explicit Fixture(net::ScriptedNetwork::Config config, std::uint64_t seed = 0xC0FFEEull)
        : network(seed, config) {
        register_components(server_world);
        register_components(client_world);
    }
};

} // namespace

TEST_CASE("the wire schema excludes components holding an entity reference") {
    ecs::World world;
    register_components(world);
    const auto schema = replication::WireSchema::build(world.components());

    // Parent is { ecs::Entity value; } — a handle into the SENDER's entity directory, which names
    // nothing in the receiver's. Shipping the raw bytes would be silent corruption, so the type is
    // dropped from the replicated set and named, loudly, in excluded_names().
    bool parent_excluded = false;
    for (const std::string& name : schema.excluded_names()) {
        if (name.find("Parent") != std::string::npos) {
            parent_excluded = true;
        }
    }
    CHECK(parent_excluded);

    // LocalTransform is pure floats all the way down, so it replicates.
    CHECK(schema.wire_id_of(world.component_id<ecs::LocalTransform>()) !=
          replication::kInvalidWireComponentId);
    CHECK(schema.wire_id_of(world.component_id<ecs::Parent>()) ==
          replication::kInvalidWireComponentId);
}

TEST_CASE("packed_size is exactly what serialize writes") {
    // The contract a framed reader depends on: given only the type, packed_size says how far to
    // advance. If it ever disagreed with serialize, the reader would desynchronize on the first
    // component and misread every one after it in the packet.
    CHECK(core::packed_size(core::reflect<ecs::LocalTransform>()) ==
          core::serialize(ecs::LocalTransform{}).size());
    CHECK(core::packed_size(core::reflect<ecs::Entity>()) == core::serialize(ecs::Entity{}).size());
    // And it is a *packed* length: Transform is 10 floats, so 40 bytes, whatever the compiler
    // decided sizeof should be.
    CHECK(core::packed_size(core::reflect<ecs::LocalTransform>()) == 40);
}

TEST_CASE("the ack watermark only advances past a tick whose every part arrived") {
    replication::AckTracker tracker;
    CHECK(tracker.watermark() == 0);

    // A single-part tick completes immediately.
    tracker.observe(10, 0, 1);
    CHECK(tracker.watermark() == 10);

    // A two-part tick where only part 1 shows up must NOT advance the watermark. This is the exact
    // bug the design exists to prevent: acking tick 11 here would make the server compute its next
    // delta as "changed since 11", and the entities written at 11 that lived in the lost part 0
    // would never be re-offered — a permanent, silent divergence.
    tracker.observe(11, 1, 2);
    CHECK(tracker.watermark() == 10);

    // The missing part arrives: now 11 is complete.
    tracker.observe(11, 0, 2);
    CHECK(tracker.watermark() == 11);

    // An incomplete tick abandoned by a newer one leaves the watermark where it was — conservative,
    // which is the safe direction (the server keeps re-offering).
    tracker.observe(12, 0, 3);
    tracker.observe(13, 0, 1);
    CHECK(tracker.watermark() == 13);

    // A straggler from an abandoned tick cannot drag the watermark backwards.
    tracker.observe(12, 1, 3);
    tracker.observe(12, 2, 3);
    CHECK(tracker.watermark() == 13);
}

TEST_CASE("a moving world converges bit-identically across a lossy, reordering link") {
    // 20% loss with jitter wide enough to reorder — the proof must hold under a link that really
    // misbehaves, not one that merely could.
    Fixture fx({/*loss_rate=*/0.20f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/5,
                /*max_latency_ms=*/40});

    net::Link& server_link = fx.network.add_node(fx.server_endpoint);
    net::Link& client_link = fx.network.add_node(fx.client_endpoint);

    net::NetDriver::Config server_config;
    server_config.app_id = 0x52494D45u; // 'RIME'
    server_config.schema_hash = ecs::component_schema_hash(fx.server_world);
    server_config.salt_seed = 0x1111ull;

    net::NetDriver::Config client_config = server_config;
    client_config.schema_hash = ecs::component_schema_hash(fx.client_world);
    client_config.salt_seed = 0x2222ull;

    // The handshake's own guarantee, restated as a precondition of everything below: both sides
    // derive the wire schema from registries this number proves identical.
    REQUIRE(server_config.schema_hash == client_config.schema_hash);

    net::NetDriver server_driver{server_link, server_config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator server{fx.server_world};
    replication::ClientReplicator client{fx.client_world};

    // Enough entities to force the multi-packet path: one LocalTransform record is ~51 bytes, so
    // ~22 fit a 1150-byte payload. 60 guarantees at least three parts per full tick.
    constexpr int kEntities = 60;
    std::vector<ecs::Entity> movers;
    movers.reserve(kEntities);
    for (int i = 0; i < kEntities; ++i) {
        const ecs::Entity e = fx.server_world.spawn_with(ecs::LocalTransform{});
        (void)server.replicate(e);
        movers.push_back(e);
    }

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    // One tick of the whole system, in the order Application's sim stage prescribes (A8):
    // PreSim = poll + apply remote, then the sim's own writes, then Publish = send.
    const auto tick = [&](bool move) {
        fx.now_ms += kTickMs;
        fx.network.advance_time(fx.now_ms);

        // ── PreSim ──
        fx.events.clear();
        server_driver.update(fx.now_ms, fx.events);
        server.on_session_events(fx.events);
        (void)server.apply_inbound(server_driver);

        fx.events.clear();
        client_driver.update(fx.now_ms, fx.events);
        client.apply_inbound(client_driver);

        // ── the simulation itself ──
        // advance_version() before the tick's writes, so they stamp a version later than any
        // consumer's last checkpoint (world.hpp's documented discipline for a Schedule-less
        // driver).
        fx.server_world.advance_version();
        if (move) {
            for (std::size_t i = 0; i < movers.size(); ++i) {
                auto* transform = fx.server_world.get<ecs::LocalTransform>(movers[i]);
                REQUIRE(transform != nullptr);
                transform->value.translation.x += 1.0f + static_cast<float>(i);
                transform->value.translation.y -= 0.5f;
                fx.server_world.mark_changed<ecs::LocalTransform>(movers[i]);
            }
        }

        // ── Publish ──
        server.publish(server_driver, fx.now_ms);
        client.send_ack(client_driver, fx.now_ms);
    };

    // Run the movers for a good while.
    for (int i = 0; i < 200; ++i) {
        tick(/*move=*/true);
    }
    REQUIRE(client_driver.session_count() == 1);
    const ecs::Version freeze_version = fx.server_world.version();

    // Freeze, then pump until the client has confirmed a baseline at or past the freeze AND holds
    // every entity. A bounded loop, so a failure is the deadline expiring, never a hang.
    bool converged = false;
    for (int i = 0; i < 2000 && !converged; ++i) {
        tick(/*move=*/false);
        const net::SessionId sid = server_driver.session_ids().front();
        converged = server.acked_baseline(sid) >= freeze_version &&
                    client.map().size() == static_cast<std::size_t>(kEntities);
    }
    REQUIRE_MESSAGE(converged, "worlds did not converge before the deadline");

    // ── The claim ──
    const std::uint64_t server_hash = replicated_state_hash(fx.server_world, server.map());
    const std::uint64_t client_hash = replicated_state_hash(fx.client_world, client.map());
    CHECK(server_hash == client_hash);

    // NEGATIVE CONTROL. An equality assertion is only worth what its comparator can distinguish: a
    // hash that folded nothing, or walked no entities, would match just as happily. Perturb one
    // float on one mirrored entity and require the hash to notice.
    const ecs::Entity probe = client.map().resolve(server.map().net_id_of(movers.front()).value());
    REQUIRE(probe.is_valid());
    auto* probe_transform = fx.client_world.get<ecs::LocalTransform>(probe);
    REQUIRE(probe_transform != nullptr);
    probe_transform->value.translation.x += 1.0f;
    CHECK(replicated_state_hash(fx.client_world, client.map()) != client_hash);

    // ── Non-vacuousness: prove the scenario actually exercised what it claims ──
    CHECK(fx.network.packets_dropped() > 0);     // the link really lost packets
    CHECK(server.multipart_ticks() > 0);         // the split path really ran
    CHECK(client.spawns_applied() == kEntities); // structure really replicated
    CHECK(client.deltas_applied() > 0);          // state really replicated
    CHECK(client.malformed_messages() == 0);     // and nothing was garbled doing it

    // The cross-channel race really happened: unreliable deltas arrived naming entities whose
    // reliable Spawn was still in flight, and NetIdMap::resolve dropped them instead of writing
    // into the wrong mirror. Deterministic under the fixed seed and virtual clock, so this is a
    // reproducible fact about this trace rather than a probabilistic hope.
    CHECK(client.records_dropped_unmapped() > 0);
}

TEST_CASE("a despawn removes the mirror, and a recycled NetId does not alias it") {
    // No loss here: this case is about ORDERING and identity, not delivery. Latency still varies,
    // so the two channels remain free to interleave.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/20});

    net::Link& server_link = fx.network.add_node(fx.server_endpoint);
    net::Link& client_link = fx.network.add_node(fx.client_endpoint);

    net::NetDriver::Config config;
    config.schema_hash = ecs::component_schema_hash(fx.server_world);
    config.salt_seed = 0x3333ull;
    net::NetDriver::Config client_config = config;
    client_config.salt_seed = 0x4444ull;

    net::NetDriver server_driver{server_link, config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator server{fx.server_world};
    replication::ClientReplicator client{fx.client_world};

    const ecs::Entity first = fx.server_world.spawn_with(ecs::LocalTransform{});
    const replication::NetId first_id = server.replicate(first);

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    const auto tick = [&]() {
        fx.now_ms += kTickMs;
        fx.network.advance_time(fx.now_ms);
        fx.events.clear();
        server_driver.update(fx.now_ms, fx.events);
        server.on_session_events(fx.events);
        (void)server.apply_inbound(server_driver);
        fx.events.clear();
        client_driver.update(fx.now_ms, fx.events);
        client.apply_inbound(client_driver);
        fx.server_world.advance_version();
        server.publish(server_driver, fx.now_ms);
        client.send_ack(client_driver, fx.now_ms);
    };

    for (int i = 0; i < 40; ++i) {
        tick();
    }
    REQUIRE(client.map().size() == 1);
    const ecs::Entity mirror = client.map().resolve(first_id);
    REQUIRE(mirror.is_valid());
    CHECK(fx.client_world.is_alive(mirror));

    // Despawn through the replicator (never world.despawn directly — that would leave a phantom on
    // every client forever), then immediately spawn a replacement. The allocator recycles index 0,
    // so the new id shares an index with the dead one and differs only in generation.
    server.despawn(first);
    const ecs::Entity second = fx.server_world.spawn_with(ecs::LocalTransform{});
    const replication::NetId second_id = server.replicate(second);
    CHECK(second_id.index == first_id.index);
    CHECK(second_id.generation != first_id.generation);

    for (int i = 0; i < 40; ++i) {
        tick();
    }

    // The old mirror is gone, a distinct new one exists, and the stale handle resolves to nothing —
    // the generation check doing its job. Were the map keyed on index alone, first_id would now
    // resolve to the replacement and the server's state would smear onto the wrong entity.
    CHECK(!fx.client_world.is_alive(mirror));
    CHECK(!client.map().resolve(first_id).is_valid());
    const ecs::Entity new_mirror = client.map().resolve(second_id);
    REQUIRE(new_mirror.is_valid());
    CHECK(new_mirror != mirror);
    CHECK(client.map().size() == 1);
    CHECK(client.despawns_applied() >= 1);
}

TEST_CASE("state arriving before its Spawn is held and replayed, not dropped") {
    // ADR-0033 A14. The cross-channel race is deliberate (§3): a reliable Spawn can land after the
    // unreliable Delta that first names an entity. What the client does with that Delta decides
    // whether a wall that is written once and then stands still ever appears on the client at all.
    //
    // This proof drives the case that broke the two earlier answers: entities that are written ONCE
    // and then never again. Under the original m11.3 rule their state was discarded and never
    // re-offered; under A13's rule it arrived, but only after the baseline had stalled for the
    // whole burst. A14 holds the bytes, so neither happens.
    Fixture fx({/*loss_rate=*/0.20f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/5,
                /*max_latency_ms=*/40});

    net::Link& server_link = fx.network.add_node(fx.server_endpoint);
    net::Link& client_link = fx.network.add_node(fx.client_endpoint);

    net::NetDriver::Config server_config;
    server_config.app_id = 0x52494D45u;
    server_config.schema_hash = ecs::component_schema_hash(fx.server_world);
    server_config.salt_seed = 0x1111ull;
    net::NetDriver::Config client_config = server_config;
    client_config.schema_hash = ecs::component_schema_hash(fx.client_world);
    client_config.salt_seed = 0x2222ull;

    net::NetDriver server_driver{server_link, server_config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator server{fx.server_world};
    replication::ClientReplicator client{fx.client_world};

    // Written once at spawn, never touched again — the static prop / standing wall case.
    constexpr int kStatics = 80;
    std::vector<ecs::Entity> statics;
    for (int i = 0; i < kStatics; ++i) {
        ecs::LocalTransform t{};
        t.value.translation.x = static_cast<float>(i);
        const ecs::Entity e = fx.server_world.spawn_with(t);
        (void)server.replicate(e);
        statics.push_back(e);
    }

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    const auto tick = [&]() {
        fx.now_ms += kTickMs;
        fx.network.advance_time(fx.now_ms);

        fx.events.clear();
        server_driver.update(fx.now_ms, fx.events);
        server.on_session_events(fx.events);
        (void)server.apply_inbound(server_driver);

        fx.events.clear();
        client_driver.update(fx.now_ms, fx.events);
        client.apply_inbound(client_driver);

        fx.server_world.advance_version(); // ticks pass; the statics are never rewritten
        server.publish(server_driver, fx.now_ms);
        client.send_ack(client_driver, fx.now_ms);
    };

    for (int i = 0; i < 120; ++i) {
        tick();
    }

    // Non-vacuousness: the race really happened, and the hold path really ran. A run in which every
    // Spawn beat its Delta would prove nothing about either.
    CHECK(client.records_deferred() > 0);
    CHECK(client.records_replayed() > 0);
    CHECK(client.records_evicted() == 0); // an honest peer never exhausts the buffer
    CHECK(fx.network.packets_dropped() > 0);

    // Every static's state arrived, despite never being rewritten after the tick it was spawned on.
    // This is the assertion the original m11.3 code fails.
    int mirrored = 0;
    client.map().for_each([&](replication::NetId, ecs::Entity mirror) {
        if (fx.client_world.get<ecs::LocalTransform>(mirror) != nullptr) {
            ++mirrored;
        }
    });
    CHECK(mirrored == kStatics);

    // And the baseline is live rather than pinned at zero — the cost A13 paid and A14 does not.
    CHECK(client.watermark() > 0);
    CHECK(server.acked_baseline(*server_driver.session_ids().begin()) > 0);

    // Bit-identical convergence, the same standard as the moving-world proof.
    CHECK(replicated_state_hash(fx.server_world, server.map()) ==
          replicated_state_hash(fx.client_world, client.map()));
}

TEST_CASE("entities withheld over the packet budget are still delivered eventually") {
    // The claim m11.3 made about its own over-budget path — "the baseline mechanism re-offers it
    // next tick, so this is latency, never loss" — is false for an entity that stops changing, and
    // m11.5 turns that path from an overflow case into the normal one (relevancy withholds
    // deliberately, every tick). So it has to be true before relevancy can be built on it.
    //
    // The failure it guards: tick T exceeds the packet budget, so entity E is not sent. Every
    // packet that WAS sent arrives, so the client completes tick T and acks it. The server advances
    // the baseline to T — past E's write, which happened at or before T. E never changes again, so
    // it is never re-offered, and the client's mirror of it stays empty forever.
    Fixture fx({/*loss_rate=*/0.0f, // no loss: this is about the SERVER withholding, not the link
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});

    net::Link& server_link = fx.network.add_node(fx.server_endpoint);
    net::Link& client_link = fx.network.add_node(fx.client_endpoint);

    net::NetDriver::Config server_config;
    server_config.app_id = 0x52494D45u;
    server_config.schema_hash = ecs::component_schema_hash(fx.server_world);
    server_config.salt_seed = 0x1111ull;
    net::NetDriver::Config client_config = server_config;
    client_config.schema_hash = ecs::component_schema_hash(fx.client_world);
    client_config.salt_seed = 0x2222ull;

    net::NetDriver server_driver{server_link, server_config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator server{fx.server_world};
    replication::ClientReplicator client{fx.client_world};

    // Comfortably past the per-tick ceiling: a LocalTransform record is ~51 bytes, ~22 fit a
    // packet, and a tick may use at most kMaxDeltaPartsPerTick packets — so ~176 entities fit and
    // the rest must be withheld. All written once, at spawn, and never touched again.
    constexpr int kEntities = 400;
    for (int i = 0; i < kEntities; ++i) {
        ecs::LocalTransform t{};
        t.value.translation.x = static_cast<float>(i);
        (void)server.replicate(fx.server_world.spawn_with(t));
    }

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    for (int i = 0; i < 200; ++i) {
        fx.now_ms += kTickMs;
        fx.network.advance_time(fx.now_ms);

        fx.events.clear();
        server_driver.update(fx.now_ms, fx.events);
        server.on_session_events(fx.events);
        (void)server.apply_inbound(server_driver);

        fx.events.clear();
        client_driver.update(fx.now_ms, fx.events);
        client.apply_inbound(client_driver);

        fx.server_world.advance_version(); // time passes; nothing is ever rewritten
        server.publish(server_driver, fx.now_ms);
        client.send_ack(client_driver, fx.now_ms);
    }

    // Non-vacuousness: the budget really was exceeded, or this proves nothing about the path.
    CHECK(server.entities_dropped_over_budget() > 0);

    // 200 ticks is far more than the ~3 the backlog needs to drain at ~176 entities a tick. Every
    // entity must have arrived with its state.
    int mirrored = 0;
    client.map().for_each([&](replication::NetId, ecs::Entity mirror) {
        if (fx.client_world.get<ecs::LocalTransform>(mirror) != nullptr) {
            ++mirrored;
        }
    });
    CHECK(mirrored == kEntities);

    CHECK(replicated_state_hash(fx.server_world, server.map()) ==
          replicated_state_hash(fx.client_world, client.map()));
}
