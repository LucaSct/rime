// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "match_fixture.hpp"
#include "rime/gameplay_net/predictor.hpp"

// m12.4's proofs: client-side prediction and reconciliation — the hardest brick in M12.
//
// EVERY CASE HERE CARRIES ITS OWN NEGATIVE CONTROL, because ADR-0035 §4 asks for exactly that:
// "prediction-off shows ≥ RTT ticks of latency, reconciliation-off diverges, and a lossy run with
// ZERO corrections fails — because that would prove the comparison is dead rather than that the
// network was kind." A prediction test without a prediction-off arm is a test of nothing: the
// client would look responsive because it is drawing a number it computed, whether or not that
// number has anything to do with the server.
//
// The controls are cheap here because the mechanism is one boolean on the fixture (`predict`), so
// the two arms share their seed, their geometry, their link and their input tape, and differ in
// exactly one thing.
using namespace rime;
using namespace rime_test;

namespace {

constexpr std::uint64_t kOneWayMs = 48;                                    // 3 ticks each way
constexpr int kRoundTripTicks = static_cast<int>(2 * kOneWayMs / kTickMs); // 6

// A match on a fixed-latency link with the level standing on both sides.
[[nodiscard]] std::unique_ptr<Match>
fixed_latency_match(bool predict, float loss = 0.0f, std::uint64_t seed = 0xC0FFEEull) {
    auto match = std::make_unique<Match>(
        net::ScriptedNetwork::Config{loss, 0.0f, kOneWayMs, kOneWayMs}, seed);
    ClientPeer& client = match->add_client();
    client.predict = predict;
    match->stand_level();
    match->settle(400);
    return match;
}

// Walk forward, one command per tick, and return how many of the client's OWN ticks pass before
// its own avatar — as the client would DRAW it — has visibly responded.
//
// "Visibly responded" is measured as movement of the drawn pose, not as a sequence number coming
// back, because that is what a player actually perceives. The prediction-off arm therefore has to
// wait for the round trip; the prediction-on arm should not have to wait at all.
[[nodiscard]] int ticks_until_visible_response(Match& match, ClientPeer& client, int max_ticks) {
    const core::Vec3 before = client.visible_state().position;
    for (int elapsed = 1; elapsed <= max_ticks; ++elapsed) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
        const core::Vec3 now = client.visible_state().position;
        if (core::length(now - before) > 1e-4f) {
            return elapsed;
        }
    }
    return -1;
}

// Let a lossy run come to rest, and KEEP SENDING INPUT while it does.
//
// This is not padding, it is the difference between a converging run and one that hangs a tick
// short forever. `consumed_through` steps over a permanently lost command only when a LATER command
// arrives to move the frontier past it (input.hpp). So if the last thing a client ever sends is
// dropped, the server's frontier stops just before it, the client keeps that command in its ring
// as a legitimate un-acknowledged prediction, and the two sides sit exactly one tick of travel
// apart — for as long as nobody says anything else. (Measured: 0.1 m at 6 m/s, which is 6 m/s x
// one tick, on the nose.)
//
// A real client does exactly this: a player standing still is still sending commands. And a `still`
// command predicted from a settled state produces the identical state, so a dropped one at the very
// end costs nothing.
inline void settle_with_input(Match& match, ClientPeer& client, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        (void)client.send_input(still(), match.now_ms);
        match.tick();
    }
}

} // namespace

TEST_CASE(
    "prediction makes your own input visible in one tick — and it is prediction that does it") {
    // THE HEADLINE, and ADR-0035 §1's "own-input response" clause made falsifiable: "≤ 1 tick,
    // against a prediction-off control showing ≥ RTT ticks, so prediction is provably the reason."
    //
    // Both arms run the same tape over the same 48 ms link. The only difference is one boolean.
    auto predicting = fixed_latency_match(/*predict=*/true);
    auto following = fixed_latency_match(/*predict=*/false);

    const int with = ticks_until_visible_response(*predicting, *predicting->clients[0], 60);
    const int without = ticks_until_visible_response(*following, *following->clients[0], 60);

    REQUIRE(with > 0);
    REQUIRE(without > 0);
    MESSAGE("m12.4 own-input response at "
            << kOneWayMs << " ms one-way: prediction ON = " << with << " tick(s), prediction OFF = "
            << without << " tick(s) (round trip is " << kRoundTripTicks << " ticks).");

    // The clause, asserted.
    CHECK(with <= 1);
    // The control, asserted separately — so a regression that broke prediction AND the control
    // together could not pass by making both numbers equal.
    CHECK(without >= kRoundTripTicks);
    CHECK(with < without);
}

TEST_CASE("the predicted avatar tracks the authority instead of running away from it") {
    // Responsiveness is worthless if it is fiction. With prediction on, the drawn pose must stay
    // close to the authoritative one at all times — not merely agree at the end — because a
    // prediction that drifts and then snaps is exactly what a player experiences as rubber-banding.
    auto match = fixed_latency_match(/*predict=*/true);
    ClientPeer& client = *match->clients[0];
    const ecs::Entity avatar = match->spawned.front();

    float worst = 0.0f;
    for (int i = 0; i < 120; ++i) {
        (void)client.send_input(walk(0.4f, 0.9f, 0.3f), match->now_ms);
        match->tick();
        const gameplay::CharacterState* authoritative = match->server_state(avatar);
        REQUIRE(authoritative != nullptr);
        worst = std::max(worst,
                         core::length(client.visible_state().position - authoritative->position));
    }

    MESSAGE("m12.4 worst prediction-vs-authority distance while walking: " << worst << " m.");
    // The prediction runs AHEAD of the authority by up to a round trip of travel — that is the
    // whole point — so the bound is "a round trip of walking", not "zero". At 6 m/s and 6 ticks
    // that is ~0.6 m; the allowance is generous because the exact tick a delta lands on shifts.
    CHECK(worst < match->character_config.max_speed * kDt * 12.0f);
    // Non-vacuity: it really was ahead at some point. A zero here would mean the "prediction" is
    // just the mirror under another name.
    CHECK(worst > 0.0f);
    CHECK(client.predictor.seeded());
    CHECK(client.predictor.newest_sequence() > 100);
}

TEST_CASE("at quiescence the prediction equals the authority BIT FOR BIT") {
    // ADR-0035 §4: "same binary, same function, same inputs, so exact equality is the assertion and
    // epsilons are confined to the mid-flight tolerance gate." An approximate assertion here would
    // pass on a predictor that had genuinely drifted and merely stayed inside its own tolerance —
    // which is the one failure this whole design has to rule out.
    auto match = fixed_latency_match(/*predict=*/true);
    ClientPeer& client = *match->clients[0];
    const ecs::Entity avatar = match->spawned.front();

    for (int i = 0; i < 120; ++i) {
        (void)client.send_input(walk(0.7f, 0.7f, 0.5f), match->now_ms);
        match->tick();
    }
    // Then let everything settle — still sending input, because a player standing still is still
    // sending commands and because of the frontier property `settle_with_input` documents. The
    // server stops the avatar (one step per command, none on a starved tick — m12.3), the last
    // snapshots arrive, and the predictor converges onto them.
    settle_with_input(*match, client, 60);

    const gameplay::CharacterState* authoritative = match->server_state(avatar);
    REQUIRE(authoritative != nullptr);
    const gameplay::CharacterState predicted = client.visible_state();

    CHECK(predicted.position.x == authoritative->position.x);
    CHECK(predicted.position.y == authoritative->position.y);
    CHECK(predicted.position.z == authoritative->position.z);
    CHECK(predicted.velocity.x == authoritative->velocity.x);
    CHECK(predicted.velocity.y == authoritative->velocity.y);
    CHECK(predicted.velocity.z == authoritative->velocity.z);
    CHECK(predicted.grounded == authoritative->grounded);

    // Non-vacuity: the run has to have gone somewhere, or "identical" is a claim about two
    // untouched default structs.
    CHECK(core::length(authoritative->position) > 1.0f);
    CHECK(client.predictor.reconciles() > 10);
}

TEST_CASE("under real loss the predictor corrects — and a run with zero corrections fails") {
    // ADR-0035 §4's sharpest instruction, and the one most likely to be quietly skipped: "a lossy
    // run with ZERO corrections fails — because that would prove the comparison is dead rather than
    // that the network was kind."
    //
    // So `corrections() > 0` is not a nice-to-have here. It is the assertion that the mechanism is
    // alive at all. Everything else in this file would still pass with `reconcile` hard-wired to
    // return false.
    Match match({/*loss_rate=*/0.25f,
                 /*duplicate_rate=*/0.0f,
                 /*min_latency_ms=*/16,
                 /*max_latency_ms=*/64},
                0xBEEFull);
    ClientPeer& client = match.add_client();
    client.predict = true;
    match.stand_level();
    match.settle(400);
    const ecs::Entity avatar = match.spawned.front();

    for (int i = 0; i < 300; ++i) {
        (void)client.send_input(walk(0.5f, 0.8f, 0.25f), match.now_ms);
        match.tick();
    }

    MESSAGE("m12.4 under 25% loss: "
            << client.predictor.reconciles() << " pairings, " << client.predictor.corrections()
            << " corrections, " << client.predictor.corrections_skipped() << " within tolerance, "
            << client.predictor.commands_replayed() << " commands replayed, "
            << "worst error " << client.predictor.max_correction_distance() << " m.");

    // The link really misbehaved, and the server really saw gaps — without these the correction
    // count below could be zero for an honest reason.
    CHECK(match.network.packets_dropped() > 0);
    CHECK(match.input.gaps_observed() > 0);

    // THE ASSERTION. Zero corrections under this much loss means the comparison never fired.
    CHECK(client.predictor.corrections() > 0);
    CHECK(client.predictor.commands_replayed() > 0);
    // …and the mechanism is not merely thrashing: most pairings still agree, which is what says the
    // prediction is usually right rather than usually replaced.
    CHECK(client.predictor.corrections_skipped() > client.predictor.corrections());
    // The ring reached back far enough every time. Non-zero here would mean corrections were
    // applied without ever being CHECKED, which is a different (and worse) mode.
    CHECK(client.predictor.corrections_unverifiable() == 0);
    CHECK(client.predictor.history_evicted() == 0);

    // Despite all of it, the client still converges on the truth.
    settle_with_input(match, client, 200);
    const gameplay::CharacterState* authoritative = match.server_state(avatar);
    REQUIRE(authoritative != nullptr);
    const gameplay::CharacterState predicted = client.visible_state();
    CHECK(predicted.position.x == authoritative->position.x);
    CHECK(predicted.position.y == authoritative->position.y);
    CHECK(predicted.position.z == authoritative->position.z);
}

TEST_CASE("reconciliation off diverges — the control that makes correction the reason") {
    // The second negative control ADR-0035 §4 asks for. Prediction WITHOUT reconciliation is dead
    // reckoning: the client runs the mover forever on its own guesses and nothing ever pulls it
    // back. Under loss the two must come apart, and visibly.
    //
    // Reconciliation is disabled the honest way — by never calling `reconcile` — rather than by
    // widening the tolerance, because a huge tolerance would still SEED the predictor and still
    // trim its ring, and the arm would be testing something other than "no correction happened".
    const auto run = [](bool reconcile_on) {
        Match match({0.25f, 0.0f, 16, 64}, 0xD00Dull);
        ClientPeer& client = match.add_client();
        client.predict = true;
        match.stand_level();
        match.settle(400);
        const ecs::Entity avatar = match.spawned.front();

        // Seed once so both arms start from the same place; after that the reconcile-off arm is on
        // its own.
        const gameplay::CharacterState* seed = match.world.get<gameplay::CharacterState>(avatar);
        client.predictor.reset(*seed);
        client.predict = reconcile_on;

        for (int i = 0; i < 300; ++i) {
            const replication::InputCommand command = walk(0.5f, 0.8f, 0.25f);
            (void)client.send_input(command, match.now_ms);
            if (!reconcile_on) {
                // Hand-run the prediction half only: no reconcile, ever.
                const physics::BodyId self = client.local_body();
                if (self.is_valid()) {
                    (void)client.predictor.predict(
                        client.last, client.config, client.physics, self, kDt, nullptr);
                }
                client.unpredicted.clear();
            }
            match.tick();
        }
        const gameplay::CharacterState* authoritative = match.server_state(avatar);
        return core::length(client.predictor.state().position - authoritative->position);
    };

    const float with_reconcile = run(true);
    const float without_reconcile = run(false);

    MESSAGE("m12.4 divergence after 300 lossy ticks: reconciliation ON = "
            << with_reconcile << " m, OFF = " << without_reconcile << " m.");

    // With reconciliation the prediction stays within a round trip of the truth; without it, it
    // walks away. The gap is the mechanism, measured.
    //
    // The assertion is RELATIVE (a multiple of the reconciled arm) rather than a fixed metre count,
    // because the absolute divergence depends on how the seed's losses happen to fall and on how
    // far the tape travels — neither of which this case is about. An absolute bound tuned to one
    // run is a test that goes red when someone changes the floor, which is exactly what happened to
    // its first draft.
    CHECK(with_reconcile < 1.0f);
    CHECK(without_reconcile > 4.0f * with_reconcile);
    CHECK(without_reconcile > 0.5f);
}

TEST_CASE("a lost command is excluded from the replay, and both sides agree it never happened") {
    // ADR-0035 §4's first named property. A command every copy of which was dropped is one the
    // server will NEVER act on: `consumed_through` steps over the gap, `q` moves past it, the ring
    // is trimmed past it, and the replay cannot include it. The client's prediction — which DID
    // include it — therefore disagrees at `q`, gets corrected, and ends up agreeing with a server
    // that never saw the input.
    //
    // This is the case a command-list diff would get wrong, which is why ADR-0035 §4 forbids one.
    Match match({0.35f, 0.0f, 16, 48}, 0x5EEDull);
    ClientPeer& client = match.add_client();
    client.predict = true;
    match.stand_level();
    match.settle(500);
    const ecs::Entity avatar = match.spawned.front();

    for (int i = 0; i < 400; ++i) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
    }

    // Gaps really opened: commands whose every redundant copy was lost.
    REQUIRE(match.input.gaps_observed() > 0);
    MESSAGE("m12.4 with " << match.input.gaps_observed() << " permanently lost commands: "
                          << client.predictor.corrections() << " corrections, worst error "
                          << client.predictor.max_correction_distance() << " m.");
    CHECK(client.predictor.corrections() > 0);

    // The server consumed strictly fewer commands than the client sent — that difference IS the
    // lost input, and it is what the two sides have to end up agreeing about.
    CHECK(match.gameplay.commands_consumed() < 400);

    // Let it settle, then demand exact agreement. The client predicted motion that never happened
    // on the server; if the replay had included those commands, the two would differ by exactly
    // that much, forever.
    settle_with_input(match, client, 200);
    const gameplay::CharacterState* authoritative = match.server_state(avatar);
    REQUIRE(authoritative != nullptr);
    const gameplay::CharacterState predicted = client.visible_state();
    CHECK(predicted.position.x == authoritative->position.x);
    CHECK(predicted.position.y == authoritative->position.y);
    CHECK(predicted.position.z == authoritative->position.z);
}

TEST_CASE("the replay is complete even when the ack is fresher than the snapshot") {
    // THE HOLE IN ADR-0035 §4's SKETCH, isolated. §4 says "replay every `unacked()` command with
    // sequence > q". `unacked()` retires on `ClientInputSender::acked_through`, which comes from
    // `InputAck` — a different unreliable superseding stream from the one carrying the snapshot's
    // `q`. So the ack is routinely FRESHER, and a predictor replaying only `unacked()` would skip
    // every command in (q, acked_through]: commands the server consumed and the client predicted.
    //
    // The symptom would be the local avatar sliding backwards a few ticks' worth on every
    // correction — under exactly the conditions prediction exists to smooth over.
    //
    // This case makes the two frontiers come apart on purpose and then demands the prediction not
    // fall behind.
    Match match(
        {/*loss_rate=*/0.30f, // loses snapshots, so `q` lags…
         /*duplicate_rate=*/0.0f,
         /*min_latency_ms=*/16,
         /*max_latency_ms=*/80}, // …and wide jitter reorders the two streams against each other
        0xACE5ull);
    ClientPeer& client = match.add_client();
    client.predict = true;
    match.stand_level();
    match.settle(500);
    const ecs::Entity avatar = match.spawned.front();

    int ticks_ack_ahead = 0;
    for (int i = 0; i < 300; ++i) {
        (void)client.send_input(walk(0.0f, 1.0f), match.now_ms);
        match.tick();
        if (client.sender.acked_through() > client.last_processed()) {
            ++ticks_ack_ahead;
        }
    }

    MESSAGE("m12.4 ack-ahead-of-snapshot on " << ticks_ack_ahead << " of 300 ticks; corrections "
                                              << client.predictor.corrections() << ", replayed "
                                              << client.predictor.commands_replayed() << ".");

    // NON-VACUITY, and it is the whole case: if the two frontiers never came apart, this test
    // would be indistinguishable from the plain lossy one and would prove nothing about the hole.
    CHECK(ticks_ack_ahead > 10);
    CHECK(client.predictor.corrections() > 0);

    // The prediction is AHEAD of the authority (it has run commands the server has not yet), never
    // behind it. Falling behind is precisely what replaying from `unacked()` would cause, and along
    // a straight walk in -Z "behind" is measurable as a larger z.
    const gameplay::CharacterState* authoritative = match.server_state(avatar);
    REQUIRE(authoritative != nullptr);
    CHECK(client.visible_state().position.z <= authoritative->position.z + 1e-3f);

    // And it still converges exactly once the walking stops.
    settle_with_input(match, client, 250);
    const gameplay::CharacterState* settled = match.server_state(avatar);
    const gameplay::CharacterState predicted = client.visible_state();
    CHECK(predicted.position.x == settled->position.x);
    CHECK(predicted.position.y == settled->position.y);
    CHECK(predicted.position.z == settled->position.z);
}

TEST_CASE("the predicted pose is what drives the local capsule, and CharacterState stays the "
          "authority's") {
    // The wiring rule the fixture documents, asserted rather than assumed: the client publishes its
    // PREDICTION to the transforms (so the avatar is drawn and collided where the player sees it)
    // and never to `CharacterState` (which is the authority's word, and the input to the next
    // comparison). Overwriting the latter would make reconciliation compare the prediction against
    // itself and agree forever — a predictor that can never find itself wrong.
    auto match = fixed_latency_match(/*predict=*/true);
    ClientPeer& client = *match->clients[0];

    for (int i = 0; i < 60; ++i) {
        (void)client.send_input(walk(0.0f, 1.0f), match->now_ms);
        match->tick();
    }

    const ecs::Entity mirror = client.local_player();
    REQUIRE(mirror.is_valid());

    // The transform carries the PREDICTION…
    const ecs::LocalTransform* local = client.world.get<ecs::LocalTransform>(mirror);
    REQUIRE(local != nullptr);
    CHECK(local->value.translation.z == doctest::Approx(client.predictor.state().position.z));

    // …and the physics body followed it, so the local capsule is where the player sees themselves.
    const physics::BodyId self = client.local_body();
    REQUIRE(self.is_valid());
    physics::BodyState body{};
    REQUIRE(client.physics.get_body_state(self, body));
    CHECK(body.position.z == doctest::Approx(client.predictor.state().position.z));

    // …while CharacterState still holds the authority's, which is strictly behind it.
    const gameplay::CharacterState* authoritative = client.mirrored_state();
    REQUIRE(authoritative != nullptr);
    CHECK(authoritative->position.z > client.predictor.state().position.z);
}
