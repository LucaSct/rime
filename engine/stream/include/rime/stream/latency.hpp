// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The **latency ledger** — Track S / s1.3 (ADR-0030 §5). Streaming lives or dies on *feel*, and
// "feels responsive" only becomes an engineering target once it is a **number**. This is that
// number: per frame, a timestamp at each stage of the capture -> present pipeline, so the cost of
// every hop is visible and a regression is caught, not felt.
//
// The honest hard part is **two clocks**. The server stamps its stages in the server's monotonic
// clock; the client stamps its stages in the client's. There is no NTP here, so the two clocks
// share no zero. Two consequences shape the whole design:
//
//   - **Within-side durations are exact.** encode-time = encode_us - readback_us is a difference of
//     two *server* stamps, so the unknown offset cancels. Same for the client's decode/present
//     gaps. These need no clock sync and are always trustworthy.
//   - **The cross-side total is measured offset-free by an echo.** The client stamps each input it
//     sends with its own clock (input_client_us); the server echoes that stamp back on the frame
//     that first *reflects* the input; the client subtracts it from the frame's present time — both
//     client-clock stamps, so the **input-to-photon** latency (the one users actually feel) falls
//     out with no clock-offset guess at all.
//
// A rough server<->client offset is *also* estimable (estimate_offset, the midpoint method) to lay
// server and client stamps on one timeline — but it carries an honest error bar (~half the round
// trip) and is never needed for the two numbers above. Don't oversell it.
namespace rime::stream {

// Saturating unsigned subtraction: a - b, or 0 if b > a. Timestamps can arrive equal or (across a
// dropped/re-ordered frame) mildly out of order; a duration is never negative, so we floor at 0
// rather than wrap to a garbage 18-quintillion microseconds.
[[nodiscard]] constexpr std::uint64_t sat_sub(std::uint64_t a, std::uint64_t b) noexcept {
    return a >= b ? a - b : 0;
}

// One frame's stamps. Server stages (capture..wire) are in the server clock and ride in the
// FrameMessage; client stages (recv..present) are in the client clock and are stamped as the frame
// flows through the client. The input echo is the server handing back the identity + client-clock
// send time of the most recent input it has applied (input_seq == 0 => no input echoed this frame).
struct LatencyLedger {
    // ── server clock (carried on the wire) ──
    std::uint64_t capture_us = 0;  // FrameStreamer::begin_capture submitted the readback copy
    std::uint64_t readback_us = 0; // try_get_frame() returned the CPU pixels
    std::uint64_t encode_us = 0;   // the codec produced the packet
    std::uint64_t wire_us = 0;     // just before ProtocolConnection::send_frame
    // ── client clock (stamped on receipt) ──
    std::uint64_t recv_us = 0;    // recv_message() returned this frame
    std::uint64_t decode_us = 0;  // the decoder produced pixels
    std::uint64_t present_us = 0; // the frame was presented / consumed
    // ── input echo (client clock) ──
    std::uint32_t input_seq = 0;       // the input this frame first reflects (0 = none echoed)
    std::uint64_t input_client_us = 0; // that input's client-clock send time

    // Within-side stage durations — exact (same-clock differences, offset cancels).
    [[nodiscard]] std::uint64_t readback_dur_us() const { return sat_sub(readback_us, capture_us); }

    [[nodiscard]] std::uint64_t encode_dur_us() const { return sat_sub(encode_us, readback_us); }

    [[nodiscard]] std::uint64_t server_dur_us() const { return sat_sub(wire_us, capture_us); }

    [[nodiscard]] std::uint64_t decode_dur_us() const { return sat_sub(decode_us, recv_us); }

    [[nodiscard]] std::uint64_t client_dur_us() const { return sat_sub(present_us, recv_us); }

    // Offset-free input-to-photon (both stamps are client-clock). 0 when no input was echoed.
    [[nodiscard]] std::uint64_t input_to_photon_us() const {
        return input_seq == 0 ? 0 : sat_sub(present_us, input_client_us);
    }
};

// The pipeline stages the stats aggregator tracks, in flow order. Kept as a small dense enum so a
// LatencyStats is just an array of sample rings.
enum class Stage : std::uint8_t {
    Readback = 0,        // GPU readback (capture -> pixels)         [server]
    Encode,              // codec encode                             [server]
    ServerPipeline,      // capture -> wire (the whole server half)  [server]
    ClientDecodePresent, // recv -> present (the whole client half) [client]
    InputToPhoton,       // input send -> present (the felt latency) [cross, offset-free]
    kCount
};

[[nodiscard]] const char* stage_name(Stage s) noexcept;

// Rolling per-stage percentiles over a bounded window of recent frames. Percentiles (median, p95)
// rather than a mean because latency is spiky — a p95 in the budget is what "responsive" means, and
// an average hides the stalls users actually notice. Cheap: a fixed ring per stage; percentile()
// copies the live samples and nth_elements them (a few hundred entries — nothing on this path).
class LatencyStats {
public:
    // How many recent frames each stage remembers. A few seconds at 30-60 fps — enough for a stable
    // p95, small enough that percentile() stays trivial.
    static constexpr std::size_t kWindow = 256;

    // Fold one frame's ledger in. Each stage takes its own duration; InputToPhoton is skipped on
    // frames that echoed no input (a 0 there is "not measured", not "0 microseconds").
    void record(const LatencyLedger& ledger);

    // The p-th percentile (p in [0,1]) of `stage` over the window, in microseconds; 0 if no samples
    // yet. p=0.5 is the median, p=0.95 the tail the budget cares about.
    [[nodiscard]] std::uint64_t percentile(Stage stage, double p) const;

    [[nodiscard]] std::size_t count(Stage stage) const noexcept;

    // A human-readable multi-line summary (median + p95 per stage) for the stats dump / logs.
    [[nodiscard]] std::string dump() const;

private:
    struct Ring {
        std::array<std::uint64_t, kWindow> samples{};
        std::size_t size = 0;    // live entries (<= kWindow before the ring wraps)
        std::size_t next = 0;    // write cursor
        std::uint64_t total = 0; // running count of records (for context in the dump)
    };

    std::array<Ring, static_cast<std::size_t>(Stage::kCount)> rings_{};
};

// A server<->client clock-offset estimate (offset = server_clock - client_clock) from a single
// frame that echoed an input, by the **midpoint method**: assume the frame's wire_us (server clock)
// fell at the midpoint of the client-clock interval [input_client_us, present_us]. Then
// offset ~= wire_us - (input_client_us + present_us)/2, with error ~ half that interval (a
// symmetric-path assumption — asymmetric routing skews it, which is why the error bar is reported
// and the felt-latency number above never relies on this). Returns nullopt on a frame with no input
// echo or incomplete stamps.
struct ClockOffset {
    std::int64_t offset_us = 0; // server_clock - client_clock
    std::uint64_t error_us = 0; // ~half the round trip; the honest uncertainty
};

[[nodiscard]] std::optional<ClockOffset> estimate_offset(const LatencyLedger& ledger);

} // namespace rime::stream
