// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "rime/core/math/vec.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/gameplay/character.hpp"
#include "rime/gameplay/weapon.hpp"
#include "rime/gameplay_net/player_registry.hpp"
#include "rime/gameplay_net/wire.hpp"
#include "rime/net/net_driver.hpp"
#include "rime/physics/body.hpp"
#include "rime/replication/input.hpp"
#include "rime/replication/net_id.hpp"

namespace rime::ecs {
class World;
}

namespace rime::physics {
class PhysicsWorld;
}

// GameplayServer (m12.3, ADR-0035 §4) — the authority half of the networked player: turn each
// client's stream of intent into authoritative motion, and stamp the pairing that m12.4 will
// reconcile against.
//
// The whole loop, per tick, in the order it happens and with the reason for each step:
//
//   1. DRAIN each session's accepted commands (`ServerInputReceiver::drain`). Draining is what
//      advances `consumed_through`, and that ordering is load-bearing: the ack must mean "the game
//      has this", never "a packet arrived carrying it" (input.hpp). So we drain when we are about
//      to act, not when we feel like looking.
//   2. RUN `step_character` ONCE PER COMMAND, in sequence order.
//   3. RESOLVE FIRE for that same command, from the pose the move just produced.
//   4. WRITE the results — CharacterState, WeaponState, WorldTransform — and stamp
//      `LastProcessedInput{sequence}` on the entity, which is how the pairing crosses the wire.
//
// ── ONE STEP PER COMMAND, AND NOT ONE PER TICK ────────────────────────────────────────────────
//
// This is the design's least obvious commitment and it is not an optimization. m12.4 reconciles by
// comparing the client's PREDICTED state after command q against the server's state at q — the
// like-for-like comparison ADR-0035 §4 insists on. That comparison is only meaningful if the two
// sides ran the mover the same number of times over the same inputs. So:
//
//   * a tick that consumed three commands advances the mover three times, and
//   * a tick that consumed NONE advances it not at all — the player freezes for that tick.
//
// The second half is the one that looks like a bug and is not. A server that helpfully re-ran the
// last command on a starved tick would move the player a tick further than the client predicted,
// and every packet loss would then produce a phantom correction whose cause is the server's own
// helpfulness. Freezing is honest, it is counted (`ticks_starved`), and it is precisely the
// artefact prediction exists to hide — which is why m12.3's proof measures it and m12.4's has to
// beat it.
//
// ── THE RATE BUDGET, AND WHY DROPPING IS HONEST ───────────────────────────────────────────────
//
// "One step per command" hands a client a speed multiplier if it simply sends faster: two commands
// a tick is two ticks of motion a tick. So each player carries an allowance that refills by ONE
// per tick and saturates at `Config::max_command_burst`. A client sending at the tick rate never
// touches it; a client catching up after jitter spends the slack and is fully served; a client
// sending persistently over rate is served at exactly one command per tick and the surplus is
// DROPPED — oldest first, because under a genuine over-rate burst the newest command is the one
// that reflects what the player is doing now and the older ones are already stale.
//
// Dropping (rather than deferring) is what keeps the acknowledgement honest. `consumed_through` is
// explicitly NOT a completeness claim — it steps over commands the server will never act on
// (input.hpp) — so a client retiring a dropped command has learned the truth: "the server will
// never act on this." Deferring instead would advance the frontier over commands still sitting in
// a queue, which is the replication invariant violated upstream, and m12.4's predictor would then
// drop commands from its replay set that the server had not yet applied. Every drop is counted.
namespace rime::gameplay_net {

// One shot, flattened for the consumer's glue. Everything a damage call needs is here, already
// resolved, so the glue never has to look a weapon config back up or re-derive an impulse — and,
// more importantly, so `gameplay_net` can hand destruction its input without linking it.
//
// THE MISS IS AN EVENT TOO (`did_hit == false`). A tracer, a muzzle flash and a report all happen
// on a miss, and m12.6's FX families read exactly this list. An event stream that only carried hits
// would make missing invisible, which is the opposite of "the shot feels connected".
struct ShotEvent {
    net::SessionId shooter{};
    ecs::Entity shooter_entity = ecs::kNullEntity;
    std::uint32_t sequence = 0; // the command that pulled the trigger

    core::Vec3 origin{0.0f, 0.0f, 0.0f};
    core::Vec3 direction{0.0f, 0.0f, -1.0f}; // unit

    bool did_hit = false;
    physics::BodyId body{}; // what it struck; null on a miss
    // The compound child the ray pierced. On an INTACT destructible this is the part index
    // (query.hpp / ADR-0029's remap table) — the equivalence that lets hitscan name a part
    // without this module knowing what a part is.
    std::uint16_t child = 0;
    core::Vec3 point{0.0f, 0.0f, 0.0f};
    core::Vec3 normal{0.0f, 0.0f, 0.0f};
    float distance = 0.0f;

    // The damage call's arguments, resolved from the shooter's WeaponConfig. `impulse` is world
    // space and points ALONG THE SHOT, never along the surface normal — a bullet pushes the way it
    // was already travelling.
    float damage = 0.0f;
    float damage_radius = 0.0f;
    core::Vec3 impulse{0.0f, 0.0f, 0.0f};
};

class GameplayServer {
public:
    struct Config {
        // The ceiling on a player's saved-up command allowance, in commands. At 60 Hz the default
        // is ~133 ms of catch-up: enough to absorb a client hitch or a burst of jitter without
        // dropping anything, short enough that a recovering client cannot cross a room in one tick.
        std::uint32_t max_command_burst = 8;

        // Hard ceiling on `shots()` between consume() calls, so a pathological tick cannot grow the
        // event list without bound. Overflow is counted, never silent.
        std::size_t max_shot_events = 256;
    };

    void set_config(const Config& config) noexcept { config_ = config; }

    [[nodiscard]] const Config& config() const noexcept { return config_; }

    // Spawn and reap avatars as sessions come and go. Call from PreSim with the SAME event batch
    // `ServerReplicator::on_session_events` and `ServerInputReceiver::on_session_events` get.
    //
    // Both hooks are callbacks rather than work this class does itself, and that is the
    // `DestructionServer::sync_debris` argument verbatim: the game owns spawn policy — which
    // prefab, which spawn point, which CharacterConfig, whether to replicate the avatar at all,
    // and which relevancy group it joins. An engine that minted the entity would be choosing all
    // of that on the game's behalf.
    //
    //   `spawn`  — mint an avatar for a newly Connected session. Returning kNullEntity declines
    //              (a spectator, a full server, a client awaiting authorization); the session is
    //              simply not bound and nothing is consumed for it.
    //   `retire` — the departing session and the entity it drove, for a Disconnected/ConnectFailed
    //              event. Despawn it through `ServerReplicator::despawn`, never `world.despawn` —
    //              a bare despawn leaves a phantom on every client forever.
    void on_session_events(std::span<const net::SessionEvent> events,
                           const std::function<ecs::Entity(net::SessionId)>& spawn,
                           const std::function<void(net::SessionId, ecs::Entity)>& retire);

    // The consume loop (see the file header). Call from the GAMEPLAY stage of the tick — step 2 of
    // docs/design/simulation-tick.md, after `world.advance_version()` and BEFORE
    // `propagate_transforms` and `PhysicsSync`, so the transforms this writes are what push_in
    // drives the kinematic capsules to in the same tick.
    //
    // `dt` is the fixed timestep, once per COMMAND rather than once per tick.
    //
    // Clears `shots()` at entry: the list describes the tick that is running, exactly as
    // `DestructionWorld::committed_ops()` describes the update that just ran. A consumer reads it
    // after this returns and before the next call.
    void consume(ecs::World& world,
                 const physics::PhysicsWorld& physics,
                 replication::ServerInputReceiver& input,
                 float dt);

    // Tell each client which NetId is its own avatar. Call from Publish, AFTER
    // `ServerReplicator::publish`.
    //
    // The ordering is a nicety rather than a correctness requirement — publishing structure first
    // puts the avatar's Spawn ahead of this message on the same reliable-ordered channel, so a
    // well-behaved client can resolve the id the moment it arrives. It stays merely a nicety
    // because the receiver holds the NetId and resolves late (wire.hpp), so an assignment that
    // overtakes its Spawn costs a tick of not knowing rather than a permanently unresolvable id.
    //
    // Self-healing by diffing rather than by an event queue: each session's announced id is
    // compared against the one its entity currently carries, and any difference is re-sent. A send
    // the channel refuses is simply not recorded as announced, so the next tick retries.
    void publish(net::NetDriver& driver, const replication::NetIdMap& map, std::uint64_t now_ms);

    [[nodiscard]] const PlayerRegistry& players() const noexcept { return players_; }

    [[nodiscard]] PlayerRegistry& players() noexcept { return players_; }

    // This tick's shots, for the consumer's glue (weapon → destruction lives in the game, never
    // here — see CMakeLists.txt on why this module does not link `rime::destruction`).
    [[nodiscard]] std::span<const ShotEvent> shots() const noexcept { return shots_; }

    // ── Counters: every skip path has a number, because a proof that cannot see what it skipped
    // reads exactly like a passing one (guardrail 5). ────────────────────────────────────────────

    [[nodiscard]] std::uint64_t commands_consumed() const noexcept { return commands_consumed_; }

    // Commands drained and then discarded because the player was over its rate budget. Non-zero
    // means a client is sending faster than the server ticks; sustained non-zero on a client you
    // trust means the budget is mis-tuned, and on one you do not, that the budget is working.
    [[nodiscard]] std::uint64_t commands_dropped_over_rate() const noexcept {
        return commands_dropped_over_rate_;
    }

    // Commands drained for a session whose avatar is no longer alive — despawned by the game
    // (a kill, a team switch) while the session itself continues. They are still DRAINED rather
    // than left to rot: an undrained buffer fills, and `ServerInputReceiver` then evicts the
    // overflow and advances the frontier over it anyway, so not draining would only move the same
    // loss somewhere it is harder to see.
    [[nodiscard]] std::uint64_t commands_dropped_no_avatar() const noexcept {
        return commands_dropped_no_avatar_;
    }

    // Player-ticks on which no command was available to consume, so the avatar did not advance.
    // The honest measure of what packet loss and jitter cost a NON-predicting client, and the
    // baseline m12.4 has to improve. Zero on a clean local link with a client sending every tick.
    [[nodiscard]] std::uint64_t ticks_starved() const noexcept { return ticks_starved_; }

    // Commands aimed at an entity that is not a usable character — missing CharacterConfig,
    // CharacterState, or the physics::RigidBodyHandle that names the body to exclude from its own
    // queries. Counted per COMMAND, so a persistent setup fault is loud rather than a one-off.
    // Non-zero is a bug in the game's spawn callback, and the symptom without this counter is
    // "that player's input does nothing", which points nowhere near the cause.
    [[nodiscard]] std::uint64_t players_unbound() const noexcept { return players_unbound_; }

    [[nodiscard]] std::uint64_t players_spawned() const noexcept { return players_spawned_; }

    [[nodiscard]] std::uint64_t players_retired() const noexcept { return players_retired_; }

    // Connected sessions the game's spawn callback declined to give an avatar.
    [[nodiscard]] std::uint64_t players_declined() const noexcept { return players_declined_; }

    [[nodiscard]] std::uint64_t shots_fired() const noexcept { return shots_fired_; }

    [[nodiscard]] std::uint64_t shots_hit() const noexcept { return shots_hit_; }

    // Shot events discarded because `Config::max_shot_events` was reached — a consumer that stopped
    // reading, or a tick with an implausible amount of shooting. The damage was NOT applied for
    // these, so a non-zero value is a real loss of gameplay and is worth failing a proof over.
    [[nodiscard]] std::uint64_t shot_events_dropped() const noexcept {
        return shot_events_dropped_;
    }

    [[nodiscard]] std::uint64_t assignments_sent() const noexcept { return assignments_sent_; }

    // Assignments that could not be sent this tick: the avatar carries no NetId yet (the game has
    // not called `ServerReplicator::replicate` on it), or the reliable channel refused. Both retry
    // next tick, so this is a latency measure — except that a value which never returns to a stable
    // total means an avatar is permanently unaddressable, which is a setup bug.
    [[nodiscard]] std::uint64_t assignments_deferred() const noexcept {
        return assignments_deferred_;
    }

    // Aggregate mover and weapon instrument panels across every command this server has consumed.
    // Their per-tick meanings are documented on the structs; here they are vacuity witnesses (a
    // proof asserting `casts > 0` knows the mover really ran) and early warnings (`stuck` and
    // `slide_exhausted` climbing means players are freezing somewhere).
    [[nodiscard]] const gameplay::StepStats& step_stats() const noexcept { return step_stats_; }

    [[nodiscard]] const gameplay::FireStats& fire_stats() const noexcept { return fire_stats_; }

private:
    struct PlayerState {
        net::SessionId id{};
        bool in_use = false;
        // Command allowance, in commands: +1 per tick, saturating at max_command_burst. See the
        // rate-budget note in the file header.
        std::uint32_t allowance = 0;
        // The NetId this session has been told is its own, or kNullNetId for "never announced".
        // Diffed against the entity's current id each publish, which is what makes the
        // announcement self-healing rather than an event queue needing repair.
        replication::NetId announced = replication::kNullNetId;
    };

    PlayerState& state_for(net::SessionId id);
    PlayerState* find_state(net::SessionId id) noexcept;

    // Apply one command to one bound avatar. Split out because it is the unit m12.4 will replay,
    // and because keeping it a function makes "the server did exactly this, in this order" a thing
    // a reader can check rather than infer.
    void apply_command(ecs::World& world,
                       const physics::PhysicsWorld& physics,
                       net::SessionId session,
                       ecs::Entity player,
                       const replication::InputCommand& command,
                       float dt);

    Config config_{};
    PlayerRegistry players_;
    std::vector<PlayerState> states_;
    std::vector<ShotEvent> shots_;

    // Reused across ticks so the steady state allocates nothing.
    std::vector<replication::InputCommand> pending_;
    std::vector<std::byte> scratch_;

    std::uint64_t commands_consumed_ = 0;
    std::uint64_t commands_dropped_over_rate_ = 0;
    std::uint64_t commands_dropped_no_avatar_ = 0;
    std::uint64_t ticks_starved_ = 0;
    std::uint64_t players_unbound_ = 0;
    std::uint64_t players_spawned_ = 0;
    std::uint64_t players_retired_ = 0;
    std::uint64_t players_declined_ = 0;
    std::uint64_t shots_fired_ = 0;
    std::uint64_t shots_hit_ = 0;
    std::uint64_t shot_events_dropped_ = 0;
    std::uint64_t assignments_sent_ = 0;
    std::uint64_t assignments_deferred_ = 0;

    gameplay::StepStats step_stats_{};
    gameplay::FireStats fire_stats_{};
};

} // namespace rime::gameplay_net
