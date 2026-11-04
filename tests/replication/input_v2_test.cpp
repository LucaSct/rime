// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "rime/replication/input_v2.hpp"

using namespace rime::replication;

// Keep the production leaf free of MessageTag while proving these values do not collide with the
// two tags input.cpp handles today.
static_assert(kInputV2Tag != static_cast<std::uint8_t>(MessageTag::InputCommands));
static_assert(kInputV2Tag != static_cast<std::uint8_t>(MessageTag::InputAck));
static_assert(kInputHelloTag != static_cast<std::uint8_t>(MessageTag::InputCommands));
static_assert(kInputHelloTag != static_cast<std::uint8_t>(MessageTag::InputAck));
static_assert(kInputHelloReplyTag != static_cast<std::uint8_t>(MessageTag::InputCommands));
static_assert(kInputHelloReplyTag != static_cast<std::uint8_t>(MessageTag::InputAck));
static_assert(kInputResultTag != static_cast<std::uint8_t>(MessageTag::InputCommands));
static_assert(kInputResultTag != static_cast<std::uint8_t>(MessageTag::InputAck));

namespace {

InputCommand command(std::uint32_t sequence) {
    InputCommand value{};
    value.sequence = sequence;
    value.move_x = 0.5f;
    value.move_y = -1.0f;
    value.yaw = 2.0f;
    value.pitch = 1.0f;
    value.held = 0xAABBCCDDu;
    value.pressed = 0x11223344u;
    return value;
}

std::vector<std::byte> packet(std::size_t count) {
    std::vector<InputCommand> commands;
    for (std::size_t i = 0; i < count; ++i) {
        commands.push_back(command(static_cast<std::uint32_t>(100 + i)));
    }
    std::vector<std::byte> bytes(kInputV2HeaderBytes + count * kInputV2CommandBytes);
    CHECK(encode_input_v2(commands, bytes) == bytes.size());
    return bytes;
}

void check_refusal(std::span<const std::byte> bytes, InputV2Refusal expected) {
    std::vector<InputCommand> out{command(77)};
    const auto result = decode_input_v2(bytes, out);
    CHECK(result.refusal == expected);
    CHECK(out.size() == 1);
    CHECK(out[0].sequence == 77);
    CHECK(out[0].held == 0xAABBCCDDu);
}

} // namespace

TEST_CASE("InputV2 round-trips one, three, and sixteen commands") {
    for (const std::size_t count : {1u, 3u, 16u}) {
        std::vector<std::byte> bytes = packet(count);
        std::vector<InputCommand> out;
        REQUIRE(decode_input_v2(bytes, out));
        REQUIRE(out.size() == count);
        for (std::size_t i = 0; i < count; ++i) {
            CHECK(out[i].sequence == 100 + i);
            CHECK(out[i].move_x == 0.5f);
            CHECK(out[i].move_y == -1.0f);
            CHECK(out[i].yaw == 2.0f);
            CHECK(out[i].pitch == 1.0f);
            CHECK(out[i].held == 0xAABBCCDDu);
            CHECK(out[i].pressed == 0x11223344u);
        }
    }
}

TEST_CASE("InputV2 writes the specified little-endian bytes") {
    const InputCommand value = command(0x01020304u);
    std::array<std::byte, kInputV2HeaderBytes + kInputV2CommandBytes> bytes{};
    REQUIRE(encode_input_v2(std::span{&value, 1}, bytes) == bytes.size());
    const std::array<std::uint8_t, 32> expected{
        0xE0, 0x02, 0x01, 0x00, // envelope
        0x04, 0x03, 0x02, 0x01, // sequence
        0x00, 0x00, 0x00, 0x3F, // 0.5f
        0x00, 0x00, 0x80, 0xBF, // -1.0f
        0x00, 0x00, 0x00, 0x40, // 2.0f
        0x00, 0x00, 0x80, 0x3F, // 1.0f
        0xDD, 0xCC, 0xBB, 0xAA, // held
        0x44, 0x33, 0x22, 0x11, // pressed
    };
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        CHECK(std::to_integer<std::uint8_t>(bytes[i]) == expected[i]);
    }
    std::array<std::byte, 31> short_output{};
    CHECK(encode_input_v2(std::span{&value, 1}, short_output) == 0);
    CHECK(encode_input_v2({}, bytes) == 0);
    std::array<InputCommand, 17> too_many{};
    CHECK(encode_input_v2(too_many, bytes) == 0);
}

TEST_CASE("every InputV2 refusal leaves the output untouched") {
    const std::vector<std::byte> good = packet(1);
    check_refusal(std::span{good}.first(3), InputV2Refusal::Truncated);
    check_refusal(std::span{good}.first(good.size() - 1), InputV2Refusal::Truncated);

    auto bad = good;
    bad.push_back(std::byte{0});
    check_refusal(bad, InputV2Refusal::BadLength);
    bad = good;
    bad[2] = std::byte{0};
    check_refusal(bad, InputV2Refusal::BadCount);
    bad[2] = std::byte{17};
    check_refusal(bad, InputV2Refusal::BadCount);
    bad = good;
    bad[1] = std::byte{3};
    check_refusal(bad, InputV2Refusal::UnknownCodec);
    bad = good;
    bad[3] = std::byte{1};
    check_refusal(bad, InputV2Refusal::ReservedNonZero);
    bad = packet(2);
    // +infinity in command two's first f32, at offset 36: command one parsed before refusal.
    bad[36] = std::byte{0};
    bad[37] = std::byte{0};
    bad[38] = std::byte{0x80};
    bad[39] = std::byte{0x7F};
    check_refusal(bad, InputV2Refusal::NonFinite);
    bad = packet(2);
    bad[32] = std::byte{105}; // second sequence is 105, not 101
    check_refusal(bad, InputV2Refusal::SequenceOrder);
}

TEST_CASE("hello, reply, selection, and schema hash are deterministic") {
    const auto hash = input_schema_hash("move", 0x01020304u);
    CHECK(hash == input_schema_hash("move", 0x01020304u));
    CHECK(hash != input_schema_hash("move", 0x01020305u));
    CHECK(hash != input_schema_hash("Move", 0x01020304u));
    // Independently calculated FNV-1a 64 for ASCII 'a' then version bytes 04 03 02 01.
    CHECK(input_schema_hash("a", 0x01020304u) == 0x08025DDD07B099A8ull);
    const auto remote_hash = input_schema_hash("move", 0x01020304u, 1.0 / 60.0);
    CHECK(remote_hash == input_schema_hash("move", 0x01020304u, 1.0 / 60.0));
    CHECK(remote_hash != input_schema_hash("move", 0x01020304u, 1.0 / 30.0));
    CHECK(remote_hash != hash);
    // Independently calculated from ASCII 'a', 04 03 02 01, then the LE bits of 1/60.
    CHECK(input_schema_hash("a", 0x01020304u, 1.0 / 60.0) == 0x953BE3A916F55B6Aull);

    const InputHello hello{1, 3, hash};
    std::array<std::byte, kInputHelloBytes> bytes{};
    REQUIRE(encode_input_hello(hello, bytes) == bytes.size());
    InputHello decoded{};
    REQUIRE(decode_input_hello(bytes, decoded));
    CHECK(decoded.codec_min == 1);
    CHECK(decoded.codec_max == 3);
    CHECK(decoded.schema_hash == hash);
    CHECK_FALSE(decode_input_hello(std::span{bytes}.first(10), decoded));
    CHECK_FALSE(decode_input_hello(std::vector<std::byte>(12), decoded));

    const InputHelloReply selected = select_input_codec(hello, hash, false);
    CHECK(selected.codec == 2);
    CHECK(selected.refusal == InputHelloRefusal::None);
    CHECK(select_input_codec({3, 4, hash}, hash, false).refusal == InputHelloRefusal::Version);
    CHECK(select_input_codec(hello, hash + 1, false).refusal == InputHelloRefusal::Schema);
    CHECK(select_input_codec(hello, hash, true).refusal == InputHelloRefusal::Full);
    std::array<std::byte, kInputHelloReplyBytes> reply_bytes{};
    REQUIRE(encode_input_hello_reply(selected, reply_bytes) == reply_bytes.size());
    InputHelloReply reply{};
    REQUIRE(decode_input_hello_reply(reply_bytes, reply));
    CHECK(reply.codec == 2);
    CHECK(reply.refusal == InputHelloRefusal::None);
    CHECK_FALSE(decode_input_hello_reply(std::span{reply_bytes}.first(2), reply));
    CHECK_FALSE(decode_input_hello_reply(std::vector<std::byte>(4), reply));
}

TEST_CASE("hello reply v3 carries a seat only on a codec-3 acceptance (m20.8b)") {
    const std::uint64_t hash = 0x1234;
    // A multi-seat server speaks codec 3 only: a codec-2 client is a version refusal, and a
    // client offering both is answered in 3.
    CHECK(select_input_codec({2, 2, hash}, hash, false, kInputCodecV3).refusal ==
          InputHelloRefusal::Version);
    CHECK(select_input_codec({2, 3, hash}, hash, false, kInputCodecV3).codec == kInputCodecV3);
    // ...and a one-seat server keeps codec 2: a codec-3-only client is refused as version.
    CHECK(select_input_codec({3, 3, hash}, hash, false).refusal == InputHelloRefusal::Version);
    CHECK(select_input_codec({2, 3, hash}, hash, false).codec == kInputCodecV2);

    InputHelloReply seated{};
    seated.codec = kInputCodecV3;
    seated.seated = true;
    seated.seat = 0x0102;
    seated.seat_generation = 0xA0B0;
    std::array<std::byte, kInputHelloReplyV3Bytes> bytes{};
    REQUIRE(encode_input_hello_reply(seated, bytes) == kInputHelloReplyV3Bytes);
    // Little-endian seat and generation after m20.3's three bytes.
    CHECK(bytes[3] == std::byte{0x02});
    CHECK(bytes[4] == std::byte{0x01});
    CHECK(bytes[5] == std::byte{0xB0});
    CHECK(bytes[6] == std::byte{0xA0});
    InputHelloReply decoded{};
    REQUIRE(decode_input_hello_reply(bytes, decoded));
    CHECK(decoded.codec == kInputCodecV3);
    CHECK(decoded.refusal == InputHelloRefusal::None);
    CHECK(decoded.seated);
    CHECK(decoded.seat == 0x0102);
    CHECK(decoded.seat_generation == 0xA0B0);
    // An output too small for the seat writes nothing rather than a truncated reply.
    std::array<std::byte, kInputHelloReplyBytes> short_out{};
    CHECK(encode_input_hello_reply(seated, short_out) == 0);

    // A refusal never carries a seat and stays 3 bytes, so every client can decode it -- the new
    // seat refusals included.
    for (const auto refusal : {InputHelloRefusal::Full,
                               InputHelloRefusal::DuplicateEndpoint,
                               InputHelloRefusal::RateLimited,
                               InputHelloRefusal::GameRefused}) {
        InputHelloReply refused{};
        refused.refusal = refusal;
        refused.seated = true; // ignored: a refusal has no seat
        REQUIRE(encode_input_hello_reply(refused, bytes) == kInputHelloReplyBytes);
        InputHelloReply out{};
        REQUIRE(decode_input_hello_reply(std::span{bytes}.first(kInputHelloReplyBytes), out));
        CHECK(out.refusal == refusal);
        CHECK_FALSE(out.seated);
    }
    // A codec-2 acceptance stays m20.3's 3 bytes even if asked to carry a seat.
    InputHelloReply v2{};
    v2.codec = kInputCodecV2;
    v2.seated = true;
    CHECK(encode_input_hello_reply(v2, bytes) == kInputHelloReplyBytes);

    // Seven bytes that are not a codec-3 acceptance are malformed, not "a seat we ignore".
    REQUIRE(encode_input_hello_reply(seated, bytes) == kInputHelloReplyV3Bytes);
    auto wrong_codec = bytes;
    wrong_codec[1] = std::byte{kInputCodecV2};
    CHECK_FALSE(decode_input_hello_reply(wrong_codec, decoded));
    auto refusal_with_seat = bytes;
    refusal_with_seat[2] = std::byte{static_cast<std::uint8_t>(InputHelloRefusal::Full)};
    CHECK_FALSE(decode_input_hello_reply(refusal_with_seat, decoded));
    // Unknown refusal codes and other lengths stay refused.
    auto unknown = bytes;
    unknown[2] = std::byte{static_cast<std::uint8_t>(InputHelloRefusal::GameRefused) + 1};
    CHECK_FALSE(decode_input_hello_reply(std::span{unknown}.first(3), decoded));
    CHECK_FALSE(decode_input_hello_reply(std::span{bytes}.first(5), decoded));
    CHECK_FALSE(decode_input_hello_reply(std::vector<std::byte>(8), decoded));
}

TEST_CASE("schedule waits for slack then applies in order with an execution mask") {
    InputSchedule schedule(2);
    CHECK_FALSE(schedule.step().started);
    const InputCommand first = command(20);
    schedule.offer(std::span{&first, 1});
    CHECK_FALSE(schedule.step().started);
    CHECK_FALSE(schedule.result(3).any_retired);
    const InputCommand second = command(21);
    schedule.offer(std::span{&second, 1});
    const ScheduledInput a = schedule.step();
    const ScheduledInput b = schedule.step();
    CHECK(a.started);
    CHECK(a.applied);
    CHECK(a.command.sequence == 20);
    CHECK(b.applied);
    CHECK(b.command.sequence == 21);
    CHECK(schedule.applied() == 2);
    CHECK(schedule.accepted() == 2);
    const InputResult result = schedule.result(9);
    CHECK(result.any_retired);
    CHECK(result.server_tick == 9);
    CHECK(result.retired_through == 21);
    CHECK(result.applied_mask[0] == 3);
}

TEST_CASE("schedule synthesizes a missing command, clears its edge, and rejects its late copy") {
    InputSchedule schedule;
    const InputCommand first = command(50);
    const InputCommand third = command(52);
    schedule.offer(std::span{&first, 1});
    schedule.offer(std::span{&third, 1});
    REQUIRE(schedule.step().applied);
    const ScheduledInput missing = schedule.step();
    CHECK(missing.started);
    CHECK(missing.synthesized);
    CHECK_FALSE(missing.applied);
    CHECK(missing.command.sequence == 51);
    CHECK(missing.command.move_x == first.move_x);
    CHECK(missing.command.held == first.held);
    CHECK(missing.command.pressed == 0);
    REQUIRE(schedule.step().applied);
    const InputResult result = schedule.result(3);
    CHECK(result.retired_through == 52);
    CHECK(result.applied_mask[0] == 0b101);
    const InputCommand late = command(51);
    schedule.offer(std::span{&late, 1});
    CHECK(schedule.late() == 1);
    CHECK(schedule.applied() == 2);
}

TEST_CASE("schedule neutralizes after six silent ticks and counts duplicate and future") {
    InputSchedule schedule;
    const InputCommand first = command(1);
    const InputCommand far = command(65);
    schedule.offer(std::span{&first, 1});
    schedule.offer(std::span{&first, 1});
    schedule.offer(std::span{&far, 1});
    CHECK(schedule.duplicate() == 1);
    CHECK(schedule.future() == 1);
    REQUIRE(schedule.step().applied);
    for (int i = 0; i < 5; ++i) {
        const ScheduledInput repeated = schedule.step();
        CHECK(repeated.command.held == first.held);
        CHECK(repeated.command.pressed == 0);
    }
    const ScheduledInput neutral = schedule.step();
    CHECK(neutral.synthesized);
    CHECK(neutral.command.held == 0);
    CHECK(neutral.command.move_x == 0.0f);
    CHECK(neutral.command.move_y == 0.0f);
    CHECK(schedule.neutralized() == 1);
}

TEST_CASE("window sends newest three without pruning and classifies retired commands") {
    InputWindow window;
    for (std::uint32_t seq = 10; seq <= 14; ++seq) {
        window.push(command(seq));
    }
    for (int repeat = 0; repeat < 4; ++repeat) {
        const auto sent = window.packet();
        REQUIRE(sent.size() == 3);
        CHECK(sent[0].sequence == 12);
        CHECK(sent[1].sequence == 13);
        CHECK(sent[2].sequence == 14);
        CHECK(window.unretired().size() == 5);
    }
    InputResult result{};
    result.any_retired = true;
    result.retired_through = 12;
    result.applied_mask[0] = 0b101; // 12 and 10 applied; 11 synthesized
    window.apply_result(result);
    REQUIRE(window.unretired().size() == 2);
    CHECK(window.unretired()[0].sequence == 13);
    CHECK(window.retired_applied() == 2);
    CHECK(window.retired_unapplied() == 1);
    REQUIRE(window.unapplied_sequences().size() == 1);
    CHECK(window.unapplied_sequences()[0] == 11);
    window.apply_result(result);
    CHECK(window.stale_results() == 1);
    CHECK(window.unretired().size() == 2);
    window.push(command(16)); // last push was 14, even though older commands were retired
    CHECK(window.rejected_sequence() == 1);
    CHECK(window.unretired().size() == 2);
}

TEST_CASE("window bounds history at 128 and exposes the mask boundary") {
    InputWindow window;
    for (std::uint32_t seq = 1; seq <= 129; ++seq) {
        window.push(command(seq));
    }
    CHECK(window.evicted() == 1);
    REQUIRE(window.unretired().size() == 128);
    CHECK(window.unretired()[0].sequence == 2);

    InputResult result{};
    result.any_retired = true;
    result.retired_through = 129;
    result.applied_mask[0] = (std::uint64_t{1} << 0) | (std::uint64_t{1} << 63);
    result.applied_mask[1] = (std::uint64_t{1} << 0) | (std::uint64_t{1} << 63);
    window.apply_result(result);
    CHECK(window.retired_applied() == 4); // offsets 0, 63, 64, 127
    CHECK(window.retired_unapplied() == 124);
    CHECK(window.retired_unknown() == 0);

    InputWindow older;
    older.push(command(1));
    InputResult distant{};
    distant.any_retired = true;
    distant.retired_through = 129; // offset 128 is outside both mask words
    distant.applied_mask = {~std::uint64_t{0}, ~std::uint64_t{0}};
    older.apply_result(distant);
    CHECK(older.retired_unknown() == 1);
    CHECK(older.retired_applied() == 0);
}

TEST_CASE("InputResult preserves the mask and retired flag on wire") {
    InputResult result{};
    result.server_tick = 0x0102030405060708ull;
    result.retired_through = 0;
    result.applied_mask = {1, std::uint64_t{1} << 63};
    result.any_retired = true;
    std::array<std::byte, kInputResultBytes> bytes{};
    REQUIRE(encode_input_result(result, bytes) == bytes.size());
    CHECK(std::to_integer<std::uint8_t>(bytes[0]) == kInputResultTag);
    CHECK(std::to_integer<std::uint8_t>(bytes[1]) == 1);
    InputResult decoded{};
    REQUIRE(decode_input_result(bytes, decoded));
    CHECK(decoded.any_retired);
    CHECK(decoded.retired_through == 0);
    CHECK(decoded.server_tick == result.server_tick);
    CHECK(decoded.applied_mask == result.applied_mask);
    CHECK_FALSE(decode_input_result(std::span{bytes}.first(29), decoded));
    CHECK_FALSE(decode_input_result(std::vector<std::byte>(31), decoded));
}

// ---- m20.7a (ADR-0107): late_missed, the adaptive schedule, margin, the pacing codec ----------

TEST_CASE("late_missed separates a redundant copy from a command that truly missed its slot") {
    InputSchedule schedule;
    const InputCommand a = command(10);
    schedule.offer(std::span{&a, 1});
    REQUIRE(schedule.step().applied);
    schedule.offer(std::span{&a, 1}); // redundant copy of an applied command
    CHECK(schedule.late() == 1);
    CHECK(schedule.late_missed() == 0);
    CHECK(schedule.late_unclassified() == 0);
    // The existing synthesis scenario: 11 never arrives in time, 12 does, then 11 shows up late.
    const InputCommand third = command(12);
    schedule.offer(std::span{&third, 1});
    REQUIRE_FALSE(schedule.step().applied); // 11 synthesized
    REQUIRE(schedule.step().applied);       // 12
    const InputCommand late = command(11);
    schedule.offer(std::span{&late, 1});
    CHECK(schedule.late() == 2);
    CHECK(schedule.late_missed() == 1);
}

TEST_CASE("late_unclassified counts a late command older than the 128-bit mask") {
    InputSchedule schedule;
    InputCommand c = command(0);
    schedule.offer(std::span{&c, 1});
    for (std::uint32_t seq = 1; seq < 200; ++seq) {
        (void)schedule.step();
        c = command(seq);
        schedule.offer(std::span{&c, 1});
    }
    (void)schedule.step();
    const InputCommand ancient = command(3);
    schedule.offer(std::span{&ancient, 1});
    CHECK(schedule.late_unclassified() == 1);
    CHECK(schedule.late_missed() == 0);
}

TEST_CASE("adaptive: a silent client stalls, retires nothing, and its late command is applied") {
    InputSchedule adaptive(1, 6, InputBufferMode::Adaptive);
    InputSchedule fixed(1, 6, InputBufferMode::Fixed);
    CHECK(adaptive.mode() == InputBufferMode::Adaptive);
    const InputCommand a = command(10);
    const InputCommand b = command(11);
    for (InputSchedule* s : {&adaptive, &fixed}) {
        s->offer(std::span{&a, 1});
        s->offer(std::span{&b, 1});
        REQUIRE(s->step().applied);
        REQUIRE(s->step().applied);
    }
    const InputResult before = adaptive.result(5);
    for (int i = 0; i < 3; ++i) {
        const ScheduledInput stalled = adaptive.step();
        CHECK(stalled.started);
        CHECK(stalled.stalled);
        CHECK(stalled.synthesized);
        CHECK_FALSE(stalled.applied);
        CHECK(stalled.command.sequence == 12);
        CHECK(stalled.command.held == 0); // neutral, not the held axes repeated
        CHECK(stalled.command.move_x == 0.0f);
    }
    const InputResult after = adaptive.result(5);
    CHECK(after.retired_through == before.retired_through);
    CHECK(after.applied_mask == before.applied_mask);
    CHECK(adaptive.stalls() == 3);
    CHECK(adaptive.neutralized() == 0); // silence_ticks_ untouched
    const InputCommand late = command(12);
    adaptive.offer(std::span{&late, 1});
    CHECK(adaptive.late() == 0);
    const ScheduledInput applied = adaptive.step();
    CHECK(applied.applied);
    CHECK_FALSE(applied.stalled);
    CHECK(applied.command.sequence == 12);
    CHECK(adaptive.late_missed() == 0);

    // Control: Fixed mode on the same script retires 12 synthesized, and the arrival is a miss.
    for (int i = 0; i < 3; ++i) {
        const ScheduledInput s = fixed.step();
        CHECK(s.synthesized);
        CHECK_FALSE(s.stalled);
    }
    CHECK(fixed.stalls() == 0);
    fixed.offer(std::span{&late, 1});
    CHECK(fixed.late_missed() == 1);
}

TEST_CASE("adaptive gap: a missing sequence with a later one held is retired synthesized") {
    InputSchedule schedule(1, 6, InputBufferMode::Adaptive);
    const InputCommand a = command(50);
    const InputCommand c = command(52);
    schedule.offer(std::span{&a, 1});
    schedule.offer(std::span{&c, 1});
    REQUIRE(schedule.step().applied);
    const ScheduledInput gap = schedule.step();
    CHECK(gap.synthesized);
    CHECK_FALSE(gap.stalled);
    CHECK(gap.command.sequence == 51);
    CHECK(gap.command.held == a.held); // the Fixed rules: held axes repeated
    REQUIRE(schedule.step().applied);
    CHECK(schedule.gaps_retired() == 1);
    CHECK(schedule.stalls() == 0);
    CHECK(schedule.result(1).retired_through == 52);
    CHECK(schedule.result(1).applied_mask[0] == 0b101);
}

TEST_CASE(
    "adaptive hitch: ten commands arriving at once after ten stalls are all applied in order") {
    InputSchedule schedule(1, 6, InputBufferMode::Adaptive);
    const InputCommand first = command(100);
    schedule.offer(std::span{&first, 1});
    REQUIRE(schedule.step().applied);
    for (int i = 0; i < 10; ++i) {
        REQUIRE(schedule.step().stalled);
    }
    CHECK(schedule.stalls() == 10);
    for (std::uint32_t seq = 101; seq <= 110; ++seq) {
        const InputCommand c = command(seq);
        schedule.offer(std::span{&c, 1});
    }
    for (std::uint32_t seq = 101; seq <= 110; ++seq) {
        const ScheduledInput s = schedule.step();
        REQUIRE(s.applied);
        CHECK(s.command.sequence == seq);
        if (seq == 101) {
            CHECK(schedule.margin() == 9); // the surplus the pacer will see
        }
    }
    CHECK(schedule.max_margin() == 9);
    CHECK(schedule.margin() == 0);
    CHECK(schedule.late_missed() == 0);
    CHECK(schedule.late() == 0);
}

TEST_CASE("adaptive blackout resynchronizes an empty ring and acknowledges skipped commands") {
    InputSchedule adaptive(1, 6, InputBufferMode::Adaptive);
    InputSchedule fixed(1, 6, InputBufferMode::Fixed);
    const InputCommand first = command(10);
    for (InputSchedule* schedule : {&adaptive, &fixed}) {
        schedule->offer(std::span{&first, 1});
        REQUIRE(schedule->step().applied);
    }
    for (int i = 0; i < 100; ++i) {
        CHECK(adaptive.step().stalled);
        CHECK_FALSE(fixed.step().stalled);
    }
    const InputCommand resumed = command(111);
    adaptive.offer(std::span{&resumed, 1});
    const InputResult frontier = adaptive.result(0);
    CHECK(frontier.retired_through == resumed.sequence - 1);
    CHECK(frontier.applied_mask[0] == 0);
    CHECK(frontier.applied_mask[1] == (std::uint64_t{1} << 36));
    CHECK(adaptive.resyncs() == 1);
    CHECK(adaptive.resync_retired() == 100);
    CHECK(adaptive.future() == 0);
    const ScheduledInput applied = adaptive.step();
    REQUIRE(applied.applied);
    CHECK(applied.command.sequence == resumed.sequence);

    // Fixed already retired the silent ticks, so the same arrival needs no resync.
    fixed.offer(std::span{&resumed, 1});
    CHECK(fixed.resyncs() == 0);
    CHECK(fixed.resync_retired() == 0);
    CHECK(fixed.result(0).retired_through == resumed.sequence - 1);
    CHECK(fixed.step().applied);
}

TEST_CASE("adaptive keeps a future command out while its ring still holds input") {
    InputSchedule schedule(1, 6, InputBufferMode::Adaptive);
    const InputCommand first = command(50);
    const InputCommand held = command(51);
    const InputCommand future = command(115);
    schedule.offer(std::span{&first, 1});
    schedule.offer(std::span{&held, 1});
    REQUIRE(schedule.step().applied);
    schedule.offer(std::span{&future, 1});
    CHECK(schedule.future() == 1);
    CHECK(schedule.resyncs() == 0);
    CHECK(schedule.resync_retired() == 0);
    CHECK(schedule.step().applied);
}

TEST_CASE("margin counts contiguous held commands at next, in both modes") {
    for (InputBufferMode mode : {InputBufferMode::Fixed, InputBufferMode::Adaptive}) {
        InputSchedule schedule(1, 6, mode);
        for (std::uint32_t seq : {20u, 21u, 22u, 24u}) {
            const InputCommand c = command(seq);
            schedule.offer(std::span{&c, 1});
        }
        (void)schedule.step(); // applies 20; 21, 22 contiguous, 23 missing
        CHECK(schedule.margin() == 2);
        CHECK(schedule.max_margin() == 2);
        (void)schedule.step();
        (void)schedule.step();
        CHECK(schedule.margin() == 0); // next_ = 23, not held
        CHECK(schedule.max_margin() == 2);
    }
}

TEST_CASE("InputPacing round-trips and refuses a wrong tag or length") {
    InputSchedule schedule(1, 6, InputBufferMode::Adaptive);
    const InputCommand c = command(1);
    schedule.offer(std::span{&c, 1});
    (void)schedule.step();
    (void)schedule.step(); // one stall
    const InputPacing sent = schedule.pacing(0x0102030405060708ull);
    CHECK(sent.stalls == 1);
    std::array<std::byte, kInputPacingBytes> bytes{};
    REQUIRE(encode_input_pacing(sent, bytes) == kInputPacingBytes);
    CHECK(std::to_integer<std::uint8_t>(bytes[0]) == kInputPacingTag);
    CHECK(std::to_integer<std::uint8_t>(bytes[1]) == 0x08); // little-endian server_tick
    InputPacing got{};
    REQUIRE(decode_input_pacing(bytes, got));
    CHECK(got.server_tick == sent.server_tick);
    CHECK(got.margin == sent.margin);
    CHECK(got.stalls == sent.stalls);
    auto wrong_tag = bytes;
    wrong_tag[0] = std::byte{kInputResultTag};
    CHECK_FALSE(decode_input_pacing(wrong_tag, got));
    CHECK_FALSE(decode_input_pacing(std::span{bytes}.first(kInputPacingBytes - 1), got));
    CHECK_FALSE(decode_input_pacing(std::vector<std::byte>(kInputPacingBytes + 1), got));
    std::array<std::byte, kInputPacingBytes - 1> small{};
    CHECK(encode_input_pacing(sent, small) == 0);
    static_assert(kInputPacingTag != kInputResultTag);
}
