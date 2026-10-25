// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "rime/replication/input_v2.hpp"

// Client-side prediction with server reconciliation restores an authoritative checkpoint, then
// replays unretired commands (ADR-0104 §1 and §3). Generation-safe IDs and an epoch prevent
// restoring the wrong set; group state and complete membership keep one simulation step coherent;
// bounded history and replay trigger reseeding instead of silently losing commands; the execution
// mask is diagnostic because both applied and synthesized retired commands must stay retired. Only
// a checkpoint prunes prediction history: a standalone InputResult belongs to InputWindow's
// retransmission path and may be newer than the state being restored here.
namespace rime::replication {

inline constexpr std::uint8_t kOwnerCheckpointTag = 0xE4;
inline constexpr std::size_t kCheckpointMaxEntities = 16;
inline constexpr std::size_t kCheckpointMaxStateBytes = 512;
inline constexpr std::size_t kCheckpointMaxGroupBytes = 512;
inline constexpr std::size_t kCheckpointMaxMessageBytes = 1200;

struct PredictedEntityId {
    std::uint32_t index = 0;
    std::uint32_t generation = 0;
    [[nodiscard]] bool operator==(const PredictedEntityId&) const = default;
};

struct PredictedEntityState {
    PredictedEntityId id{};
    std::vector<std::byte> state;
};

struct OwnerCheckpoint {
    std::uint64_t server_tick = 0;
    std::uint32_t membership_epoch = 0;
    InputResult result{};
    std::vector<PredictedEntityState> entities;
    std::vector<std::byte> group_state;
};

enum class CheckpointRefusal {
    None,
    Truncated,
    BadLength,
    BadTag,
    TooManyEntities,
    EntityStateTooLarge,
    GroupStateTooLarge,
    MessageTooLarge,
    InvalidInputResult,
    DuplicateEntity,
    TickMismatch,
};

struct CheckpointEncode {
    std::size_t bytes_written = 0;
    CheckpointRefusal refusal = CheckpointRefusal::None;

    [[nodiscard]] explicit operator bool() const noexcept {
        return refusal == CheckpointRefusal::None;
    }
};

// Encoding writes nothing on refusal. Decoding replaces out only after the entire message
// validates.
[[nodiscard]] CheckpointEncode encode_owner_checkpoint(const OwnerCheckpoint& checkpoint,
                                                       std::span<std::byte> out);
[[nodiscard]] CheckpointRefusal decode_owner_checkpoint(std::span<const std::byte> in,
                                                        OwnerCheckpoint& out);
// Decodes one checkpoint from the FRONT of `in` and reports how many bytes it used, leaving any
// trailing bytes to the caller (m20.5's negotiated spawn extension, ADR-0105 §2). Every bound and
// refusal is decode_owner_checkpoint's -- including the 1200-byte bound on the whole of `in` --
// except that trailing bytes are not a BadLength. `out` and `consumed` change only on success.
[[nodiscard]] CheckpointRefusal decode_owner_checkpoint_prefix(std::span<const std::byte> in,
                                                               OwnerCheckpoint& out,
                                                               std::size_t& consumed);

// What one predicted step is told, beyond the command (ADR-0104 §1). Bundled so a later brick can
// add a field without breaking every game's hook.
struct PredictionStep {
    // WHICH tick this step produces, on the SERVER's timeline: a checkpoint is the state at
    // `server_tick`, and the first unretired command it is replayed with produces `server_tick +
    // 1`. It is the address of the step's query environment -- a game that raycasts or overlaps
    // during a predicted step must answer against the world AS OF THIS TICK (its authoritative
    // mirror), never against interpolated render poses, or a replay hits a wall that was somewhere
    // else then. It is not the input sequence and not a replication version; ADR-0104 §3 keeps the
    // three apart.
    std::uint64_t sim_tick = 0;
    // The game's fixed timestep, passed rather than assumed so prediction and the server cannot
    // integrate with different dt.
    double fixed_dt = 0.0;
    // True only while replaying after a checkpoint: suppress sounds, particles and anything else a
    // player must not experience twice.
    bool replaying = false;
};

struct PredictionHooks {
    // Capture/restore must cover all simulation state, including hidden per-entity and group state.
    // Hooks are synchronous and must not throw; restore is called only after complete validation.
    std::function<std::vector<std::byte>(PredictedEntityId)> capture;
    std::function<void(PredictedEntityId, std::span<const std::byte>)> restore;
    std::function<std::vector<std::byte>()> capture_group;
    std::function<void(std::span<const std::byte>)> restore_group;
    // One deterministic fixed step over the whole set (ADR-0104 §1).
    std::function<
        void(std::span<const PredictedEntityId>, const InputCommand&, const PredictionStep&)>
        simulate;
};

struct ReconcilerBudget {
    std::size_t max_history = 128;
    std::size_t max_replay = 64;
};

struct ReconcileOutcome {
    bool applied = false;
    // The checkpoint was taken as a reseed (history had overflowed or a replay exceeded its
    // budget): restored whole, nothing replayed.
    bool reseeded = false;
    std::size_t replayed = 0;
    std::size_t retired_applied = 0;
    std::size_t retired_synthesized = 0;
};

class Reconciler {
public:
    Reconciler(PredictionHooks hooks, double fixed_dt, ReconcilerBudget budget = {});
    void set_membership(std::uint32_t epoch, std::vector<PredictedEntityId> members);
    void predict(const InputCommand& command);
    [[nodiscard]] ReconcileOutcome apply_checkpoint(const OwnerCheckpoint& checkpoint);

    [[nodiscard]] bool needs_reseed() const noexcept { return needs_reseed_; }

    // Caller has restored a full authoritative state; old commands cannot be replayed across it.
    void reseeded() noexcept;

    [[nodiscard]] std::size_t last_correction_bytes_changed() const noexcept {
        return last_correction_bytes_changed_;
    }

    [[nodiscard]] std::uint64_t corrections() const noexcept { return corrections_; }

    [[nodiscard]] std::uint64_t stale_checkpoints() const noexcept { return stale_checkpoints_; }

    [[nodiscard]] std::uint64_t epoch_mismatch() const noexcept { return epoch_mismatch_; }

    [[nodiscard]] std::uint64_t incomplete_checkpoints() const noexcept {
        return incomplete_checkpoints_;
    }

    [[nodiscard]] std::uint64_t history_overflow() const noexcept { return history_overflow_; }

    [[nodiscard]] std::uint64_t replay_budget_exceeded() const noexcept {
        return replay_budget_exceeded_;
    }

    [[nodiscard]] std::uint64_t reseeds() const noexcept { return reseeds_; }

private:
    PredictionHooks hooks_;
    ReconcilerBudget budget_;
    double fixed_dt_ = 0.0;
    std::vector<PredictedEntityId> members_;
    std::vector<InputCommand> history_;
    std::uint32_t epoch_ = 0;
    std::uint64_t next_sim_tick_ = 0;
    std::uint64_t last_server_tick_ = 0;
    bool has_membership_ = false;
    bool has_checkpoint_ = false;
    bool needs_reseed_ = false;
    std::size_t last_correction_bytes_changed_ = 0;
    std::uint64_t corrections_ = 0;
    std::uint64_t stale_checkpoints_ = 0;
    std::uint64_t epoch_mismatch_ = 0;
    std::uint64_t incomplete_checkpoints_ = 0;
    std::uint64_t history_overflow_ = 0;
    std::uint64_t replay_budget_exceeded_ = 0;
    std::uint64_t reseeds_ = 0;
};

} // namespace rime::replication
