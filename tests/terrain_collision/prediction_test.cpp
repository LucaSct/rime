// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "rime/gameplay/character.hpp"
#include "rime/gameplay_net/convert.hpp"
#include "rime/gameplay_net/predictor.hpp"
#include "rime/replication/input.hpp"
#include "terrain_fixture.hpp"

// m19.8c proof (g): THE CLIENT NEVER PREDICTS OVER GROUND IT DOES NOT HAVE.
//
// A client runs the same terrain module over its own physics world and feeds its own loader. Its
// disk may be slower than the server's — and prediction is `step_character` run against the
// CLIENT's world, so predicting across a tile that has not arrived would have the local avatar
// walk off the edge of the world and fall, then be yanked back when the authority disagreed.
//
// So the client asks the same question the server does: its predicted body (and the authoritative
// state a reconcile would replay from) are DEMANDS, and a barrier that answers `Waiting` blocks
// prediction and replay for that tick. The input is still sent — the server, which has the ground,
// keeps simulating the player — and when the tile lands, reconciliation puts the client where the
// authority says it is.
//
// The fixture is deliberately minimal (the spec's word): the real `Predictor` and the real
// `step_character`, two physics worlds, two terrain modules, and a fixed-delay queue in place of a
// wire. No transport, no replication — those are the next brick's.
using namespace rime;
using namespace rime_test;

namespace {

constexpr int kDelayTicks = 4; // each way

struct Peer {
    Peer(FakeTileSource::Options so, const tc::Config& config)
        : source(std::move(so)), terrain(config, source, world) {
        (void)gameplay::validate(character);
    }

    // The box the capsule can touch this tick: its own extent, a tick of travel at full speed in
    // any direction, and the controller's probes (ground snap, step-up) with slack.
    [[nodiscard]] physics::Aabb reach(core::Vec3 position) const {
        const core::Vec3 half{
            character.radius, character.half_height + character.radius, character.radius};
        return tc::swept_bounds(
            box_around(position, half), {0.0f, 0.0f, 0.0f}, kDt, character.max_speed * kDt + 0.5f);
    }

    void place_body(core::Vec3 position) {
        if (!body.is_valid()) {
            physics::BodyDesc d;
            d.motion = physics::MotionType::Kinematic;
            d.shape = gameplay::character_shape(character);
            d.position = position;
            body = world.create_body(d);
            return;
        }
        physics::BodyState s;
        if (world.get_body_state(body, s)) {
            s.position = position;
            (void)world.set_body_state(body, s);
        }
    }

    FakeTileSource source;
    physics::PhysicsWorld world;
    tc::TerrainCollision terrain;
    gameplay::CharacterConfig character{};
    physics::BodyId body{};
    std::uint64_t tick = 0;
};

struct Snapshot {
    std::uint64_t arrives = 0; // global tick
    gameplay::CharacterState state{};
    std::uint32_t q = 0; // LastProcessedInput
};

struct Session {
    Session(const FakeTileSource::Options& server_source,
            const FakeTileSource::Options& client_source)
        : server(server_source, Sim::default_config()),
          client(client_source, Sim::default_config()) {
        gameplay_net::Predictor::Config pc;
        // Zero tolerance and no smoothing: any disagreement with the authority is corrected, so
        // "the two sides end bit-identical" is a statement about reconciliation and not about a
        // 2 cm gate happening to be wide enough.
        pc.position_tolerance = 0.0f;
        pc.velocity_tolerance = 0.0f;
        pc.smoothing_decay = 0.0f;
        predictor.set_config(pc);
    }

    // Put the avatar into both worlds through admission, and let it land.
    [[nodiscard]] bool spawn(core::Vec3 position) {
        const auto admit = [&](Peer& peer) {
            if (peer.terrain.request_admission(1, peer.reach(position)) !=
                tc::AdmissionResult::Deferred) {
                return false;
            }
            for (int i = 0; i < 64; ++i) {
                peer.source.pump();
                const tc::Plan plan = peer.terrain.plan(peer.tick++, {});
                if (peer.terrain.try_commit(plan) != tc::CommitStatus::Ready) {
                    return false;
                }
                if (!peer.terrain.admitted().empty()) {
                    peer.place_body(position);
                    return true;
                }
            }
            return false;
        };
        if (!admit(server) || !admit(client)) {
            return false;
        }
        authority.position = position;
        predictor.reset(authority);
        // The spawn state is the first thing a client is told. Leaving the mirror at the origin
        // would make "where a correction would rewind to" a demand on tiles nobody is near.
        mirror.state = authority;
        return true;
    }

    // One global tick: the client samples and sends an input and (if its ground allows) predicts;
    // the server consumes whatever input has arrived.
    void tick(const gameplay::CharacterInput& input) {
        // ── Client ────────────────────────────────────────────────────────────────────────────
        replication::InputCommand command;
        command.sequence = ++sequence;
        command.move_x = input.move_x;
        command.move_y = input.move_y;
        command.yaw = input.yaw;
        // Sent regardless of whether the client can predict it: the player pressed the key.
        uplink.emplace_back(now + kDelayTicks, command);

        while (!downlink.empty() && downlink.front().arrives <= now) {
            mirror = downlink.front();
            downlink.pop_front();
        }

        client.source.pump();
        // Demands: where the prediction is, and where a correction would rewind to. Both, because
        // a reconcile replays `step_character` from the authoritative state.
        const std::vector<tc::Demand> demands{
            {client.reach(predictor.state().position), tc::DemandKind::Body},
            {client.reach(mirror.state.position), tc::DemandKind::Body}};
        const tc::Plan plan = client.terrain.plan(client.tick++, demands);
        const tc::CommitStatus status = client.terrain.try_commit(plan);
        if (status == tc::CommitStatus::Ready) {
            if (mirror.q != 0) {
                (void)predictor.reconcile(
                    mirror.state, mirror.q, client.character, client.world, client.body, kDt);
            }
            // The belt on top of the braces: the barrier said Ready, so this must hold.
            if (!client.terrain.require_coverage(client.reach(predictor.state().position))) {
                ++predicted_uncovered;
            }
            (void)predictor.predict(command, client.character, client.world, client.body, kDt);
            client.place_body(predictor.state().position);
            ++predicted_ticks;
        } else {
            // Blocked: no reconcile, no replay, no predict. The avatar stays where it was last
            // legitimately predicted; the network and the loader carry on.
            ++prediction_blocked;
        }

        // ── Server ────────────────────────────────────────────────────────────────────────────
        server.source.pump();
        const std::vector<tc::Demand> server_demands{
            {server.reach(authority.position), tc::DemandKind::Body}};
        const tc::Plan server_plan = server.terrain.plan(server.tick++, server_demands);
        tc::CommitStatus server_status = server.terrain.try_commit(server_plan);
        while (server_status == tc::CommitStatus::Waiting) {
            server.source.pump();
            server_status = server.terrain.try_commit(server_plan);
        }
        server_ok = server_ok && server_status == tc::CommitStatus::Ready;
        while (!uplink.empty() && uplink.front().first <= now) {
            const replication::InputCommand& arrived = uplink.front().second;
            authority = gameplay::step_character(authority,
                                                 gameplay_net::to_character_input(arrived),
                                                 server.character,
                                                 server.world,
                                                 server.body,
                                                 kDt);
            processed = arrived.sequence;
            uplink.pop_front();
        }
        server.place_body(authority.position);
        downlink.push_back({now + kDelayTicks, authority, processed});
        ++now;
    }

    Peer server;
    Peer client;
    gameplay_net::Predictor predictor;
    gameplay::CharacterState authority{};
    Snapshot mirror{};
    std::deque<std::pair<std::uint64_t, replication::InputCommand>> uplink;
    std::deque<Snapshot> downlink;
    std::uint64_t now = 0;
    std::uint32_t sequence = 0;
    std::uint32_t processed = 0;
    std::uint64_t prediction_blocked = 0;
    std::uint64_t predicted_ticks = 0;
    std::uint64_t predicted_uncovered = 0;
    bool server_ok = true;
};

// Exact, component-wise: the claim is bit-identity, so no tolerance is offered.
[[nodiscard]] bool identical(core::Vec3 a, core::Vec3 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

[[nodiscard]] gameplay::CharacterInput forward() {
    gameplay::CharacterInput in;
    in.move_y = 1.0f; // +move_y at yaw 0 is world -Z
    return in;
}

[[nodiscard]] FakeTileSource::Options field() {
    FakeTileSource::Options so;
    so.min_x = -4;
    so.max_x = 4;
    so.min_z = -8;
    so.max_z = 4;
    so.latency = 2;
    return so;
}

} // namespace

TEST_CASE("m19.8c (g): the client does not predict over a tile it has not installed, and "
          "reconciles to the authority once it has") {
    // The client's loader is slow for exactly one row of tiles: z in [-32, -16).
    FakeTileSource::Options slow = field();
    Session session(field(), slow);
    for (std::int32_t x = slow.min_x; x <= slow.max_x; ++x) {
        session.client.source.set_latency(x, -2, 420);
    }
    const float standing = session.client.character.half_height + session.client.character.radius;
    REQUIRE(session.spawn({8.0f, standing + 0.2f, 8.0f}));

    // Settle, then walk north (-Z) for five seconds: 30 m, across z = 0 and z = -16.
    for (int i = 0; i < 60; ++i) {
        session.tick({});
    }
    REQUIRE(session.authority.grounded);
    REQUIRE(session.predictor.state().grounded);
    REQUIRE(session.prediction_blocked == 0);
    // The controller really is standing on the streamed heightfield, at ground level.
    CHECK(session.authority.position.y > standing - 0.01f);
    CHECK(session.authority.position.y < standing + 0.1f);

    const std::uint64_t blocked_tile_stalls_before =
        session.client.terrain.counters().stalls_admitted_body;
    float northmost_predicted_while_missing = 1.0e9f;
    std::uint32_t newest_when_block_began = 0;
    for (int i = 0; i < 300; ++i) {
        const std::uint64_t blocked_before = session.prediction_blocked;
        const core::Vec3 before = session.predictor.state().position;
        session.tick(forward());
        if (session.prediction_blocked != blocked_before) {
            if (newest_when_block_began == 0) {
                newest_when_block_began = session.predictor.newest_sequence();
            }
            // A blocked tick changed NOTHING in the prediction: not the pose, not the ring.
            CHECK(identical(session.predictor.state().position, before));
            CHECK(session.predictor.newest_sequence() == newest_when_block_began);
        }
        if (session.client.terrain.tile_state({0, -2, 1}) != tc::TileState::Installed) {
            northmost_predicted_while_missing =
                std::min(northmost_predicted_while_missing, session.predictor.state().position.z);
        }
    }

    // The server had the ground and walked the player onto the slow row…
    CHECK(session.server_ok);
    CHECK(session.authority.position.z < -18.0f);
    CHECK(session.server.terrain.counters().stalls_admitted_body == 0);
    // …the client did not have it, and stopped SHORT of it: its capsule never crossed z = -16
    // while the tile was missing, and the blocked counters — the fixture's and the module's — both
    // moved.
    CHECK(session.client.terrain.tile_state({0, -2, 1}) != tc::TileState::Installed);
    CHECK(session.prediction_blocked > 30);
    CHECK(session.client.terrain.counters().stalls_admitted_body - blocked_tile_stalls_before ==
          session.prediction_blocked);
    CHECK(northmost_predicted_while_missing - session.client.character.radius > -16.0f);
    CHECK(session.predictor.state().position.z > session.authority.position.z + 3.0f);
    // It was not falling, either: still at standing height, on ground that exists.
    CHECK(session.predictor.state().position.y > standing - 0.01f);
    CHECK(session.predicted_uncovered == 0);

    // Stand still until the slow row arrives, and a while longer.
    const std::uint64_t corrections_before = session.predictor.corrections();
    const std::uint64_t blocked_at_stop = session.prediction_blocked;
    for (int i = 0; i < 500; ++i) {
        session.tick({});
    }
    REQUIRE(session.client.terrain.tile_state({0, -2, 1}) == tc::TileState::Installed);
    CHECK(session.prediction_blocked > blocked_at_stop); // it stayed blocked until the tile came
    const std::uint64_t blocked_final = session.prediction_blocked;
    for (int i = 0; i < 30; ++i) {
        session.tick({});
    }
    CHECK(session.prediction_blocked == blocked_final); // …and not after

    // Reconciled: a correction fired, and the two sides now agree BIT FOR BIT — same function,
    // same inputs, same ground under both.
    CHECK(session.predictor.corrections() > corrections_before);
    CHECK(identical(session.predictor.state().position, session.authority.position));
    CHECK(identical(session.predictor.state().velocity, session.authority.velocity));
    CHECK(session.predictor.state().grounded == session.authority.grounded);
    CHECK(session.authority.grounded);
    CHECK(session.predicted_uncovered == 0);
    CHECK(session.client.terrain.counters().coverage_refusals == 0);
}

TEST_CASE("m19.8c (g): the control — a client with a fast loader is never blocked") {
    Session session(field(), field());
    const float standing = session.client.character.half_height + session.client.character.radius;
    REQUIRE(session.spawn({8.0f, standing + 0.2f, 8.0f}));
    for (int i = 0; i < 60; ++i) {
        session.tick({});
    }
    for (int i = 0; i < 300; ++i) {
        session.tick(forward());
    }
    for (int i = 0; i < 60; ++i) {
        session.tick({});
    }
    CHECK(session.prediction_blocked == 0);
    CHECK(session.client.terrain.counters().stalls_admitted_body == 0);
    CHECK(session.predictor.state().position.z < -18.0f); // it walked the same thirty metres
    CHECK(identical(session.predictor.state().position, session.authority.position));
    CHECK(identical(session.predictor.state().velocity, session.authority.velocity));
}
