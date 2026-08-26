// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/gameplay_net/gameplay_server.hpp"

#include <algorithm>

#include "rime/core/byte_cursor.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/gameplay_net/components.hpp"
#include "rime/physics/components.hpp"
#include "rime/physics/world.hpp"

namespace rime::gameplay_net {

namespace {

// The wire types and the mover's types are the same fields with different owners (character.hpp on
// why they are mirrored rather than shared). This is the whole conversion, and it lives in
// `gameplay_net` precisely so that neither side has to know about the other.
//
// `sequence` does NOT cross. It is the network's bookkeeping, not the mover's, and leaving it out
// is what keeps a replay tape a statement about intent rather than about a particular session.
[[nodiscard]] gameplay::CharacterInput
to_character_input(const replication::InputCommand& command) noexcept {
    gameplay::CharacterInput input;
    input.move_x = command.move_x;
    input.move_y = command.move_y;
    input.yaw = command.yaw;
    input.pitch = command.pitch;
    input.held = command.held;
    input.pressed = command.pressed;
    return input;
}

} // namespace

GameplayServer::PlayerState* GameplayServer::find_state(net::SessionId id) noexcept {
    for (PlayerState& state : states_) {
        if (state.in_use && state.id == id) {
            return &state;
        }
    }
    return nullptr;
}

GameplayServer::PlayerState& GameplayServer::state_for(net::SessionId id) {
    if (PlayerState* found = find_state(id); found != nullptr) {
        return *found;
    }
    // Reuse a freed slot before growing, and RESET it on reuse. A SessionId is generational so a
    // recycled slot cannot be mistaken for its predecessor — but a stale `allowance` or a stale
    // `announced` id would still be inherited by the new client, which would either hand it a free
    // burst or convince the server it had already been told which avatar is its own. The same
    // reset, for the same reason, as `ServerInputReceiver::client_for`.
    for (PlayerState& state : states_) {
        if (!state.in_use) {
            state = PlayerState{};
            state.id = id;
            state.in_use = true;
            return state;
        }
    }
    PlayerState fresh{};
    fresh.id = id;
    fresh.in_use = true;
    states_.push_back(fresh);
    return states_.back();
}

void GameplayServer::on_session_events(
    std::span<const net::SessionEvent> events,
    const std::function<ecs::Entity(net::SessionId)>& spawn,
    const std::function<void(net::SessionId, ecs::Entity)>& retire) {
    for (const net::SessionEvent& event : events) {
        switch (event.kind) {
            case net::SessionEvent::Kind::Connected: {
                const ecs::Entity player = spawn ? spawn(event.id) : ecs::kNullEntity;
                if (!player.is_valid()) {
                    ++players_declined_; // a spectator, or a game that is not ready to admit them
                    continue;
                }
                players_.bind(event.id, player);
                (void)state_for(event.id); // mint the per-player row now, so publish() sees it
                ++players_spawned_;
                break;
            }
            case net::SessionEvent::Kind::Disconnected:
            case net::SessionEvent::Kind::ConnectFailed: {
                const ecs::Entity player = players_.player_for(event.id);
                if (player.is_valid()) {
                    if (retire) {
                        retire(event.id, player);
                    }
                    ++players_retired_;
                }
                // Reaped whether or not an avatar existed: a ConnectFailed session never had one,
                // and leaving its row behind is one dead entry per failed connect, forever.
                players_.forget(event.id);
                if (PlayerState* state = find_state(event.id); state != nullptr) {
                    *state = PlayerState{};
                }
                break;
            }
        }
    }
}

void GameplayServer::apply_command(ecs::World& world,
                                   const physics::PhysicsWorld& physics,
                                   net::SessionId session,
                                   ecs::Entity player,
                                   const replication::InputCommand& command,
                                   float dt) {
    // The three components a character cannot move without. Looked up directly rather than through
    // `gameplay::step_character_entity` because this loop needs the RESULTING state to aim the
    // weapon from, and calling the convenience wrapper would mean a second lookup of the state it
    // just wrote — plus it would hide the "this entity is not a character" case behind a silent
    // no-op, which is exactly what `players_unbound` exists to make loud.
    gameplay::CharacterConfig* config = world.get<gameplay::CharacterConfig>(player);
    gameplay::CharacterState* state = world.get<gameplay::CharacterState>(player);
    const physics::RigidBodyHandle* handle = world.get<physics::RigidBodyHandle>(player);
    if (config == nullptr || state == nullptr || handle == nullptr) {
        ++players_unbound_;
        return;
    }

    const gameplay::CharacterInput input = to_character_input(command);

    // ── The move ──────────────────────────────────────────────────────────────────────────────
    *state =
        gameplay::step_character(*state, input, *config, physics, handle->body, dt, &step_stats_);
    world.mark_changed<gameplay::CharacterState>(player);

    // The handoff to physics: the controller owns the pose, and writing it into WorldTransform is
    // how it says so. `PhysicsSync::push_in` reads that transform next and drives the kinematic
    // capsule there with the velocity the move implied — which is what makes a player PUSH a crate
    // rather than teleport through it (physics/sync.hpp, gameplay/components.hpp).
    if (ecs::WorldTransform* transform = world.get<ecs::WorldTransform>(player);
        transform != nullptr) {
        transform->value.translation = state->position;
        world.mark_changed<ecs::WorldTransform>(player);
    }

    // ── The shot ──────────────────────────────────────────────────────────────────────────────
    //
    // AFTER the move, from the pose the move produced. A player who steps out of cover and fires in
    // the same command has left cover by the time the ray is cast, which is what the input said and
    // what the player will see. Resolving before the move would fire from where they used to be —
    // an off-by-one-tick that presents as "my shots clip the corner I just left".
    gameplay::WeaponConfig* weapon_config = world.get<gameplay::WeaponConfig>(player);
    gameplay::WeaponState* weapon_state = world.get<gameplay::WeaponState>(player);
    if (weapon_config != nullptr && weapon_state != nullptr) {
        const gameplay::Aim aim = gameplay::character_aim(*state, input, *weapon_config);
        gameplay::ShotResult shot;
        *weapon_state = gameplay::step_weapon(
            *weapon_state, input, *weapon_config, aim, physics, handle->body, &shot, &fire_stats_);
        // Stamped unconditionally: the cooldown ages every tick, so the component changes on far
        // more ticks than it fires on, and a stamp gated on `shot.fired` would leave clients
        // holding a cooldown that only ever ticks down when someone shoots.
        world.mark_changed<gameplay::WeaponState>(player);

        if (shot.fired) {
            ++shots_fired_;
            if (shots_.size() >= config_.max_shot_events) {
                // The damage this shot would have done is genuinely lost. Counted rather than
                // silently truncated, because a consumer that stopped reading looks identical to a
                // quiet match otherwise.
                ++shot_events_dropped_;
            } else {
                ShotEvent event;
                event.shooter = session;
                event.shooter_entity = player;
                event.sequence = command.sequence;
                event.origin = shot.shot.origin;
                event.direction = shot.shot.direction;
                event.did_hit = shot.did_hit;
                event.damage = weapon_config->damage;
                event.damage_radius = weapon_config->damage_radius;
                event.impulse = shot.shot.direction * weapon_config->impulse;
                if (shot.did_hit) {
                    ++shots_hit_;
                    event.body = shot.hit.body;
                    event.child = shot.hit.child;
                    event.point = shot.hit.point;
                    event.normal = shot.hit.normal;
                    event.distance = shot.hit.distance;
                }
                shots_.push_back(event);
            }
        }
    }

    // ── The pairing ───────────────────────────────────────────────────────────────────────────
    //
    // Written LAST, and only on a command that was actually applied, because that is exactly what
    // it claims: "the state on this entity is the result of your input through `sequence`." Adding
    // the component if it is missing keeps the game's spawn callback from having to know about a
    // replication detail.
    //
    // "LAST" IS LOAD-BEARING AND NOT MERELY TIDY. `add_component` is a STRUCTURAL change: it moves
    // the entity to a different archetype, which invalidates every component pointer taken above —
    // `config`, `state`, `weapon_config`, `weapon_state`. They are all finished with by the time
    // this runs, and that is the only reason this is safe. Anything added to this function after
    // this block must re-fetch, or go before it.
    //
    // The move happens at most once per avatar (the second command finds the component already
    // there and takes the plain-write path), so the archetype churn is one hop per player per
    // session rather than per tick.
    if (LastProcessedInput* last = world.get<LastProcessedInput>(player); last != nullptr) {
        last->sequence = command.sequence;
        world.mark_changed<LastProcessedInput>(player);
    } else {
        (void)world.add_component(player, LastProcessedInput{command.sequence});
    }

    ++commands_consumed_;
}

void GameplayServer::consume(ecs::World& world,
                             const physics::PhysicsWorld& physics,
                             replication::ServerInputReceiver& input,
                             float dt) {
    shots_.clear();

    // Walked in ascending session-index order (PlayerRegistry::for_each). Deterministic ordering
    // between players matters even though each player's commands only touch their own avatar: the
    // shots they fire are resolved against a world the OTHER players' moves have already changed,
    // so a different walk order is a different set of hits. Same rule, same reason, as ADR-0029
    // §3's canonical op sort.
    players_.for_each([&](net::SessionId session, ecs::Entity player) {
        PlayerState& state = state_for(session);

        // A stale binding: the avatar was despawned without the session going away (a kill, a
        // team switch). Nothing to consume, and nothing to complain about — but the commands must
        // still be drained, or they pile up until the buffer overflows and the client is told its
        // input was consumed when the server never looked at it.
        const bool alive = world.is_alive(player);

        pending_.clear();
        (void)input.drain(session, pending_);

        // A burst ceiling of zero would park every player forever, which is a configuration
        // mistake that presents as "the server ignores input". One is the floor: at one command
        // per tick a client sending at the tick rate is still fully served.
        const std::uint32_t burst = std::max(config_.max_command_burst, 1u);
        state.allowance = std::min(state.allowance + 1u, burst);

        if (!alive) {
            commands_dropped_no_avatar_ += pending_.size();
            return;
        }

        const std::size_t applied =
            std::min<std::size_t>(pending_.size(), static_cast<std::size_t>(state.allowance));
        // Drop from the FRONT: under a genuine over-rate burst the newest command is what the
        // player is doing now and the older ones are already stale. See the header's rate-budget
        // note on why dropping (not deferring) is the choice that keeps the ack honest.
        const std::size_t dropped = pending_.size() - applied;
        commands_dropped_over_rate_ += dropped;

        for (std::size_t i = dropped; i < pending_.size(); ++i) {
            apply_command(world, physics, session, player, pending_[i], dt);
        }
        state.allowance -= static_cast<std::uint32_t>(applied);

        if (applied == 0) {
            // The avatar did not advance this tick. Not an error — it is what a server that will
            // not invent input looks like when a packet is late, and it is the cost m12.4 exists
            // to hide.
            ++ticks_starved_;
        }
    });
}

void GameplayServer::publish(net::NetDriver& driver,
                             const replication::NetIdMap& map,
                             std::uint64_t now_ms) {
    players_.for_each([&](net::SessionId session, ecs::Entity player) {
        PlayerState& state = state_for(session);

        const std::optional<replication::NetId> net_id = map.net_id_of(player);
        if (!net_id.has_value()) {
            // The avatar exists but the game has not opted it into replication yet. Deferred, not
            // dropped: announcing a name the client cannot resolve would be worse than silence,
            // and the diff below re-offers it every tick until it can be named.
            ++assignments_deferred_;
            return;
        }
        if (*net_id == state.announced) {
            return; // already told, and it has not changed
        }

        net::Session* session_ptr = driver.session(session);
        if (session_ptr == nullptr || session_ptr->state() != net::SessionState::Connected) {
            // Reaped between PreSim and Publish, or lingering through a graceful close. The same
            // guard `ServerReplicator::publish` applies, and for the same reason: a reliable send
            // into a session that is on its way out buys a retransmit nobody will read, on the
            // channel where the in-flight window is scarcest.
            ++assignments_deferred_;
            return;
        }

        scratch_.clear();
        core::ByteWriter writer{scratch_};
        writer.u8(static_cast<std::uint8_t>(MessageTag::AssignPlayer));
        writer.u32(net_id->index);
        writer.u32(net_id->generation);

        if (!session_ptr->send_reliable(scratch_, now_ms)) {
            // The reliable channel is saturated. NOT recorded as announced, so the diff re-offers
            // it next tick — the same self-healing shape the spawn/despawn announcements use, and
            // the reason neither needs a repair protocol.
            ++assignments_deferred_;
            return;
        }
        state.announced = *net_id;
        ++assignments_sent_;
    });
}

} // namespace rime::gameplay_net
