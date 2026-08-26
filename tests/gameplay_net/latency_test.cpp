// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "match_fixture.hpp"

// m12.3's HEADLINE PROOF, and the reason this brick exists as its own step on the ladder:
//
//     with no prediction, a player's own input takes a full round trip to become visible.
//
// ADR-0035 §1 makes "own-input response" a falsifiable clause — "≤ 1 tick, against a
// prediction-off control showing ≥ RTT ticks, so prediction is provably THE REASON". This file is
// that control, measured now, before the thing it controls for exists. m12.4 must beat it, and a
// number recorded after the fact is a number chosen to be beaten.
//
// HOW LATENCY IS MEASURED WITH NO CLOCK. There is no clock synchronisation anywhere in this
// codebase and M12 deliberately does not add one (ADR-0033 A11, ADR-0035 §4). So the measurement
// is offset-free by construction: count the client's OWN ticks between stamping sequence S onto a
// command and seeing a mirrored `LastProcessedInput >= S` arrive on its own avatar. Both endpoints
// are events on one machine's clock, so no offset can enter — the same trick ADR-0030 §5 uses for
// the remote-view latency number.
using namespace rime;
using namespace rime_test;

namespace {

// One measurement: send a distinguishable command, then tick until the client's own mirror
// reports the server acted on it. Bounded, so a broken path is a deadline rather than a hang.
[[nodiscard]] int ticks_until_own_input_lands(Match& match, ClientPeer& client, int max_ticks) {
    const std::uint32_t sequence = client.send_input(walk(0.0f, 1.0f), match.now_ms);
    for (int elapsed = 1; elapsed <= max_ticks; ++elapsed) {
        match.tick();
        if (client.last_processed() >= sequence) {
            return elapsed;
        }
        // Keep the intent alive so the avatar behaves like a walking player rather than one who
        // taps forward once. Held state is a LEVEL and restating it is what a real client does
        // every tick; only the sequence being watched is the one measured.
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
    }
    return -1;
}

} // namespace

TEST_CASE("own-input latency is the round trip, in ticks — the number m12.4 must beat") {
    // A deliberately unambiguous link: a FIXED one-way latency, no loss, no jitter. Fixed because
    // the claim is about the mechanism rather than about a distribution — with jitter the answer
    // would be a histogram and the control would be arguing with itself.
    //
    // 48 ms each way at a 16 ms tick is 3 ticks out and 3 back. The command must travel to the
    // server, be consumed, have the resulting state published, and travel back: a full round trip
    // plus the tick boundaries either end.
    constexpr std::uint64_t kOneWayMs = 48;
    constexpr int kOneWayTicks = static_cast<int>(kOneWayMs / kTickMs); // 3
    constexpr int kRoundTripTicks = 2 * kOneWayTicks;                   // 6

    Match match({/*loss_rate=*/0.0f,
                 /*duplicate_rate=*/0.0f,
                 /*min_latency_ms=*/kOneWayMs,
                 /*max_latency_ms=*/kOneWayMs});
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle(400);
    REQUIRE(client.local_player().is_valid());

    // Several measurements, because one could be a boundary artefact of where the sample happened
    // to land inside a tick.
    std::vector<int> samples;
    for (int i = 0; i < 8; ++i) {
        const int measured = ticks_until_own_input_lands(match, client, 60);
        REQUIRE(measured > 0);
        samples.push_back(measured);
    }
    const int best = *std::min_element(samples.begin(), samples.end());
    const int worst = *std::max_element(samples.begin(), samples.end());

    MESSAGE("m12.3 own-input latency (prediction OFF): best "
            << best << " ticks, worst " << worst << " ticks, at " << kOneWayMs << " ms one-way ("
            << kRoundTripTicks << " round-trip ticks). This is the number m12.4"
            << " must reduce to <= 1.");

    // THE ASSERTION THAT MATTERS: it is at least the round trip. Not "roughly RTT" — at least,
    // because no mechanism in this brick can beat the speed of the link, and a measurement that
    // came in UNDER the round trip would mean the number is measuring something other than what it
    // claims (a stale read, a cached sequence, a mirror written locally).
    CHECK(best >= kRoundTripTicks);

    // And it is not wildly worse than the round trip either: the loop adds tick-boundary quantum,
    // not a multiplier. A bound of RTT + 4 catches a regression that starts costing whole extra
    // round trips (a publish that misses its tick, an ack that waits for a redundancy window).
    CHECK(worst <= kRoundTripTicks + 4);

    // The negative control on the control: a LOCAL link measures far less, so the number above is
    // the network and not the harness. Same code, same assertions, one input changed.
    Match local;
    add_ground(local.physics);
    ClientPeer& local_client = local.add_client();
    local.settle();
    const int local_ticks = ticks_until_own_input_lands(local, local_client, 60);
    REQUIRE(local_ticks > 0);
    MESSAGE("…on a zero-latency link the same measurement is " << local_ticks << " ticks.");
    CHECK(local_ticks < best);
}

TEST_CASE("what the client SEES lags what the server HOLDS by that latency") {
    // The same fact stated as a distance rather than as a count, because that is what a player
    // experiences: with prediction off, the avatar under your hand is drawn where it was a round
    // trip ago. This is the artefact m12.4 removes, and it is worth measuring in metres so the
    // improvement can be too.
    constexpr std::uint64_t kOneWayMs = 48;
    Match match({0.0f, 0.0f, kOneWayMs, kOneWayMs});
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle(400);
    const ecs::Entity avatar = match.spawned.front();

    for (int i = 0; i < 60; ++i) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
    }

    const gameplay::CharacterState* authoritative = match.server_state(avatar);
    const gameplay::CharacterState* mirrored = client.mirrored_state();
    REQUIRE(authoritative != nullptr);
    REQUIRE(mirrored != nullptr);

    const float lag = core::length(authoritative->position - mirrored->position);
    MESSAGE("m12.3 visible position lag (prediction OFF): "
            << lag << " m while walking at " << match.character_config.max_speed << " m/s.");
    // It is behind — not level. A zero here would mean the mirror is being written from something
    // other than the wire.
    CHECK(lag > 0.0f);
    // …and behind by about a round trip of walking, not by a random amount. Six ticks at 6 m/s is
    // 0.6 m; the bound is generous on both sides because the exact tick a delta lands on shifts.
    CHECK(lag < match.character_config.max_speed * kDt * 12.0f);
    // The client is strictly BEHIND along the direction of travel (forward is -Z), never ahead: it
    // is following, and a mirror that overshot would mean something local is integrating.
    CHECK(mirrored->position.z > authoritative->position.z);
}

TEST_CASE("at quiescence the client's mirror equals the server's state, bit for bit") {
    // Convergence, and the reason the pairing is worth having at all: once the world stops
    // changing, "what I am looking at" and "what is true" must be the same bytes. Approximate
    // equality would pass on a mirror that had drifted, which is precisely the failure a snapshot
    // system is built to prevent.
    Match match({0.20f, 0.0f, 8, 40}); // lossy AND jittery, so convergence is earned
    add_ground(match.physics);
    ClientPeer& client = match.add_client();
    match.settle(400);
    const ecs::Entity avatar = match.spawned.front();

    std::uint32_t last_sequence = 0;
    for (int i = 0; i < 120; ++i) {
        last_sequence = client.send_input(walk(0.6f, 0.8f, 0.4f), match.now_ms);
        match.tick();
    }
    // Then stop moving and let the link settle. The re-offer machinery (baselines, the completeness
    // watermark) is what has to close the gap; nothing here helps it.
    for (int i = 0; i < 120; ++i) {
        match.tick();
    }

    CHECK(match.network.packets_dropped() > 0); // non-vacuity: the link really misbehaved

    const gameplay::CharacterState* authoritative = match.server_state(avatar);
    const gameplay::CharacterState* mirrored = client.mirrored_state();
    REQUIRE(authoritative != nullptr);
    REQUIRE(mirrored != nullptr);
    CHECK(mirrored->position.x == authoritative->position.x);
    CHECK(mirrored->position.y == authoritative->position.y);
    CHECK(mirrored->position.z == authoritative->position.z);
    CHECK(mirrored->velocity.x == authoritative->velocity.x);
    CHECK(mirrored->velocity.y == authoritative->velocity.y);
    CHECK(mirrored->velocity.z == authoritative->velocity.z);
    CHECK(mirrored->grounded == authoritative->grounded);

    // The pairing converged too, and it names a command that really was sent. Under loss the
    // frontier steps over gaps, so it may be short of the last sequence — but it must be close,
    // and it must never exceed it (that would be the server acting on a command nobody sent).
    const std::uint32_t pairing = client.last_processed();
    CHECK(pairing <= last_sequence);
    CHECK(pairing + 20 >= last_sequence);
}

TEST_CASE("each client is told which avatar is its own, and they are not the same one") {
    // Two clients, and the assignment message earning its keep. Every player entity carries a
    // `LastProcessedInput` and every client's sequence numbering starts at 1, so two players moving
    // in step are INDISTINGUISHABLE in the snapshot: nothing but the session knows who is who, and
    // the session is server-side only.
    Match match;
    add_ground(match.physics);
    ClientPeer& first = match.add_client();
    ClientPeer& second = match.add_client();
    match.settle(400);

    REQUIRE(match.server_driver->session_count() == 2);
    REQUIRE(match.spawned.size() == 2);
    CHECK(match.gameplay.players_spawned() == 2);
    CHECK(match.gameplay.assignments_sent() == 2);

    const ecs::Entity mine = first.local_player();
    const ecs::Entity theirs = second.local_player();
    REQUIRE(mine.is_valid());
    REQUIRE(theirs.is_valid());
    // Different LOCAL entities in different worlds is not the interesting claim — each client
    // mirrors both avatars, so the claim is that the two clients were pointed at DIFFERENT NetIds.
    CHECK(first.gameplay.local_player_id() != second.gameplay.local_player_id());

    // …and each NetId is the one the server bound to that client's session. This is the assertion
    // that would fail if the assignment were sent to the wrong session — a mistake that is
    // invisible with one client, which is why this case has two.
    const auto& registry = match.gameplay.players();
    for (std::size_t i = 0; i < match.clients.size(); ++i) {
        const ecs::Entity avatar = match.spawned[i];
        const std::optional<net::SessionId> session = registry.session_for(avatar);
        REQUIRE(session.has_value());
        const auto net_id = match.replicator->map().net_id_of(avatar);
        REQUIRE(net_id.has_value());
        // Find the client whose driver holds that session… by elimination: this harness assigns
        // sessions in connection order, and `spawned` is appended in the same order.
        CHECK(match.clients[i]->gameplay.local_player_id() == *net_id);
    }

    // Each client drives only its own avatar. Move the first and the second's must not budge.
    const core::Vec3 before = match.server_state(match.spawned[1])->position;
    for (int i = 0; i < 20; ++i) {
        (void)first.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
    }
    CHECK(match.server_state(match.spawned[0])->position.z < before.z - 0.5f);
    CHECK(match.server_state(match.spawned[1])->position.z == before.z);
    // The second client was silent for twenty ticks, so its avatar starved for every one of them.
    CHECK(match.gameplay.ticks_starved() >= 20);
}
