// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "match_fixture.hpp"
#include "rime/gameplay_net/predictor.hpp"

// m12.5's predicted-player smoothing.
//
// m12.4 made corrections CORRECT. They were also visible: a rewind moves the avatar instantly, and
// a player calls that rubber-banding. Being right about where you are and drawing yourself getting
// there are different jobs, so a correction's displacement is absorbed into a visual offset that
// decays away over a few ticks.
//
// THE LOAD-BEARING CLAIM IS THAT THIS CHANGES NOTHING BUT THE PICTURE. A smoothing layer that fed
// back into the simulation would be a controller with a hidden accumulator — exactly what m12.2's
// purity rule forbids, and it would break replay in a way that only shows up as a rare desync. So
// the second case here is the important one: with smoothing on and off, the SIMULATION's state
// trajectory must be bit-identical, tick for tick, over a lossy run with real corrections in it.
using namespace rime;
using namespace rime_test;

namespace {

// A lossy match whose corrections are frequent enough to measure.
[[nodiscard]] std::unique_ptr<Match> lossy_match(float smoothing_decay, std::uint64_t seed) {
    auto match = std::make_unique<Match>(net::ScriptedNetwork::Config{0.30f, 0.0f, 16, 96}, seed);
    ClientPeer& client = match->add_client();
    client.predict = true;
    gameplay_net::Predictor::Config config;
    config.smoothing_decay = smoothing_decay;
    client.predictor.set_config(config);
    match->stand_level();
    match->settle(500);
    return match;
}

} // namespace

TEST_CASE("a correction slides instead of jumping") {
    // Both arms run the same seed, the same tape and the same link, and differ only in
    // `smoothing_decay`. What is measured is the DRAWN pose — `drawn_position()` — because that is
    // the only thing smoothing is allowed to affect.
    constexpr int kTicks = 300;

    const auto run = [](float decay) {
        auto match = lossy_match(decay, 0x5A0011ull);
        ClientPeer& client = *match->clients[0];
        std::vector<core::Vec3> drawn;
        for (int i = 0; i < kTicks; ++i) {
            (void)client.send_input(walk(0.0f, 1.0f), match->now_ms);
            match->tick();
            drawn.push_back(client.drawn_position());
        }
        float worst = 0.0f;
        for (std::size_t i = 1; i < drawn.size(); ++i) {
            worst = std::max(worst, core::length(drawn[i] - drawn[i - 1]));
        }
        struct Result {
            float worst_step;
            std::uint64_t corrections;
            std::uint64_t smoothed;
            float max_offset;
        };
        return Result{worst,
                      client.predictor.corrections(),
                      client.predictor.corrections_smoothed(),
                      client.predictor.max_smoothing_offset()};
    };

    const auto smoothed = run(0.75f);
    const auto snapping = run(0.0f); // m12.4's behaviour, exactly

    MESSAGE("m12.5 worst single-tick jump in the DRAWN pose over "
            << kTicks << " lossy ticks: smoothing ON = " << smoothed.worst_step
            << " m, OFF (m12.4) = " << snapping.worst_step << " m; " << smoothed.corrections
            << " corrections, " << smoothed.smoothed << " of them slid, largest offset "
            << smoothed.max_offset << " m.");

    // NON-VACUITY: corrections really happened in both arms, and they were the same corrections.
    // Without this the comparison could be of two runs that never had to smooth anything.
    CHECK(smoothed.corrections > 0);
    CHECK(snapping.corrections == smoothed.corrections);
    CHECK(smoothed.smoothed > 0);

    // The drawn pose moves less abruptly with smoothing on. A walking player covers 0.1 m per tick
    // at 6 m/s, so a step much above that is a correction being drawn as a jump.
    CHECK(smoothed.worst_step < snapping.worst_step);
    CHECK(smoothed.max_offset > 0.0f);
}

TEST_CASE("smoothing changes the picture and nothing else — the simulation is bit-identical") {
    // THE CASE THAT MATTERS. A presentation layer that leaked into the simulation would be a
    // hidden accumulator in a function m12.2 proved pure, and the symptom would be a rare desync
    // rather than a failing test. So: two runs, same seed, same tape, same link, differing only in
    // `smoothing_decay` — and the SIMULATION's trajectory must match bit for bit at every tick.
    constexpr int kTicks = 250;

    const auto run = [](float decay) {
        auto match = lossy_match(decay, 0xB17E5ull);
        ClientPeer& client = *match->clients[0];
        std::vector<gameplay::CharacterState> states;
        std::vector<core::Vec3> drawn;
        for (int i = 0; i < kTicks; ++i) {
            (void)client.send_input(walk(0.3f, 0.9f, 0.15f), match->now_ms);
            match->tick();
            states.push_back(client.visible_state()); // the SIMULATION's answer
            drawn.push_back(client.drawn_position()); // what a renderer would draw
        }
        return std::pair{states, drawn};
    };

    const auto [smoothed_states, smoothed_drawn] = run(0.75f);
    const auto [snapped_states, snapped_drawn] = run(0.0f);

    REQUIRE(smoothed_states.size() == snapped_states.size());

    int drawn_differences = 0;
    for (std::size_t i = 0; i < smoothed_states.size(); ++i) {
        // BIT-identical, not approximately equal: an epsilon here would pass on a simulation that
        // had genuinely drifted and merely stayed close.
        CHECK(smoothed_states[i].position.x == snapped_states[i].position.x);
        CHECK(smoothed_states[i].position.y == snapped_states[i].position.y);
        CHECK(smoothed_states[i].position.z == snapped_states[i].position.z);
        CHECK(smoothed_states[i].velocity.x == snapped_states[i].velocity.x);
        CHECK(smoothed_states[i].velocity.z == snapped_states[i].velocity.z);
        CHECK(smoothed_states[i].grounded == snapped_states[i].grounded);

        if (core::length(smoothed_drawn[i] - snapped_drawn[i]) > 0.0f) {
            ++drawn_differences;
        }
    }

    MESSAGE("m12.5 simulation identical across "
            << smoothed_states.size() << " ticks; drawn pose differed on " << drawn_differences
            << " of them.");

    // …and the two arms REALLY DID differ in the picture. Without this the identity above could be
    // the identity of two runs in which smoothing never engaged, which would prove nothing.
    CHECK(drawn_differences > 0);
}

TEST_CASE("a correction too large to hide is shown at once, and counted") {
    // `max_smoothing_distance`. Sliding a player smoothly across two metres of a firefight is worse
    // than moving them at once: for the whole slide they are drawn somewhere they demonstrably are
    // not, and they will shoot from there. Smoothing is for the small, frequent errors.
    //
    // Driven at the unit level, because manufacturing a metre-scale correction over a scripted link
    // would be a test of how badly the link can misbehave rather than of this decision.
    physics::PhysicsWorld world;
    add_tiled_ground(world);

    gameplay_net::Predictor predictor;
    gameplay_net::Predictor::Config config;
    config.smoothing_decay = 0.75f;
    config.max_smoothing_distance = 0.5f;
    predictor.set_config(config);

    gameplay::CharacterConfig character{};
    (void)gameplay::validate(character);

    gameplay::CharacterState start;
    start.position = {0.0f, rest_y(character), 0.0f};
    predictor.reset(start);

    // A small disagreement: absorbed into a slide, so the drawn pose does NOT jump to the truth.
    gameplay::CharacterState nudged = start;
    nudged.position.z -= 0.20f;
    CHECK(predictor.reconcile(nudged, 1, character, world, physics::BodyId{}, kDt));
    CHECK(predictor.corrections_smoothed() == 1);
    CHECK(predictor.corrections_snapped() == 0);
    CHECK(predictor.smoothing_offset() > 0.0f);
    // The simulation moved all the way; the picture did not.
    CHECK(predictor.state().position.z == doctest::Approx(nudged.position.z));
    CHECK(predictor.visual_position().z > predictor.state().position.z);

    // A large one: shown at once. The offset is cleared rather than accumulated, so the drawn pose
    // IS the truth from this tick on.
    gameplay::CharacterState yanked = start;
    yanked.position.z -= 3.0f;
    CHECK(predictor.reconcile(yanked, 2, character, world, physics::BodyId{}, kDt));
    CHECK(predictor.corrections_snapped() == 1);
    CHECK(predictor.smoothing_offset() == 0.0f);
    CHECK(predictor.visual_position().z == doctest::Approx(predictor.state().position.z));
}

TEST_CASE("a slide finishes, rather than decaying forever") {
    // Geometric decay never reaches zero, so without a floor the offset would be a permanently
    // non-zero number and every "the picture agrees with the truth" assertion in the suite would
    // need an epsilon written around it. It ends.
    physics::PhysicsWorld world;
    add_tiled_ground(world);

    gameplay_net::Predictor predictor;
    gameplay::CharacterConfig character{};
    (void)gameplay::validate(character);

    gameplay::CharacterState start;
    start.position = {0.0f, rest_y(character), 0.0f};
    predictor.reset(start);

    gameplay::CharacterState nudged = start;
    nudged.position.z -= 0.15f;
    CHECK(predictor.reconcile(nudged, 1, character, world, physics::BodyId{}, kDt));
    REQUIRE(predictor.smoothing_offset() > 0.0f);

    // Reconcile is called every tick by a real client, including the many that carry nothing new —
    // which is exactly why the decay lives at the top of it, above every early-out. If it lived
    // below them, this loop would never decay anything and the slide would stick forever.
    int ticks = 0;
    while (predictor.smoothing_offset() > 0.0f && ticks < 200) {
        (void)predictor.reconcile(nudged, 1, character, world, physics::BodyId{}, kDt);
        ++ticks;
    }

    MESSAGE("m12.5 a 0.15 m correction finished sliding after " << ticks << " ticks.");
    CHECK(predictor.smoothing_offset() == 0.0f);
    CHECK(ticks > 1);   // it really slid rather than snapping
    CHECK(ticks < 100); // …and it is a slide, not a drift
    // Once finished, the picture and the truth are the same value — no epsilon needed.
    CHECK(predictor.visual_position().z == predictor.state().position.z);
}
