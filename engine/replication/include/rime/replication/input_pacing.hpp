// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/replication/input_v2.hpp"

// InputPacer (m20.7a, ADR-0107): the CLIENT half of the adaptive input buffer.
//
// The server's adaptive schedule grows its buffer for free: an underrun is a stall, and the late
// command is then applied (a playout buffer that grows on underrun, as in voice and video jitter
// buffers). A buffer that only grows would add input delay forever, so something has to shrink it.
// Dropping queued commands would lose presses and cost corrections. Overwatch and Unity Netcode
// for Entities instead do CLIENT-SIDE TIME DILATION: the client runs its tick slightly slower so
// fewer commands arrive per server tick, and the surplus drains losslessly.
//
// Asymmetric on purpose: growth is immediate (a stall), shrinkage is slow (6 % slow motion), and
// the pacer never returns a scale below 1 -- it never speeds the client up, because underrun is the
// server's job. Pure leaf: no clock; the caller feeds it reports and asks for a scale per tick.
namespace rime::replication {

struct InputPacerParams {
    std::uint32_t window_reports = 60; // fresh reports that make one observation window
    std::uint8_t drain_above = 1;      // drain when the window's minimum margin is > this
    std::uint32_t drain_ticks = 16;    // dilated ticks per drained tick of buffer
    double dilation = 1.0 / 16.0;      // period scale while draining = 1 + dilation
};

class InputPacer {
public:
    explicit InputPacer(InputPacerParams params = {}) noexcept;

    // A report not newer than the newest seen is counted as stale and ignored.
    void on_report(const InputPacing& report) noexcept;
    // Once per client tick: the period scale for THIS tick (1.0, or 1 + dilation while draining).
    [[nodiscard]] double tick() noexcept;

    [[nodiscard]] std::uint64_t reports() const noexcept { return reports_; }

    [[nodiscard]] std::uint64_t stale_reports() const noexcept { return stale_reports_; }

    [[nodiscard]] std::uint64_t windows() const noexcept { return windows_; }

    [[nodiscard]] std::uint64_t windows_with_stall() const noexcept { return windows_with_stall_; }

    [[nodiscard]] std::uint64_t drains() const noexcept { return drains_; }

    [[nodiscard]] std::uint64_t dilated_ticks() const noexcept { return dilated_ticks_; }

    // Sum over drain episodes of (window minimum - drain_above): ticks of buffer asked to drain.
    [[nodiscard]] std::uint64_t drained_ticks_of_buffer() const noexcept { return drained_buffer_; }

    [[nodiscard]] std::uint8_t last_margin() const noexcept { return last_margin_; }

    [[nodiscard]] const InputPacerParams& params() const noexcept { return params_; }

private:
    InputPacerParams params_;
    std::uint64_t newest_tick_ = 0;
    bool has_newest_ = false;
    std::uint32_t prev_stalls_ = 0;
    std::uint32_t window_count_ = 0;
    std::uint8_t window_min_ = 255;
    bool window_stalled_ = false;
    std::uint64_t drain_remaining_ = 0;
    std::uint8_t last_margin_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t stale_reports_ = 0;
    std::uint64_t windows_ = 0;
    std::uint64_t windows_with_stall_ = 0;
    std::uint64_t drains_ = 0;
    std::uint64_t dilated_ticks_ = 0;
    std::uint64_t drained_buffer_ = 0;
};

} // namespace rime::replication
