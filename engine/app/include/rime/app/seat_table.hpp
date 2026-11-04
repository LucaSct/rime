// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// SeatTable (m20.8a, ADR-0108 §1/§5): the engine-owned roster of who sits where on a server that
// hosts several remote players.
//
// The technique is a GENERATIONAL SLOT MAP -- the same idea as ecs::Entity and net::SessionId.
// Seats live in a dense array so per-seat state elsewhere (an input schedule, an owner checkpoint)
// is a plain O(1) index. But an index alone is a dangling pointer in disguise: seat 3 is vacated, a
// new player takes seat 3, and a late packet addressed to "seat 3" would act for the wrong person.
// So each index carries a generation that is bumped when the seat is released; a PlayerSlot
// remembers the generation it was issued under, and once that no longer matches it addresses
// nothing.
//
// Why a seat is neither of the two things it resembles:
//   * Not a session. A session is a transport fact -- a connection that can drop and be re-made --
//     and the host's own local player has none, yet still needs a seat.
//   * Not an entity. A player who dies and respawns gets a new entity in the SAME seat; per-seat
//     state (input history, score, schedule) must survive that, so it cannot hang off the entity.
//
// Admission is where a server meets hostile or merely careless clients, so it has three refusals,
// checked in this order and each counted: Full (no free seat), DuplicateEndpoint (one seat per
// source endpoint, so one machine cannot hold the table), then RateLimited (a sliding window of
// `join_window_ticks` that allows `join_burst` SUCCESSFUL admissions). Only successes spend the
// rate budget, so a flood of refused joins cannot lock out honest ones.
//
// A pure leaf: no sockets, no clock (time is a caller-supplied tick), no threads, and no allocation
// after construction. `session` and `endpoint` are opaque 64-bit keys the caller derives (SessionId
// bits; address+port). It is not wired to anything yet.
namespace rime::app {

// A handle to one seat. `generation` is a u16 and WRAPS after 65536 releases of the same index; a
// stale handle could then alias a live one, but only if it is held across that many re-seatings of
// one index, which a session lifetime does not approach.
struct PlayerSlot {
    std::uint16_t index = 0;
    std::uint16_t generation = 0;
    friend bool operator==(PlayerSlot, PlayerSlot) = default;
};

enum class SeatRefusal : std::uint8_t { None, Full, DuplicateEndpoint, RateLimited };

struct SeatTableParams {
    std::uint16_t max_seats = 16; // 1..128; the constructor clamps and max_seats() reports it
    std::uint32_t join_burst = 4; // admissions allowed per join window...
    std::uint32_t join_window_ticks = 60; // ...a sliding window of this many caller ticks
};

struct SeatAdmission {
    SeatRefusal refusal = SeatRefusal::None;
    PlayerSlot slot{};
    bool already_seated = false;
};

class SeatTable {
public:
    static constexpr std::uint16_t kMaxSeatsLimit = 128;

    explicit SeatTable(SeatTableParams params = {});

    // Seats `session` (from `endpoint`) at `now_tick`. A session already seated gets its existing
    // slot back with `already_seated = true` and nothing is counted (a re-sent hello is harmless).
    SeatAdmission admit(std::uint64_t session, std::uint64_t endpoint, std::uint64_t now_tick);

    // Vacates the session's seat and bumps that index's generation. nullopt (nothing counted) if
    // the session holds no seat. Returns the slot as it was BEFORE the bump (now stale).
    std::optional<PlayerSlot> release(std::uint64_t session);

    [[nodiscard]] std::optional<PlayerSlot> slot_of(std::uint64_t session) const noexcept;
    // nullopt when the slot is stale or the seat is empty.
    [[nodiscard]] std::optional<std::uint64_t> session_of(PlayerSlot slot) const noexcept;
    [[nodiscard]] bool valid(PlayerSlot slot) const noexcept;

    [[nodiscard]] std::uint16_t seated() const noexcept { return seated_; }

    [[nodiscard]] std::uint16_t max_seats() const noexcept { return max_seats_; }

    // Visits seated players as f(PlayerSlot, session) in ASCENDING index order -- the order a tick
    // runs joins and inputs -- whatever order they were admitted in.
    template <class F> void for_each_seated(F&& f) const {
        for (std::size_t i = 0; i < seats_.size(); ++i) {
            const Seat& s = seats_[i];
            if (s.occupied) {
                f(PlayerSlot{static_cast<std::uint16_t>(i), s.generation}, s.session);
            }
        }
    }

    [[nodiscard]] std::uint64_t admitted() const noexcept { return admitted_; }

    [[nodiscard]] std::uint64_t released() const noexcept { return released_; }

    [[nodiscard]] std::uint64_t refused_full() const noexcept { return refused_full_; }

    [[nodiscard]] std::uint64_t refused_duplicate() const noexcept { return refused_duplicate_; }

    [[nodiscard]] std::uint64_t refused_rate() const noexcept { return refused_rate_; }

private:
    struct Seat {
        std::uint64_t session = 0;
        std::uint64_t endpoint = 0;
        std::uint16_t generation = 0;
        bool occupied = false;
    };

    std::vector<Seat> seats_;
    // Ring of the last `join_burst` successful admission ticks: the sliding window needs only the
    // oldest of the last `burst` to decide whether another fits.
    std::vector<std::uint64_t> recent_;
    std::size_t recent_next_ = 0;
    std::size_t recent_count_ = 0;
    std::uint32_t join_window_ticks_;
    std::uint16_t max_seats_;
    std::uint16_t seated_ = 0;
    std::uint64_t admitted_ = 0;
    std::uint64_t released_ = 0;
    std::uint64_t refused_full_ = 0;
    std::uint64_t refused_duplicate_ = 0;
    std::uint64_t refused_rate_ = 0;
};

} // namespace rime::app
