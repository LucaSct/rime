// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "character_fixture.hpp"
#include "rime/core/hash.hpp"

// m12.2 proofs: REPLAY DETERMINISM — the property m12.4's client-side prediction is built on, and
// the reason step_character is a pure function rather than a method on a stateful mover.
//
// Prediction means replaying the last N ticks of input from a corrected state and arriving at the
// same place. If the controller remembers ANYTHING between ticks that is not in CharacterState —
// a cached ground normal, a coyote timer, an accumulator — the replay diverges, and it diverges as
// a rare desync in a live session rather than as a failing test. So the claim is made here,
// explicitly, and in the form that would break first.
//
// THE CLAIM IS SAME-BINARY BIT-IDENTITY, AND ONLY THAT. Two runs of this executable agree to the
// bit. Cross-platform bit-identity is a recorded NON-GOAL (ADR-0033, docs/ROADMAP.md) — m12.1
// measured x86-64 and arm64 rounding a shape cast's advance differently — so there is deliberately
// NO committed golden hash here. A golden constant would quietly smuggle in a claim the project
// has already recorded as false; each CI OS asserts the property against itself instead.
using namespace rime_test;

namespace {

// A fingerprint of a CharacterState's raw bytes. FNV-1a over a value-initialised copy, so padding
// is a fixed pattern rather than whatever was on the stack, and "identical" means identical bits
// rather than identical to some tolerance.
[[nodiscard]] std::uint64_t hash_state(const gameplay::CharacterState& s,
                                       std::uint64_t seed = core::kFnv1a64OffsetBasis) {
    const std::array<float, 10> flat = {s.position.x,
                                        s.position.y,
                                        s.position.z,
                                        s.velocity.x,
                                        s.velocity.y,
                                        s.velocity.z,
                                        s.ground_normal.x,
                                        s.ground_normal.y,
                                        s.ground_normal.z,
                                        s.grounded ? 1.0f : 0.0f};
    return core::fnv1a_64(std::as_bytes(std::span<const float>{flat}), seed);
}

// ── The tape ──────────────────────────────────────────────────────────────────────────────────
// A scripted 600-tick run that visits every feature the controller has: a walk, a turn, a jump, a
// wall grind, a stair climb, and a stretch of standing still. The vacuity guards below assert it
// really does contain each of those, because a determinism proof over a tape of zeroes proves
// nothing at all.
constexpr int kTapeTicks = 600;

[[nodiscard]] std::vector<gameplay::CharacterInput> build_tape() {
    std::vector<gameplay::CharacterInput> tape;
    tape.reserve(kTapeTicks);
    for (int t = 0; t < kTapeTicks; ++t) {
        gameplay::CharacterInput in;
        if (t < 80) {
            in = walk(0.0f, 1.0f); // straight at the stairs
        } else if (t < 120) {
            in = walk(0.0f, 1.0f);
            if (t == 90) {
                in.pressed = gameplay::kActionJump; // …and a jump partway
            }
        } else if (t < 200) {
            in = walk(0.7f, 0.7f, 0.6f); // a turn: yaw rotates the basis
        } else if (t < 260) {
            in = idle(); // stand still, and stay put
        } else if (t < 380) {
            in = walk(1.0f, 0.2f, -0.9f); // grind along whatever is to the side
        } else if (t < 420) {
            in = walk(-1.0f, -0.6f);
            if (t == 400) {
                in.pressed = gameplay::kActionJump;
            }
        } else {
            in = walk(-0.4f, -1.0f, 2.1f);
        }
        in.pitch = 0.01f * static_cast<float>(t); // carried, never read — part of the tape's shape
        tape.push_back(in);
    }
    return tape;
}

// A course with something for each of those inputs to run into: a floor, two steps, a wall, and a
// ramp. All inside the size regime the fixture documents.
void build_course(physics::PhysicsWorld& w) {
    (void)add_ground(w);
    (void)add_static_box(w, {6.0f, 0.1f, 2.0f}, {0.0f, 0.1f, -4.0f}); // step 1: top y = 0.2
    (void)add_static_box(w, {6.0f, 0.2f, 2.0f}, {0.0f, 0.2f, -7.5f}); // step 2: top y = 0.4
    (void)add_static_box(w, {0.5f, 4.0f, 8.0f}, {4.0f, 0.0f, 0.0f});  // wall at x = +3.5
    (void)add_ramp(w, 0.45f, {4.0f, 0.4f, 4.0f});                     // a ~26° ramp at the origin
}

// Run the tape and return the per-tick state hash chain plus the final state.
[[nodiscard]] std::uint64_t run_tape(const std::vector<gameplay::CharacterInput>& tape,
                                     gameplay::CharacterState& out_final,
                                     gameplay::StepStats& out_stats,
                                     int from_tick = 0,
                                     const gameplay::CharacterState* initial = nullptr) {
    physics::PhysicsWorld w;
    build_course(w);

    Character ch;
    ch.spawn(w, gameplay::CharacterConfig{}, {0.0f, rest_y(gameplay::CharacterConfig{}), 1.0f});
    if (initial != nullptr) {
        ch.state = *initial;
    }

    std::uint64_t chain = core::kFnv1a64OffsetBasis;
    for (int t = from_tick; t < static_cast<int>(tape.size()); ++t) {
        ch.tick(tape[t]);
        chain = hash_state(ch.state, chain);
    }
    out_final = ch.state;
    out_stats = ch.stats;
    return chain;
}

} // namespace

TEST_CASE("m12.2 replay: the same tape from the same state is bit-identical, twice") {
    const std::vector<gameplay::CharacterInput> tape = build_tape();

    gameplay::CharacterState final_a;
    gameplay::CharacterState final_b;
    gameplay::StepStats stats_a;
    gameplay::StepStats stats_b;
    const std::uint64_t a = run_tape(tape, final_a, stats_a);
    const std::uint64_t b = run_tape(tape, final_b, stats_b);

    // The whole trajectory, not just its end: a chain hash catches a divergence that later
    // converges, which is exactly the shape a prediction bug takes.
    CHECK(a == b);
    CHECK(hash_state(final_a) == hash_state(final_b));

    // Vacuity: a tape that never touched anything would hash identically and prove nothing. These
    // assert the course was actually exercised.
    CHECK(stats_a.casts > 0);
    CHECK(stats_a.snaps > 0);
    CHECK(stats_a.steps_climbed > 0);
    CHECK(stats_a.slide_iterations > kTapeTicks); // more than one iteration on some ticks
    CHECK(finite(final_a.position));
    CHECK(finite(final_a.velocity));

    // …and the counters themselves are a pure function of the run.
    CHECK(stats_a.casts == stats_b.casts);
    CHECK(stats_a.steps_climbed == stats_b.steps_climbed);
    CHECK(stats_a.snaps == stats_b.snaps);
    CHECK(stats_a.phantom_contacts == stats_b.phantom_contacts);
}

TEST_CASE("m12.2 replay: two independently built worlds produce identical trajectories") {
    // The same tape over two SEPARATELY CONSTRUCTED worlds. Each has its own body slots, its own
    // broadphase tree, its own allocations. If any of the controller's answers depended on slot
    // order, tree shape, or an address, this is where it would show — and run_tape already builds
    // a fresh world per call, so the previous case covers the same ground; this one states the
    // property explicitly and adds the interleaved variant, where the two runs are stepped
    // ALTERNATELY so any shared mutable state between them would corrupt both.
    const std::vector<gameplay::CharacterInput> tape = build_tape();

    physics::PhysicsWorld w1;
    physics::PhysicsWorld w2;
    build_course(w1);
    build_course(w2);

    const gameplay::CharacterConfig cfg;
    Character a;
    Character b;
    a.spawn(w1, cfg, {0.0f, rest_y(cfg), 1.0f});
    b.spawn(w2, cfg, {0.0f, rest_y(cfg), 1.0f});

    for (const gameplay::CharacterInput& in : tape) {
        a.tick(in);
        b.tick(in);
        REQUIRE(hash_state(a.state) == hash_state(b.state));
    }
    CHECK(a.stats.casts == b.stats.casts);
    CHECK(a.stats.casts > 0);
}

TEST_CASE("m12.2 replay: resuming from a mid-trajectory state reproduces the rest of it") {
    // THE state-completeness proof, and the one that would catch a hidden field. Run the tape to
    // tick 300, snapshot CharacterState and NOTHING ELSE, then replay from 300 in a fresh world
    // starting from that snapshot. If the controller remembered anything outside the struct, the
    // two tails cannot match — which is exactly the failure m12.4's rewind-and-replay would hit.
    const std::vector<gameplay::CharacterInput> tape = build_tape();
    constexpr int kResumeAt = 300;

    // Run the first half and snapshot.
    physics::PhysicsWorld w;
    build_course(w);
    const gameplay::CharacterConfig cfg;
    Character ch;
    ch.spawn(w, cfg, {0.0f, rest_y(cfg), 1.0f});
    for (int t = 0; t < kResumeAt; ++t) {
        ch.tick(tape[t]);
    }
    const gameplay::CharacterState snapshot = ch.state;

    // Continue in place …
    std::uint64_t continued = core::kFnv1a64OffsetBasis;
    for (int t = kResumeAt; t < kTapeTicks; ++t) {
        ch.tick(tape[t]);
        continued = hash_state(ch.state, continued);
    }

    // … and replay the tail from the snapshot alone, in a world that has never seen tick 0.
    gameplay::CharacterState resumed_final;
    gameplay::StepStats resumed_stats;
    const std::uint64_t resumed =
        run_tape(tape, resumed_final, resumed_stats, kResumeAt, &snapshot);

    CHECK(continued == resumed);
    CHECK(hash_state(ch.state) == hash_state(resumed_final));
    // Vacuity: the tail must actually do something, or "identical" is the identity function.
    CHECK(resumed_stats.casts > 0);
    CHECK(core::length(resumed_final.position - snapshot.position) > 0.5f);
}

TEST_CASE("m12.2 replay: the controller never writes to the world it observes") {
    // Purity, as the physics can see it. The controller takes a const PhysicsWorld and must leave
    // it byte-identical — world_hash() is the engine's own exact-equality witness for that.
    const std::vector<gameplay::CharacterInput> tape = build_tape();

    physics::PhysicsWorld w;
    build_course(w);
    const std::uint64_t before = w.world_hash();

    // Step the controller WITHOUT the fixture's kinematic body sync, so the only thing that could
    // change the world is the controller itself.
    const gameplay::CharacterConfig cfg;
    gameplay::CharacterState state;
    state.position = {0.0f, rest_y(cfg), 1.0f};
    gameplay::StepStats stats;
    for (const gameplay::CharacterInput& in : tape) {
        state = gameplay::step_character(state, in, cfg, w, physics::BodyId{}, kDt, &stats);
    }

    CHECK(w.world_hash() == before);
    CHECK(stats.casts > 0); // vacuity: it really did query the world it left untouched
}
