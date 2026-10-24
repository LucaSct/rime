// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/replication/prediction.hpp"

using namespace rime::replication;

namespace {

constexpr PredictedEntityId kPlayer{7, 3};
constexpr PredictedEntityId kOther{8, 1};
constexpr float kDt = 2.0f;

std::vector<std::byte> state(float position) {
    const auto bits = std::bit_cast<std::uint32_t>(position);
    return {std::byte(bits & 0xFF),
            std::byte((bits >> 8) & 0xFF),
            std::byte((bits >> 16) & 0xFF),
            std::byte((bits >> 24) & 0xFF)};
}

float position(std::span<const std::byte> bytes) {
    REQUIRE(bytes.size() == 4);
    std::uint32_t bits = 0;
    for (unsigned i = 0; i < 4; ++i)
        bits |= std::uint32_t{std::to_integer<std::uint8_t>(bytes[i])} << (8 * i);
    return std::bit_cast<float>(bits);
}

InputCommand command(std::uint32_t sequence, float move = 1.0f) {
    InputCommand value{};
    value.sequence = sequence;
    value.move_x = move;
    return value;
}

OwnerCheckpoint checkpoint(std::uint64_t tick,
                           std::uint32_t retired,
                           float server_position,
                           std::uint64_t mask = 1) {
    OwnerCheckpoint value{};
    value.server_tick = tick;
    value.membership_epoch = 4;
    value.result = {tick, retired, {mask, 0}, true};
    value.entities.push_back({kPlayer, state(server_position)});
    value.group_state = {std::byte{9}};
    return value;
}

std::vector<std::byte> wire(const OwnerCheckpoint& value) {
    std::vector<std::byte> bytes(kCheckpointMaxMessageBytes);
    const auto result = encode_owner_checkpoint(value, bytes);
    REQUIRE(result);
    bytes.resize(result.bytes_written);
    return bytes;
}

void refused(std::span<const std::byte> bytes, CheckpointRefusal reason) {
    OwnerCheckpoint out = checkpoint(99, 2, 17.0f);
    CHECK(decode_owner_checkpoint(bytes, out) == reason);
    CHECK(out.server_tick == 99);
    CHECK(out.entities.size() == 1);
    CHECK(position(out.entities[0].state) == 17.0f);
}

struct Game {
    float x = 0.0f;
    std::byte group{0};
    std::vector<bool> replay_flags;
    std::vector<std::uint64_t> ticks;
    std::size_t restores = 0;

    PredictionHooks hooks() {
        return {[this](PredictedEntityId id) {
                    CHECK(id == kPlayer);
                    return state(x);
                },
                [this](PredictedEntityId id, std::span<const std::byte> bytes) {
                    CHECK(id == kPlayer);
                    x = position(bytes);
                    ++restores;
                },
                [this] { return std::vector<std::byte>{group}; },
                [this](std::span<const std::byte> bytes) {
                    REQUIRE(bytes.size() == 1);
                    group = bytes[0];
                    ++restores;
                },
                [this](std::span<const PredictedEntityId> members,
                       const InputCommand& input,
                       const PredictionStep& step) {
                    REQUIRE(members.size() == 1);
                    CHECK(members[0] == kPlayer);
                    CHECK(step.fixed_dt == doctest::Approx(kDt));
                    x += input.move_x * static_cast<float>(step.fixed_dt);
                    replay_flags.push_back(step.replaying);
                    ticks.push_back(step.sim_tick);
                }};
    }
};

} // namespace

TEST_CASE("owner checkpoint round-trips little-endian fields and InputResult") {
    auto value = checkpoint(0x0102030405060708ull, 12, 4.0f, 0b101);
    value.entities.push_back({kOther, {std::byte{0xAB}}});
    const auto bytes = wire(value);
    CHECK(std::to_integer<std::uint8_t>(bytes[0]) == 0xE4);
    CHECK(std::to_integer<std::uint8_t>(bytes[1]) == 0x08);
    CHECK(std::to_integer<std::uint8_t>(bytes[8]) == 0x01);
    CHECK(std::to_integer<std::uint8_t>(bytes[13]) == 2);
    CHECK(std::to_integer<std::uint8_t>(bytes[16]) == kInputResultTag);
    OwnerCheckpoint out{};
    REQUIRE(decode_owner_checkpoint(bytes, out) == CheckpointRefusal::None);
    CHECK(out.server_tick == value.server_tick);
    CHECK(out.membership_epoch == 4);
    CHECK(out.result.server_tick == value.server_tick);
    CHECK(out.result.retired_through == 12);
    CHECK(out.result.applied_mask[0] == 0b101);
    REQUIRE(out.entities.size() == 2);
    CHECK(out.entities[0].id == kPlayer);
    CHECK(position(out.entities[0].state) == 4.0f);
    CHECK(out.entities[1].id == kOther);
    CHECK(out.group_state == value.group_state);
}

TEST_CASE("every checkpoint refusal leaves decode output untouched") {
    const auto good = wire(checkpoint(10, 1, 2.0f));
    refused(std::span{good}.first(45), CheckpointRefusal::Truncated);
    refused(std::span{good}.first(good.size() - 1), CheckpointRefusal::Truncated);
    auto bad = good;
    bad.push_back(std::byte{0});
    refused(bad, CheckpointRefusal::BadLength);
    bad = good;
    bad[0] = std::byte{0xE3};
    refused(bad, CheckpointRefusal::BadTag);
    bad = good;
    bad[13] = std::byte{17};
    refused(bad, CheckpointRefusal::TooManyEntities);
    bad = good;
    bad[54] = std::byte{1};
    bad[55] = std::byte{2}; // 513 bytes
    refused(bad, CheckpointRefusal::EntityStateTooLarge);
    bad = good;
    bad[14] = std::byte{1};
    bad[15] = std::byte{2}; // 513 bytes
    refused(bad, CheckpointRefusal::GroupStateTooLarge);
    bad = good;
    bad.resize(1201);
    refused(bad, CheckpointRefusal::MessageTooLarge);
    bad = good;
    bad[16] = std::byte{0};
    refused(bad, CheckpointRefusal::InvalidInputResult);
    bad = good;
    bad[18] = std::byte{11};
    refused(bad, CheckpointRefusal::TickMismatch);
    auto duplicate = checkpoint(10, 1, 2.0f);
    duplicate.entities.push_back(duplicate.entities[0]);
    bad = wire(checkpoint(10, 1, 2.0f));
    std::array<std::byte, kCheckpointMaxMessageBytes> output{};
    CHECK(encode_owner_checkpoint(duplicate, output).refusal == CheckpointRefusal::DuplicateEntity);
    // A second entry with the same ID is invalid on the wire as well.
    bad[13] = std::byte{2};
    bad.insert(bad.end() - 1, good.begin() + 46, good.begin() + 60);
    refused(bad, CheckpointRefusal::DuplicateEntity);

    auto oversized = checkpoint(10, 1, 2.0f);
    oversized.entities[0].state.resize(513);
    CHECK(encode_owner_checkpoint(oversized, output).refusal ==
          CheckpointRefusal::EntityStateTooLarge);
    oversized = checkpoint(10, 1, 2.0f);
    oversized.group_state.resize(513);
    CHECK(encode_owner_checkpoint(oversized, output).refusal ==
          CheckpointRefusal::GroupStateTooLarge);
    oversized = checkpoint(10, 1, 2.0f);
    oversized.entities.resize(17);
    CHECK(encode_owner_checkpoint(oversized, output).refusal == CheckpointRefusal::TooManyEntities);
    oversized = checkpoint(10, 1, 2.0f);
    oversized.entities[0].state.resize(512);
    oversized.group_state.resize(512);
    oversized.entities.push_back({kOther, std::vector<std::byte>(512)});
    CHECK(encode_owner_checkpoint(oversized, output).refusal == CheckpointRefusal::MessageTooLarge);
    oversized = checkpoint(10, 1, 2.0f);
    oversized.result.server_tick = 11;
    CHECK(encode_owner_checkpoint(oversized, output).refusal == CheckpointRefusal::TickMismatch);
    CHECK(encode_owner_checkpoint(checkpoint(10, 1, 2.0f), std::span{output}.first(1)).refusal ==
          CheckpointRefusal::Truncated);
}

TEST_CASE("perfect prediction replays without correction and marks replaying") {
    Game game;
    Reconciler reconciler(game.hooks(), kDt);
    reconciler.set_membership(4, {kPlayer});
    reconciler.predict(command(1));
    reconciler.predict(command(2));
    reconciler.predict(command(3));
    const auto result = reconciler.apply_checkpoint(checkpoint(20, 1, 2.0f));
    CHECK(result.applied);
    CHECK(result.replayed == 2);
    CHECK(result.retired_applied == 1);
    CHECK(result.retired_synthesized == 0);
    CHECK(game.x == 6.0f);
    CHECK(reconciler.corrections() == 0);
    CHECK(reconciler.last_correction_bytes_changed() == 0);
    CHECK(game.replay_flags == std::vector<bool>{false, false, false, true, true});
    CHECK(game.ticks == std::vector<std::uint64_t>{0, 1, 2, 21, 22});
}

TEST_CASE("divergent authority corrects exact forward state and stale/reordered ticks do nothing") {
    Game game;
    Reconciler reconciler(game.hooks(), kDt);
    reconciler.set_membership(4, {kPlayer});
    reconciler.predict(command(1));
    reconciler.predict(command(2));
    reconciler.predict(command(3));
    const auto applied = reconciler.apply_checkpoint(checkpoint(20, 1, 5.0f));
    CHECK(applied.applied);
    CHECK(applied.replayed == 2);
    CHECK(game.x == 9.0f); // server 5 + commands 2 and 3 at dt=2
    CHECK(reconciler.corrections() == 1);
    CHECK(reconciler.last_correction_bytes_changed() > 0);
    const auto restores = game.restores;
    CHECK_FALSE(reconciler.apply_checkpoint(checkpoint(20, 2, 100.0f)).applied);
    CHECK_FALSE(reconciler.apply_checkpoint(checkpoint(19, 2, 100.0f)).applied);
    CHECK(reconciler.stale_checkpoints() == 2);
    CHECK(game.x == 9.0f);
    CHECK(game.restores == restores);
}

TEST_CASE("epoch mismatch and incomplete membership restore nothing") {
    Game game;
    Reconciler reconciler(game.hooks(), kDt);
    reconciler.set_membership(4, {kPlayer});
    game.x = 7.0f;
    auto wrong_epoch = checkpoint(10, 1, 100.0f);
    wrong_epoch.membership_epoch = 5;
    CHECK_FALSE(reconciler.apply_checkpoint(wrong_epoch).applied);
    CHECK(reconciler.epoch_mismatch() == 1);
    auto missing = checkpoint(10, 1, 100.0f);
    missing.entities.clear();
    CHECK_FALSE(reconciler.apply_checkpoint(missing).applied);
    auto extra = checkpoint(10, 1, 100.0f);
    extra.entities.push_back({kOther, state(100.0f)});
    CHECK_FALSE(reconciler.apply_checkpoint(extra).applied);
    CHECK(reconciler.incomplete_checkpoints() == 2);
    CHECK(game.x == 7.0f);
    CHECK(game.group == std::byte{0});
    CHECK(game.restores == 0);
}

TEST_CASE("a synthesized retired command is never replayed") {
    Game game;
    Reconciler reconciler(game.hooks(), kDt);
    reconciler.set_membership(4, {kPlayer});
    reconciler.predict(command(1));
    reconciler.predict(command(2));
    reconciler.predict(command(3));
    const auto result = reconciler.apply_checkpoint(checkpoint(10, 2, 2.0f, 0b10));
    CHECK(result.applied);
    CHECK(result.retired_applied == 1);
    CHECK(result.retired_synthesized == 1);
    CHECK(result.replayed == 1);
    CHECK(game.x == 4.0f);
    CHECK(game.replay_flags == std::vector<bool>{false, false, false, true});
}

TEST_CASE("history and replay budgets require an authoritative reseed") {
    Game game;
    Reconciler reconciler(game.hooks(), kDt, {2, 1});
    reconciler.set_membership(4, {kPlayer});
    reconciler.predict(command(1));
    reconciler.predict(command(2));
    reconciler.predict(command(3));
    CHECK(reconciler.needs_reseed());
    CHECK(reconciler.history_overflow() == 1);
    // A checkpoint arriving while a reseed is owed IS the reseed: restored whole, nothing replayed,
    // counted -- never dropped silently.
    const auto reseed = reconciler.apply_checkpoint(checkpoint(10, 1, 2.0f));
    CHECK(reseed.applied);
    CHECK(reseed.reseeded);
    CHECK(reseed.replayed == 0);
    CHECK(game.x == 2.0f);
    CHECK_FALSE(reconciler.needs_reseed());
    CHECK(reconciler.reseeds() == 1);
    // The caller-driven path still exists for a reseed delivered some other way.
    reconciler.reseeded();
    CHECK(reconciler.reseeds() == 2);
    reconciler.predict(command(4));
    reconciler.predict(command(5));
    const auto result = reconciler.apply_checkpoint(checkpoint(11, 3, 6.0f));
    CHECK(result.applied);
    CHECK(result.replayed == 0);
    CHECK(reconciler.needs_reseed());
    CHECK(reconciler.replay_budget_exceeded() == 1);
    CHECK(game.x == 6.0f);
}
