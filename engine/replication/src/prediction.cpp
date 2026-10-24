// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/prediction.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace rime::replication {
namespace {

constexpr std::size_t kOwnerCheckpointHeaderBytes = 1 + 8 + 4 + 1 + 2 + kInputResultBytes;

struct Writer {
    std::span<std::byte> bytes;
    std::size_t at = 0;

    void u8(std::uint8_t value) { bytes[at++] = static_cast<std::byte>(value); }

    void u16(std::uint16_t value) {
        for (unsigned i = 0; i < 2; ++i)
            u8(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    void u32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    void u64(std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i)
            u8(static_cast<std::uint8_t>(value >> (8 * i)));
    }

    void blob(std::span<const std::byte> value) {
        std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
        at += value.size();
    }
};

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at = 0;

    [[nodiscard]] bool has(std::size_t count) const { return count <= bytes.size() - at; }

    std::uint8_t u8() { return std::to_integer<std::uint8_t>(bytes[at++]); }

    std::uint16_t u16() {
        std::uint16_t value = 0;
        for (unsigned i = 0; i < 2; ++i)
            value |= std::uint16_t{u8()} << (8 * i);
        return value;
    }

    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= std::uint32_t{u8()} << (8 * i);
        return value;
    }

    std::uint64_t u64() {
        std::uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i)
            value |= std::uint64_t{u8()} << (8 * i);
        return value;
    }

    std::span<const std::byte> blob(std::size_t count) {
        const auto value = bytes.subspan(at, count);
        at += count;
        return value;
    }
};

[[nodiscard]] bool duplicate_id(std::span<const PredictedEntityState> entities) {
    for (std::size_t i = 0; i < entities.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            if (entities[i].id == entities[j].id)
                return true;
        }
    }
    return false;
}

[[nodiscard]] std::size_t bytes_changed(std::span<const std::byte> before,
                                        std::span<const std::byte> after) {
    const auto common = std::min(before.size(), after.size());
    std::size_t changed = std::max(before.size(), after.size()) - common;
    for (std::size_t i = 0; i < common; ++i)
        changed += before[i] != after[i];
    return changed;
}

} // namespace

CheckpointEncode encode_owner_checkpoint(const OwnerCheckpoint& checkpoint,
                                         std::span<std::byte> out) {
    if (checkpoint.entities.size() > kCheckpointMaxEntities)
        return {0, CheckpointRefusal::TooManyEntities};
    if (checkpoint.group_state.size() > kCheckpointMaxGroupBytes)
        return {0, CheckpointRefusal::GroupStateTooLarge};
    if (checkpoint.result.server_tick != checkpoint.server_tick)
        return {0, CheckpointRefusal::TickMismatch};
    if (duplicate_id(checkpoint.entities))
        return {0, CheckpointRefusal::DuplicateEntity};
    std::size_t needed = kOwnerCheckpointHeaderBytes + checkpoint.group_state.size();
    for (const auto& entity : checkpoint.entities) {
        if (entity.state.size() > kCheckpointMaxStateBytes)
            return {0, CheckpointRefusal::EntityStateTooLarge};
        needed += 10 + entity.state.size();
    }
    if (needed > kCheckpointMaxMessageBytes)
        return {0, CheckpointRefusal::MessageTooLarge};
    if (out.size() < needed)
        return {0, CheckpointRefusal::Truncated};

    Writer writer{out};
    writer.u8(kOwnerCheckpointTag);
    writer.u64(checkpoint.server_tick);
    writer.u32(checkpoint.membership_epoch);
    writer.u8(static_cast<std::uint8_t>(checkpoint.entities.size()));
    writer.u16(static_cast<std::uint16_t>(checkpoint.group_state.size()));
    std::array<std::byte, kInputResultBytes> result_bytes{};
    if (encode_input_result(checkpoint.result, result_bytes) != result_bytes.size())
        return {0, CheckpointRefusal::InvalidInputResult};
    writer.blob(result_bytes);
    for (const auto& entity : checkpoint.entities) {
        writer.u32(entity.id.index);
        writer.u32(entity.id.generation);
        writer.u16(static_cast<std::uint16_t>(entity.state.size()));
        writer.blob(entity.state);
    }
    writer.blob(checkpoint.group_state);
    return {writer.at, CheckpointRefusal::None};
}

CheckpointRefusal decode_owner_checkpoint_prefix(std::span<const std::byte> in,
                                                  OwnerCheckpoint& out,
                                                  std::size_t& consumed) {
    if (in.size() > kCheckpointMaxMessageBytes)
        return CheckpointRefusal::MessageTooLarge;
    if (in.size() < kOwnerCheckpointHeaderBytes)
        return CheckpointRefusal::Truncated;
    Reader reader{in};
    if (reader.u8() != kOwnerCheckpointTag)
        return CheckpointRefusal::BadTag;
    OwnerCheckpoint decoded{};
    decoded.server_tick = reader.u64();
    decoded.membership_epoch = reader.u32();
    const auto count = reader.u8();
    if (count > kCheckpointMaxEntities)
        return CheckpointRefusal::TooManyEntities;
    const auto group_size = reader.u16();
    if (group_size > kCheckpointMaxGroupBytes)
        return CheckpointRefusal::GroupStateTooLarge;
    if (!decode_input_result(reader.blob(kInputResultBytes), decoded.result))
        return CheckpointRefusal::InvalidInputResult;
    if (decoded.result.server_tick != decoded.server_tick)
        return CheckpointRefusal::TickMismatch;
    decoded.entities.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (!reader.has(10))
            return CheckpointRefusal::Truncated;
        PredictedEntityState entity{};
        entity.id = {reader.u32(), reader.u32()};
        const auto state_size = reader.u16();
        if (state_size > kCheckpointMaxStateBytes)
            return CheckpointRefusal::EntityStateTooLarge;
        if (!reader.has(state_size))
            return CheckpointRefusal::Truncated;
        const auto state = reader.blob(state_size);
        entity.state.assign(state.begin(), state.end());
        for (const auto& previous : decoded.entities) {
            if (previous.id == entity.id)
                return CheckpointRefusal::DuplicateEntity;
        }
        decoded.entities.push_back(std::move(entity));
    }
    if (!reader.has(group_size))
        return CheckpointRefusal::Truncated;
    const auto group = reader.blob(group_size);
    decoded.group_state.assign(group.begin(), group.end());
    consumed = reader.at;
    out = std::move(decoded);
    return CheckpointRefusal::None;
}

CheckpointRefusal decode_owner_checkpoint(std::span<const std::byte> in, OwnerCheckpoint& out) {
    // The m20.4 contract, unchanged: the checkpoint must be the WHOLE message. Trailing bytes are a
    // BadLength refusal and `out` is untouched -- which is also why ADR-0105's spawn extension has
    // to be negotiated rather than merely appended (spawn_prediction.hpp).
    OwnerCheckpoint decoded{};
    std::size_t consumed = 0;
    const auto refusal = decode_owner_checkpoint_prefix(in, decoded, consumed);
    if (refusal != CheckpointRefusal::None)
        return refusal;
    if (consumed != in.size())
        return CheckpointRefusal::BadLength;
    out = std::move(decoded);
    return CheckpointRefusal::None;
}

Reconciler::Reconciler(PredictionHooks hooks, double fixed_dt, ReconcilerBudget budget)
    : hooks_(std::move(hooks)), budget_(budget), fixed_dt_(fixed_dt) {}

void Reconciler::set_membership(std::uint32_t epoch, std::vector<PredictedEntityId> members) {
    epoch_ = epoch;
    members_ = std::move(members);
    has_membership_ = true;
}

void Reconciler::predict(const InputCommand& command) {
    hooks_.simulate(members_, command, PredictionStep{next_sim_tick_++, fixed_dt_, false});
    if (history_.size() >= budget_.max_history) {
        ++history_overflow_;
        needs_reseed_ = true;
        return;
    }
    history_.push_back(command);
}

ReconcileOutcome Reconciler::apply_checkpoint(const OwnerCheckpoint& checkpoint) {
    ReconcileOutcome outcome{};
    if (has_checkpoint_ && checkpoint.server_tick <= last_server_tick_) {
        ++stale_checkpoints_;
        return outcome;
    }
    if (!has_membership_ || checkpoint.membership_epoch != epoch_) {
        ++epoch_mismatch_;
        return outcome;
    }
    if (checkpoint.result.server_tick != checkpoint.server_tick ||
        checkpoint.entities.size() != members_.size() || duplicate_id(checkpoint.entities)) {
        ++incomplete_checkpoints_;
        return outcome;
    }
    for (const auto& member : members_) {
        if (std::count_if(checkpoint.entities.begin(),
                          checkpoint.entities.end(),
                          [&](const auto& entity) { return entity.id == member; }) != 1) {
            ++incomplete_checkpoints_;
            return outcome;
        }
    }
    if (needs_reseed_) {
        // A checkpoint IS the predicted set's full authoritative state, which is exactly what a
        // reseed needs -- so rather than dropping it (a skip path with no counter, which guardrail
        // 5 forbids, and a client stuck until someone remembers to call reseeded()), take it whole:
        // restore, forget the history that overflowed, replay nothing. The player snaps to where
        // the server has it; prediction resumes from the next command.
        hooks_.restore_group(checkpoint.group_state);
        for (const auto& entity : checkpoint.entities)
            hooks_.restore(entity.id, entity.state);
        history_.clear();
        needs_reseed_ = false;
        ++reseeds_;
        last_server_tick_ = checkpoint.server_tick;
        has_checkpoint_ = true;
        next_sim_tick_ = checkpoint.server_tick + 1;
        outcome.applied = true;
        outcome.reseeded = true;
        return outcome;
    }

    std::vector<std::vector<std::byte>> before;
    before.reserve(members_.size());
    for (const auto& member : members_)
        before.push_back(hooks_.capture(member));

    hooks_.restore_group(checkpoint.group_state);
    for (const auto& entity : checkpoint.entities)
        hooks_.restore(entity.id, entity.state);
    if (checkpoint.result.any_retired) {
        const auto first_live =
            std::find_if(history_.begin(), history_.end(), [&](const auto& cmd) {
                return cmd.sequence > checkpoint.result.retired_through;
            });
        for (auto it = history_.begin(); it != first_live; ++it) {
            const auto offset = checkpoint.result.retired_through - it->sequence;
            if (offset < 128 &&
                (checkpoint.result.applied_mask[offset / 64] & (std::uint64_t{1} << (offset % 64))))
                ++outcome.retired_applied;
            else if (offset < 128)
                ++outcome.retired_synthesized;
        }
        history_.erase(history_.begin(), first_live);
    }
    last_server_tick_ = checkpoint.server_tick;
    has_checkpoint_ = true;
    outcome.applied = true;
    if (history_.size() > budget_.max_replay) {
        ++replay_budget_exceeded_;
        needs_reseed_ = true;
    } else {
        auto sim_tick = checkpoint.server_tick + 1;
        for (const auto& command : history_) {
            hooks_.simulate(members_, command, PredictionStep{sim_tick++, fixed_dt_, true});
            ++outcome.replayed;
        }
        next_sim_tick_ = sim_tick;
    }
    last_correction_bytes_changed_ = 0;
    for (std::size_t i = 0; i < members_.size(); ++i) {
        last_correction_bytes_changed_ += bytes_changed(before[i], hooks_.capture(members_[i]));
    }
    corrections_ += last_correction_bytes_changed_ != 0;
    return outcome;
}

void Reconciler::reseeded() noexcept {
    history_.clear();
    needs_reseed_ = false;
    ++reseeds_;
}

} // namespace rime::replication
