// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/reflect/serialize.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/render_transform.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/interpolation.hpp"
#include "rime/replication/relevancy.hpp"
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

TEST_CASE("an entity killed behind the replicator's back is retracted by the backstop") {
    // `ServerReplicator::despawn` has always carried a warning — never `world.despawn()` on
    // anything holding a NetId — and until m12.3 that was all it carried. ADR-0035 §6 ruled the
    // backstop into the first brick to add a new replication consumer, and this is its proof.
    //
    // WHAT THE MISTAKE COSTS WITHOUT ONE. `despawn` is the only path that retracts an id, so an
    // entity destroyed directly leaves its NetId live in the allocator and bound in the map. The
    // per-client structure diff then sees an id that is still live and never emits a Despawn: the
    // client keeps a mirror of an entity that no longer exists, permanently, and no later message
    // repairs it. It is the same silent-divergence family the AckTracker and the relevancy entry
    // pass were each built to prevent, arriving through a door neither of them watches.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/8});

    net::Link& server_link = fx.network.add_node(fx.server_endpoint);
    net::Link& client_link = fx.network.add_node(fx.client_endpoint);

    net::NetDriver::Config config;
    config.schema_hash = ecs::component_schema_hash(fx.server_world);
    config.salt_seed = 0x5151ull;
    net::NetDriver::Config client_config = config;
    client_config.salt_seed = 0x6262ull;

    net::NetDriver server_driver{server_link, config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator server{fx.server_world};
    replication::ClientReplicator client{fx.client_world};

    // Two entities, so the backstop is shown to retract the RIGHT one rather than everything.
    const ecs::Entity doomed = fx.server_world.spawn_with(ecs::LocalTransform{});
    const ecs::Entity witness = fx.server_world.spawn_with(ecs::LocalTransform{});
    const replication::NetId doomed_id = server.replicate(doomed);
    const replication::NetId witness_id = server.replicate(witness);

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
    REQUIRE(client.map().size() == 2);
    const ecs::Entity mirror = client.map().resolve(doomed_id);
    REQUIRE(mirror.is_valid());
    // The control: nothing has gone wrong yet, so the backstop has fired zero times. A counter that
    // was already non-zero here would make the assertion below meaningless.
    CHECK(server.net_ids_orphaned() == 0);

    // THE MISTAKE. A bare world.despawn on a replicated entity — the call site a real game gets
    // wrong once, in a kill handler somebody wrote in a hurry.
    fx.server_world.despawn(doomed);
    REQUIRE_FALSE(fx.server_world.is_alive(doomed));

    for (int i = 0; i < 40; ++i) {
        tick();
    }

    // The backstop saw it, retracted the id, and counted it — exactly once, not once per tick.
    CHECK(server.net_ids_orphaned() == 1);
    // …and the client was told, so the mirror is gone rather than a phantom standing forever.
    CHECK_FALSE(fx.client_world.is_alive(mirror));
    CHECK_FALSE(client.map().resolve(doomed_id).is_valid());
    CHECK(client.despawns_applied() >= 1);

    // The witness is untouched: the backstop retracted the dead id and nothing else.
    CHECK(client.map().resolve(witness_id).is_valid());
    CHECK(client.map().size() == 1);

    // And the recycled slot is safe to hand out. This is the half that would have been quietly
    // wrong if the backstop had only freed the allocator: `was_relevant` and `starved_ticks` are
    // per-item records keyed by INDEX, so a new tenant inheriting the dead entity's bits is
    // corollary 2 of docs/design/replication.md all over again.
    const ecs::Entity replacement = fx.server_world.spawn_with(ecs::LocalTransform{});
    const replication::NetId replacement_id = server.replicate(replacement);
    CHECK(replacement_id.index == doomed_id.index);
    CHECK(replacement_id.generation != doomed_id.generation);
    for (int i = 0; i < 40; ++i) {
        tick();
    }
    const ecs::Entity new_mirror = client.map().resolve(replacement_id);
    REQUIRE(new_mirror.is_valid());
    CHECK(new_mirror != mirror);
    CHECK(client.map().size() == 2);
    // Still exactly one orphan for the whole run: the disciplined spawn path does not trip it.
    CHECK(server.net_ids_orphaned() == 1);
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

TEST_CASE("an entity entering the relevant set is sent even though it never changed") {
    // m11.5's relevancy could not be a pure filter, and this is why. An entity that was irrelevant
    // and becomes relevant has by definition NOT changed since the client's baseline — it was
    // simply never sent. The ordinary "changed since" test therefore excludes it, and the client
    // would mirror an entity it holds no state for, permanently. Entering the set has to force a
    // send.
    Fixture fx({/*loss_rate=*/0.0f,
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

    // Written once at spawn and never again — the case a pure filter loses.
    constexpr int kNear = 10;
    constexpr int kFar = 10;
    std::vector<ecs::Entity> far_entities;
    for (int i = 0; i < kNear + kFar; ++i) {
        ecs::LocalTransform t{};
        t.value.translation.x = static_cast<float>(i);
        const ecs::Entity e = fx.server_world.spawn_with(t);
        (void)server.replicate(e);
        if (i >= kNear) {
            far_entities.push_back(e);
        }
    }

    // The policy: entities in `far_entities` are irrelevant until `admit_far` flips.
    bool admit_far = false;
    server.set_relevancy(
        [&](net::SessionId, std::span<const ecs::Entity> candidates, std::span<float> priorities) {
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                const bool is_far =
                    std::find(far_entities.begin(), far_entities.end(), candidates[i]) !=
                    far_entities.end();
                priorities[i] = (is_far && !admit_far) ? 0.0f : 1.0f;
            }
        });

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            fx.now_ms += kTickMs;
            fx.network.advance_time(fx.now_ms);
            fx.events.clear();
            server_driver.update(fx.now_ms, fx.events);
            server.on_session_events(fx.events);
            (void)server.apply_inbound(server_driver);
            fx.events.clear();
            client_driver.update(fx.now_ms, fx.events);
            client.apply_inbound(client_driver);
            fx.server_world.advance_version(); // nothing is ever rewritten
            server.publish(server_driver, fx.now_ms);
            client.send_ack(client_driver, fx.now_ms);
        }
    };

    const auto mirrored_with_state = [&]() {
        int n = 0;
        client.map().for_each([&](replication::NetId, ecs::Entity mirror) {
            if (fx.client_world.get<ecs::LocalTransform>(mirror) != nullptr) {
                ++n;
            }
        });
        return n;
    };

    run(40);
    // The cull really happened — a relevancy test in which nothing was culled proves nothing.
    CHECK(server.entities_culled_irrelevant() > 0);
    CHECK(mirrored_with_state() == kNear);

    // Now they become relevant, without ever having been written again.
    admit_far = true;
    const std::uint64_t entered_before = server.entities_sent_on_entry();
    run(40);

    CHECK(server.entities_sent_on_entry() > entered_before);
    CHECK(mirrored_with_state() == kNear + kFar);
    CHECK(replicated_state_hash(fx.server_world, server.map()) ==
          replicated_state_hash(fx.client_world, client.map()));
}

TEST_CASE("a despawned entity's slot does not pin the full-walk optimization off forever") {
    // THE DEFECT THIS PINS DOWN. `publish_delta` gives up the per-chunk "changed since baseline"
    // skip on any tick where something ENTERS a client's relevant set, because an entering entity
    // is by definition one that has NOT changed and the skip would step straight over it. That
    // trade is only cheap if entries are rare.
    //
    // The entry test reads two slot-indexed arrays: `priority_by_index_`, which defaults every slot
    // to RELEVANT (1.0), and `was_relevant`, which the cull path clears to 0. The policy only ever
    // overwrites priorities for slots the NetIdMap still holds. So a slot whose entity was culled
    // (was_relevant = 0) and then DESPAWNED (no longer a candidate, priority stuck at the 1.0
    // default) reads forever after as "relevant, and was not relevant last tick" — an entity
    // eternally entering, which no longer exists.
    //
    // Nothing observable changes: the bytes on the wire are identical and the state still
    // converges. Only the cost changes, permanently, for every client. That is exactly why this
    // needs a counter rather than an eyeball — and why m11.5's distance culling is what makes it
    // urgent, since debris are culled and despawned continuously by design.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});

    net::ScriptedLink& server_link = fx.network.add_node(fx.server_endpoint);
    net::ScriptedLink& client_link = fx.network.add_node(fx.client_endpoint);

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

    // One entity that stays relevant forever, and one that will be culled and then despawned.
    ecs::LocalTransform t{};
    const ecs::Entity keeper = fx.server_world.spawn_with(t);
    t.value.translation.x = 1.0f;
    const ecs::Entity doomed = fx.server_world.spawn_with(t);
    (void)server.replicate(keeper);
    (void)server.replicate(doomed);

    bool cull_doomed = false;
    server.set_relevancy(
        [&](net::SessionId, std::span<const ecs::Entity> candidates, std::span<float> priorities) {
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                priorities[i] = (cull_doomed && candidates[i] == doomed) ? 0.0f : 1.0f;
            }
        });

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    // The keeper MOVES every tick. That is not decoration: the cull bookkeeping lives inside the
    // per-chunk row walk, so a world in which nothing ever changes skips the chunk entirely and
    // never observes that anything became irrelevant. A relevancy defect only has teeth in a world
    // with motion in it, so the proof has to supply some.
    float keeper_x = 0.0f;
    bool keeper_moving = true;
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
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
            if (keeper_moving) {
                keeper_x += 1.0f;
                if (auto* transform = fx.server_world.get<ecs::LocalTransform>(keeper)) {
                    transform->value.translation.x = keeper_x;
                    fx.server_world.mark_changed<ecs::LocalTransform>(keeper);
                }
            }
            server.publish(server_driver, fx.now_ms);
            client.send_ack(client_driver, fx.now_ms);
        }
    };

    // Settle: everything relevant, everything delivered and acked.
    run(20);

    // Cull it, let the cull register, then despawn it through the replicator (which retracts the
    // NetId before destroying the entity, freeing the slot for reuse).
    cull_doomed = true;
    run(5);
    REQUIRE(server.entities_culled_irrelevant() > 0);
    server.despawn(doomed);
    run(10);

    // The world is now quiet and stable: one entity, permanently relevant, never rewritten. No
    // entity can legitimately be entering relevance any more, so the full walk must stop.
    const std::uint64_t entry_records_before = server.entry_pass_records();
    const std::uint64_t ticks_before = server.delta_ticks();
    run(30);
    const std::uint64_t entry_records_added = server.entry_pass_records() - entry_records_before;
    const std::uint64_t ticks_added = server.delta_ticks() - ticks_before;

    // Non-vacuousness: the delta path really did run on those ticks.
    REQUIRE(ticks_added > 0);
    // No live entity is entering, so the entry pass must emit nothing. A dead slot that still read
    // as relevant would be serialized here every tick — which is the defect, now visible directly
    // as work done rather than indirectly as an optimization switched off.
    CHECK(entry_records_added == 0);

    // And the surviving entity is still correctly mirrored — the fix must not buy its cheapness by
    // dropping the entry that matters. Let the keeper come to rest first: it has been moving every
    // tick, so a hash taken mid-flight compares the server's now against the client's now-minus-RTT
    // and would fail on a perfectly healthy link.
    keeper_moving = false;
    run(10);
    CHECK(replicated_state_hash(fx.server_world, server.map()) ==
          replicated_state_hash(fx.client_world, client.map()));
}

TEST_CASE("distance_relevancy culls by range, admits on approach, and does not thrash a boundary") {
    // The ready-made policy m11.5 ships. Three properties, because each one is a separate way the
    // obvious implementation goes wrong:
    //
    //   1. It culls by range, and an entity that comes into range is SENT even though it never
    //      changed — the entry path, exercised through the real policy rather than a test lambda.
    //   2. An entity with no transform at all is ALWAYS relevant. A distance policy that culled
    //      what it could not measure would silently stop replicating every non-spatial entity in
    //      the game, and would do it invisibly.
    //   3. An entity loitering on the boundary does not flip in and out every tick. That is what
    //      the hysteresis band is for, and its absence is measurable rather than theoretical:
    //      every re-entry costs a forced full-state send AND the per-chunk delta skip for that
    //      whole tick, for that client.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});

    net::ScriptedLink& server_link = fx.network.add_node(fx.server_endpoint);
    net::ScriptedLink& client_link = fx.network.add_node(fx.client_endpoint);

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

    constexpr float kRadius = 100.0f;

    // Near: comfortably inside. Far: comfortably outside, and it never moves or changes again —
    // the case a pure filter loses forever.
    ecs::LocalTransform near_t{};
    near_t.value.translation.x = 10.0f;
    const ecs::Entity near_entity = fx.server_world.spawn_with(near_t);

    ecs::LocalTransform far_t{};
    far_t.value.translation.x = 500.0f;
    const ecs::Entity far_entity = fx.server_world.spawn_with(far_t);

    // Straddles the boundary: oscillates between 0.95R and 1.05R, so a policy without a hysteresis
    // band would flip it on every single tick.
    ecs::LocalTransform edge_t{};
    edge_t.value.translation.x = kRadius * 0.95f;
    const ecs::Entity edge_entity = fx.server_world.spawn_with(edge_t);

    // No transform of any kind — the "cannot be measured" case. Parent is unreflected, so this
    // entity is replicated but carries nothing across the wire; what matters is that the POLICY
    // scores it relevant rather than culling it.
    const ecs::Entity placeless = fx.server_world.spawn_with(ecs::Parent{ecs::kNullEntity});

    // The sharper unmeasurable case: a CHILD with a LocalTransform but no WorldTransform. Its local
    // translation is relative to its parent, so reading it as a world position would be a
    // coordinate-space error, not a rounding one — here the parent is 900 units away while the
    // child's own local offset is 1.0, so a policy that fell back unconditionally would score it as
    // sitting essentially on top of the viewpoint. It must be treated as unmeasurable instead.
    ecs::LocalTransform parent_t{};
    parent_t.value.translation.x = 900.0f;
    const ecs::Entity distant_parent = fx.server_world.spawn_with(parent_t);
    ecs::LocalTransform child_t{};
    child_t.value.translation.x = 1.0f;
    const ecs::Entity child = fx.server_world.spawn_with(child_t, ecs::Parent{distant_parent});

    (void)server.replicate(near_entity);
    (void)server.replicate(far_entity);
    (void)server.replicate(edge_entity);
    (void)server.replicate(placeless);
    (void)server.replicate(child);

    core::Vec3 eye{0.0f, 0.0f, 0.0f};
    replication::DistanceRelevancy config;
    config.radius = kRadius;
    config.hysteresis = 0.1f; // leave at 110
    config.viewpoint = [&](net::SessionId, core::Vec3& out) {
        out = eye;
        return true;
    };
    server.set_relevancy(replication::distance_relevancy(fx.server_world, config));

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    int tick_index = 0;
    bool oscillate = false;
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
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
            if (oscillate) {
                ++tick_index;
                if (auto* t = fx.server_world.get<ecs::LocalTransform>(edge_entity)) {
                    t->value.translation.x =
                        (tick_index % 2 == 0) ? kRadius * 0.95f : kRadius * 1.05f;
                    fx.server_world.mark_changed<ecs::LocalTransform>(edge_entity);
                }
            }
            server.publish(server_driver, fx.now_ms);
            client.send_ack(client_driver, fx.now_ms);
        }
    };

    const auto mirror_of = [&](ecs::Entity server_entity) -> ecs::Entity {
        const auto id = server.map().net_id_of(server_entity);
        if (!id) {
            return ecs::kNullEntity;
        }
        ecs::Entity found = ecs::kNullEntity;
        client.map().for_each([&](replication::NetId mirrored_id, ecs::Entity mirror) {
            if (mirrored_id.index == id->index) {
                found = mirror;
            }
        });
        return found;
    };
    const auto has_state = [&](ecs::Entity server_entity) {
        const ecs::Entity mirror = mirror_of(server_entity);
        return mirror.is_valid() && fx.client_world.get<ecs::LocalTransform>(mirror) != nullptr;
    };

    run(40);

    // ── 1. Range culling, and its non-vacuousness. ──
    CHECK(server.entities_culled_irrelevant() > 0);
    CHECK(has_state(near_entity));
    CHECK(!has_state(far_entity));

    // ── 2. The unmeasurable entities were never culled. Announced and bound on the client, and in
    // the child's case its state actually delivered — even though the viewpoint is 900 units from
    // the parent it hangs off, and its own LocalTransform (1.0) would have read as near if the
    // policy had wrongly used it as a world position.
    CHECK(mirror_of(placeless).is_valid());
    CHECK(has_state(child));

    // ── 3. Approach: the far entity comes into range WITHOUT EVER BEING WRITTEN AGAIN. ──
    const std::uint64_t entered_before = server.entities_sent_on_entry();
    eye.x = 450.0f; // now 50 away from the far entity, 440 from the near one
    run(40);
    CHECK(server.entities_sent_on_entry() > entered_before);
    CHECK(has_state(far_entity));

    // ── 3b. And with the viewpoint 450 away, the CHILD must still be getting updates. This is the
    // assertion that actually discriminates the root check: an unconditional LocalTransform
    // fallback reads the child as x = 1.0, which is inside the radius while the eye sits at the
    // origin — so the earlier check passes either way. Only once the eye has moved far from the
    // child's own local offset do the two behaviours separate: unmeasurable means always relevant
    // and still updating, whereas the coordinate-space error means culled and silently stale.
    if (auto* t = fx.server_world.get<ecs::LocalTransform>(child)) {
        t->value.translation.z = 7.0f;
        fx.server_world.mark_changed<ecs::LocalTransform>(child);
    }
    run(20);
    const ecs::Entity child_mirror = mirror_of(child);
    REQUIRE(child_mirror.is_valid());
    const auto* child_state = fx.client_world.get<ecs::LocalTransform>(child_mirror);
    REQUIRE(child_state != nullptr);
    CHECK(child_state->value.translation.z == doctest::Approx(7.0f));

    // ── 4. Hysteresis: park the viewpoint at the origin and let the edge entity oscillate across
    // the radius. Once inside, it must stay inside — it never reaches the 110 exit radius — so no
    // entry, and therefore no entry-pass work at all, should be attributable to it.
    eye.x = 0.0f;
    run(40); // settle: edge entity is inside, far entity is culled again
    oscillate = true;
    const std::uint64_t entry_records_before = server.entry_pass_records();
    const std::uint64_t entries_before = server.entities_sent_on_entry();
    const std::uint64_t ticks_before = server.delta_ticks();
    run(40);

    CHECK(server.delta_ticks() - ticks_before > 0);           // the path really ran
    CHECK(server.entities_sent_on_entry() == entries_before); // nothing re-entered
    // And nothing was re-serialized either — the band held on both sides of the boundary, not just
    // in the delivery counter.
    CHECK(server.entry_pass_records() == entry_records_before);
    CHECK(has_state(edge_entity)); // and it stayed relevant throughout, rather than being dropped
}

TEST_CASE("a byte budget delays delivery without ever declaring the tick complete") {
    // The byte budget is the half of m11.5 that actually gets turned on, and it had the same defect
    // the packet budget was fixed for: trimming `records_` makes the tick LOOK complete to the
    // packing loop — every surviving record fits — so `complete_through` advanced past state that
    // was deliberately withheld. That watermark is the client's baseline clamp, so advancing it
    // retires the withheld entities from the candidate set. An entity written ONCE and then never
    // again is gone permanently: it did not change, so the delta skips it, and the budget already
    // "completed" the tick it was owed on.
    //
    // Writing once and never again is the discriminating case. Continuously-moving entities hide
    // this completely, because next tick's write re-offers them regardless — which is why a proof
    // built on a moving world would pass against the bug.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});

    net::ScriptedLink& server_link = fx.network.add_node(fx.server_endpoint);
    net::ScriptedLink& client_link = fx.network.add_node(fx.client_endpoint);

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

    constexpr int kEntities = 200;
    std::vector<ecs::Entity> entities;
    for (int i = 0; i < kEntities; ++i) {
        ecs::LocalTransform t{};
        t.value.translation.x = static_cast<float>(i);
        entities.push_back(fx.server_world.spawn_with(t));
        (void)server.replicate(entities.back());
    }

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
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
        }
    };

    // FIRST deliver everything with the budget off. This is the step that makes the test
    // discriminating: an entity that has never been delivered is retried forever by the ENTRY path
    // regardless of the baseline, so a world of first-time arrivals converges even with the
    // watermark bug present. The bug only bites an entity the client already holds, whose LATER
    // change is dropped — because that one is offered by the version delta alone, and the version
    // delta is exactly what a wrongly-advanced baseline switches off.
    run(60);
    REQUIRE(replicated_state_hash(fx.server_world, server.map()) ==
            replicated_state_hash(fx.client_world, client.map()));

    // Now clamp the budget hard and rewrite every entity ONCE, in a single burst. Writing once is
    // the discriminating case: a continuously-moving entity is re-offered by next tick's write
    // whatever the baseline says, which would hide this completely.
    replication::Budget budget;
    budget.max_bytes_per_tick = 256; // a handful of records per tick against a 200-entity burst
    server.set_budget(budget);

    const std::uint64_t dropped_before = server.entities_dropped_over_budget();
    for (int i = 0; i < kEntities; ++i) {
        if (auto* t =
                fx.server_world.get<ecs::LocalTransform>(entities[static_cast<std::size_t>(i)])) {
            t->value.translation.y = static_cast<float>(i) + 1000.0f;
            fx.server_world.mark_changed<ecs::LocalTransform>(
                entities[static_cast<std::size_t>(i)]);
        }
    }

    // Quiet from here: nothing is ever written again, so every one of those 200 changes must be
    // delivered off the backlog or not at all.
    run(400);

    // Non-vacuousness: the budget really did refuse records, or this proves only that a world small
    // enough to fit still works.
    CHECK(server.entities_dropped_over_budget() > dropped_before);

    // Delayed is fine; dropped is not.
    CHECK(replicated_state_hash(fx.server_world, server.map()) ==
          replicated_state_hash(fx.client_world, client.map()));
}

TEST_CASE("entry work is proportional to what entered, not to how big the world is") {
    // The payoff proof for the entry pass, and the property the design it replaced could not have.
    //
    // The old shape gated the per-chunk "changed since baseline" skip on one global flag: if
    // ANYTHING entered a client's relevant set, every column of every chunk of every replicated
    // archetype was re-examined for that client. One distant chunk of rubble drifting into range
    // cost a full pass over a world that had nothing to do with it — and at the ADR's 64-client,
    // ~1000-debris target that flag is on essentially every tick, so the skip was effectively off
    // whenever relevancy was on.
    //
    // Here: a settled, entirely quiet world of many entities, and then exactly ONE entity
    // becomes relevant. The entry pass must do exactly one entity's worth of work. Not "less than
    // the whole world" — exactly one, which is the claim that stays true as the world grows.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});

    net::ScriptedLink& server_link = fx.network.add_node(fx.server_endpoint);
    net::ScriptedLink& client_link = fx.network.add_node(fx.client_endpoint);

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

    // Sized to fit comfortably inside one tick's packet allowance (8 parts x 1150 bytes against
    // ~51-byte records). Deliberately NOT larger: a world that cannot be delivered in one tick hits
    // a separate, unrelated starvation defect on the relevancy path — see
    // docs/design/replication.md — and this proof is about the entry pass, not about that.
    constexpr int kBystanders = 120;
    for (int i = 0; i < kBystanders; ++i) {
        ecs::LocalTransform t{};
        t.value.translation.x = static_cast<float>(i % 20); // all comfortably inside the radius
        (void)server.replicate(fx.server_world.spawn_with(t));
    }
    // The one that will arrive later, parked far outside.
    ecs::LocalTransform newcomer_t{};
    newcomer_t.value.translation.x = 5000.0f;
    const ecs::Entity newcomer = fx.server_world.spawn_with(newcomer_t);
    (void)server.replicate(newcomer);

    core::Vec3 eye{0.0f, 0.0f, 0.0f};
    replication::DistanceRelevancy config;
    config.radius = 100.0f;
    config.viewpoint = [&](net::SessionId, core::Vec3& out) {
        out = eye;
        return true;
    };
    server.set_relevancy(replication::distance_relevancy(fx.server_world, config));

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            fx.now_ms += kTickMs;
            fx.network.advance_time(fx.now_ms);
            fx.events.clear();
            server_driver.update(fx.now_ms, fx.events);
            server.on_session_events(fx.events);
            (void)server.apply_inbound(server_driver);
            fx.events.clear();
            client_driver.update(fx.now_ms, fx.events);
            client.apply_inbound(client_driver);
            fx.server_world.advance_version(); // a quiet world: nothing is ever rewritten
            server.publish(server_driver, fx.now_ms);
            client.send_ack(client_driver, fx.now_ms);
        }
    };

    // Settle everything. The 500 bystanders enter here, which is legitimate work.
    run(120);
    REQUIRE(server.entry_pass_records() >= static_cast<std::uint64_t>(kBystanders));

    // Quiet: settled world, nothing entering, nothing written. The entry pass must go silent.
    const std::uint64_t quiet_before = server.entry_pass_records();
    run(30);
    CHECK(server.entry_pass_records() == quiet_before);

    // Now bring exactly one entity into range, by moving IT rather than the viewpoint — so the
    // other 500 keep the same relevance and cannot contribute entries of their own.
    if (auto* t = fx.server_world.get<ecs::LocalTransform>(newcomer)) {
        t->value.translation.x = 5.0f;
        fx.server_world.mark_changed<ecs::LocalTransform>(newcomer);
    }
    run(30);

    // Exactly one entity's worth of entry work, in a world of 121. This is the number that would
    // have been "the whole world" under the flag-gated design — and it stays 1 as the world grows,
    // which is the actual claim.
    CHECK(server.entry_pass_records() - quiet_before == 1);
    CHECK(replicated_state_hash(fx.server_world, server.map()) ==
          replicated_state_hash(fx.client_world, client.map()));
}

TEST_CASE("a permanently over-budget world still delivers its lowest-priority tail") {
    // THE LIVENESS RULE, on the path relevancy actually uses. `publish_delta` chooses between
    // ordering by priority and rotating by the resume cursor, and until this fix those were
    // exclusive: installing a relevancy policy took the sort branch, so the cursor — the mechanism
    // that exists precisely to stop a permanently over-budget world starving its tail — never ran.
    // The sort branch's own comment claimed otherwise.
    //
    // The result was that the same highest-priority prefix went out every tick and everything below
    // the cut-off was never delivered at all. Not late: never. That is the same bug the m11.5
    // foundation commit fixed for the no-policy path, and prioritization is exactly the feature
    // that reintroduced it.
    //
    // 501 entities at ~51 bytes a record is ~25.5 KB against a per-tick allowance of 8 packets x
    // 1150 bytes, so roughly a third of the world fits each tick and the deficit is permanent — the
    // condition has to be structural, not a burst that drains.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});

    net::ScriptedLink& server_link = fx.network.add_node(fx.server_endpoint);
    net::ScriptedLink& client_link = fx.network.add_node(fx.client_endpoint);

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

    constexpr int kEntities = 501;
    for (int i = 0; i < kEntities; ++i) {
        ecs::LocalTransform t{};
        // Spread across 20 distinct distances, all inside the radius. Distinct priorities are the
        // point: with every entity scored equally the stable sort would preserve archetype order
        // and the defect would be milder. A real distance policy produces a strict order, and a
        // strict order is what starves a tail.
        t.value.translation.x = static_cast<float>(i % 20);
        (void)server.replicate(fx.server_world.spawn_with(t));
    }

    replication::DistanceRelevancy config;
    config.radius = 100.0f;
    config.viewpoint = [](net::SessionId, core::Vec3& out) {
        out = core::Vec3{0.0f, 0.0f, 0.0f};
        return true;
    };
    server.set_relevancy(replication::distance_relevancy(fx.server_world, config));

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    // Generous but bounded: the deficit is ~1/3 of the world per tick, so a mechanism that makes
    // progress at all clears the backlog in tens of ticks. A mechanism that starves never clears it
    // no matter how long the loop runs, which is the difference this asserts.
    for (int i = 0; i < 250; ++i) {
        fx.now_ms += kTickMs;
        fx.network.advance_time(fx.now_ms);
        fx.events.clear();
        server_driver.update(fx.now_ms, fx.events);
        server.on_session_events(fx.events);
        (void)server.apply_inbound(server_driver);
        fx.events.clear();
        client_driver.update(fx.now_ms, fx.events);
        client.apply_inbound(client_driver);
        fx.server_world.advance_version(); // written once at spawn, never again
        server.publish(server_driver, fx.now_ms);
        client.send_ack(client_driver, fx.now_ms);
    }

    // Non-vacuousness: the budget really did bind, or this is a test of a world that fit.
    CHECK(server.entities_dropped_over_budget() > 0);

    int mirrored = 0;
    client.map().for_each([&](replication::NetId, ecs::Entity mirror) {
        if (fx.client_world.get<ecs::LocalTransform>(mirror) != nullptr) {
            ++mirrored;
        }
    });

    // Every one of them. Priority decides ORDER, never whether an entity is sent at all — otherwise
    // "nearest-first" silently also means "farthest-never", which is not what a byte budget
    // promises.
    CHECK(mirrored == kEntities);
    CHECK(replicated_state_hash(fx.server_world, server.map()) ==
          replicated_state_hash(fx.client_world, client.map()));
}

// A component too big to fit in one packet. LocalTransform is 40 packed bytes, so 32 of them is
// 1280 — comfortably past the 1150-byte payload budget once the header is counted. Built out of
// nested reflected structs because the reflection system has no array kind (scalars and structs
// only), which is itself the reason a component like this is easy to write by accident.
struct Bulk8 {
    core::Transform a{}, b{}, c{}, d{}, e{}, f{}, g{}, h{};
};

struct Bulk32 {
    Bulk8 q0{}, q1{}, q2{}, q3{};
};

RIME_REFLECT_BEGIN(Bulk8)
RIME_REFLECT_FIELD(a)
RIME_REFLECT_FIELD(b)
RIME_REFLECT_FIELD(c)
RIME_REFLECT_FIELD(d)
RIME_REFLECT_FIELD(e)
RIME_REFLECT_FIELD(f)
RIME_REFLECT_FIELD(g)
RIME_REFLECT_FIELD(h)
RIME_REFLECT_END()

RIME_REFLECT_BEGIN(Bulk32)
RIME_REFLECT_FIELD(q0)
RIME_REFLECT_FIELD(q1)
RIME_REFLECT_FIELD(q2)
RIME_REFLECT_FIELD(q3)
RIME_REFLECT_END()

TEST_CASE("an entity too big for one packet is dropped loudly, not retried forever") {
    // An entity's record is NEVER SPLIT — parts are independently-complete packets, not fragments
    // of one logical message. So an entity whose components exceed the payload budget on their own
    // cannot be transmitted at all, by any amount of budget or patience. That is a schema fault,
    // not a bandwidth one.
    //
    // The engine's job is to SAY SO. Both ways of not saying so were live at some point in this
    // stack: build the oversized part and let the channel refuse it, which either loses the entity
    // silently (if the tick still counts as complete) or jams `complete_through` forever (if it
    // does not — the honest reading, and the one priority aging made permanent, because the
    // undeliverable record ages and so sorts first every tick).
    //
    // What must happen instead: the oversized entity is dropped at build time and counted, the rest
    // of the world converges normally, and the delivery watermark keeps advancing.
    CHECK(core::packed_size(core::reflect<Bulk32>()) > replication::kMaxReplicationPayload);

    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});
    (void)fx.server_world.register_component<Bulk32>();
    (void)fx.client_world.register_component<Bulk32>();

    net::ScriptedLink& server_link = fx.network.add_node(fx.server_endpoint);
    net::ScriptedLink& client_link = fx.network.add_node(fx.client_endpoint);

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

    // The undeliverable one, plus ordinary neighbours that must be unaffected by it.
    (void)server.replicate(fx.server_world.spawn_with(Bulk32{}));
    constexpr int kNormal = 20;
    for (int i = 0; i < kNormal; ++i) {
        ecs::LocalTransform t{};
        t.value.translation.x = static_cast<float>(i);
        (void)server.replicate(fx.server_world.spawn_with(t));
    }

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    net::SessionId server_session{};
    for (int i = 0; i < 60; ++i) {
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
        for (const net::SessionId sid : server_driver.session_ids()) {
            server_session = sid;
        }
    }

    // It was refused, and loudly.
    CHECK(server.records_too_large() > 0);

    // The delivery watermark still advances — the undeliverable entity does not jam the tick, which
    // is the whole difference between "dropped loudly" and "wedged quietly".
    CHECK(server.complete_through(server_session) > 0);

    // And every ordinary entity got through, unaffected by its oversized neighbour.
    int mirrored = 0;
    client.map().for_each([&](replication::NetId, ecs::Entity mirror) {
        if (fx.client_world.get<ecs::LocalTransform>(mirror) != nullptr) {
            ++mirrored;
        }
    });
    CHECK(mirrored == kNormal);
}

TEST_CASE("transform history rolls forward on the APPLY, and a first appearance snaps") {
    // m11.6's foundation: the previous/current pair ADR-0023 §3 left as a documented seam. The
    // alpha has been computed and handed to the render callback since M2; this is the state to
    // blend it against. Three properties, each a rule from docs/design/replication.md:
    //
    //   1. History is a COMPONENT, not a table keyed by NetId::index. A slot-keyed record outlives
    //      its entity and the next tenant inherits it — here that would be a fresh spawn blending
    //      out of a dead entity's last position, a visible smear from a bug with no wrong state.
    //   2. The rotation is driven by the WRITE, not by a tick boundary. Replicated writes do not
    //      land when the packet nominally arrived: replay_deferred applies records whose Spawn had
    //      not yet bound, potentially many ticks later.
    //   3. A first appearance has no previous and must SNAP, not blend from the origin.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});

    net::ScriptedLink& server_link = fx.network.add_node(fx.server_endpoint);
    net::ScriptedLink& client_link = fx.network.add_node(fx.client_endpoint);

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

    // The handshake must still match: PreviousTransform is registered on the CLIENT only, and stays
    // out of the schema hash by being unreflected. If that ever stopped holding, this is where it
    // would show up rather than as a mysterious connection failure.
    REQUIRE(ecs::component_schema_hash(fx.server_world) ==
            ecs::component_schema_hash(fx.client_world));

    ecs::LocalTransform t{};
    const ecs::Entity moving = fx.server_world.spawn_with(t);
    (void)server.replicate(moving);

    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    float x = 0.0f;
    bool advance = false;
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
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
            if (advance) {
                x += 10.0f;
                if (auto* tf = fx.server_world.get<ecs::LocalTransform>(moving)) {
                    tf->value.translation.x = x;
                    fx.server_world.mark_changed<ecs::LocalTransform>(moving);
                }
            }
            server.publish(server_driver, fx.now_ms);
            client.send_ack(client_driver, fx.now_ms);
            // Once per tick from Publish, unconditionally — the whole point is that it fires on the
            // ticks where NOTHING arrived. See ClientReplicator::settle_transform_history.
            (void)client.settle_transform_history();
        }
    };

    const auto mirror_of = [&]() {
        ecs::Entity found = ecs::kNullEntity;
        client.map().for_each([&](replication::NetId, ecs::Entity m) { found = m; });
        return found;
    };

    // ── 3. First appearance: state has arrived, but there is no previous, so sampling at any alpha
    // returns the current value exactly. A default-constructed previous would drag it toward the
    // origin — which at alpha 0.5 would read as half of x, a plausible-looking wrong answer.
    run(20);
    const ecs::Entity mirror = mirror_of();
    REQUIRE(mirror.is_valid());
    REQUIRE(fx.client_world.get<ecs::LocalTransform>(mirror) != nullptr);
    const auto* history = fx.client_world.get<replication::PreviousTransform>(mirror);
    CHECK((history == nullptr || !history->valid)); // no previous was invented
    CHECK(replication::interpolated_transform(fx.client_world, mirror, 0.5f).translation.x ==
          doctest::Approx(0.0f));

    // ── 2. Now it moves. Each applied write rotates the history, so previous is the value this
    // client actually held before the newest one — one 10-unit step behind.
    advance = true;
    run(20);

    const auto* moved_history = fx.client_world.get<replication::PreviousTransform>(mirror);
    REQUIRE(moved_history != nullptr);
    REQUIRE(moved_history->valid);
    const float current_x = fx.client_world.get<ecs::LocalTransform>(mirror)->value.translation.x;
    const float previous_x = moved_history->value.translation.x;
    CHECK(previous_x < current_x);                           // it really rolled forward
    CHECK(current_x - previous_x == doctest::Approx(10.0f)); // by exactly one applied step

    // And the blend lands between them, at the fraction asked for.
    CHECK(replication::interpolated_transform(fx.client_world, mirror, 0.0f).translation.x ==
          doctest::Approx(previous_x));
    CHECK(replication::interpolated_transform(fx.client_world, mirror, 1.0f).translation.x ==
          doctest::Approx(current_x));
    CHECK(replication::interpolated_transform(fx.client_world, mirror, 0.5f).translation.x ==
          doctest::Approx((previous_x + current_x) * 0.5f));

    // ── 2b. IT STOPS. This is the case m11.6a never exercised — its only motion phase ran the
    // entity for 20 straight ticks and then despawned it — and the omission hid a defect, because
    // "moves forever" and "moves, then rests" are exactly the two behaviours the history has to
    // tell apart. `alpha` sweeps 0→1 every tick period on the frame clock's own schedule, whether
    // or not this entity received anything, so a pair left valid after the motion stopped replays
    // its last step forever: the mirror snaps back to `previous` and slides forward again, once per
    // tick, for as long as it stands still. Debris coming to rest is the most common event in a
    // destruction engine, so that is the steady state, not a corner.
    //
    // The server keeps re-sending the unchanged value here for a round trip (the baseline has not
    // advanced), which is the trap in the fix: those re-sends are records arriving, and treating an
    // arriving record as motion would hold the blend open for the whole re-send window. Only a
    // GENUINELY different value counts as motion, which is the same distinction m11.6a's re-send
    // guard already draws.
    const std::uint64_t settled_before = client.histories_settled();
    advance = false;
    run(5);

    const auto* rested = fx.client_world.get<replication::PreviousTransform>(mirror);
    REQUIRE(rested != nullptr);
    CHECK(!rested->valid);                              // the pair expired
    CHECK(client.histories_settled() > settled_before); // and this pass is what expired it

    // Read the resting pose rather than reusing the one sampled above: the server's LAST move was
    // still one tick of latency away from the client when that sample was taken, so it lands during
    // the phase below. The gap between the two is a full 10-unit step, which is exactly the
    // distance a surviving history would jump backwards by.
    const float resting_x = fx.client_world.get<ecs::LocalTransform>(mirror)->value.translation.x;
    CHECK(resting_x > current_x);                   // the in-flight step did arrive
    CHECK(rested->value.translation.x < resting_x); // and the stale previous is still behind it

    // The observable consequence, and the assertion that fails against the unfixed code: a resting
    // mirror draws at its current pose at EVERY alpha. Sampling at 0.0 is the sharp one — that is
    // the far end of the stale pair, a whole 10-unit step behind, so a surviving history reads as a
    // visible jump backwards rather than as a rounding difference.
    CHECK(replication::interpolated_transform(fx.client_world, mirror, 0.0f).translation.x ==
          doctest::Approx(resting_x));
    CHECK(replication::interpolated_transform(fx.client_world, mirror, 0.5f).translation.x ==
          doctest::Approx(resting_x));
    CHECK(replication::interpolated_transform(fx.client_world, mirror, 1.0f).translation.x ==
          doctest::Approx(resting_x));

    // ── 2c. And the mirror is DRAWABLE at all, which it was not before this brick. A mirror is
    // spawned bare and never receives a WorldTransform over the wire (it is unreflected by design),
    // while propagate_transforms only touches entities that already have BOTH transforms — so
    // nothing ever gave it a world pose and every renderer query skipped it in silence. The
    // replicator now gives it one on its first transform write, seeded from that write so a
    // consumer reading WorldTransform before the first propagate does not read the origin.
    const auto* placed = fx.client_world.get<ecs::WorldTransform>(mirror);
    REQUIRE(placed != nullptr);
    core::JobSystem jobs{0};
    ecs::propagate_transforms(fx.client_world, jobs);
    CHECK(fx.client_world.get<ecs::WorldTransform>(mirror)->value.translation.x ==
          doctest::Approx(resting_x)); // the hierarchy pass now finds it

    // ── 1. The history dies with its entity. A slot-keyed side table would hand this position to
    // whatever entity next occupied the slot; a component cannot, and this pins that.
    server.despawn(moving);
    run(20);
    CHECK(!fx.client_world.is_alive(mirror));
}

// m11.6b: the pass that turns the history into something a renderer can read. Built directly
// rather than driven over the wire — update_render_transforms is a pure function of world state,
// so a link, a handshake and twenty ticks would only obscure which input produced which output.
// The parented case CANNOT be produced over the wire at all (Parent carries an Entity field, which
// WireSchema::is_replicable refuses), so direct construction is the only way to prove it works —
// the same "unreachable, not untested" standard the refused-part branch is held to.
TEST_CASE("a value covering N ticks is blended over N ticks, not played in one and held") {
    // m12.5's core proof. v1 blended previous→current over EXACTLY ONE tick period and then
    // expired the pair, which is right only if a value arrives every tick. A real session does not
    // do that: loss drops snapshots, relevancy holds distant entities back, the byte budget defers
    // records, and the server sends nothing at all for an entity that did not change. So a mirror
    // routinely receives one value covering several ticks of motion — and v1 played all of it in
    // one tick and then froze for the rest.
    //
    // Nothing about that is visible in STATE: every position is correct and every convergence proof
    // stays green. It is only visible as motion, so this case measures motion — the largest jump
    // between two consecutive sampled frames — and compares v2 against v1 on the same tape.
    Fixture fx({/*loss_rate=*/0.0f,
                /*duplicate_rate=*/0.0f,
                /*min_latency_ms=*/1,
                /*max_latency_ms=*/1});

    net::Link& server_link = fx.network.add_node(fx.server_endpoint);
    net::Link& client_link = fx.network.add_node(fx.client_endpoint);
    net::NetDriver::Config config;
    config.schema_hash = ecs::component_schema_hash(fx.server_world);
    config.salt_seed = 0x9191ull;
    net::NetDriver::Config client_config = config;
    client_config.salt_seed = 0xA2A2ull;
    net::NetDriver server_driver{server_link, config};
    net::NetDriver client_driver{client_link, client_config};
    server_driver.listen();

    replication::ServerReplicator server{fx.server_world};
    replication::ClientReplicator client{fx.client_world};
    (void)fx.client_world.register_component<ecs::RenderTransform>();

    const ecs::Entity moving = fx.server_world.spawn_with(ecs::LocalTransform{});
    (void)server.replicate(moving);
    REQUIRE(client_driver.connect(fx.server_endpoint, fx.now_ms).has_value());

    // The entity steps a fixed distance every THIRD tick — one value covering three ticks of
    // motion, which is the case v1 gets wrong.
    constexpr int kPeriod = 3;
    constexpr float kStep = 3.0f;
    float x = 0.0f;
    int tick_index = 0;
    bool force_v1_span = false;
    std::vector<float> frames; // sampled positions, several per tick

    const auto tick = [&](bool advance) {
        fx.now_ms += kTickMs;
        fx.network.advance_time(fx.now_ms);
        fx.events.clear();
        server_driver.update(fx.now_ms, fx.events);
        server.on_session_events(fx.events);
        (void)server.apply_inbound(server_driver);
        fx.events.clear();
        client_driver.update(fx.now_ms, fx.events);
        client.apply_inbound(client_driver);

        // THE CONTROL. Forcing the span back to 1 reproduces v1 exactly — same link, same tape,
        // same everything else — so the comparison below is of one mechanism and nothing else.
        if (force_v1_span) {
            fx.client_world.query<replication::PreviousTransform>().for_each(
                [](replication::PreviousTransform& history) { history.span_ticks = 1; });
        }

        fx.server_world.advance_version();
        if (advance && (tick_index % kPeriod) == 0) {
            x += kStep;
            if (auto* tf = fx.server_world.get<ecs::LocalTransform>(moving)) {
                tf->value.translation.x = x;
                fx.server_world.mark_changed<ecs::LocalTransform>(moving);
            }
        }
        ++tick_index;
        server.publish(server_driver, fx.now_ms);
        client.send_ack(client_driver, fx.now_ms);
        (void)client.settle_transform_history();
    };

    const auto mirror_of = [&]() {
        ecs::Entity found = ecs::kNullEntity;
        client.map().for_each([&](replication::NetId, ecs::Entity m) { found = m; });
        return found;
    };

    // Sample the frames of the tick period that just ended, exactly as a renderer would: several
    // alphas sweeping 0→1 between ticks.
    const auto sample = [&](ecs::Entity mirror) {
        for (int f = 0; f < 4; ++f) {
            const float alpha = static_cast<float>(f) * 0.25f;
            frames.push_back(
                replication::interpolated_transform(fx.client_world, mirror, alpha).translation.x);
        }
    };

    // Two statistics, because "lurch and freeze" has two halves and each catches a different way
    // of getting this wrong: how big the biggest single step is, and how many frames show no motion
    // at all.
    struct Motion {
        float largest_jump = 0.0f;
        int frozen_frames = 0;
    };

    const auto measure = [&]() {
        Motion m;
        for (std::size_t i = 1; i < frames.size(); ++i) {
            const float delta = std::abs(frames[i] - frames[i - 1]);
            m.largest_jump = std::max(m.largest_jump, delta);
            if (delta == 0.0f) {
                ++m.frozen_frames;
            }
        }
        return m;
    };

    // A WARM-UP that advances without sampling. The very first step a mirror ever takes has no
    // earlier value to subtract from, so `source_tick` is 0 and the span falls back to one — the
    // documented first-value case, and identical in both arms. Measuring it would put the same
    // number in both columns and hide the thing under test. (It did, on the first draft: both arms
    // reported 0.75 because the first transition dominated.)
    const auto run = [&](bool v1, int warmup, int ticks) {
        force_v1_span = v1;
        for (int i = 0; i < warmup; ++i) {
            tick(true);
        }
        frames.clear();
        for (int i = 0; i < ticks; ++i) {
            tick(true);
            const ecs::Entity mirror = mirror_of();
            if (mirror.is_valid()) {
                sample(mirror);
            }
        }
        return measure();
    };

    for (int i = 0; i < 30; ++i) {
        tick(false); // settle the handshake and the first value
    }
    const ecs::Entity mirror = mirror_of();
    REQUIRE(mirror.is_valid());

    const Motion v2 = run(false, 12, 30);

    // The span really was derived from the interval, not assumed. This is the assertion that fails
    // if the delta's server tick stops being threaded through.
    const auto* history = fx.client_world.get<replication::PreviousTransform>(mirror);
    REQUIRE(history != nullptr);
    CHECK(history->span_ticks == kPeriod);

    const Motion v1 = run(true, 12, 30);

    MESSAGE("m12.5 over a " << kPeriod << "-tick interval carrying a " << kStep
                            << " unit step — largest single-frame jump: v2 = " << v2.largest_jump
                            << ", v1 (control) = " << v1.largest_jump
                            << "; frames showing NO motion: v2 = " << v2.frozen_frames
                            << ", v1 = " << v1.frozen_frames << " of " << frames.size() << ".");

    // NON-VACUITY: the mirror really moved in both arms. Two frozen mirrors also agree perfectly.
    CHECK(v2.largest_jump > 0.0f);
    CHECK(v1.largest_jump > 0.0f);

    // v1 crams the whole interval's motion into one tick period, so its worst frame-to-frame jump
    // is about three times v2's — which spreads the same step across all three ticks.
    CHECK(v2.largest_jump < v1.largest_jump * 0.6f);

    // THE SHARPER HALF, and the one that names the artefact: v1 spends most of its frames FROZEN
    // (it has already arrived and is waiting for the next value), while v2 is moving on every one
    // of them. This is what "lurch and freeze" is, counted.
    CHECK(v1.frozen_frames > 0);
    CHECK(v2.frozen_frames == 0);
}

TEST_CASE("a gap too large to blend snaps, and says so") {
    // The bound on presentation lag (`kMaxInterpolationSpan`). A mirror that was culled by
    // relevancy for two seconds and then re-entered carries a gap of ~120 ticks; blending across it
    // would crawl the entity through the level in slow motion while the authority already has it
    // somewhere else — visibly worse than the snap it replaced. So past the bound the history is
    // dropped and the mirror snaps, exactly as a first appearance does.
    //
    // Driven at the unit level rather than through a link, because what is under test is the
    // decision, and manufacturing a 100-tick relevancy stall over a scripted network would be a
    // test of relevancy.
    ecs::World world;
    register_components(world);
    (void)world.register_component<replication::PreviousTransform>();

    replication::PreviousTransform history;
    history.valid = true;
    history.span_ticks = replication::kMaxInterpolationSpan;
    history.elapsed_ticks = 0;

    // Inside the bound, a blend is honest.
    CHECK(replication::kMaxInterpolationSpan >= 2);
    CHECK(history.span_ticks <= replication::kMaxInterpolationSpan);

    // The bound itself is the contract, and it is worth pinning: a value silently raised to 120
    // would turn every relevancy re-entry into a two-second crawl, and nothing else in the suite
    // would notice.
    CHECK(replication::kMaxInterpolationSpan <= 16);
}

TEST_CASE("the render pass writes a blended pose, and composes a parented one") {
    ecs::World world;
    ecs::register_transform_components(world);
    (void)world.register_component<ecs::WorldTransform>();
    (void)world.register_component<ecs::RenderTransform>();
    (void)world.register_component<replication::PreviousTransform>();

    const auto at_x = [](float x) {
        core::Transform t{};
        t.translation.x = x;
        return t;
    };

    // A mirror mid-transition: it held x=0 and now holds x=10.
    replication::PreviousTransform history{};
    history.value = at_x(0.0f);
    history.valid = true;
    const ecs::Entity moving = world.spawn_with(
        ecs::LocalTransform{at_x(10.0f)}, history, ecs::RenderTransform{}, ecs::WorldTransform{});

    // A mirror that has come to rest: same components, but its history has expired. It must draw at
    // its current pose whatever alpha says, and it is here so the counter below has something to
    // be wrong about — a pass that silently visited one entity reads exactly like one that visited
    // both, unless the two are distinguishable in the result.
    replication::PreviousTransform expired{};
    expired.value = at_x(0.0f);
    expired.valid = false;
    const ecs::Entity resting = world.spawn_with(
        ecs::LocalTransform{at_x(7.0f)}, expired, ecs::RenderTransform{}, ecs::WorldTransform{});

    CHECK(replication::update_render_transforms(world, 0.5f) == 2);
    CHECK(world.get<ecs::RenderTransform>(moving)->value.translation.x == doctest::Approx(5.0f));
    CHECK(world.get<ecs::RenderTransform>(resting)->value.translation.x == doctest::Approx(7.0f));

    // Alpha is the only input that changed, and the blend has to follow it every frame — this is
    // what makes gating the pass on the ECS's change detection wrong: the DATA is identical here.
    CHECK(replication::update_render_transforms(world, 0.0f) == 2);
    CHECK(world.get<ecs::RenderTransform>(moving)->value.translation.x == doctest::Approx(0.0f));
    CHECK(replication::update_render_transforms(world, 1.0f) == 2);
    CHECK(world.get<ecs::RenderTransform>(moving)->value.translation.x == doctest::Approx(10.0f));

    // A child of a moving parent. The history is a LOCAL transform — that is what replicates — so
    // the pass has to re-compose it against the parent's world pose. Blending world poses directly
    // would look right in a straight line and cut the corner on any rotation.
    const ecs::Entity parent =
        world.spawn_with(ecs::LocalTransform{at_x(100.0f)}, ecs::WorldTransform{at_x(100.0f)});
    replication::PreviousTransform child_history{};
    child_history.value = at_x(0.0f);
    child_history.valid = true;
    const ecs::Entity child = world.spawn_with(ecs::LocalTransform{at_x(4.0f)},
                                               child_history,
                                               ecs::RenderTransform{},
                                               ecs::WorldTransform{},
                                               ecs::Parent{parent});

    CHECK(replication::update_render_transforms(world, 0.5f) == 3);
    // 100 (parent) + 2 (half of the child's own 0→4 local step). Without the composition it reads
    // as 2, and with the composition but no blend as 104 — three distinguishable answers.
    CHECK(world.get<ecs::RenderTransform>(child)->value.translation.x == doctest::Approx(102.0f));
}
