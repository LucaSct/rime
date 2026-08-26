// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/gameplay_net/predictor.hpp"

#include <algorithm>
#include <cmath>

#include "rime/gameplay_net/convert.hpp"
#include "rime/physics/world.hpp"

namespace rime::gameplay_net {
namespace {

[[nodiscard]] float distance_between(core::Vec3 a, core::Vec3 b) noexcept {
    return std::sqrt(core::length_squared(a - b));
}

// BITWISE vector equality, spelled out because core::Vec3 deliberately has no operator== (a
// float type with a default == invites exactly the approximate comparisons this file is careful
// to keep in one place). This one is asking "are these the same bytes I already handled", which is
// the one question about two float vectors that == is the right answer to.
[[nodiscard]] bool same_bits(core::Vec3 a, core::Vec3 b) noexcept {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

} // namespace

void Predictor::reset(const gameplay::CharacterState& state) {
    history_.clear();
    predicted_ = state;
    seeded_ = true;
    newest_sequence_ = 0;
    have_last_ = false;
    last_q_ = 0;
}

const gameplay::CharacterState* Predictor::find_entry(std::uint32_t sequence) const noexcept {
    for (const Entry& entry : history_) {
        if (entry.sequence == sequence) {
            return &entry.state;
        }
    }
    return nullptr;
}

void Predictor::trim_before(std::uint32_t sequence) {
    const auto first_kept = std::find_if(
        history_.begin(), history_.end(), [&](const Entry& e) { return e.sequence >= sequence; });
    history_.erase(history_.begin(), first_kept);
}

const gameplay::CharacterState& Predictor::predict(const replication::InputCommand& command,
                                                   const gameplay::CharacterConfig& config,
                                                   const physics::PhysicsWorld& world,
                                                   physics::BodyId self,
                                                   float dt,
                                                   gameplay::StepStats* stats) {
    if (!seeded_) {
        // Nothing to predict FROM. Returning the (default) state rather than inventing an origin is
        // deliberate: a predictor that started at (0,0,0) would draw the local avatar inside the
        // floor for the first round trip, and then snap. Seeding is `reconcile`'s job on the first
        // authoritative state, or the game's via `reset`.
        return predicted_;
    }

    // THE SAME FUNCTION THE SERVER WILL RUN, over the SAME conversion (convert.hpp). Both halves
    // matter — a second copy of either is a divergence with no divergent input behind it.
    predicted_ = gameplay::step_character(
        predicted_, to_character_input(command), config, world, self, dt, stats);
    newest_sequence_ = command.sequence;

    Entry entry;
    entry.sequence = command.sequence;
    entry.command = command;
    entry.state = predicted_;
    history_.push_back(entry);

    // A cap of zero would evict every entry the moment it was recorded, leaving the ring
    // permanently empty and every reconcile `unverifiable` — a predictor that can never check
    // itself, from one mis-set config field. One is the floor.
    const std::size_t cap = std::max<std::size_t>(config_.max_history, 1);
    if (history_.size() > cap) {
        // The client is further behind than the ring is deep. Counted, because the consequence is
        // not "we forgot something old" but "a correction arriving now cannot replay everything it
        // should" — which surfaces a moment later as `corrections_unverifiable`.
        const std::size_t excess = history_.size() - cap;
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(excess));
        evicted_ += excess;
    }
    return predicted_;
}

bool Predictor::reconcile(const gameplay::CharacterState& authoritative,
                          std::uint32_t q,
                          const gameplay::CharacterConfig& config,
                          const physics::PhysicsWorld& world,
                          physics::BodyId self,
                          float dt) {
    if (!seeded_) {
        // The first authoritative state we ever see IS the seed. Doing it here rather than making
        // the game call `reset` means a client that simply wires the loop up gets a correct start
        // with no ordering rule to remember.
        reset(authoritative);
        last_q_ = q;
        last_authoritative_ = authoritative;
        have_last_ = true;
        return false;
    }

    if (q == 0) {
        return false; // the server has not acted on any of our commands yet
    }

    // An unchanged pairing carries no information. Most ticks deliver no new snapshot for this
    // entity, and counting those would make `reconciles()` a measure of the tick rate.
    //
    // BOTH halves are compared, not just `q`: a teleport moves the avatar without consuming a
    // command, so the state changes while `q` stands still. Comparing state bitwise is exactly
    // right here — this is asking "are these the same bytes I already handled", not "are these
    // close enough", and the latter question belongs a few lines down.
    if (have_last_ && q == last_q_ &&
        same_bits(authoritative.position, last_authoritative_.position) &&
        same_bits(authoritative.velocity, last_authoritative_.velocity) &&
        authoritative.grounded == last_authoritative_.grounded) {
        return false;
    }
    last_q_ = q;
    last_authoritative_ = authoritative;
    have_last_ = true;
    ++reconciles_;

    // ── Was the prediction right? ─────────────────────────────────────────────────────────────
    const gameplay::CharacterState* mine = find_entry(q);
    if (mine != nullptr) {
        const float position_error = distance_between(mine->position, authoritative.position);
        const float velocity_error = distance_between(mine->velocity, authoritative.velocity);
        // `grounded` is compared EXACTLY and the tolerances do not apply to it. It is a boolean
        // fact about what the mover will do next — a state that is airborne where the authority is
        // standing accelerates differently, jumps differently, and slides differently — so two
        // states that disagree about it are not "close", however close their positions are.
        //
        // `ground_normal` is deliberately NOT in the gate: it is meaningful only while grounded,
        // and two poses within a couple of centimetres on the same surface carry normals that
        // differ by rounding. It is still RESTORED by a correction, because it is part of the state
        // being replaced.
        if (position_error <= config_.position_tolerance &&
            velocity_error <= config_.velocity_tolerance &&
            mine->grounded == authoritative.grounded) {
            ++skipped_;
            trim_before(q);
            return false;
        }
        max_correction_ = std::max(max_correction_, position_error);
    } else {
        // We cannot CHECK the prediction, only replace it. Distinct from a mismatch and counted
        // separately, because it means something different about the session: the ring did not
        // reach back to `q`.
        ++unverifiable_;
    }

    // ── Rewind, and replay everything the server has not acted on yet ─────────────────────────
    ++corrections_;

    // The commands still to be re-run are exactly those the ring holds after `q`. Snapshot them
    // before the rewind clears it — replaying out of the vector being rebuilt is the kind of thing
    // that works until the first reallocation.
    std::vector<replication::InputCommand> to_replay;
    to_replay.reserve(history_.size());
    for (const Entry& entry : history_) {
        if (entry.sequence > q) {
            to_replay.push_back(entry.command);
        }
    }

    predicted_ = authoritative;
    history_.clear();
    for (const replication::InputCommand& command : to_replay) {
        predicted_ = gameplay::step_character(
            predicted_, to_character_input(command), config, world, self, dt, nullptr);
        Entry entry;
        entry.sequence = command.sequence;
        entry.command = command;
        entry.state = predicted_;
        history_.push_back(entry);
        ++replayed_;
    }
    // `newest_sequence_` follows the ring. An empty ring after a correction means the server has
    // caught up with everything we predicted, so the newest sequence we know about is `q` itself —
    // leaving the old value would have this reporting a command that no longer exists anywhere.
    newest_sequence_ = history_.empty() ? q : history_.back().sequence;
    return true;
}

} // namespace rime::gameplay_net
