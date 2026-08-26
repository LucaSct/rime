// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/gameplay/character.hpp"
#include "rime/physics/body.hpp"
#include "rime/replication/input.hpp"

namespace rime::physics {
class PhysicsWorld;
}

// Client-side prediction and reconciliation (m12.4, ADR-0035 §4) — the hardest brick in M12, and
// the one the previous three exist to make debuggable.
//
// ── THE PROBLEM ───────────────────────────────────────────────────────────────────────────────
//
// m12.3 shipped a server-authoritative player and measured what it costs: your own input takes a
// full round trip to move your own avatar — 6 ticks at 48 ms one-way, 0.30 m of visible lag at
// walking pace. No amount of bandwidth fixes that; it is the speed of the link.
//
// The fix is not to trust the client. It is to have the client run THE SAME FUNCTION the server
// will run, immediately, and then check its work when the authority's answer arrives. The player
// sees their input at once; the server still decides what actually happened.
//
// ── WHY THIS IS BUILDABLE AT ALL ──────────────────────────────────────────────────────────────
//
// Because `step_character` is a pure function over physics queries (m12.2), replaying N ticks of
// input from a corrected state is just calling it N times. Everything the mover remembers is in
// `CharacterState`, so "rewind" is a struct assignment. That property was proven at m12.2 with a
// replay test precisely so that this brick debugs RECONCILIATION and never the mover.
//
// ── THE COMPARISON IS ON STATE, NEVER ON COMMAND LISTS ────────────────────────────────────────
//
// When authoritative state arrives stamped `LastProcessedInput = q`, the question is: "is my
// prediction for the moment after command q the same state the server ended up in?" It is NOT
// "which commands does the server have that I do not". That distinction is m11.6c's, it is written
// into ADR-0035 §4, and it is not stylistic: `consumed_through` steps over PERMANENT gaps, so a
// command leaving the client's send buffer means *the server will never act on it*, not *the server
// acted on it*. A predictor that diffed command lists would be wrong exactly under loss — the
// condition it exists for.
//
// ── WHY THIS KEEPS ITS OWN COMMAND HISTORY, AND DOES NOT REPLAY FROM `unacked()` ──────────────
//
// ADR-0035 §4 sketches the replay as "replay every `unacked()` command with sequence > q". Building
// it showed that has a hole, and the hole is worth stating because it is invisible on a clean link:
//
//   `unacked()` retires on `ClientInputSender::acked_through`, which comes from the `InputAck`
//   message. That ack rides an unreliable SUPERSEDING stream, and the snapshot carrying `q` rides a
//   different one — so the newest ack a client holds is routinely FRESHER than the newest snapshot
//   it holds, and relevancy or a byte budget can hold the player's record back further still. So
//   `acked_through >= q`, often strictly. Replaying only `unacked()` would then skip every command
//   in (q, acked_through] — commands the server DID consume and the client DID predict — and the
//   prediction would silently fall a few ticks behind, visible as the local avatar rubber-banding
//   backwards under exactly the conditions prediction exists to smooth over.
//
// So the ring holds `{sequence, command, resulting state}` and is retired on the RECONCILED q,
// which is the only frontier that makes the replay set complete. `unacked()` remains what it always
// was — the bound on the client's send buffer — and is not this class's input.
//
// The lost-command property ADR §4 promises survives unchanged: a command the server never received
// sits below the ack jump, so `q` moves past it, the ring is trimmed past it, and the replay
// excludes it. The state comparison at `q` then disagrees (the client predicted a tick the server
// never ran), a correction fires, and the client snaps to the truth. That is the mechanism working,
// not failing.
namespace rime::gameplay_net {

class Predictor {
public:
    struct Config {
        // How far the prediction may sit from the authority before it is worth snapping.
        //
        // WHY THERE IS A TOLERANCE AT ALL, given both sides run the same function on the same
        // inputs: they do not run it against the same WORLD. The client's physics world is built
        // from replicated mirrors that arrive at their own pace, so a wall the server has and the
        // client does not yet have makes an identical function return a different answer. Without a
        // gate the predictor would correct every tick on a link with no loss, which is a correction
        // storm rather than a correction.
        //
        // The epsilons are confined HERE, mid-flight. At quiescence — same binary, same function,
        // same inputs, same settled world — the two states must be equal BIT FOR BIT, and that is
        // what the proof asserts (ADR-0035 §4). A tolerance that leaked into the quiescent
        // assertion would hide exactly the drift it is meant to bound.
        float position_tolerance = 0.02f; // metres; ~2 skin widths on the default capsule
        float velocity_tolerance = 0.10f; // m/s

        // Commands kept for replay. At 60 Hz this is two seconds — well past any round trip a
        // playable session has, and it matches `replication::kMaxUnackedCommands` so the two
        // bounds cannot disagree about how far back a correction may reach.
        std::size_t max_history = replication::kMaxUnackedCommands;
    };

    void set_config(const Config& config) noexcept { config_ = config; }

    [[nodiscard]] const Config& config() const noexcept { return config_; }

    // Seed the prediction. Call when the local avatar first appears (or after a teleport the game
    // knows about). Clears the ring: nothing recorded before a reseed can be replayed onto it.
    void reset(const gameplay::CharacterState& state);

    // Run one command locally, right now, and remember it.
    //
    // Call once per tick from PreSim, immediately after `ClientInputSender::record` stamped the
    // sequence — that ordering is what makes "the player sees their input this frame" true. The
    // returned state is what the local avatar should be drawn at.
    //
    // A no-op returning the current state if the predictor has never been seeded.
    const gameplay::CharacterState& predict(const replication::InputCommand& command,
                                            const gameplay::CharacterConfig& config,
                                            const physics::PhysicsWorld& world,
                                            physics::BodyId self,
                                            float dt,
                                            gameplay::StepStats* stats = nullptr);

    // Check the prediction against the authority and repair it if it was wrong.
    //
    // Call from PreSim AFTER the snapshot has been applied and BEFORE this tick's `predict`, with
    // the `CharacterState` and `LastProcessedInput` the client's own avatar mirror is carrying.
    // Returns true if a correction was applied.
    //
    // `q == 0` means the server has not acted on any of this client's commands yet; there is
    // nothing to compare against and this returns false without counting anything.
    //
    // Reconciling twice against the same (state, q) is a no-op — the snapshot stream delivers on
    // its own schedule, so most ticks carry nothing new, and re-counting an unchanged pairing would
    // make `reconciles()` a measure of tick rate rather than of traffic.
    bool reconcile(const gameplay::CharacterState& authoritative,
                   std::uint32_t q,
                   const gameplay::CharacterConfig& config,
                   const physics::PhysicsWorld& world,
                   physics::BodyId self,
                   float dt);

    // The state the local avatar should be drawn at: the newest prediction, or the last
    // authoritative state if nothing has been predicted on top of it.
    [[nodiscard]] const gameplay::CharacterState& state() const noexcept { return predicted_; }

    [[nodiscard]] bool seeded() const noexcept { return seeded_; }

    // The newest sequence this predictor has run locally. Zero before the first predict.
    [[nodiscard]] std::uint32_t newest_sequence() const noexcept { return newest_sequence_; }

    [[nodiscard]] std::size_t history_size() const noexcept { return history_.size(); }

    // ── Counters (guardrail 5, and the proof's whole surface) ─────────────────────────────────

    // Pairings compared — i.e. how many times the authority told us something new about our own
    // avatar. The denominator for everything below.
    [[nodiscard]] std::uint64_t reconciles() const noexcept { return reconciles_; }

    // Rewind-and-replay events. **A lossy run in which this is ZERO fails the proof**, and the
    // reason is ADR-0035 §4's: zero corrections under real loss does not show the predictor is
    // excellent, it shows the comparison is dead. A gate that cannot fire is not a gate.
    [[nodiscard]] std::uint64_t corrections() const noexcept { return corrections_; }

    // Pairings that matched within tolerance — the healthy steady state, and the counter that says
    // so. M11's rule in its harder half: count the skips that stop happening.
    [[nodiscard]] std::uint64_t corrections_skipped() const noexcept { return skipped_; }

    // Corrections forced because the ring no longer held an entry at `q` — so the prediction could
    // not be CHECKED, only replaced. Distinct from a genuine mismatch, because it says something
    // different: the client fell further behind than `max_history`, or the server jumped its
    // frontier further than the ring reaches. Should be zero in a healthy session.
    [[nodiscard]] std::uint64_t corrections_unverifiable() const noexcept { return unverifiable_; }

    [[nodiscard]] std::uint64_t commands_replayed() const noexcept { return replayed_; }

    // The largest position error a correction ever had to repair, in metres. The honest measure of
    // how wrong prediction got — and the number to watch when tuning `position_tolerance`, because
    // a tolerance above this is a gate nothing can fail.
    [[nodiscard]] float max_correction_distance() const noexcept { return max_correction_; }

    // Ring entries dropped because `max_history` was reached — commands a correction could have
    // replayed and now cannot. Non-zero means the client is further behind than the ring is deep,
    // which is the condition `corrections_unverifiable` then reports.
    [[nodiscard]] std::uint64_t history_evicted() const noexcept { return evicted_; }

private:
    struct Entry {
        std::uint32_t sequence = 0;
        replication::InputCommand command{};
        gameplay::CharacterState state{}; // the state AFTER `command` ran
    };

    // Drop everything strictly older than `sequence`. Called after a comparison, never before one:
    // the entry AT q is the thing being compared against.
    void trim_before(std::uint32_t sequence);

    // The state this predictor produced after running `sequence`, or null if the ring no longer
    // reaches back that far. A linear scan: the ring is at most `max_history` entries and the
    // sequence being looked for is normally within a round trip of its tail.
    [[nodiscard]] const gameplay::CharacterState* find_entry(std::uint32_t sequence) const noexcept;

    Config config_{};
    std::vector<Entry> history_; // oldest first; sequences strictly increasing
    gameplay::CharacterState predicted_{};
    bool seeded_ = false;
    std::uint32_t newest_sequence_ = 0;

    // The pairing last reconciled against, so an unchanged one is recognised and skipped. Both
    // halves are needed: `q` alone would miss a teleport (the server moved the avatar without
    // consuming a command, so the state changed and `q` did not).
    std::uint32_t last_q_ = 0;
    gameplay::CharacterState last_authoritative_{};
    bool have_last_ = false;

    std::uint64_t reconciles_ = 0;
    std::uint64_t corrections_ = 0;
    std::uint64_t skipped_ = 0;
    std::uint64_t unverifiable_ = 0;
    std::uint64_t replayed_ = 0;
    std::uint64_t evicted_ = 0;
    float max_correction_ = 0.0f;
};

} // namespace rime::gameplay_net
