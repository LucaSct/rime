// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "rime/core/jobs/job_system.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/destruction/components.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/schema_hash.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/components.hpp"
#include "rime/gameplay/weapon.hpp"
#include "rime/gameplay_net/components.hpp"
#include "rime/gameplay_net/gameplay_client.hpp"
#include "rime/gameplay_net/gameplay_server.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/physics/physics.hpp"
#include "rime/replication/client_replicator.hpp"
#include "rime/replication/input.hpp"
#include "rime/replication/server_replicator.hpp"

// Shared scaffolding for the m12.3 networked-player proofs: one authoritative server and N clients
// on a ScriptedNetwork over a virtual clock.
//
// The harness discipline is m11.1's through m11.6c's, unchanged and worth restating because every
// assertion in this suite leans on it: loss and latency are INPUTS rather than environment luck,
// every wait is a bounded tick loop so a failure is a deadline instead of a hang, and every proof
// asserts its own non-vacuousness — a loss test that dropped nothing proves nothing.
//
// WHAT THE TICK ORDER IS, AND WHY IT IS SPELLED OUT RATHER THAN WRAPPED. `Match::tick` runs the
// canonical order from docs/design/simulation-tick.md plus ADR-0033 A5's PreSim/Publish split, with
// gameplay_net's consume loop occupying step 2. It is written out longhand instead of hidden behind
// a helper because the ORDER is the thing under test in half these cases — consume before
// propagate before push_in before step, and publish after everything the tick will mutate has
// mutated. A helper would make the one property a reader most needs to check the hardest to see.
namespace rime_test {

using namespace rime;

inline constexpr std::uint64_t kTickMs = 16;
inline constexpr float kDt = 1.0f / 60.0f;
inline constexpr std::uint16_t kServerPort = 7901;
inline constexpr std::uint16_t kClientPortBase = 7902;

// Both peers must register the SAME components in the same way: the schema hash is part of the
// connection handshake, so a mismatch is refused with a diagnostic rather than silently
// desynchronised. Keeping it one function is what makes that hard to get wrong.
//
// Destruction is OPT-IN rather than always registered, and the reason is not tidiness: only the
// glue proof links `rime::destruction`, and the whole architectural claim of ADR-0035 §3 is that
// `gameplay_net` does not need it. A fixture that registered destruction components for every case
// would make the suite's own dependency graph disagree with the module's.
inline void register_all(ecs::World& world, bool with_destruction) {
    ecs::register_transform_components(world);
    physics::register_physics_components(world);
    gameplay::register_gameplay_components(world);
    gameplay_net::register_gameplay_net_components(world);
    if (with_destruction) {
        destruction::register_destruction_components(world);
    }
}

// ── Geometry helpers, mirroring tests/gameplay's fixture ──────────────────────────────────────

[[nodiscard]] inline physics::ShapeDesc box_shape(core::Vec3 half) {
    physics::ShapeDesc s;
    s.type = physics::ShapeType::Box;
    s.half_extents = half;
    return s;
}

inline physics::BodyId add_static_box(physics::PhysicsWorld& w, core::Vec3 half, core::Vec3 pos) {
    physics::BodyDesc d;
    d.motion = physics::MotionType::Static;
    d.shape = box_shape(half);
    d.position = pos;
    return w.create_body(d);
}

// A floor whose TOP SURFACE is exactly y = 0. Ten metres of half-extent, which is the measured
// ceiling on trustworthy GJK overlap answers (tests/gameplay/character_fixture.hpp documents the
// table); a larger floor would swallow the capsule and report nothing.
inline physics::BodyId add_ground(physics::PhysicsWorld& w, float extent = 10.0f) {
    return add_static_box(w, {extent, 0.5f, extent}, {0.0f, -0.5f, 0.0f});
}

// Where a capsule of `cfg` comes to rest on flat ground: half-height + radius + the 1.5-skin
// contact clearance the controller's inflated probe settles at.
[[nodiscard]] inline float rest_y(const gameplay::CharacterConfig& cfg, float surface_y = 0.0f) {
    return surface_y + cfg.half_height + cfg.radius + 1.5f * cfg.skin;
}

// ── The client half ───────────────────────────────────────────────────────────────────────────

struct ClientPeer {
    ecs::World world;
    net::Link* link = nullptr;
    std::unique_ptr<net::NetDriver> driver;
    std::unique_ptr<replication::ClientReplicator> replicator;
    replication::ClientInputSender sender;
    gameplay_net::GameplayClient gameplay;
    net::Endpoint endpoint{};

    // The last command this client built, so a test can restate held levels without re-deriving.
    replication::InputCommand last{};

    // Build and send one command. Returns the sequence it was stamped with.
    //
    // `record` is called once per tick from PreSim (after sampling, before the local tick runs), so
    // the sequence advances at the SIMULATION's cadence rather than the renderer's — the contract
    // ClientInputSender documents and the one every latency number here is measured in.
    std::uint32_t send_input(replication::InputCommand command, std::uint64_t now_ms) {
        last = sender.record(command);
        sender.send(*driver, now_ms);
        return last.sequence;
    }

    [[nodiscard]] ecs::Entity local_player() const {
        return gameplay.local_player(replicator->map());
    }

    [[nodiscard]] const gameplay::CharacterState* mirrored_state() const {
        const ecs::Entity player = local_player();
        return player.is_valid() ? world.get<gameplay::CharacterState>(player) : nullptr;
    }

    [[nodiscard]] std::uint32_t last_processed() const {
        return gameplay.last_processed_input(world, replicator->map());
    }
};

// ── The whole match ───────────────────────────────────────────────────────────────────────────

struct Match {
    net::ScriptedNetwork network;

    ecs::World world;
    physics::PhysicsWorld physics;
    physics::PhysicsSync sync;
    // Zero workers: `propagate_transforms` wants a JobSystem, and a single-threaded one keeps every
    // proof here deterministic without asserting anything about the scheduler. The parallel path is
    // proven where it belongs (tests/ecs), not incidentally by a netcode suite.
    core::JobSystem jobs{0};

    net::Link* server_link = nullptr;
    std::unique_ptr<net::NetDriver> server_driver;
    std::unique_ptr<replication::ServerReplicator> replicator;
    replication::ServerInputReceiver input;
    gameplay_net::GameplayServer gameplay;

    std::vector<std::unique_ptr<ClientPeer>> clients;

    net::Endpoint server_endpoint{0x7F000001u, kServerPort};
    std::uint64_t now_ms = 0;
    std::uint64_t tick_index = 0;

    // Scratch, reused so a long run does not spend its time in the allocator.
    std::vector<net::SessionEvent> events;
    std::vector<net::Received> inbox;

    // What the game's spawn callback produced, newest last — so a test can name the avatar the
    // server bound to a session without reaching into the registry's privates.
    std::vector<ecs::Entity> spawned;

    // The character every avatar is minted with. A test may change it before connecting.
    gameplay::CharacterConfig character_config{};
    gameplay::WeaponConfig weapon_config{};
    bool give_weapons = true;
    core::Vec3 spawn_point{0.0f, 0.0f, 0.0f};

    // Knobs that must be settled BEFORE the constructor computes a schema hash, so they cannot be
    // a plain member a test assigns afterwards.
    struct Options {
        bool with_destruction = false;
    };

    Options options{};

    explicit Match(net::ScriptedNetwork::Config config = {}, std::uint64_t seed = 0xC0FFEEull)
        : Match(Options{}, config, seed) {}

    explicit Match(Options opts,
                   net::ScriptedNetwork::Config config = {},
                   std::uint64_t seed = 0xC0FFEEull)
        : network(seed, config), options(opts) {
        register_all(world, options.with_destruction);
        server_link = &network.add_node(server_endpoint);

        net::NetDriver::Config driver_config;
        driver_config.app_id = 0x52494D45u; // 'RIME'
        driver_config.schema_hash = ecs::component_schema_hash(world);
        driver_config.salt_seed = 0x1111ull;
        server_driver = std::make_unique<net::NetDriver>(*server_link, driver_config);
        server_driver->listen();

        replicator = std::make_unique<replication::ServerReplicator>(world);
    }

    // Add a client and start its handshake. Call before `settle()`.
    ClientPeer& add_client() {
        auto peer = std::make_unique<ClientPeer>();
        register_all(peer->world, options.with_destruction);
        peer->endpoint = {0x7F000001u,
                          static_cast<std::uint16_t>(kClientPortBase + clients.size())};
        peer->link = &network.add_node(peer->endpoint);

        net::NetDriver::Config config;
        config.app_id = 0x52494D45u;
        config.schema_hash = ecs::component_schema_hash(peer->world);
        config.salt_seed = 0x2222ull + clients.size();
        peer->driver = std::make_unique<net::NetDriver>(*peer->link, config);
        peer->replicator = std::make_unique<replication::ClientReplicator>(peer->world);

        clients.push_back(std::move(peer));
        ClientPeer& added = *clients.back();
        (void)added.driver->connect(server_endpoint, now_ms);
        return added;
    }

    // The game's spawn policy — an avatar with everything the consume loop needs, opted into
    // replication. This is the callback ADR-0035 keeps OUT of the engine: which prefab, which
    // spawn point, which config, and whether to replicate at all are all decided right here.
    ecs::Entity spawn_avatar(net::SessionId) {
        core::Transform placement;
        placement.translation = spawn_point;
        // ONE SPAWN POINT PER PLAYER, spaced along +X. Not a nicety: two capsules minted at the
        // same coordinates start deeply overlapping, and the controller's depenetration recovery
        // then correctly shoves them apart — so the second player's avatar drifts on ticks nobody
        // sent it input for, and every "did MY input move MY avatar" assertion reads a position
        // that two mechanisms are writing. (Caught by the two-client case, which measured an
        // avatar moving +Z while its owner walked -Z.) The gap is a capsule diameter plus slack.
        placement.translation.x +=
            static_cast<float>(spawned.size()) * (4.0f * character_config.radius);
        placement.translation.y = rest_y(character_config, spawn_point.y);

        physics::RigidBody body;
        body.motion = static_cast<std::uint32_t>(physics::MotionType::Kinematic);
        physics::Collider collider;
        collider.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Capsule);
        collider.radius = character_config.radius;
        collider.half_height = character_config.half_height;

        gameplay::CharacterState state;
        state.position = placement.translation;

        const ecs::Entity entity = world.spawn_with(ecs::LocalTransform{placement},
                                                    ecs::WorldTransform{placement},
                                                    body,
                                                    collider,
                                                    character_config,
                                                    state);
        if (give_weapons) {
            (void)world.add_component(entity, weapon_config);
            (void)world.add_component(entity, gameplay::WeaponState{});
        }
        (void)replicator->replicate(entity);
        spawned.push_back(entity);
        return entity;
    }

    // ONE tick of the whole system, in the canonical order. See the file header on why this is
    // longhand.
    void tick() {
        now_ms += kTickMs;
        ++tick_index;
        network.advance_time(now_ms);

        // ── PreSim: server ──
        events.clear();
        server_driver->update(now_ms, events);
        replicator->on_session_events(events);
        input.on_session_events(events);
        gameplay.on_session_events(
            events,
            [this](net::SessionId id) { return spawn_avatar(id); },
            [this](net::SessionId, ecs::Entity player) { replicator->despawn(player); });
        // ONE drain per session, fanned out — drain_received MOVES messages, so a second drain
        // finds an empty inbox and whichever reader went second silently never gets its mail.
        for (const net::SessionId id : server_driver->session_ids()) {
            net::Session* session = server_driver->session(id);
            if (session == nullptr) {
                continue;
            }
            inbox.clear();
            (void)session->drain_received(inbox);
            (void)replicator->apply_messages(id, inbox);
            (void)input.apply_messages(id, inbox);
        }

        // ── PreSim: clients ──
        for (auto& client : clients) {
            events.clear();
            client->driver->update(now_ms, events);
            for (const net::SessionId id : client->driver->session_ids()) {
                net::Session* session = client->driver->session(id);
                if (session == nullptr) {
                    continue;
                }
                inbox.clear();
                (void)session->drain_received(inbox);
                (void)client->replicator->apply_messages(inbox);
                client->sender.apply_messages(inbox);
                (void)client->gameplay.apply_messages(inbox);
            }
        }

        // ── The simulation (server only: m12.3 has no client-side prediction) ──
        world.advance_version();
        gameplay.consume(world, physics, input, kDt); // step 2: gameplay
        ecs::propagate_transforms(world, jobs);       // step 3
        sync.reconcile(world, physics);               // step 4
        sync.push_in(world, physics, kDt);            // step 4b (m12.1)
        physics.step(kDt);                            // step 5
        sync.write_back(world, physics);              // step 6

        // ── Publish ──
        replicator->publish(*server_driver, now_ms);                 // structure first…
        gameplay.publish(*server_driver, replicator->map(), now_ms); // …then "which one is you"
        input.send_acks(*server_driver, now_ms);
        for (auto& client : clients) {
            client->replicator->send_ack(*client->driver, now_ms);
        }
    }

    // Run ticks until every client is connected AND has been told which avatar is its own, or fail
    // on a deadline. Bounded, so a broken handshake is a failing test rather than a hung one.
    void settle(int max_ticks = 200) {
        for (int i = 0; i < max_ticks; ++i) {
            bool ready = server_driver->session_count() == clients.size();
            for (const auto& client : clients) {
                ready = ready && client->local_player().is_valid();
            }
            if (ready) {
                return;
            }
            tick();
        }
    }

    [[nodiscard]] const gameplay::CharacterState* server_state(ecs::Entity player) const {
        return world.get<gameplay::CharacterState>(player);
    }
};

// ── Input helpers ─────────────────────────────────────────────────────────────────────────────
// The mover's basis: at yaw = 0, +move_x is world +X and +move_y is world -Z.

[[nodiscard]] inline replication::InputCommand walk(float move_x, float move_y, float yaw = 0.0f) {
    replication::InputCommand c;
    c.move_x = move_x;
    c.move_y = move_y;
    c.yaw = yaw;
    return c;
}

[[nodiscard]] inline replication::InputCommand still() {
    return replication::InputCommand{};
}

} // namespace rime_test
