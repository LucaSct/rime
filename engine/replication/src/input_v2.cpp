// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/input_v2.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace rime::replication {
namespace {

// Like input.cpp's ByteWriter/ByteReader, these helpers spell out little-endian fields instead of
// copying structs. The leaf needs only spans and the standard library, so it keeps them local.
struct Writer {
    std::span<std::byte> bytes;
    std::size_t at = 0;

    void u8(std::uint8_t value) { bytes[at++] = static_cast<std::byte>(value); }

    void u32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) {
            u8(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }

    void u64(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) {
            u8(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }

    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
};

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at = 0;

    std::uint8_t u8() { return std::to_integer<std::uint8_t>(bytes[at++]); }

    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) {
            value |= std::uint32_t{u8()} << (8 * i);
        }
        return value;
    }

    std::uint64_t u64() {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i) {
            value |= std::uint64_t{u8()} << (8 * i);
        }
        return value;
    }

    float f32() { return std::bit_cast<float>(u32()); }
};

[[nodiscard]] bool finite(const InputCommand& command) noexcept {
    return std::isfinite(command.move_x) && std::isfinite(command.move_y) &&
           std::isfinite(command.yaw) && std::isfinite(command.pitch);
}

} // namespace

std::size_t encode_input_v2(std::span<const InputCommand> oldest_first, std::span<std::byte> out) {
    const std::size_t count = oldest_first.size();
    if (count == 0 || count > kInputV2MaxCommands ||
        out.size() < kInputV2HeaderBytes + count * kInputV2CommandBytes) {
        return 0;
    }
    Writer writer{out};
    writer.u8(kInputV2Tag);
    writer.u8(kInputCodecV2);
    writer.u8(static_cast<std::uint8_t>(count));
    writer.u8(0); // reserved stays zero until the protocol gives it a meaning
    for (const InputCommand& command : oldest_first) {
        writer.u32(command.sequence);
        writer.f32(command.move_x);
        writer.f32(command.move_y);
        writer.f32(command.yaw);
        writer.f32(command.pitch);
        writer.u32(command.held);
        writer.u32(command.pressed);
    }
    return writer.at;
}

InputV2Decode decode_input_v2(std::span<const std::byte> in, std::vector<InputCommand>& out) {
    if (in.size() < kInputV2HeaderBytes) {
        return {InputV2Refusal::Truncated};
    }
    Reader reader{in};
    const std::uint8_t tag = reader.u8();
    const std::uint8_t codec = reader.u8();
    const std::uint8_t count = reader.u8();
    const std::uint8_t reserved = reader.u8();
    // No BadTag reason exists in this contract; a foreign tag is an unknown V2 codec here.
    if (tag != kInputV2Tag || codec != kInputCodecV2) {
        return {InputV2Refusal::UnknownCodec};
    }
    if (count == 0 || count > kInputV2MaxCommands) {
        return {InputV2Refusal::BadCount};
    }
    if (reserved != 0) {
        return {InputV2Refusal::ReservedNonZero};
    }
    const std::size_t expected = kInputV2HeaderBytes + count * kInputV2CommandBytes;
    if (in.size() < expected) {
        return {InputV2Refusal::Truncated};
    }
    if (in.size() > expected) {
        return {InputV2Refusal::BadLength};
    }

    // Validate into a temporary: a late NaN or bad sequence must not append the earlier commands.
    std::vector<InputCommand> decoded;
    decoded.reserve(count);
    for (std::uint8_t i = 0; i < count; ++i) {
        InputCommand command{};
        command.sequence = reader.u32();
        command.move_x = reader.f32();
        command.move_y = reader.f32();
        command.yaw = reader.f32();
        command.pitch = reader.f32();
        command.held = reader.u32();
        command.pressed = reader.u32();
        if (!finite(command)) {
            return {InputV2Refusal::NonFinite};
        }
        // Use a wider sum: UINT32_MAX followed by zero is wraparound, not a later command.
        if (!decoded.empty() &&
            std::uint64_t{command.sequence} != std::uint64_t{decoded.back().sequence} + 1) {
            return {InputV2Refusal::SequenceOrder};
        }
        decoded.push_back(command);
    }
    out.insert(out.end(), decoded.begin(), decoded.end());
    return {};
}

std::size_t encode_input_hello(const InputHello& hello, std::span<std::byte> out) {
    if (out.size() < kInputHelloBytes) {
        return 0;
    }
    Writer writer{out};
    writer.u8(kInputHelloTag);
    writer.u8(hello.codec_min);
    writer.u8(hello.codec_max);
    writer.u64(hello.schema_hash);
    return writer.at;
}

bool decode_input_hello(std::span<const std::byte> in, InputHello& out) {
    if (in.size() != kInputHelloBytes || std::to_integer<std::uint8_t>(in[0]) != kInputHelloTag) {
        return false;
    }
    Reader reader{in};
    (void)reader.u8();
    InputHello decoded{};
    decoded.codec_min = reader.u8();
    decoded.codec_max = reader.u8();
    decoded.schema_hash = reader.u64();
    out = decoded;
    return true;
}

std::size_t encode_input_hello_reply(const InputHelloReply& reply, std::span<std::byte> out) {
    const bool with_seat =
        reply.codec == kInputCodecV3 && reply.refusal == InputHelloRefusal::None && reply.seated;
    const std::size_t size = with_seat ? kInputHelloReplyV3Bytes : kInputHelloReplyBytes;
    if (out.size() < size) {
        return 0;
    }
    Writer writer{out};
    writer.u8(kInputHelloReplyTag);
    writer.u8(reply.codec);
    writer.u8(static_cast<std::uint8_t>(reply.refusal));
    if (with_seat) {
        writer.u8(static_cast<std::uint8_t>(reply.seat));
        writer.u8(static_cast<std::uint8_t>(reply.seat >> 8));
        writer.u8(static_cast<std::uint8_t>(reply.seat_generation));
        writer.u8(static_cast<std::uint8_t>(reply.seat_generation >> 8));
    }
    return writer.at;
}

bool decode_input_hello_reply(std::span<const std::byte> in, InputHelloReply& out) {
    if ((in.size() != kInputHelloReplyBytes && in.size() != kInputHelloReplyV3Bytes) ||
        std::to_integer<std::uint8_t>(in[0]) != kInputHelloReplyTag) {
        return false;
    }
    Reader reader{in};
    (void)reader.u8();
    InputHelloReply decoded{};
    decoded.codec = reader.u8();
    const std::uint8_t refusal = reader.u8();
    if (refusal > static_cast<std::uint8_t>(InputHelloRefusal::GameRefused)) {
        return false;
    }
    decoded.refusal = static_cast<InputHelloRefusal>(refusal);
    if (in.size() == kInputHelloReplyV3Bytes) {
        // The seat exists only on a codec-3 acceptance; any other 7-byte shape is malformed
        // rather than "a reply with a seat we ignore", so the two ends cannot half-agree.
        if (decoded.codec != kInputCodecV3 || decoded.refusal != InputHelloRefusal::None) {
            return false;
        }
        const auto u16 = [&reader] {
            const std::uint16_t low = reader.u8();
            return static_cast<std::uint16_t>(low | (std::uint16_t{reader.u8()} << 8));
        };
        decoded.seated = true;
        decoded.seat = u16();
        decoded.seat_generation = u16();
    }
    out = decoded;
    return true;
}

InputHelloReply select_input_codec(const InputHello& hello,
                                   std::uint64_t server_schema_hash,
                                   bool server_full,
                                   std::uint8_t server_codec) noexcept {
    if (hello.codec_min > server_codec || hello.codec_max < server_codec) {
        return {0, InputHelloRefusal::Version};
    }
    if (hello.schema_hash != server_schema_hash) {
        return {0, InputHelloRefusal::Schema};
    }
    if (server_full) {
        return {0, InputHelloRefusal::Full};
    }
    return {server_codec, InputHelloRefusal::None};
}

std::uint64_t input_schema_hash(std::string_view profile_id,
                                std::uint32_t profile_version) noexcept {
    constexpr std::uint64_t kOffset = 14695981039346656037ull;
    constexpr std::uint64_t kPrime = 1099511628211ull;
    std::uint64_t hash = kOffset;
    for (unsigned char byte : profile_id) {
        hash = (hash ^ byte) * kPrime;
    }
    for (unsigned i = 0; i < 4; ++i) {
        hash = (hash ^ ((profile_version >> (8 * i)) & 0xFFu)) * kPrime;
    }
    return hash;
}

std::uint64_t input_schema_hash(std::string_view profile_id,
                                std::uint32_t profile_version,
                                double fixed_dt) noexcept {
    constexpr std::uint64_t kPrime = 1099511628211ull;
    auto hash = input_schema_hash(profile_id, profile_version);
    const auto bits = std::bit_cast<std::uint64_t>(fixed_dt);
    for (unsigned i = 0; i < 8; ++i)
        hash = (hash ^ ((bits >> (8 * i)) & 0xFFu)) * kPrime;
    return hash;
}

std::size_t encode_input_result(const InputResult& result, std::span<std::byte> out) {
    if (out.size() < kInputResultBytes) {
        return 0;
    }
    Writer writer{out};
    writer.u8(kInputResultTag);
    // ADR-0103 lists the three data fields; this flag preserves "nothing retired" even if the
    // first client sequence is zero. Without it a decoded result would have a false frontier.
    writer.u8(result.any_retired ? 1 : 0);
    writer.u64(result.server_tick);
    writer.u32(result.retired_through);
    writer.u64(result.applied_mask[0]);
    writer.u64(result.applied_mask[1]);
    return writer.at;
}

bool decode_input_result(std::span<const std::byte> in, InputResult& out) {
    if (in.size() != kInputResultBytes || std::to_integer<std::uint8_t>(in[0]) != kInputResultTag) {
        return false;
    }
    Reader reader{in};
    (void)reader.u8();
    InputResult decoded{};
    const std::uint8_t any_retired = reader.u8();
    if (any_retired > 1) {
        return false;
    }
    decoded.any_retired = any_retired == 1;
    decoded.server_tick = reader.u64();
    decoded.retired_through = reader.u32();
    decoded.applied_mask[0] = reader.u64();
    decoded.applied_mask[1] = reader.u64();
    out = decoded;
    return true;
}

std::size_t encode_input_pacing(const InputPacing& pacing, std::span<std::byte> out) {
    if (out.size() < kInputPacingBytes) {
        return 0;
    }
    Writer writer{out};
    writer.u8(kInputPacingTag);
    writer.u64(pacing.server_tick);
    writer.u8(pacing.margin);
    writer.u32(pacing.stalls);
    return writer.at;
}

bool decode_input_pacing(std::span<const std::byte> in, InputPacing& out) {
    if (in.size() != kInputPacingBytes || std::to_integer<std::uint8_t>(in[0]) != kInputPacingTag) {
        return false;
    }
    Reader reader{in};
    (void)reader.u8();
    InputPacing decoded{};
    decoded.server_tick = reader.u64();
    decoded.margin = reader.u8();
    decoded.stalls = reader.u32();
    out = decoded;
    return true;
}

InputSchedule::InputSchedule(std::uint32_t slack_ticks,
                             std::uint32_t neutral_after_ticks,
                             InputBufferMode mode) noexcept
    : slack_ticks_(slack_ticks), neutral_after_ticks_(neutral_after_ticks), mode_(mode) {}

void InputSchedule::record_margin() noexcept {
    // Walk the ring from next_ while slots are held. held_ is only true for sequences in
    // [next_, next_ + kRing), so the walk is bounded by the ring and the 255 cap never binds.
    std::size_t run = 0;
    while (run < kRing && held_[static_cast<std::size_t>((next_ + run) % kRing)]) {
        ++run;
    }
    margin_ = static_cast<std::uint8_t>(std::min<std::size_t>(run, 255));
    max_margin_ = std::max(max_margin_, margin_);
}

void InputSchedule::offer(std::span<const InputCommand> commands) {
    for (const InputCommand& command : commands) {
        if (!has_first_) {
            next_ = command.sequence;
            has_first_ = true;
        }
        if (std::uint64_t{command.sequence} < next_) {
            ++late_;
            // Classify with the execution mask: if the slot this command belonged to ran a
            // stand-in (bit 0), the command truly missed its tick; if it was applied, this is just
            // one of the two redundant copies every packet carries. A slot older than the mask, or
            // one that was never retired (it precedes the first accepted sequence), cannot be told.
            const std::uint64_t offset = next_ - 1 - std::uint64_t{command.sequence};
            if (offset >= 128 || offset >= retired_count_) {
                ++late_unclassified_;
            } else if ((applied_mask_[offset / 64] & (std::uint64_t{1} << (offset % 64))) == 0) {
                ++late_missed_;
            }
        } else if (std::uint64_t{command.sequence} >= next_ + kRing) {
            ++future_;
        } else {
            const std::size_t slot = command.sequence % kRing;
            if (held_[slot]) {
                ++duplicate_;
            } else {
                commands_[slot] = command;
                held_[slot] = true;
                ++accepted_;
            }
        }
    }
}

ScheduledInput InputSchedule::step() noexcept {
    if (!has_first_ || (!started_ && accepted_ < slack_ticks_)) {
        ++synthesized_;
        record_margin();
        return {};
    }
    started_ = true;
    ScheduledInput scheduled{};
    scheduled.started = true;
    scheduled.command.sequence = static_cast<std::uint32_t>(next_);
    const std::size_t slot = static_cast<std::size_t>(next_ % kRing);
    bool gap = false;
    if (!held_[slot] && mode_ == InputBufferMode::Adaptive) {
        gap = false;
        for (std::size_t i = 1; i < kRing; ++i) {
            if (held_[static_cast<std::size_t>((next_ + i) % kRing)]) {
                gap = true;
                break;
            }
        }
        if (!gap) {
            // STALL. Nothing at or after next_ is here: the client is behind or silent. Retiring
            // now would doom the command to arrive "late", so run a NEUTRAL stand-in (not the held
            // axes: the player's path then stays a function of the command sequence alone) and
            // leave next_, the mask, silence_ticks_ and any_retired_ untouched.
            scheduled.stalled = true;
            ++synthesized_;
            ++stalls_;
            record_margin();
            return scheduled;
        }
        ++gaps_retired_;
    }
    if (held_[slot]) {
        scheduled.command = commands_[slot];
        scheduled.applied = true;
        scheduled.synthesized = false;
        held_[slot] = false;
        last_applied_ = scheduled.command;
        has_last_applied_ = true;
        silence_ticks_ = 0;
        ++applied_;
    } else {
        ++synthesized_;
        if (silence_ticks_ < std::numeric_limits<std::uint32_t>::max()) {
            ++silence_ticks_;
        }
        if (neutral_after_ticks_ == 0 || silence_ticks_ >= neutral_after_ticks_) {
            ++neutralized_;
        } else if (has_last_applied_) {
            scheduled.command = last_applied_;
            scheduled.command.sequence = static_cast<std::uint32_t>(next_);
            scheduled.command.pressed = 0; // edges belong only to the tick that actually ran them
        }
    }
    // Shift history on retirement, regardless of application. Bit zero describes the newest tick.
    applied_mask_[1] = (applied_mask_[1] << 1) | (applied_mask_[0] >> 63);
    applied_mask_[0] = (applied_mask_[0] << 1) | (scheduled.applied ? 1u : 0u);
    any_retired_ = true;
    ++retired_count_;
    ++next_;
    record_margin();
    return scheduled;
}

InputPacing InputSchedule::pacing(std::uint64_t server_tick) const noexcept {
    return {server_tick, margin_, static_cast<std::uint32_t>(stalls_ & 0xFFFFFFFFu)};
}

InputResult InputSchedule::result(std::uint64_t server_tick) const noexcept {
    return {server_tick,
            static_cast<std::uint32_t>(any_retired_ ? next_ - 1 : 0),
            applied_mask_,
            any_retired_};
}

void InputWindow::push(const InputCommand& command) {
    if (has_last_sequence_ && (last_sequence_ == std::numeric_limits<std::uint32_t>::max() ||
                               command.sequence != last_sequence_ + 1)) {
        ++rejected_sequence_;
        return;
    }
    has_last_sequence_ = true;
    last_sequence_ = command.sequence;
    if (commands_.size() == 128) {
        commands_.erase(commands_.begin());
        ++evicted_; // an unanswered server cannot make the client's history grow without bound
    }
    commands_.push_back(command);
}

std::span<const InputCommand> InputWindow::packet() const noexcept {
    if (commands_.empty()) {
        return {};
    }
    const std::size_t count = std::min<std::size_t>(3, commands_.size());
    return {commands_.data() + (commands_.size() - count), count};
}

void InputWindow::apply_result(const InputResult& result) {
    if (!result.any_retired) {
        if (has_last_retired_) {
            ++stale_results_;
        }
        return;
    }
    if (has_last_retired_ && result.retired_through <= last_retired_) {
        ++stale_results_;
        return;
    }
    has_last_retired_ = true;
    last_retired_ = result.retired_through;
    unapplied_sequences_.clear();
    const auto first_live = std::find_if(commands_.begin(), commands_.end(), [&](const auto& cmd) {
        return cmd.sequence > result.retired_through;
    });
    for (auto it = commands_.begin(); it != first_live; ++it) {
        const std::uint32_t offset = result.retired_through - it->sequence;
        if (offset >= 128) {
            ++retired_unknown_;
        } else if ((result.applied_mask[offset / 64] & (std::uint64_t{1} << (offset % 64))) != 0) {
            ++retired_applied_;
        } else {
            ++retired_unapplied_;
            unapplied_sequences_.push_back(it->sequence);
        }
    }
    commands_.erase(commands_.begin(), first_live);
}

} // namespace rime::replication
