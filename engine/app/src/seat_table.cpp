// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/app/seat_table.hpp"

#include <algorithm>

namespace rime::app {

SeatTable::SeatTable(SeatTableParams params)
    : join_window_ticks_(params.join_window_ticks),
      max_seats_(std::clamp<std::uint16_t>(params.max_seats, 1, kMaxSeatsLimit)) {
    seats_.resize(max_seats_);
    recent_.assign(params.join_burst, 0);
}

SeatAdmission
SeatTable::admit(std::uint64_t session, std::uint64_t endpoint, std::uint64_t now_tick) {
    SeatAdmission out;
    std::size_t free_index = seats_.size();
    for (std::size_t i = 0; i < seats_.size(); ++i) {
        const Seat& s = seats_[i];
        if (!s.occupied) {
            if (free_index == seats_.size()) {
                free_index = i; // lowest free index
            }
            continue;
        }
        if (s.session == session) {
            out.slot = PlayerSlot{static_cast<std::uint16_t>(i), s.generation};
            out.already_seated = true;
            return out;
        }
    }
    for (const Seat& s : seats_) {
        if (s.occupied && s.endpoint == endpoint) {
            ++refused_duplicate_;
            out.refusal = SeatRefusal::DuplicateEndpoint;
            return out;
        }
    }
    if (free_index == seats_.size()) {
        ++refused_full_;
        out.refusal = SeatRefusal::Full;
        return out;
    }
    // Sliding window: the oldest of the last `burst` admissions must be a full window old. A tick
    // that runs backwards is treated as no time having passed (conservative).
    if (recent_.empty()) {
        ++refused_rate_;
        out.refusal = SeatRefusal::RateLimited;
        return out;
    }
    if (recent_count_ == recent_.size()) {
        const std::uint64_t oldest = recent_[recent_next_];
        const std::uint64_t age = now_tick >= oldest ? now_tick - oldest : 0;
        if (age < join_window_ticks_) {
            ++refused_rate_;
            out.refusal = SeatRefusal::RateLimited;
            return out;
        }
    }
    recent_[recent_next_] = now_tick;
    recent_next_ = (recent_next_ + 1) % recent_.size();
    if (recent_count_ < recent_.size()) {
        ++recent_count_;
    }

    Seat& seat = seats_[free_index];
    seat.session = session;
    seat.endpoint = endpoint;
    seat.occupied = true;
    ++seated_;
    ++admitted_;
    out.slot = PlayerSlot{static_cast<std::uint16_t>(free_index), seat.generation};
    return out;
}

std::optional<PlayerSlot> SeatTable::release(std::uint64_t session) {
    for (std::size_t i = 0; i < seats_.size(); ++i) {
        Seat& s = seats_[i];
        if (s.occupied && s.session == session) {
            const PlayerSlot old{static_cast<std::uint16_t>(i), s.generation};
            s.occupied = false;
            s.session = 0;
            s.endpoint = 0;
            s.generation = static_cast<std::uint16_t>(s.generation + 1); // wraps by design
            --seated_;
            ++released_;
            return old;
        }
    }
    return std::nullopt;
}

std::optional<PlayerSlot> SeatTable::slot_of(std::uint64_t session) const noexcept {
    for (std::size_t i = 0; i < seats_.size(); ++i) {
        if (seats_[i].occupied && seats_[i].session == session) {
            return PlayerSlot{static_cast<std::uint16_t>(i), seats_[i].generation};
        }
    }
    return std::nullopt;
}

bool SeatTable::valid(PlayerSlot slot) const noexcept {
    return slot.index < seats_.size() && seats_[slot.index].occupied &&
           seats_[slot.index].generation == slot.generation;
}

std::optional<std::uint64_t> SeatTable::session_of(PlayerSlot slot) const noexcept {
    if (!valid(slot)) {
        return std::nullopt;
    }
    return seats_[slot.index].session;
}

} // namespace rime::app
