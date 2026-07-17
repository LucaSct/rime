// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Proof for the s1.3 latency ledger (ADR-0030 §5). All of it is pure CPU bookkeeping — no device,
// no wire — so we drive it with *synthetic timelines* whose delays we chose, and check that each
// number lands in the stage it belongs to: within-side durations are exact, input-to-photon is
// offset-free, and the clock-offset estimate recovers a planted offset within its own error bar.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "rime/stream/latency.hpp"

using namespace rime;

TEST_CASE("LatencyLedger stage durations are exact same-clock differences") {
    stream::LatencyLedger l;
    l.capture_us = 1000;
    l.readback_us = 1040; // +40 us readback
    l.encode_us = 1055;   // +15 us encode
    l.wire_us = 1060;     // +5 us to the wire
    l.recv_us = 5000;     // client clock — unrelated origin
    l.decode_us = 5012;   // +12 us decode
    l.present_us = 5018;  // +6 us to present

    CHECK(l.readback_dur_us() == 40);
    CHECK(l.encode_dur_us() == 15);
    CHECK(l.server_dur_us() == 60); // wire - capture
    CHECK(l.decode_dur_us() == 12);
    CHECK(l.client_dur_us() == 18); // present - recv
}

TEST_CASE("Durations saturate at zero on equal or out-of-order stamps") {
    stream::LatencyLedger l;
    l.capture_us = 2000;
    l.readback_us = 2000; // equal → 0, not a wrap
    l.encode_us = 1990;   // *earlier* than readback (a reordered/garbage stamp) → 0, not ~1.8e19
    CHECK(l.readback_dur_us() == 0);
    CHECK(l.encode_dur_us() == 0);
    CHECK(stream::sat_sub(10, 25) == 0);
    CHECK(stream::sat_sub(25, 10) == 15);
}

TEST_CASE("input-to-photon is offset-free and only defined when an input was echoed") {
    stream::LatencyLedger l;
    l.present_us = 5100;      // client clock
    l.input_client_us = 5030; // client clock — the input's send time
    // No input echoed yet: input_seq == 0 → "not measured", reported as 0.
    CHECK(l.input_seq == 0);
    CHECK(l.input_to_photon_us() == 0);
    // Once the server echoes the input, the two client-clock stamps subtract directly — no clock
    // sync involved. 5100 - 5030 = 70 us felt latency.
    l.input_seq = 7;
    CHECK(l.input_to_photon_us() == 70);
}

TEST_CASE("LatencyStats reports median and p95 over the window") {
    stream::LatencyStats stats;
    // Feed encode durations 1..100 us (readback fixed) across 100 frames, no input echoed.
    for (int i = 1; i <= 100; ++i) {
        stream::LatencyLedger l;
        l.readback_us = 0;
        l.encode_us = static_cast<std::uint64_t>(i);
        stats.record(l);
    }
    CHECK(stats.count(stream::Stage::Encode) == 100);
    // Median of 1..100 (nearest-rank) ~ 50-51; p95 ~ 95-96. Assert tight bands, not exact ranks.
    const std::uint64_t med = stats.percentile(stream::Stage::Encode, 0.5);
    const std::uint64_t p95 = stats.percentile(stream::Stage::Encode, 0.95);
    CHECK(med >= 49);
    CHECK(med <= 52);
    CHECK(p95 >= 94);
    CHECK(p95 <= 97);
    // input-to-photon saw no echoed input, so it recorded nothing (a 0 there would be a lie).
    CHECK(stats.count(stream::Stage::InputToPhoton) == 0);
    CHECK(stats.percentile(stream::Stage::InputToPhoton, 0.5) == 0);
}

TEST_CASE("LatencyStats window is bounded — only the most recent kWindow frames count") {
    stream::LatencyStats stats;
    // Old regime: kWindow frames at 1000 us, then kWindow frames at 10 us. The old ones must age
    // out.
    for (std::size_t i = 0; i < stream::LatencyStats::kWindow; ++i) {
        stream::LatencyLedger l;
        l.encode_us = 1000;
        stats.record(l);
    }
    for (std::size_t i = 0; i < stream::LatencyStats::kWindow; ++i) {
        stream::LatencyLedger l;
        l.encode_us = 10;
        stats.record(l);
    }
    CHECK(stats.count(stream::Stage::Encode) == stream::LatencyStats::kWindow);
    CHECK(stats.percentile(stream::Stage::Encode, 0.5) == 10); // the old 1000s are gone
}

TEST_CASE("estimate_offset recovers a planted server<->client offset within its error bar") {
    // Plant an offset: server_clock = client_clock + kOffset. On the wire, the frame's wire stamp
    // is taken at the *true midpoint* of the client's input->present interval, so the
    // symmetric-path assumption holds exactly and the estimate is exact.
    constexpr std::int64_t kOffset = 1'000'000; // server clock is 1 s ahead of the client
    stream::LatencyLedger l;
    l.input_seq = 3;
    l.input_client_us = 4000; // client clock: input sent
    l.present_us = 4090;      // client clock: frame presented (RTT 90)
    const std::uint64_t client_mid = 4000 + (4090 - 4000) / 2; // 4045
    l.wire_us = static_cast<std::uint64_t>(static_cast<std::int64_t>(client_mid) + kOffset);

    const auto off = stream::estimate_offset(l);
    REQUIRE(off.has_value());
    CHECK(off->offset_us == kOffset); // exact when wire sits at the true midpoint
    CHECK(off->error_us == 45);       // half the 90 us round trip — the honest uncertainty

    // A frame with no echoed input, or out-of-order stamps, yields no estimate rather than a guess.
    stream::LatencyLedger none;
    none.wire_us = 5000;
    CHECK_FALSE(stream::estimate_offset(none).has_value());
    stream::LatencyLedger bad;
    bad.input_seq = 1;
    bad.input_client_us = 100;
    bad.present_us = 50; // present before send — refuse
    bad.wire_us = 200;
    CHECK_FALSE(stream::estimate_offset(bad).has_value());
}

TEST_CASE("estimate_offset's error bar bounds a skewed (asymmetric-path) estimate") {
    // If the server actually wired the frame *early* (not at the midpoint) — an asymmetric path —
    // the point estimate is off, but by no more than the reported error_us. This is why the felt
    // input-to-photon number never relies on the offset.
    constexpr std::int64_t kOffset = 500'000;
    stream::LatencyLedger l;
    l.input_seq = 9;
    l.input_client_us = 10'000;
    l.present_us = 10'200; // RTT 200 → error bar 100
    // True offset kOffset, but wire happened 60 us before the midpoint (server-heavy downlink).
    const std::int64_t client_mid = 10'000 + (10'200 - 10'000) / 2;
    l.wire_us = static_cast<std::uint64_t>(client_mid + kOffset - 60);

    const auto off = stream::estimate_offset(l);
    REQUIRE(off.has_value());
    const std::int64_t err = off->offset_us - kOffset; // -60 here
    CHECK(static_cast<std::uint64_t>(err < 0 ? -err : err) <= off->error_us);
}
