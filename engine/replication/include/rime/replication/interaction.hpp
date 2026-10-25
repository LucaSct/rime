// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/replication/prediction.hpp"

// Two small techniques make one predicted interaction object (ADR-0106 §2 and §4).
//
// 1. Server-declared membership with HYSTERESIS. A client may only predict an object the server has
//    put in its predicted set, and a set that flickers is worse than none: every join re-seeds the
//    object from a checkpoint and every leave drops it back to a stale mirror. So the policy joins
//    at one radius (contact distance plus how far the player can travel inside the replay budget,
//    so the object is already predicted before the player can reach it) and leaves at a LARGER
//    one, and even then only after the object has been at rest and untouched for a run of ticks.
//    A moving object therefore never leaves the set: leaving would hand a still-sliding crate back
//    to interpolation at exactly the moment it is hardest to match.
//
// 2. Correct the simulation, SMOOTH the presentation. When a checkpoint disagrees, the simulation
//    jumps to the authoritative answer and replays -- it never blends, or it would drift from the
//    server. What the player SEES is simulation + a visual error offset that starts at
//    (what was shown before) - (what the simulation now says) and decays exponentially, so a
//    correction is spread over a few frames instead of popping. The offset lives OUTSIDE
//    rewindable state (a restore must not undo it). Above a snap threshold the error is too large
//    to hide, so it is dropped and the object visibly jumps rather than sliding through the world.
//
// Pure leaf: stdlib, core math and PredictedEntityId only -- no ECS, physics, net or app code.
namespace rime::replication {

// ---------------------------------------------------------------------------- membership

struct InteractionParams {
    float contact_distance = 0.0f;
    float player_max_speed = 0.0f;
    std::uint32_t replay_ticks = 0;
    double fixed_dt = 0.0;
    // Extra distance a member must be beyond the join radius before it may leave. It is what stops
    // an object resting exactly at the join radius from joining and leaving on alternate ticks.
    float leave_margin = 1.0f;
    // Consecutive quiet ticks required before leaving: 0.5 s at 60 Hz.
    std::uint32_t rest_ticks = 30;
    // "At rest" speed bound, 0.05 m/s: one tick of motion at that speed is under a millimetre at
    // 60 Hz, below anything the player can see or the replay can resolve, yet comfortably above
    // the jitter of a resting solver, which has to stay under it or the object never "rests".
    float rest_speed = 0.05f;

    [[nodiscard]] float join_radius() const noexcept {
        return contact_distance +
               static_cast<float>(static_cast<double>(player_max_speed) * replay_ticks * fixed_dt);
    }

    [[nodiscard]] float leave_radius() const noexcept { return join_radius() + leave_margin; }
};

struct InteractionSample {
    float distance = 0.0f; // player to object
    float speed = 0.0f;    // object's linear speed
    bool touching = false; // object in contact with the player
};

enum class MembershipDecision : std::uint8_t { Stay, Join, Leave };

struct InteractionCounters {
    std::uint64_t joins = 0;
    std::uint64_t leaves = 0;
    // A join on a tick with touching = true. Joining at the join radius should happen long before
    // contact; a non-zero value means the radius is too small or the object was thrown at us.
    std::uint64_t joined_in_contact = 0;
    std::uint64_t leave_deferred_moving = 0;   // would have left, but the object was moving
    std::uint64_t leave_deferred_touching = 0; // would have left, but it was touched
};

class InteractionMembershipPolicy {
public:
    explicit InteractionMembershipPolicy(InteractionParams params) : params_(params) {}

    // Call once per tick. The returned decision and member() describe the state AFTER this tick.
    MembershipDecision update(const InteractionSample& sample);

    [[nodiscard]] bool member() const noexcept { return member_; }

    [[nodiscard]] std::uint32_t quiet_ticks() const noexcept { return quiet_; }

    [[nodiscard]] const InteractionCounters& counters() const noexcept { return counters_; }

    [[nodiscard]] const InteractionParams& params() const noexcept { return params_; }

private:
    InteractionParams params_;
    bool member_ = false;
    std::uint32_t quiet_ = 0;
    InteractionCounters counters_{};
};

// ADR-0106 §4's raw-correction bound: (v_push_max + j_max / mass) * replay_ticks * fixed_dt. One
// function so the proof and the code share the arithmetic.
[[nodiscard]] float interaction_raw_bound(float v_push_max,
                                          float j_max,
                                          float mass,
                                          std::uint32_t replay_ticks,
                                          double fixed_dt);

// ---------------------------------------------------------------------------- smoothing

struct SmootherParams {
    double tau_seconds = 0.1;
    float snap_distance = 1.0f;
    // Orientation analogue of snap_distance (not in the brief; 1 rad is ~57 degrees).
    float snap_radians = 1.0f;
};

struct SmootherCounters {
    std::uint64_t corrections = 0;
    std::uint64_t snaps = 0;
    float max_raw_jump = 0.0f;       // |before - after| over all position corrections
    float max_presented_jump = 0.0f; // largest one-step change of the position offset
    std::uint64_t rotation_corrections = 0;
    std::uint64_t rotation_snaps = 0;
    float max_raw_angle = 0.0f; // radians
};

class CorrectionSmoother {
public:
    explicit CorrectionSmoother(SmootherParams params = {}) : params_(params) {}

    // The simulation was corrected from `presented_before` (what was on screen) to
    // `simulated_after`.
    void on_correction(PredictedEntityId id,
                       rime::core::Vec3 presented_before,
                       rime::core::Vec3 simulated_after);
    void on_correction(PredictedEntityId id,
                       rime::core::Quat presented_before,
                       rime::core::Quat simulated_after);

    // Decay every offset by exp(-dt / tau). Call once per rendered/simulated step.
    void advance(double dt);

    [[nodiscard]] rime::core::Vec3 presented(PredictedEntityId id,
                                             rime::core::Vec3 simulated) const;
    [[nodiscard]] rime::core::Quat presented(PredictedEntityId id,
                                             rime::core::Quat simulated) const;
    void forget(PredictedEntityId id);

    [[nodiscard]] float offset_length(PredictedEntityId id) const;

    [[nodiscard]] const SmootherCounters& counters() const noexcept { return counters_; }

private:
    struct Entry {
        PredictedEntityId id{};
        rime::core::Vec3 offset{};
        rime::core::Quat rotation = rime::core::quat_identity();
    };

    Entry* find(PredictedEntityId id);
    const Entry* find(PredictedEntityId id) const;
    Entry& find_or_add(PredictedEntityId id);

    SmootherParams params_;
    std::vector<Entry> entries_; // a handful of members: linear search beats a map
    SmootherCounters counters_{};
};

} // namespace rime::replication
