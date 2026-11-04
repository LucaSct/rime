// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// SeatTable (m20.8a, ADR-0108): each test is named for the rule it proves. Time is a plain tick
// argument, so the rate-limit window is exercised without a clock.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "rime/app/seat_table.hpp"

using namespace rime::app;

namespace {
// Generous rate budget so tests about other rules never trip it.
SeatTableParams open_params(std::uint16_t seats) {
    SeatTableParams p;
    p.max_seats = seats;
    p.join_burst = 1000;
    return p;
}
} // namespace

TEST_CASE("seat: admitting a seated session returns its slot and counts nothing") {
    SeatTable t(open_params(4));
    const SeatAdmission a = t.admit(10, 100, 0);
    REQUIRE(a.refusal == SeatRefusal::None);
    CHECK_FALSE(a.already_seated);
    const SeatAdmission b = t.admit(10, 100, 1);
    CHECK(b.refusal == SeatRefusal::None);
    CHECK(b.already_seated);
    CHECK(b.slot == a.slot);
    CHECK(t.admitted() == 1);
    CHECK(t.seated() == 1);
    CHECK(t.refused_duplicate() == 0);
}

TEST_CASE("seat: a different session from a seated endpoint is a DuplicateEndpoint") {
    SeatTable t(open_params(4));
    REQUIRE(t.admit(1, 100, 0).refusal == SeatRefusal::None);
    const SeatAdmission d = t.admit(2, 100, 1);
    CHECK(d.refusal == SeatRefusal::DuplicateEndpoint);
    CHECK(t.refused_duplicate() == 1);
    CHECK(t.seated() == 1);
    // After the old session releases, the endpoint may seat again.
    REQUIRE(t.release(1).has_value());
    CHECK(t.admit(2, 100, 2).refusal == SeatRefusal::None);
}

TEST_CASE("seat: a full table refuses with Full") {
    SeatTable t(open_params(2));
    REQUIRE(t.admit(1, 101, 0).refusal == SeatRefusal::None);
    REQUIRE(t.admit(2, 102, 0).refusal == SeatRefusal::None);
    CHECK(t.admit(3, 103, 0).refusal == SeatRefusal::Full);
    CHECK(t.refused_full() == 1);
    CHECK(t.seated() == 2);
}

TEST_CASE("seat: the rate limit counts only successful admissions in a sliding window") {
    SeatTableParams p;
    p.max_seats = 3;
    p.join_burst = 2;
    p.join_window_ticks = 10;
    SeatTable t(p);
    CHECK(t.admit(1, 101, 0).refusal == SeatRefusal::None);
    CHECK(t.admit(2, 102, 1).refusal == SeatRefusal::None);
    CHECK(t.admit(3, 103, 2).refusal == SeatRefusal::RateLimited);
    CHECK(t.admit(3, 103, 9).refusal == SeatRefusal::RateLimited);
    CHECK(t.refused_rate() == 2);
    // The refusals consumed nothing: at tick 10 the admission at tick 0 has left the window.
    CHECK(t.admit(3, 103, 10).refusal == SeatRefusal::None);
    // Window now holds ticks 1 and 10; a fourth must wait until tick 11 -- but the table is full,
    // and Full is checked before the rate, so free a seat and watch the rate bite.
    CHECK(t.admit(4, 104, 10).refusal == SeatRefusal::Full);
    REQUIRE(t.release(1).has_value());
    CHECK(t.admit(4, 104, 10).refusal == SeatRefusal::RateLimited);
    CHECK(t.admit(4, 104, 11).refusal == SeatRefusal::None);
}

TEST_CASE("seat: full and duplicate are checked before the rate and do not spend it") {
    SeatTableParams p;
    p.max_seats = 1;
    p.join_burst = 1;
    p.join_window_ticks = 100;
    SeatTable t(p);
    REQUIRE(t.admit(1, 101, 0).refusal == SeatRefusal::None);
    CHECK(t.admit(2, 102, 1).refusal == SeatRefusal::Full);
    CHECK(t.admit(3, 101, 1).refusal == SeatRefusal::DuplicateEndpoint);
    CHECK(t.refused_rate() == 0);
}

TEST_CASE("seat: the lowest free index is reused and release bumps the generation") {
    SeatTable t(open_params(4));
    const PlayerSlot s0 = t.admit(1, 101, 0).slot;
    const PlayerSlot s1 = t.admit(2, 102, 0).slot;
    REQUIRE(s0.index == 0);
    REQUIRE(s1.index == 1);
    REQUIRE(t.release(1).has_value());
    CHECK_FALSE(t.valid(s0));
    CHECK_FALSE(t.session_of(s0).has_value());
    const PlayerSlot n = t.admit(3, 103, 1).slot;
    CHECK(n.index == 0);
    CHECK(n.generation == s0.generation + 1);
    CHECK(t.valid(n));
    CHECK(t.session_of(n) == std::optional<std::uint64_t>(3));
    CHECK_FALSE(t.valid(s0));
    CHECK(t.slot_of(3) == std::optional<PlayerSlot>(n));
    CHECK_FALSE(t.slot_of(1).has_value());
}

TEST_CASE("seat: for_each_seated visits ascending index regardless of admission order") {
    SeatTable t(open_params(4));
    t.admit(1, 101, 0);
    t.admit(2, 102, 0);
    t.admit(3, 103, 0);
    REQUIRE(t.release(1).has_value());
    t.admit(4, 104, 1);
    std::vector<std::uint64_t> sessions;
    std::vector<std::uint16_t> indices;
    t.for_each_seated([&](PlayerSlot s, std::uint64_t session) {
        indices.push_back(s.index);
        sessions.push_back(session);
    });
    CHECK(indices == std::vector<std::uint16_t>{0, 1, 2});
    CHECK(sessions == std::vector<std::uint64_t>{4, 2, 3});
}

TEST_CASE("seat: max_seats is clamped to 1..128 and one seat behaves as a single-seat pump") {
    CHECK(SeatTable(open_params(0)).max_seats() == 1);
    CHECK(SeatTable(open_params(500)).max_seats() == 128);
    CHECK(SeatTable(open_params(16)).max_seats() == 16);
    SeatTable one(open_params(0));
    REQUIRE(one.admit(1, 101, 0).refusal == SeatRefusal::None);
    CHECK(one.admit(2, 102, 0).refusal == SeatRefusal::Full);
}

TEST_CASE("seat: releasing an unknown session returns nullopt and counts nothing") {
    SeatTable t(open_params(2));
    CHECK_FALSE(t.release(42).has_value());
    CHECK(t.released() == 0);
    t.admit(1, 101, 0);
    CHECK(t.release(1).has_value());
    CHECK(t.released() == 1);
    CHECK_FALSE(t.release(1).has_value());
    CHECK(t.released() == 1);
}

TEST_CASE("seat: a stale or out-of-range slot addresses nothing") {
    SeatTable t(open_params(2));
    CHECK_FALSE(t.valid(PlayerSlot{99, 0}));
    CHECK_FALSE(t.session_of(PlayerSlot{0, 0}).has_value()); // empty seat
}
