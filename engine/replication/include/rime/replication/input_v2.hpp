// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "rime/replication/input.hpp"

// InputV2 (m20.3, ADR-0103): how a remote player's input reaches a server that has no window.
//
// It is three techniques that every well-documented modern FPS netcode shares (Overwatch, GDC 2017;
// VALORANT's 128-tick servers; Unity Netcode for Entities' command stream), and none of them is new
// to Rime -- what is new is that they are put together with the acknowledgement made HONEST:
//
//   1. REDUNDANT INPUT. Every packet carries the newest command plus its two predecessors, absolute
//      values, 28 bytes each. A lost datagram costs nothing as long as one of the next two arrives,
//      so the input path never retransmits -- it would arrive too late to matter anyway.
//   2. A SERVER JITTER BUFFER. Commands land in a ring keyed by the client's sequence, and the
//      server executes exactly one per simulation tick, starting once `slack_ticks` have arrived.
//      A command that is not there when its tick comes is SYNTHESIZED (held keys and axes repeated,
//      `pressed` edges cleared, neutral after a silence) so a hiccup is a stutter, not a stop.
//   3. EXECUTION ACKNOWLEDGEMENT. The server tells the client what it RAN, not what it received:
//      `retired_through` ("no command at or below this will ever execute") plus a 128-bit mask of
//      which of those retired commands were actually applied. A synthesized tick sets no bit.
//
// Why (3) is the part that matters here: guardrail 5 (CLAUDE.md, docs/design/replication.md) says a
// per-peer "what they have" fact may only strengthen on confirmed HOLDING, and for input, holding
// means "executed into authoritative state". The M12 receiver's `consumed_through` is a drain
// frontier -- it says a command left the buffer, which a synthesized or dropped tick can make lie.
// The client window here therefore prunes ONLY on a result, never on having sent, and it can name
// the commands that retired unapplied, which is exactly what reconciliation needs to re-predict.
//
// A pure leaf: no sockets, no clock, no threads. The session layer, the launcher and the proof
// wire it up (m20.3b); M12's ClientInputSender / ServerInputReceiver are deliberately untouched.
namespace rime::replication {

inline constexpr std::uint8_t kInputCodecV2 = 2;
// m20.8b (ADR-0108 §4): codec 3 is codec 2 plus a SEAT in the accepting hello reply. The command
// packets, the result and the pacing report keep codec 2's layout and its codec byte (2): the seat
// is a fact about the session, said once at admission, and nothing per tick needs it -- the server
// already knows which seat a datagram belongs to from the session it arrived on. A server whose
// game seats more than one player speaks ONLY codec 3, so an older client is refused as `Version`
// at the hello instead of being admitted without knowing which player it is.
inline constexpr std::uint8_t kInputCodecV3 = 3;
inline constexpr std::size_t kInputV2MaxCommands = 16;
inline constexpr std::size_t kInputV2CommandBytes = 28;
inline constexpr std::size_t kInputV2HeaderBytes = 4;
inline constexpr std::size_t kInputHelloBytes = 11;
inline constexpr std::size_t kInputHelloReplyBytes = 3;
// The codec-3 ACCEPTING reply: the 3 bytes above plus seat:u16 and generation:u16. A refusal never
// carries a seat, so every refusal stays 3 bytes and decodes on every client, old or new.
inline constexpr std::size_t kInputHelloReplyV3Bytes = 7;
inline constexpr std::size_t kInputResultBytes = 30;
inline constexpr std::size_t kInputPacingBytes = 14;

// These four tags are private to V2; the old input sender and receiver keep their own wire format.
inline constexpr std::uint8_t kInputV2Tag = 0xE0;
inline constexpr std::uint8_t kInputHelloTag = 0xE1;
inline constexpr std::uint8_t kInputHelloReplyTag = 0xE2;
inline constexpr std::uint8_t kInputResultTag = 0xE3;
// m20.7 (ADR-0107): the server's per-tick report of how deep its input buffer is.
inline constexpr std::uint8_t kInputPacingTag = 0xE7;

enum class InputV2Refusal {
    None,
    Truncated,
    BadLength,
    BadCount,
    UnknownCodec,
    ReservedNonZero,
    NonFinite,
    SequenceOrder,
};

struct InputV2Decode {
    InputV2Refusal refusal = InputV2Refusal::None;

    [[nodiscard]] explicit operator bool() const noexcept {
        return refusal == InputV2Refusal::None;
    }
};

// Exact-size output is not required: the writer uses only the prefix it needs. Decode appends a
// whole validated packet or nothing, so a caller never sees half a command window.
[[nodiscard]] std::size_t encode_input_v2(std::span<const InputCommand> oldest_first,
                                          std::span<std::byte> out);
[[nodiscard]] InputV2Decode decode_input_v2(std::span<const std::byte> in,
                                            std::vector<InputCommand>& out);

struct InputHello {
    std::uint8_t codec_min = 0;
    std::uint8_t codec_max = 0;
    std::uint64_t schema_hash = 0;
};

// The last three (m20.8b) are seat-admission refusals. Only a codec-3 server sends them, and it has
// already refused every codec-2 client as `Version` before seat admission runs, so a client that
// cannot decode them never receives one. A one-seat (codec-2) server reports a refused seat as
// `Full`, as it always has.
enum class InputHelloRefusal : std::uint8_t {
    None,
    Version,
    Schema,
    Full,
    DuplicateEndpoint,
    RateLimited,
    GameRefused,
};

struct InputHelloReply {
    std::uint8_t codec = 0;
    InputHelloRefusal refusal = InputHelloRefusal::None;
    // Codec 3, accepted only: the seat the server gave this session (app::PlayerSlot's two halves,
    // spelled out because this leaf does not depend on the app layer).
    bool seated = false;
    std::uint16_t seat = 0;
    std::uint16_t seat_generation = 0;
};

[[nodiscard]] std::size_t encode_input_hello(const InputHello& hello, std::span<std::byte> out);
[[nodiscard]] bool decode_input_hello(std::span<const std::byte> in, InputHello& out);
[[nodiscard]] std::size_t encode_input_hello_reply(const InputHelloReply& reply,
                                                   std::span<std::byte> out);
[[nodiscard]] bool decode_input_hello_reply(std::span<const std::byte> in, InputHelloReply& out);
// `server_codec` is the ONE codec this server speaks (2, or 3 for a multi-seat game); the hello's
// [codec_min, codec_max] must contain it. Encode writes kInputHelloReplyV3Bytes for an accepting
// codec-3 reply that is `seated` and kInputHelloReplyBytes otherwise; decode accepts exactly those
// two shapes and nothing else (a 7-byte reply naming codec 2, or a refusal with a seat, is
// refused).
[[nodiscard]] InputHelloReply
select_input_codec(const InputHello& hello,
                   std::uint64_t server_schema_hash,
                   bool server_full,
                   std::uint8_t server_codec = kInputCodecV2) noexcept;

// FNV-1a 64 over the profile ID's bytes, followed by the version's four LITTLE-ENDIAN bytes.
// No terminator or length prefix enters the hash, so every platform hashes the same profile.
// Remote play also hashes the eight little-endian bytes of fixed_dt's double bit pattern: peers
// that integrate the same commands at different timesteps must refuse before sharing state.
// The two-argument form stays unchanged for m20.3 profiles without a remote simulation.
[[nodiscard]] std::uint64_t input_schema_hash(std::string_view profile_id,
                                              std::uint32_t profile_version) noexcept;
[[nodiscard]] std::uint64_t input_schema_hash(std::string_view profile_id,
                                              std::uint32_t profile_version,
                                              double fixed_dt) noexcept;

struct ScheduledInput {
    InputCommand command{};
    bool applied = false;
    bool synthesized = true;
    bool started = false;
    // Adaptive mode only: the buffer was empty, a neutral stand-in ran, and NOTHING was retired.
    bool stalled = false;
};

// ADR-0107. Fixed is ADR-0103's schedule: one sequence retires per tick whatever happened.
// Adaptive STALLS when the buffer is empty instead of retiring: a playout buffer that grows on
// underrun, so a late command is applied rather than discarded.
enum class InputBufferMode : std::uint8_t { Fixed, Adaptive };

struct InputResult {
    std::uint64_t server_tick = 0;
    std::uint32_t retired_through = 0;
    std::array<std::uint64_t, 2> applied_mask{};
    bool any_retired = false;
};

[[nodiscard]] std::size_t encode_input_result(const InputResult& result, std::span<std::byte> out);
[[nodiscard]] bool decode_input_result(std::span<const std::byte> in, InputResult& out);

// The server's buffer depth, sent to the client every tick on its own unreliable stream so the
// client can drain surplus by dilating its own tick (see input_pacing.hpp).
struct InputPacing {
    std::uint64_t server_tick = 0;
    std::uint8_t margin = 0;  // contiguous commands still held after this tick's step
    std::uint32_t stalls = 0; // running stall count (truncated to 32 bits)
};

[[nodiscard]] std::size_t encode_input_pacing(const InputPacing& pacing, std::span<std::byte> out);
[[nodiscard]] bool decode_input_pacing(std::span<const std::byte> in, InputPacing& out);

class InputSchedule {
public:
    // Six ticks are 100 ms at 60 Hz; after that much silence, repeating held movement is stale.
    explicit InputSchedule(std::uint32_t slack_ticks = 1,
                           std::uint32_t neutral_after_ticks = 6,
                           InputBufferMode mode = InputBufferMode::Fixed) noexcept;

    // The first accepted sequence fixes next_: client sequence numbers do not locate server time.
    void offer(std::span<const InputCommand> commands);
    // Exactly once per simulation tick. Before the buffer has STARTED (nothing yet, or fewer than
    // `slack_ticks` commands accepted) it returns a neutral command with `started == false`,
    // retires nothing and sets no mask bit -- those ticks count toward `synthesized()` too, so "the
    // server ran a stand-in" has one counter whichever reason it had. Once started, every call
    // retires exactly one sequence, applied or synthesized, which is what makes `retired_through`
    // advance with server time even when the client goes silent.
    [[nodiscard]] ScheduledInput step() noexcept;
    [[nodiscard]] InputResult result(std::uint64_t server_tick) const noexcept;
    [[nodiscard]] InputPacing pacing(std::uint64_t server_tick) const noexcept;

    [[nodiscard]] std::uint64_t accepted() const noexcept { return accepted_; }

    [[nodiscard]] std::uint64_t late() const noexcept { return late_; }

    [[nodiscard]] std::uint64_t future() const noexcept { return future_; }

    [[nodiscard]] std::uint64_t duplicate() const noexcept { return duplicate_; }

    [[nodiscard]] std::uint64_t applied() const noexcept { return applied_; }

    [[nodiscard]] std::uint64_t synthesized() const noexcept { return synthesized_; }

    [[nodiscard]] std::uint64_t neutralized() const noexcept { return neutralized_; }

    // A late command whose retired slot ran a stand-in: input that genuinely missed its tick.
    // `late()` also counts the harmless redundant copies every packet carries; this does not.
    [[nodiscard]] std::uint64_t late_missed() const noexcept { return late_missed_; }

    // Late commands whose slot is older than the 128-bit mask (or was never retired): neither
    // redundant nor missed can be proven.
    [[nodiscard]] std::uint64_t late_unclassified() const noexcept { return late_unclassified_; }

    [[nodiscard]] std::uint64_t stalls() const noexcept { return stalls_; }

    // Gaps retired as synthesized in adaptive mode (a later command was held, so waiting is
    // futile).
    [[nodiscard]] std::uint64_t gaps_retired() const noexcept { return gaps_retired_; }

    // Contiguous held commands at next_ after the latest step, capped at 255; and its high-water
    // mark.
    [[nodiscard]] std::uint8_t margin() const noexcept { return margin_; }

    [[nodiscard]] std::uint8_t max_margin() const noexcept { return max_margin_; }

    [[nodiscard]] InputBufferMode mode() const noexcept { return mode_; }

private:
    void record_margin() noexcept;

    static constexpr std::size_t kRing = 64;
    std::array<InputCommand, kRing> commands_{};
    std::array<bool, kRing> held_{};
    std::array<std::uint64_t, 2> applied_mask_{};
    InputCommand last_applied_{};
    std::uint64_t next_ = 0;
    std::uint32_t slack_ticks_ = 1;
    std::uint32_t neutral_after_ticks_ = 6;
    std::uint32_t silence_ticks_ = 0;
    bool has_first_ = false;
    bool started_ = false;
    bool has_last_applied_ = false;
    bool any_retired_ = false;
    std::uint64_t accepted_ = 0;
    std::uint64_t late_ = 0;
    std::uint64_t future_ = 0;
    std::uint64_t duplicate_ = 0;
    std::uint64_t applied_ = 0;
    std::uint64_t synthesized_ = 0;
    std::uint64_t neutralized_ = 0;
    std::uint64_t late_missed_ = 0;
    std::uint64_t late_unclassified_ = 0;
    std::uint64_t stalls_ = 0;
    std::uint64_t gaps_retired_ = 0;
    std::uint64_t retired_count_ = 0;
    InputBufferMode mode_ = InputBufferMode::Fixed;
    std::uint8_t margin_ = 0;
    std::uint8_t max_margin_ = 0;
};

class InputWindow {
public:
    // Refuse a sequence that does not follow the last push, even after that command was retired.
    void push(const InputCommand& command);
    [[nodiscard]] std::span<const InputCommand> packet() const noexcept;
    void apply_result(const InputResult& result);

    [[nodiscard]] std::span<const InputCommand> unretired() const noexcept { return commands_; }

    [[nodiscard]] std::span<const std::uint32_t> unapplied_sequences() const noexcept {
        return unapplied_sequences_;
    }

    [[nodiscard]] std::uint64_t rejected_sequence() const noexcept { return rejected_sequence_; }

    [[nodiscard]] std::uint64_t evicted() const noexcept { return evicted_; }

    [[nodiscard]] std::uint64_t retired_applied() const noexcept { return retired_applied_; }

    [[nodiscard]] std::uint64_t retired_unapplied() const noexcept { return retired_unapplied_; }

    [[nodiscard]] std::uint64_t retired_unknown() const noexcept { return retired_unknown_; }

    [[nodiscard]] std::uint64_t stale_results() const noexcept { return stale_results_; }

private:
    std::vector<InputCommand> commands_;
    std::vector<std::uint32_t> unapplied_sequences_;
    std::uint32_t last_sequence_ = 0;
    std::uint32_t last_retired_ = 0;
    bool has_last_sequence_ = false;
    bool has_last_retired_ = false;
    std::uint64_t rejected_sequence_ = 0;
    std::uint64_t evicted_ = 0;
    std::uint64_t retired_applied_ = 0;
    std::uint64_t retired_unapplied_ = 0;
    std::uint64_t retired_unknown_ = 0;
    std::uint64_t stale_results_ = 0;
};

} // namespace rime::replication
