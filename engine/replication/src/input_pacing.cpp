// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/input_pacing.hpp"

#include <algorithm>

namespace rime::replication {

InputPacer::InputPacer(InputPacerParams params) noexcept : params_(params) {}

void InputPacer::on_report(const InputPacing& report) noexcept {
    if (has_newest_ && report.server_tick <= newest_tick_) {
        ++stale_reports_; // reordered or duplicated: it describes a past the newest already covers
        return;
    }
    const bool first = !has_newest_;
    has_newest_ = true;
    newest_tick_ = report.server_tick;
    ++reports_;
    last_margin_ = report.margin;
    // The first report only seeds the stall comparison: a running counter has no "rise" until
    // there is a previous value to rise from. Compared even mid-drain so the baseline stays fresh.
    const bool rose = !first && report.stalls != prev_stalls_;
    prev_stalls_ = report.stalls;

    // During a drain episode the margin on the wire is the PRE-drain past (reports lag the client's
    // slowdown by a round trip), so folding it into a window would re-trigger on its own tail.
    if (drain_remaining_ > 0) {
        if (rose) {
            // An underrun proves the planned surplus is gone; do not keep slowing the client.
            drain_remaining_ = 0;
            ++drains_cancelled_;
        }
        return;
    }
    window_min_ = std::min(window_min_, report.margin);
    window_stalled_ = window_stalled_ || rose;
    if (++window_count_ < params_.window_reports) {
        return;
    }
    ++windows_;
    if (window_stalled_) {
        ++windows_with_stall_;
    } else if (window_min_ > params_.drain_above) {
        // The window minimum is a LOWER BOUND on the server's margin, and during the window nothing
        // but stalls (which only add) moves it, so draining the whole surplus
        // (min - drain_above ticks of buffer) at once can never push the margin below drain_above.
        // It also pays a 12-tick hitch back in ~3 s rather than ~15 s of one-tick-at-a-time
        // episodes. CAPPED per episode, though (review, 2026-10-26): a 62-tick surplus would
        // otherwise dilate the client for 992 ticks (~17.6 s) on one stale reading; another clean
        // window drains what remains, and a stall inside the episode cancels it (above).
        const std::uint64_t surplus = std::min<std::uint64_t>(
            std::uint64_t{window_min_} - params_.drain_above, params_.max_drain_per_episode);
        drain_remaining_ = surplus * params_.drain_ticks;
        drained_buffer_ += surplus;
        ++drains_;
    }
    window_count_ = 0;
    window_min_ = 255;
    window_stalled_ = false;
}

double InputPacer::tick() noexcept {
    if (drain_remaining_ == 0) {
        return 1.0;
    }
    --drain_remaining_;
    ++dilated_ticks_;
    return 1.0 + params_.dilation;
}

} // namespace rime::replication
