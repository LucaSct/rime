// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "terrain_fixture.hpp"

// m19.8c proof (a): THE OUTCOME DOES NOT DEPEND ON WHICH LOAD FINISHED FIRST.
//
// One scripted session — rolling bodies crossing tile seams over uneven ground, a teleport into
// terrain that is not loaded, a query pin taken and dropped — is run several times. The only thing
// that changes between runs is the order (and, in the second case, the timing) in which the fake
// loader completes its loads. The activation journal, the tick each admission happened on, and the
// physics world hash AFTER EVERY TICK must be identical.
//
// Each case also checks that the runs really were different (the completion logs disagree), because
// a determinism proof whose inputs were accidentally identical proves nothing.
using namespace rime;
using namespace rime_test;

namespace {

struct Outcome {
    std::vector<tc::JournalEntry> journal;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> admission_ticks;
    std::vector<std::uint64_t> hashes;
    std::vector<std::pair<std::uint64_t, tc::TileKey>> completions;
    tc::Counters counters;
    bool completed = false;
};

[[nodiscard]] double hills(double x, double z) {
    return 3.0 + 1.5 * std::sin(x * 0.21) * std::cos(z * 0.17);
}

[[nodiscard]] Outcome
run_session(Order order, std::uint64_t seed, std::uint32_t jitter, std::uint32_t lead_ticks) {
    FakeTileSource::Options so;
    so.min_x = -6;
    so.max_x = 14;
    so.min_z = -4;
    so.max_z = 4;
    so.latency = 3;
    so.jitter = jitter;
    so.order = order;
    so.seed = seed;
    so.height = hills;

    tc::Config config = Sim::default_config();
    config.activation_lead_ticks = lead_ticks;

    Sim sim(so, config);
    const auto ground = [](float x, float z) { return static_cast<float>(hills(x, z)); };

    // Three bodies heading in different directions, so several tiles are requested at once and
    // "the order they completed in" has something to permute.
    (void)sim.spawn_sphere(1, {2.0f, ground(2.0f, 3.0f) + 1.0f, 3.0f}, {7.0f, 0.0f, 1.5f});
    (void)sim.spawn_sphere(2, {30.0f, ground(30.0f, -6.0f) + 1.0f, -6.0f}, {-4.0f, 0.0f, 3.0f});
    (void)sim.spawn_sphere(3, {-20.0f, ground(-20.0f, 20.0f) + 1.0f, 20.0f}, {2.0f, 0.0f, -6.0f});

    Outcome out;
    tc::PinToken query{};
    for (int i = 0; i < 300; ++i) {
        if (i == 10) {
            // A query pin over ground no body is near, at the manifest's revision.
            query = sim.terrain.pin(box_around({-70.0f, 3.0f, 40.0f}, {4.0f, 4.0f, 4.0f}), 1);
        }
        if (i == 60) {
            // Body 2 teleports across the map, onto a z = 0 seam (two tiles under it).
            (void)sim.teleport(2, {170.0f, ground(170.0f, 0.0f) + 1.0f, 0.0f});
        }
        if (i == 150) {
            (void)sim.terrain.unpin(query);
        }
        if (sim.step() != tc::CommitStatus::Ready) {
            return out;
        }
    }
    out.journal.assign(sim.terrain.journal().begin(), sim.terrain.journal().end());
    out.admission_ticks = sim.admission_ticks;
    out.hashes = sim.hashes;
    out.completions = sim.source.completions;
    out.counters = sim.terrain.counters();
    out.completed = true;
    return out;
}

void check_same(const Outcome& a, const Outcome& b) {
    REQUIRE(a.completed);
    REQUIRE(b.completed);
    CHECK(a.journal == b.journal);
    CHECK(a.admission_ticks == b.admission_ticks);
    REQUIRE(a.hashes.size() == b.hashes.size());
    // Per tick, not just the final one: a divergence that later heals is still a divergence.
    std::size_t first_difference = a.hashes.size();
    for (std::size_t i = 0; i < a.hashes.size(); ++i) {
        if (a.hashes[i] != b.hashes[i]) {
            first_difference = i;
            break;
        }
    }
    CHECK(first_difference == a.hashes.size());
    CHECK(a.counters.installs == b.counters.installs);
    CHECK(a.counters.evictions == b.counters.evictions);
    CHECK(a.counters.admitted_after_deferral == b.counters.admitted_after_deferral);
}

} // namespace

TEST_CASE("m19.8c (a): permuted load completions give one journal, one set of admission ticks, "
          "one world hash per tick") {
    const Outcome fifo = run_session(Order::Fifo, 1, 0, 0);
    const Outcome lifo = run_session(Order::Lifo, 1, 0, 0);
    const Outcome shuffled_a = run_session(Order::Shuffled, 0xA11CEull, 0, 0);
    const Outcome shuffled_b = run_session(Order::Shuffled, 0xB0Bull, 0, 0);

    // Vacuity: the session did real work…
    REQUIRE(fifo.completed);
    CHECK(fifo.counters.installs > 20);
    CHECK(fifo.counters.evictions > 0);
    CHECK(fifo.counters.admissions_deferred >= 4); // three spawns and the teleport
    CHECK(fifo.counters.admitted_after_deferral == fifo.counters.admissions_deferred);
    CHECK(fifo.counters.pins_taken > 0);
    CHECK(fifo.hashes.size() == 300);
    // …the safety net stayed out of it (every body entered by admission, the envelope was enough)…
    CHECK(fifo.counters.stalls_admitted_body == 0);
    CHECK(fifo.counters.stall_retries == 0);
    // …and the four runs genuinely completed their loads in different orders.
    CHECK(fifo.completions != lifo.completions);
    CHECK(fifo.completions != shuffled_a.completions);
    CHECK(shuffled_a.completions != shuffled_b.completions);

    check_same(fifo, lifo);
    check_same(fifo, shuffled_a);
    check_same(fifo, shuffled_b);
}

TEST_CASE("m19.8c (a): with an activation lead, load TIMING does not leak either") {
    // Stronger than a permutation: here each load takes a different, seeded number of pumps, so
    // tiles become ready on different TICKS from run to run. With `activation_lead_ticks` longer
    // than the slowest load, a prefetched tile is installed at `requested + lead` regardless — the
    // install tick is a function of the request tick alone.
    constexpr std::uint32_t kJitter = 5; // loads take 3..8 pumps
    constexpr std::uint32_t kLead = 10;  // > 8
    const Outcome a = run_session(Order::Shuffled, 0x1111ull, kJitter, kLead);
    const Outcome b = run_session(Order::Shuffled, 0x2222ull, kJitter, kLead);
    const Outcome c = run_session(Order::Lifo, 0x3333ull, kJitter, kLead);

    REQUIRE(a.completed);
    CHECK(a.counters.installs > 20);
    CHECK(a.counters.installs_held_for_lead > 0); // the lead actually held something back
    CHECK(a.counters.stalls_admitted_body == 0);
    // The runs differ in WHEN loads completed, not merely in what order within a pump.
    std::vector<std::uint64_t> pumps_a, pumps_b;
    for (const auto& completion : a.completions) {
        pumps_a.push_back(completion.first);
    }
    for (const auto& completion : b.completions) {
        pumps_b.push_back(completion.first);
    }
    CHECK(pumps_a != pumps_b);

    check_same(a, b);
    check_same(a, c);
}

TEST_CASE("m19.8c (a): the control — without a lead, different load timing DOES move the journal") {
    // Not a defect, the reason the lead exists: with lead 0 a prefetched tile is installed at the
    // first barrier that finds it ready, so the journal records a different tick when the disk is
    // slower. The server is the authority and the journal is its record, which is why this is
    // acceptable for one machine and why the next brick ships activation ticks to clients instead
    // of letting each derive its own. This case pins the behaviour so the claim above is not
    // mistaken for "timing never matters".
    const Outcome a = run_session(Order::Fifo, 0x1111ull, 5, 0);
    const Outcome b = run_session(Order::Fifo, 0x2222ull, 5, 0);
    REQUIRE(a.completed);
    REQUIRE(b.completed);
    CHECK(a.journal != b.journal);
    CHECK(a.counters.stalls_admitted_body == 0);
    CHECK(b.counters.stalls_admitted_body == 0);
}
