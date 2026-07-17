// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/stream/latency.hpp"

#include <algorithm>
#include <cstdio>

// The latency ledger's aggregation + clock-offset math. All of this is pure CPU bookkeeping — no
// device, no wire — so it is exhaustively unit-testable on synthetic timelines (tests/stream/
// latency_test.cpp feeds known delays and checks the right stage reports them). See latency.hpp for
// the two-clocks reasoning this implements.
namespace rime::stream {

const char* stage_name(Stage s) noexcept {
    switch (s) {
        case Stage::Readback:
            return "readback";
        case Stage::Encode:
            return "encode";
        case Stage::ServerPipeline:
            return "server";
        case Stage::ClientDecodePresent:
            return "client";
        case Stage::InputToPhoton:
            return "input->photon";
        case Stage::kCount:
            break;
    }
    return "?";
}

namespace {

// The per-stage duration a ledger contributes. Kept beside record() so the stage list has one home.
std::uint64_t stage_sample(const LatencyLedger& l, Stage s) {
    switch (s) {
        case Stage::Readback:
            return l.readback_dur_us();
        case Stage::Encode:
            return l.encode_dur_us();
        case Stage::ServerPipeline:
            return l.server_dur_us();
        case Stage::ClientDecodePresent:
            return l.client_dur_us();
        case Stage::InputToPhoton:
            return l.input_to_photon_us();
        case Stage::kCount:
            break;
    }
    return 0;
}

} // namespace

void LatencyStats::record(const LatencyLedger& ledger) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(Stage::kCount); ++i) {
        const auto stage = static_cast<Stage>(i);
        // InputToPhoton is only defined on frames that echoed an input — a 0 there means "not
        // measured this frame", so skip it rather than poison the percentile with a fake zero.
        if (stage == Stage::InputToPhoton && ledger.input_seq == 0) {
            continue;
        }
        Ring& r = rings_[i];
        r.samples[r.next] = stage_sample(ledger, stage);
        r.next = (r.next + 1) % kWindow;
        if (r.size < kWindow) {
            ++r.size;
        }
        ++r.total;
    }
}

std::uint64_t LatencyStats::percentile(Stage stage, double p) const {
    const Ring& r = rings_[static_cast<std::size_t>(stage)];
    if (r.size == 0) {
        return 0;
    }
    // Copy the live window and partial-sort to the target rank — nth_element is O(n) and n is at
    // most kWindow, so this is cheap enough to call per HUD refresh. Clamp p into [0,1].
    std::vector<std::uint64_t> v(r.samples.begin(),
                                 r.samples.begin() + static_cast<std::ptrdiff_t>(r.size));
    const double pc = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
    auto rank = static_cast<std::size_t>(pc * static_cast<double>(v.size() - 1) + 0.5);
    if (rank >= v.size()) {
        rank = v.size() - 1;
    }
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(rank), v.end());
    return v[rank];
}

std::size_t LatencyStats::count(Stage stage) const noexcept {
    return rings_[static_cast<std::size_t>(stage)].size;
}

std::string LatencyStats::dump() const {
    std::string out = "latency (us) — median / p95:\n";
    char line[128];
    for (std::size_t i = 0; i < static_cast<std::size_t>(Stage::kCount); ++i) {
        const auto stage = static_cast<Stage>(i);
        std::snprintf(line,
                      sizeof(line),
                      "  %-14s %8llu / %8llu  (n=%zu)\n",
                      stage_name(stage),
                      static_cast<unsigned long long>(percentile(stage, 0.5)),
                      static_cast<unsigned long long>(percentile(stage, 0.95)),
                      count(stage));
        out += line;
    }
    return out;
}

std::optional<ClockOffset> estimate_offset(const LatencyLedger& ledger) {
    // Needs a frame that echoed an input (so both client-clock endpoints exist) and a server
    // wire stamp to place between them.
    if (ledger.input_seq == 0 || ledger.input_client_us == 0 || ledger.present_us == 0 ||
        ledger.wire_us == 0) {
        return std::nullopt;
    }
    if (ledger.present_us < ledger.input_client_us) {
        return std::nullopt; // stamps out of order — refuse rather than report a garbage offset
    }
    // Midpoint of the client-clock round trip, in client clock.
    const std::uint64_t client_mid =
        ledger.input_client_us + (ledger.present_us - ledger.input_client_us) / 2;
    ClockOffset r;
    r.offset_us = static_cast<std::int64_t>(ledger.wire_us) - static_cast<std::int64_t>(client_mid);
    r.error_us = (ledger.present_us - ledger.input_client_us) / 2;
    return r;
}

} // namespace rime::stream
