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
