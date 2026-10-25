// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// hello-game — the rules. See game.hpp for what moved in m20.1 and what did not.
//
// **The pickups are real trigger volumes**, not a distance check. `Collider::sensor` was reflected
// and honoured by nothing until m15.6 made it fire `TriggerEvent`s; a sample that faked the overlap
// with a `length(a - b) < r` test would quietly stop exercising the feature it looks like it uses.
// The player is a KINEMATIC capsule, so `PhysicsSync::push_in` drives it from the transform the
// game writes — which is also why the crate is *pushed* at walking speed instead of being fired
// across the floor by a teleport-induced penetration.

#include "game.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"

namespace hello_game {
namespace {

using namespace rime;

// Component field values spelled as constants, because `RigidBody::motion` is a `std::uint32_t`
// rather than the enum (the reflected components stay POD) and `RigidBody{.motion = 0}` at a call
// site says nothing at all.
constexpr std::uint32_t kStatic = static_cast<std::uint32_t>(physics::MotionType::Static);
constexpr std::uint32_t kKinematic = static_cast<std::uint32_t>(physics::MotionType::Kinematic);
constexpr std::uint32_t kDynamic = static_cast<std::uint32_t>(physics::MotionType::Dynamic);
constexpr std::uint32_t kSphereShape = static_cast<std::uint32_t>(physics::ShapeType::Sphere);
constexpr std::uint32_t kBoxShape = static_cast<std::uint32_t>(physics::ShapeType::Box);
constexpr std::uint32_t kCapsuleShape = static_cast<std::uint32_t>(physics::ShapeType::Capsule);

constexpr float kArenaHalf = 9.0f;
constexpr float kFloorHalfY = 0.5f;
constexpr float kCrateHalf = 0.5f;
// The CENTRE of the ring, not a point on it. Starting on the ring would put the player inside
// marker 0 at tick zero and collect it for free — which passed the self-check and made the first
// reported collection tick 1, a number that looks like a working trigger and is really a spawn
// overlap. A game's starting position is a rule, and this is the rule: you have to walk.
constexpr core::Vec3 kPlayerStart{0.0f,
                                  HelloGame::kPlayerRadius + HelloGame::kPlayerHalfHeight,
                                  0.0f};
constexpr float kThrowSpeed = 18.0f;
constexpr float kThrowGravity = -9.8f;
constexpr float kTargetRadius = 0.08f;

using SpawnId = replication::PredictedEntityId;
using SpawnRequest =
    std::function<std::optional<SpawnId>(const replication::SpawnKey&, std::span<const std::byte>)>;

// The mapping survives replay while the world may be restored repeatedly. Adoption changes only
// the id here, keeping the same ECS entity and its presentation identity.
struct ClientSpawnState {
    ecs::World* world = nullptr;
    std::uint32_t incarnation = 0;
    SpawnRequest request;
    std::vector<std::pair<SpawnId, ecs::Entity>> entities;

    ecs::Entity entity_for(SpawnId id) const {
        for (const auto& [known, entity] : entities)
            if (known == id)
                return entity;
        return {};
    }
};

std::vector<std::byte> encode_projectile(const ThrownProjectile& projectile) {
    std::vector<std::byte> bytes;
    bytes.reserve(28);
    const auto append = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            bytes.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    };
    // Match the server's capture_spawned bytes exactly so checkpoint restore is atomic.
    append(std::bit_cast<std::uint32_t>(projectile.position.x));
    append(std::bit_cast<std::uint32_t>(projectile.position.y));
    append(std::bit_cast<std::uint32_t>(projectile.position.z));
    append(std::bit_cast<std::uint32_t>(projectile.velocity.x));
    append(std::bit_cast<std::uint32_t>(projectile.velocity.y));
    append(std::bit_cast<std::uint32_t>(projectile.velocity.z));
    append(projectile.age);
    return bytes;
}

void restore_projectile(ecs::World& world, ecs::Entity entity, std::span<const std::byte> bytes) {
    if (bytes.size() != 28 || !world.is_alive(entity))
        return;
    const auto read = [&](std::size_t offset) {
        std::uint32_t bits = 0;
        for (unsigned i = 0; i < 4; ++i)
            bits |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
        return bits;
    };
    ThrownProjectile projectile{{std::bit_cast<float>(read(0)),
                                 std::bit_cast<float>(read(4)),
                                 std::bit_cast<float>(read(8))},
                                {std::bit_cast<float>(read(12)),
                                 std::bit_cast<float>(read(16)),
                                 std::bit_cast<float>(read(20))},
                                read(24)};
    if (auto* existing = world.get<ThrownProjectile>(entity))
        *existing = projectile;
    else
        world.add_component(entity, projectile);
    core::Transform transform{};
    transform.translation = projectile.position;
    if (auto* wt = world.get<ecs::WorldTransform>(entity))
        wt->value = transform;
    else
        world.add_component(entity, ecs::WorldTransform{transform});
    if (auto* local = world.get<ecs::LocalTransform>(entity))
        local->value = transform;
    else
        world.add_component(entity, ecs::LocalTransform{transform});
}

// A swept segment test catches a fast projectile crossing the small target BETWEEN endpoints;
// an endpoint-only overlap would miss this fixed launch at 60 Hz.
bool segment_hits_target(core::Vec3 from, core::Vec3 to, core::Vec3 target) {
    const core::Vec3 delta = to - from;
    const float length_squared = core::dot(delta, delta);
    const float t = length_squared > 0.0f
                        ? std::clamp(core::dot(target - from, delta) / length_squared, 0.0f, 1.0f)
                        : 0.0f;
    const core::Vec3 closest = from + delta * t;
    const core::Vec3 offset = target - closest;
    return core::dot(offset, offset) <= kTargetRadius * kTargetRadius;
}

physics::Collider box_collider(float hx, float hy, float hz) {
    return physics::Collider{.shape_type = kBoxShape, .half_x = hx, .half_y = hy, .half_z = hz};
}

// The three bodies the crate interaction involves, defined ONCE. The authoritative world builds
// them as components (setup, build_arena) and the client's prediction physics world builds them as
// bodies (InteractionPhysics) -- from these same values, so a retuned crate mass or player radius
// cannot silently give the two worlds different physics.
constexpr physics::RigidBody kFloorRigidBody{.motion = kStatic};
constexpr physics::RigidBody kPlayerRigidBody{.motion = kKinematic};
constexpr physics::RigidBody kCrateRigidBody{.motion = kDynamic, .mass = 6.0f};

physics::Collider floor_collider() {
    return box_collider(kArenaHalf, kFloorHalfY, kArenaHalf);
}

physics::Collider player_collider() {
    return physics::Collider{.shape_type = kCapsuleShape,
                             .radius = HelloGame::kPlayerRadius,
                             .half_height = HelloGame::kPlayerHalfHeight};
}

physics::Collider crate_collider() {
    return box_collider(kCrateHalf, kCrateHalf, kCrateHalf);
}

// FNV-1a, 64-bit: the digest's mixing function. Chosen for being one line a reader can verify by
// eye rather than for strength — a state digest has to detect divergence between two honest runs,
// not resist an adversary. Floats are hashed by their BIT PATTERN, never by value: two runs that
// agree to six decimal places have diverged, and a digest that rounds cannot say so.
class Fnv1a {
public:
    void bytes(const void* data, std::size_t n) noexcept {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < n; ++i) {
            h_ = (h_ ^ p[i]) * 0x100000001b3ull;
        }
    }

    void u64(std::uint64_t v) noexcept { bytes(&v, sizeof v); }

    void f32(float v) noexcept { u64(std::bit_cast<std::uint32_t>(v)); }

    void vec3(core::Vec3 v) noexcept {
        f32(v.x);
        f32(v.y);
        f32(v.z);
    }

    [[nodiscard]] std::uint64_t value() const noexcept { return h_; }

private:
    std::uint64_t h_ = 0xcbf29ce484222325ull;
};

// The game's reading of the engine's ActionState — see `Intent` in game.hpp.
Intent intent_from(const app::ActionState& input) {
    Intent intent{};
    intent.move = {input.axis(kAxisMoveX), 0.0f, input.axis(kAxisMoveZ)};
    intent.quit = input.was_pressed(kButtonQuit);
    return intent;
}

std::uint64_t& collection_time(ReplicatedGameState& state, std::size_t index) {
    switch (index) {
        case 0:
            return state.collected0;
        case 1:
            return state.collected1;
        case 2:
            return state.collected2;
        case 3:
            return state.collected3;
        default:
            return state.collected4;
    }
}

std::uint64_t collection_time(const ReplicatedGameState& state, std::size_t index) {
    switch (index) {
        case 0:
            return state.collected0;
        case 1:
            return state.collected1;
        case 2:
            return state.collected2;
        case 3:
            return state.collected3;
        default:
            return state.collected4;
    }
}

// Fixed tick and prediction call this exact arithmetic, so a replay cannot drift because its
// controller used a separately maintained interpretation of movement or clamping.
void integrate_player(ReplicatedGameState& state, const Intent& intent, float dt) {
    const core::Vec3 before = state.player_position;
    core::Vec3 dir = intent.move;
    const float len = core::length(dir);
    // Normalising prevents diagonal speed gain; the zero guard prevents a NaN from entering
    // both the authoritative component and every later checkpoint.
    if (len > 1e-4f) {
        dir = dir * (1.0f / len);
    } else {
        dir = {0.0f, 0.0f, 0.0f};
    }
    state.player_position = state.player_position + dir * (HelloGame::kPlayerSpeed * dt);
    const float limit = kArenaHalf - HelloGame::kPlayerRadius;
    state.player_position.x = std::clamp(state.player_position.x, -limit, limit);
    state.player_position.z = std::clamp(state.player_position.z, -limit, limit);
    state.player_position.y = kPlayerStart.y;
    // The clamp can stop motion even while an axis stays held. Record actual displacement per
    // second, which is the kinematic velocity the physics bridge sees, not the unfulfilled wish.
    state.player_velocity = (state.player_position - before) * (1.0f / dt);
}

// A fixed-angle chase camera: behind and above the player, looking down at it. Fixed rather than
// free because this sample is about the game loop, and a free camera is a second input system
// competing for the reader's attention (07-first-light already demonstrates one).
core::Transform chase_camera(core::Vec3 target) {
    const core::Quat q = core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.0f) *
                                         core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.55f));
    const core::Vec3 back = core::rotate(q, {0.0f, 0.0f, 1.0f});
    core::Transform t{};
    t.rotation = q;
    t.translation = target + back * 11.0f;
    return t;
}

} // namespace

core::Vec3 HelloGame::marker_position(int index) {
    constexpr float kTwoPi = 6.28318531f;
    const float angle = kTwoPi * static_cast<float>(index) / static_cast<float>(kPickupCount);
    return {kRingRadius * std::sin(angle), kPickupRadius + 0.1f, kRingRadius * std::cos(angle)};
}

// Build the SIMULATED world: the bodies the rules touch, and nothing that exists only to be seen.
// The meshes, the sun and the camera are `present_setup`'s — so `dedicated`, which never calls it,
// simulates exactly the world `play` simulates, entity for entity.
bool HelloGame::setup(app::SetupContext& ctx) {
    using ecs::WorldTransform;
    ecs::World& world = ctx.world;
    world_ = &world;
    physics::register_physics_components(world);
    physics_.set_job_system(&ctx.jobs);
    physics_.set_sleeping_enabled(true);

    // The floor is CONTENT since m20.2: it arrives from `content/hello-game.rscene`, the entry
    // scene the engine loads from the content root before this runs. So the arena is already here —
    // and if it is not, this refuses rather than dropping the player into an empty world, where the
    // autopilot would still "win" by walking through the markers in mid-air. A game's own check of
    // its own content, on top of the engine's "the root and the scene exist".
    {
        int floors = 0;
        world.query<physics::RigidBody, physics::Collider>().for_each(
            [&](ecs::Entity, const physics::RigidBody& rb, const physics::Collider& c) {
                floors += (rb.motion == kStatic && c.shape_type == kBoxShape && !c.sensor) ? 1 : 0;
            });
        if (floors != 1) {
            return false;
        }
    }

    // The player: KINEMATIC, so the game owns its pose and `PhysicsSync::push_in` drives the body
    // from it. Dynamic would mean the solver owns the pose and the game would be fighting it for
    // control of a character — the thing a character controller exists not to do.
    {
        core::Transform tf{};
        tf.translation = kPlayerStart;
        player_ = world.spawn_with(
            ecs::LocalTransform{tf}, WorldTransform{tf}, kPlayerRigidBody, player_collider());
    }

    // The crate: the one DYNAMIC body, and the reason the player is kinematic-with-velocity rather
    // than teleported. Walking into it pushes it.
    {
        core::Transform tf{};
        tf.translation = {0.0f, kCrateHalf, -2.5f};
        // CrateBody starts at the spawn pose and at rest; record_crate overwrites it after every
        // step with what the solver actually produced.
        crate_ = world.spawn_with(ecs::LocalTransform{tf},
                                  WorldTransform{tf},
                                  kCrateRigidBody,
                                  crate_collider(),
                                  CrateBody{tf.translation, tf.rotation, {}, {}, 0});
    }

    // The five markers: SENSOR spheres. A sensor joins the broadphase and the exact narrowphase
    // like any other body but is skipped by the solver, so it reports overlaps and exchanges no
    // impulse — the player walks through it and the trigger fires.
    //
    // Placed on a ring rather than at random, because a sample's scene is documentation: a reader
    // can see at a glance that five of something is five of something.
    for (int i = 0; i < kPickupCount; ++i) {
        core::Transform tf{};
        tf.translation = marker_position(i);
        const ecs::Entity e = world.spawn_with(
            ecs::LocalTransform{tf},
            WorldTransform{tf},
            physics::RigidBody{.motion = kStatic},
            physics::Collider{.shape_type = kSphereShape, .radius = kPickupRadius, .sensor = true});
        pickups_.push_back(Pickup{e, physics::BodyId{}});
    }
    ReplicatedGameState state{};
    state.player_position = kPlayerStart;
    if (const auto* wt = world.get<WorldTransform>(crate_)) {
        state.crate_position = wt->value.translation;
        state.crate_rotation = wt->value.rotation;
    }
    state_entity_ = world.spawn_with(WorldTransform{}, state);
    return true;
}

// Receive the player's wish for the next tick. Before m20.1 a host called this per FRAME and the
// tick read what was stored; now the engine delivers an ActionState per TICK and the game reads it
// here at the top of the tick — the frame-versus-tick split is the engine's to get right, which is
// the point of the engine owning input.
void HelloGame::apply_intent(const Intent& intent) {
    intent_ = intent;
    if (intent.quit) {
        quit_ = true;
    }
}

// One fixed simulation step, and the rules.
//
// Order matters and is the canonical one (`docs/design/simulation-tick.md`): move the player (the
// game owns a kinematic pose), then let the bridge push that pose in and step, then read what the
// step reported. Reading triggers before the step would read the previous tick's.
void HelloGame::fixed_tick(app::TickContext& ctx) {
    apply_intent(intent_from(ctx.input));
    const float step = static_cast<float>(ctx.dt);
    // `--interaction-fault kick@T`: a SERVER-ONLY impulse on the crate (m20.6c). Nothing in the
    // client's input, prediction or checkpoint knows about it until the next checkpoint carries
    // the changed velocity -- which is the point: it is a disturbance the prediction cannot
    // foresee. +z is toward the player in the proof's tape (the player walks in from +z). The
    // body is bound after the first step, and a kick before that is a no-op the server counted.
    if (ctx.interaction_kick && bound_) {
        physics::BodyState crate{};
        if (physics_.get_body_state(crate_body_, crate))
            physics_.apply_impulse(
                crate_body_, core::Vec3{0.0f, 0.0f, kCrateKickImpulse}, crate.position);
    }
    move_player(ctx.world, step);
    sync_.step(ctx.world, physics_, step);
    // Bind the markers' body ids once the bridge has created them. It cannot be done in `setup`:
    // `RigidBodyHandle` is added by `reconcile`, which first runs inside this call.
    bind_bodies(ctx.world);
    collect(ctx.world);
    record_crate(ctx.world);
    if (auto* replicated = state()) {
        throw_step(ctx, *replicated);
        ++replicated->ticks;
        if (const auto* wt = ctx.world.get<ecs::WorldTransform>(crate_)) {
            replicated->crate_position = wt->value.translation;
            replicated->crate_rotation = wt->value.rotation;
            if (auto* local = ctx.world.get<ecs::LocalTransform>(crate_)) {
                local->value = wt->value;
                ctx.world.mark_changed<ecs::LocalTransform>(crate_);
            }
        }
        ctx.world.mark_changed<ReplicatedGameState>(state_entity_);
    }
}

void HelloGame::throw_step(app::TickContext& ctx, ReplicatedGameState& replicated) {
    // A cooldown measures full ticks AFTER a launch. Charges never regenerate: exactly three
    // launches are possible per run, making the refusal arithmetic observable in the proof.
    if (replicated.throw_cooldown > 0)
        --replicated.throw_cooldown;
    const core::Vec3 wished{ctx.input.axis(kAxisMoveX), 0.0f, ctx.input.axis(kAxisMoveZ)};
    if (core::length(wished) > 1e-4f)
        replicated.player_facing = wished * (1.0f / core::length(wished));

    // A synthesized stand-in repeats levels but does not own a new edge. Guard `applied` too:
    // no guessed server input may consume an ability the client never executed.
    const bool real_command = !ctx.command || ctx.command->applied;
    if (ctx.input.was_pressed(kButtonThrow) && real_command) {
        const auto key =
            ctx.command ? replication::SpawnKey{ctx.command->incarnation, ctx.command->sequence, 0}
                        : replication::SpawnKey{};
        std::uint8_t refusal = 0;
        if (ctx.refuse_spawn || replicated.throw_charges == 0)
            refusal = 1;
        else if (ctx.world.is_alive(projectile_))
            refusal = 3;
        else if (replicated.throw_cooldown > 0)
            refusal = 2;
        if (refusal != 0) {
            if (ctx.command && ctx.spawns)
                ctx.spawns->push_back({app::SpawnReport::Kind::Refused, key, {}, refusal});
        } else {
            --replicated.throw_charges;
            replicated.throw_cooldown = 60;
            const core::Vec3 start = replicated.player_position + core::Vec3{0.0f, 0.12f, 0.0f};
            ThrownProjectile projectile{start, replicated.player_facing * kThrowSpeed, 0};
            core::Transform transform{};
            transform.translation = start;
            projectile_ = ctx.world.spawn_with(
                ecs::LocalTransform{transform}, ecs::WorldTransform{transform}, projectile);
            projectile_key_ = key;
            if (ctx.command && ctx.spawns)
                ctx.spawns->push_back({app::SpawnReport::Kind::Launched, key, projectile_, 0});
        }
    }
    if (!ctx.world.is_alive(projectile_))
        return;
    auto* projectile = ctx.world.get<ThrownProjectile>(projectile_);
    if (!projectile)
        return;
    const core::Vec3 before = projectile->position;
    const float dt = static_cast<float>(ctx.dt);
    projectile->velocity.y += kThrowGravity * dt;
    projectile->position = projectile->position + projectile->velocity * dt;
    ++projectile->age;
    // Only the authoritative game tick awards a point. The prediction hook advances flight but
    // never tests the target, so rollback cannot predict a hit or duplicate a score.
    // fixed_tick only runs where the simulation is authoritative -- the dedicated server, or a
    // local game -- while a connected client predicts through `prediction_hooks`, which has no hit
    // test at all. So no provenance check here: local play scores too.
    const bool hit = segment_hits_target(before, projectile->position, throw_target_);
    if (hit)
        ++replicated.target_score;
    if (hit || projectile->age >= 45) {
        if (ctx.command && ctx.spawns)
            ctx.spawns->push_back({app::SpawnReport::Kind::Ended, projectile_key_, projectile_, 0});
        // The server host retracts a replicated NetId when it consumes Ended. Local play owns no
        // NetId and can despawn immediately.
        // A listening server pump despawns it through the replicator (retracting its NetId
        // first); with nobody listening the game owns the despawn itself.
        if (!(ctx.command && ctx.spawns))
            ctx.world.despawn(projectile_);
        projectile_ = {};
        return;
    }
    if (auto* transform = ctx.world.get<ecs::WorldTransform>(projectile_))
        transform->value.translation = projectile->position;
    if (auto* local = ctx.world.get<ecs::LocalTransform>(projectile_)) {
        local->value.translation = projectile->position;
        ctx.world.mark_changed<ecs::LocalTransform>(projectile_);
    }
    ctx.world.mark_changed<ThrownProjectile>(projectile_);
}

// Integrate the stored intent into the player's transform, and clamp it to the arena.
//
// The clamp is a game rule rather than a wall, on purpose: an arena fenced with static boxes would
// be more realistic and would also mean a kinematic body pressed into a static one every time the
// player holds a direction, which is a contact the solver cannot resolve (neither body yields) and
// a permanent source of jitter. Games clamp.
void HelloGame::move_player(ecs::World& world, float dt) {
    auto* replicated = state();
    if (!replicated)
        return;
    integrate_player(*replicated, intent_, dt);
    if (auto* wt = world.get<ecs::WorldTransform>(player_)) {
        wt->value.translation = replicated->player_position;
    }
    if (auto* local = world.get<ecs::LocalTransform>(player_)) {
        local->value.translation = replicated->player_position;
        world.mark_changed<ecs::LocalTransform>(player_);
    }
}

void HelloGame::bind_bodies(ecs::World& world) {
    if (bound_) {
        return;
    }
    bool all = true;
    for (Pickup& p : pickups_) {
        if (const auto* handle = world.get<physics::RigidBodyHandle>(p.entity)) {
            p.body = handle->body;
        } else {
            all = false;
        }
    }
    if (const auto* handle = world.get<physics::RigidBodyHandle>(player_)) {
        player_body_ = handle->body;
    } else {
        all = false;
    }
    if (const auto* handle = world.get<physics::RigidBodyHandle>(crate_)) {
        crate_body_ = handle->body;
    } else {
        all = false;
    }
    bound_ = all;
}

// Read the tick's trigger stream and collect whatever the player entered.
//
// `TriggerEvent`'s `a`/`b` are in canonical index order, so EITHER end may be the sensor and a
// consumer has to check both — the header says so, and a consumer that checks only `b` works until
// the day body indices happen to come out the other way round.
void HelloGame::collect(ecs::World& world) {
    auto* replicated = state();
    if (!replicated)
        return;
    for (const physics::TriggerEvent& ev : physics_.trigger_events()) {
        if (ev.phase != physics::ContactPhase::Began) {
            continue; // Persisted/Ended would collect the same marker again, or on the way out
        }
        const bool player_involved = ev.a == player_body_ || ev.b == player_body_;
        if (!player_involved) {
            continue; // a trigger the crate rolled through is not a pickup
        }
        const physics::BodyId other = ev.a == player_body_ ? ev.b : ev.a;
        for (std::size_t index = 0; index < pickups_.size(); ++index) {
            Pickup& p = pickups_[index];
            if (collection_time(*replicated, index) != 0 || !(p.body == other)) {
                continue;
            }
            collection_time(*replicated, index) = replicated->ticks + 1;
            ++replicated->score;
            // Despawning takes the RigidBodyHandle with it, which is precisely why the bridge keeps
            // its own entity↔body roster: next tick's `reconcile` destroys the orphaned sensor body
            // from that roster rather than failing to find it in a query.
            world.despawn(p.entity);
        }
    }
}

// Copy the crate's solver state into its CrateBody after the step (ADR-0106 §2): the owner
// checkpoint needs the velocity the physics world keeps privately, and the membership policy needs
// to know whether the player is touching it -- a contact REGION between the two bodies that is
// Began or Persisted this tick (an Ended region is the tick they separated). Marked changed only
// when something changed, so a sleeping crate costs the world stream nothing.
void HelloGame::record_crate(ecs::World& world) {
    auto* record = world.get<CrateBody>(crate_);
    physics::BodyState body{};
    if (record == nullptr || !physics_.get_body_state(crate_body_, body))
        return;
    std::uint32_t touching = 0;
    for (const physics::ContactEvent& ev : physics_.contact_events()) {
        const bool pair = (ev.a == player_body_ && ev.b == crate_body_) ||
                          (ev.a == crate_body_ && ev.b == player_body_);
        if (pair && ev.phase != physics::ContactPhase::Ended)
            touching = 1;
    }
    const CrateBody next{
        body.position, body.orientation, body.linear_velocity, body.angular_velocity, touching};
    // Field by field, not memcmp: Quat is 16-byte aligned, so CrateBody has padding bytes.
    const auto same = [](core::Vec3 a, core::Vec3 b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    const bool unchanged = same(record->position, next.position) &&
                           same(record->linear_velocity, next.linear_velocity) &&
                           same(record->angular_velocity, next.angular_velocity) &&
                           record->orientation.x == next.orientation.x &&
                           record->orientation.y == next.orientation.y &&
                           record->orientation.z == next.orientation.z &&
                           record->orientation.w == next.orientation.w &&
                           record->touching_player == next.touching_player;
    if (!unchanged) {
        *record = next;
        world.mark_changed<CrateBody>(crate_);
    }
}

// ── The autopilot ─────────────────────────────────────────────────────────────────────────────
//
// The self-check's input device, now a method the ENGINE can call (`--autopilot`): it walks the
// ring by steering toward each uncollected marker in turn — a fixed, device-free, frame-rate-free
// sequence of actions. Until m20.1 this was `ScriptedPlayer`, a host-side class holding a reference
// to the game; it is the game's now because only the game knows where its markers are, and a host
// carrying its own copy of the scene keeps passing after the scene moves.
app::ActionState HelloGame::autopilot() const {
    app::ActionState input{};
    for (int i = 0; i < kPickupCount; ++i) {
        if (collected_on(static_cast<std::size_t>(i)) != 0) {
            continue;
        }
        const core::Vec3 target = marker_position(i);
        const auto position = player_position();
        input.axes[kAxisMoveX] = target.x - position.x;
        input.axes[kAxisMoveZ] = target.z - position.z;
        return input;
    }
    input.pressed = 1u << kButtonQuit; // everything collected: ask to stop
    return input;
}

// What a dedicated server and a player must agree on: the rules' state and the one pose the
// SOLVER owns (the crate — the player's pose is the game's own arithmetic, the crate's is physics).
// Not the camera, not anything presentation touched.
std::uint64_t replicated_digest(const ecs::World& world) {
    Fnv1a h;
    // Query is read-only here; World currently exposes query() only on a non-const object.
    auto& readable = const_cast<ecs::World&>(world);
    readable.query<ReplicatedGameState>().for_each(
        [&](ecs::Entity, const ReplicatedGameState& state) {
            h.u64(state.ticks);
            h.u64(static_cast<std::uint64_t>(state.score));
            for (std::size_t i = 0; i < HelloGame::kPickupCount; ++i)
                h.u64(collection_time(state, i));
            h.vec3(state.player_position);
            h.vec3(state.crate_position);
            h.f32(state.crate_rotation.x);
            h.f32(state.crate_rotation.y);
            h.f32(state.crate_rotation.z);
            h.f32(state.crate_rotation.w);
            h.u64(state.throw_charges);
            h.u64(state.throw_cooldown);
            h.u64(state.target_score);
            h.vec3(state.player_facing);
        });
    readable.query<ThrownProjectile>().for_each(
        [&](ecs::Entity, const ThrownProjectile& projectile) {
            h.vec3(projectile.position);
            h.vec3(projectile.velocity);
            h.u64(projectile.age);
        });
    return h.value();
}

std::uint64_t HelloGame::state_digest(const ecs::World& world) const {
    return replicated_digest(world);
}

ReplicatedGameState* HelloGame::state() const noexcept {
    return world_ ? world_->get<ReplicatedGameState>(state_entity_) : nullptr;
}

int HelloGame::score() const noexcept {
    const auto* replicated = state();
    return replicated ? static_cast<int>(replicated->score) : 0;
}

std::uint64_t HelloGame::ticks() const noexcept {
    const auto* replicated = state();
    return replicated ? replicated->ticks : 0;
}

core::Vec3 HelloGame::player_position() const noexcept {
    const auto* replicated = state();
    return replicated ? replicated->player_position : core::Vec3{};
}

std::uint64_t HelloGame::collected_on(std::size_t index) const {
    const auto* replicated = state();
    return replicated && index < kPickupCount ? collection_time(*replicated, index) : 0;
}

// ── Presentation ──────────────────────────────────────────────────────────────────────────────
//
// Everything below runs only in a mode with a device. It ADDS render components to the entities
// `setup` made and spawns the purely visual ones; it moves nothing the rules read. The dedicated
// proof in main.cpp compares a run with this hook against a run without it, which is how "must not
// change what the simulation computes" is held rather than hoped.
void HelloGame::present_setup(app::PresentSetupContext& ctx) {
    ++presentation_calls_;
    ecs::World& world = ctx.world;
    render::register_render_components(world);
    ctx.renderer.set_ambient(0.04f, 0.045f, 0.06f);

    const render::MeshId floor_mesh =
        ctx.meshes.add(render::make_plane(kArenaHalf, 12.0f), "floor");
    const render::MeshId crate_mesh = ctx.meshes.add(render::make_cube(kCrateHalf), "crate");
    const render::MeshId marker_mesh =
        ctx.meshes.add(render::make_uv_sphere(kPickupRadius, 16, 24), "marker");
    // The player is drawn as a sphere rather than a capsule because `make_capsule` does not exist
    // and inventing one in a sample would be an engine change in `samples/` clothing. The COLLIDER
    // is a real capsule; only the silhouette is approximate, and this comment says so rather than
    // the reader having to notice.
    const render::MeshId player_mesh =
        ctx.meshes.add(render::make_uv_sphere(kPlayerRadius, 16, 24), "player");
    const render::MeshId projectile_mesh =
        ctx.meshes.add(render::make_uv_sphere(0.12f, 12, 16), "projectile");

    render::PbrMaterialDesc m{};
    m.base_color[0] = 0.32f;
    m.base_color[1] = 0.34f;
    m.base_color[2] = 0.38f;
    m.roughness = 0.8f;
    const render::MaterialId floor_mat = ctx.materials.add(m);
    m.base_color[0] = 0.62f;
    m.base_color[1] = 0.42f;
    m.base_color[2] = 0.22f;
    m.roughness = 0.65f;
    const render::MaterialId crate_mat = ctx.materials.add(m);
    // Vivid green, and the choice is the TEST's rather than the art's: the headless check in
    // main.cpp identifies the markers by hue, so their colour has to be one nothing else in the
    // scene can be mistaken for. Gold was the first choice and the brown crate satisfied it — the
    // falsification caught that, which is the entire reason to run one.
    m.base_color[0] = 0.10f;
    m.base_color[1] = 0.92f;
    m.base_color[2] = 0.28f;
    m.metallic = 0.0f;
    m.roughness = 0.3f;
    const render::MaterialId marker_mat = ctx.materials.add(m);
    m.base_color[0] = 0.20f;
    m.base_color[1] = 0.55f;
    m.base_color[2] = 0.85f;
    m.metallic = 0.0f;
    m.roughness = 0.4f;
    const render::MaterialId player_mat = ctx.materials.add(m);
    m.base_color[0] = 0.96f;
    m.base_color[1] = 0.65f;
    m.base_color[2] = 0.18f;
    const render::MaterialId projectile_mat = ctx.materials.add(m);

    // The visual floor plane sits at y = 0 — the slab's TOP — while the collider is centred half a
    // slab lower. Its own entity because a plane mesh and a box collider do not share a centre, and
    // scaling one to fit the other is how a sample grows a subtle offset every later reader has to
    // re-derive.
    (void)world.spawn_with(ecs::WorldTransform{core::Transform{}},
                           render::MeshRef{floor_mesh},
                           render::MaterialRef{floor_mat});

    world.add_component(player_, render::MeshRef{player_mesh});
    world.add_component(player_, render::MaterialRef{player_mat});
    world.add_component(crate_, render::MeshRef{crate_mesh});
    world.add_component(crate_, render::MaterialRef{crate_mat});
    for (const Pickup& p : pickups_) {
        if (world.is_alive(p.entity)) {
            world.add_component(p.entity, render::MeshRef{marker_mesh});
            world.add_component(p.entity, render::MaterialRef{marker_mat});
        }
    }

    // The sun. Aiming a light is rotating its entity (its forward is −z), the same convention a
    // camera looks down, so this is a light tilted down and to the side.
    {
        core::Transform tf{};
        tf.rotation = core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.6f) *
                                      core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.05f));
        (void)world.spawn_with(ecs::WorldTransform{tf},
                               render::DirectionalLight{0.98f, 0.96f, 0.90f, 3.2f});
    }
    camera_ =
        world.spawn_with(ecs::WorldTransform{chase_camera(player_position())}, render::Camera{});
    // A single render proxy follows the predicted projectile, then its adopted roster identity.
    // It starts outside the arena so no device work is needed when the Throw is absent.
    core::Transform hidden{};
    hidden.translation.y = -1000.0f;
    projectile_visual_ = world.spawn_with(ecs::WorldTransform{hidden},
                                          render::MeshRef{projectile_mesh},
                                          render::MaterialRef{projectile_mat});
}

void HelloGame::present_frame(app::PresentFrameContext& ctx) {
    ++presentation_calls_;
    if (auto* wt = ctx.world.get<ecs::WorldTransform>(camera_)) {
        wt->value = chase_camera(player_position());
    }
    present_projectile(ctx.world, ctx.world);
}

void HelloGame::present_prediction(app::PresentFrameContext& ctx, ecs::World& prediction_world) {
    // This second pose pass wins over the mirror in connected play, including the first local
    // tick before the authoritative projectile exists. The proxy itself never multiplies.
    present_projectile(ctx.world, prediction_world);
}

void HelloGame::present_projectile(ecs::World& render_world, ecs::World& source_world) {
    auto* visual = render_world.get<ecs::WorldTransform>(projectile_visual_);
    if (!visual)
        return;
    core::Vec3 position{0.0f, -1000.0f, 0.0f};
    source_world.query<ThrownProjectile>().for_each(
        [&](ecs::Entity, const ThrownProjectile& projectile) {
            if (projectile.age < 45)
                position = projectile.position;
        });
    visual->value.translation = position;
}

bool HelloGame::bind_remote_world(ecs::World& world) {
    world_ = &world;
    pickups_.clear();
    player_ = owned_player(world);
    world.query<ReplicatedGameState>().for_each(
        [&](ecs::Entity entity, const ReplicatedGameState&) { state_entity_ = entity; });
    world.query<physics::RigidBody, physics::Collider>().for_each(
        [&](ecs::Entity entity, const physics::RigidBody& body, const physics::Collider& collider) {
            if (body.motion == kDynamic)
                crate_ = entity;
            if (collider.sensor)
                pickups_.push_back(Pickup{entity, physics::BodyId{}});
        });
    return world.is_alive(player_) && world.is_alive(state_entity_) && world.is_alive(crate_) &&
           pickups_.size() == kPickupCount;
}

// ── The content ───────────────────────────────────────────────────────────────────────────────
//
// The floor: a static slab whose top surface is exactly y = 0, so every other height in this file
// is a height above the ground and not an offset from a slab's centre. Written with a
// LocalTransform, the reflected (savable) half of a pose — WorldTransform is derived and never in a
// file.
void build_arena(ecs::World& world) {
    ecs::register_transform_components(world);
    physics::register_physics_components(world);
    core::Transform tf{};
    tf.translation = {0.0f, -kFloorHalfY, 0.0f};
    (void)world.spawn_with(ecs::LocalTransform{tf}, kFloorRigidBody, floor_collider());
}

void register_components(ecs::World& world) {
    ecs::register_transform_components(world);
    physics::register_physics_components(world);
    (void)world.register_component<ReplicatedGameState>();
    (void)world.register_component<ThrownProjectile>();
    (void)world.register_component<CrateBody>(); // last: earlier component ids do not move
}

ecs::Entity owned_player(ecs::World& world) {
    ecs::Entity result{};
    world.query<physics::RigidBody, physics::Collider>().for_each(
        [&](ecs::Entity entity, const physics::RigidBody& body, const physics::Collider& collider) {
            if (body.motion == kKinematic && collider.shape_type == kCapsuleShape)
                result = entity;
        });
    return result;
}

replication::PredictionHooks prediction_hooks_impl(ecs::World& world,
                                                   ecs::Entity player,
                                                   std::shared_ptr<ClientSpawnState> spawns) {
    ecs::Entity state_entity{};
    world.query<ReplicatedGameState>().for_each(
        [&](ecs::Entity entity, const ReplicatedGameState&) { state_entity = entity; });
    auto* bound = &world;
    replication::PredictionHooks hooks{};
    hooks.capture = [bound, state_entity, spawns](replication::PredictedEntityId id) {
        if (spawns && spawns->entity_for(id) != ecs::kNullEntity) {
            const auto* projectile = bound->get<ThrownProjectile>(spawns->entity_for(id));
            return projectile ? encode_projectile(*projectile) : std::vector<std::byte>{};
        }
        std::vector<std::byte> bytes;
        const auto* state = bound->get<ReplicatedGameState>(state_entity);
        if (!state)
            return bytes;
        bytes.reserve(24);
        const auto append = [&](float value) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(std::byte{static_cast<std::uint8_t>(bits >> shift)});
        };
        append(state->player_position.x);
        append(state->player_position.y);
        append(state->player_position.z);
        append(state->player_velocity.x);
        append(state->player_velocity.y);
        append(state->player_velocity.z);
        return bytes;
    };
    hooks.restore = [bound, state_entity, player, spawns](replication::PredictedEntityId id,
                                                          std::span<const std::byte> bytes) {
        if (spawns && spawns->entity_for(id) != ecs::kNullEntity) {
            restore_projectile(*bound, spawns->entity_for(id), bytes);
            return;
        }
        if (bytes.size() != 24)
            return;
        auto* state = bound->get<ReplicatedGameState>(state_entity);
        if (!state)
            return;
        const auto read = [&](std::size_t offset) {
            std::uint32_t bits = 0;
            for (unsigned i = 0; i < 4; ++i)
                bits |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (i * 8);
            return std::bit_cast<float>(bits);
        };
        state->player_position = {read(0), read(4), read(8)};
        state->player_velocity = {read(12), read(16), read(20)};
        bound->mark_changed<ReplicatedGameState>(state_entity);
        if (auto* transform = bound->get<ecs::WorldTransform>(player))
            transform->value.translation = state->player_position;
        if (auto* local = bound->get<ecs::LocalTransform>(player)) {
            local->value.translation = state->player_position;
            bound->mark_changed<ecs::LocalTransform>(player);
        }
    };
    // Charges, cooldown and facing form the owner's GROUP state: a checkpoint must restore them
    // atomically with the player and the projectile, or replay could launch using old resources.
    hooks.capture_group = [bound, state_entity] {
        std::vector<std::byte> bytes;
        const auto* state = bound->get<ReplicatedGameState>(state_entity);
        if (!state)
            return bytes;
        const auto append = [&](std::uint32_t value) {
            for (unsigned shift = 0; shift < 32; shift += 8)
                bytes.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
        };
        append(state->throw_charges);
        append(state->throw_cooldown);
        append(std::bit_cast<std::uint32_t>(state->player_facing.x));
        append(std::bit_cast<std::uint32_t>(state->player_facing.y));
        append(std::bit_cast<std::uint32_t>(state->player_facing.z));
        return bytes;
    };
    hooks.restore_group = [bound, state_entity](std::span<const std::byte> bytes) {
        if (bytes.size() != 20)
            return;
        auto* state = bound->get<ReplicatedGameState>(state_entity);
        if (!state)
            return;
        const auto read = [&](std::size_t offset) {
            std::uint32_t value = 0;
            for (unsigned i = 0; i < 4; ++i)
                value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8 * i);
            return value;
        };
        state->throw_charges = read(0);
        state->throw_cooldown = read(4);
        state->player_facing = {std::bit_cast<float>(read(8)),
                                std::bit_cast<float>(read(12)),
                                std::bit_cast<float>(read(16))};
        bound->mark_changed<ReplicatedGameState>(state_entity);
    };
    hooks.simulate = [bound, state_entity, player, spawns](
                         std::span<const replication::PredictedEntityId> members,
                         const replication::InputCommand& command,
                         const replication::PredictionStep& step) {
        auto* state = bound->get<ReplicatedGameState>(state_entity);
        if (!state)
            return;
        app::ActionState actions{};
        actions.axes[kAxisMoveX] = command.move_x;
        actions.axes[kAxisMoveZ] = command.move_y;
        integrate_player(*state, intent_from(actions), static_cast<float>(step.fixed_dt));
        if (state->throw_cooldown > 0)
            --state->throw_cooldown;
        const core::Vec3 wished{command.move_x, 0.0f, command.move_y};
        if (core::length(wished) > 1e-4f)
            state->player_facing = wished * (1.0f / core::length(wished));
        // "At most one live projectile" asks about THIS step's simulated set, as the server asks
        // about its own `projectile_`. A guess the reconciler has dropped for replay is still an
        // entity in this world, holding its pre-replay age, until the replay recreates or
        // discards it; querying the whole world would let that leftover veto its own recreation,
        // and the guess would be lost under the very replay meant to make it again (measured:
        // `spawn_not_recreated` 1 and `spawn_adopted` 0 whenever a replay preceded the verdict).
        bool live_projectile = false;
        if (spawns) {
            for (const auto id : members) {
                const auto* p = bound->get<ThrownProjectile>(spawns->entity_for(id));
                live_projectile |= p != nullptr && p->age < 45;
            }
        } else {
            bound->query<ThrownProjectile>().for_each(
                [&](ecs::Entity, const ThrownProjectile& p) { live_projectile |= p.age < 45; });
        }
        if ((command.pressed & (1u << kButtonThrow)) != 0 && state->throw_charges > 0 &&
            state->throw_cooldown == 0 && !live_projectile) {
            bool launched = true;
            if (spawns) {
                const core::Vec3 start = state->player_position + core::Vec3{0.0f, 0.12f, 0.0f};
                const ThrownProjectile projectile{start, state->player_facing * kThrowSpeed, 0};
                const auto bytes = encode_projectile(projectile);
                // This is a step-context provisional overlay request. The reconciler owns the
                // replay-stable id; the game spends a charge only when it grants the guess.
                launched =
                    spawns->request({spawns->incarnation, command.sequence, 0}, bytes).has_value();
            }
            if (launched) {
                --state->throw_charges;
                state->throw_cooldown = 60;
            }
        }
        if (spawns) {
            // The supplied set contains both authoritative roster and provisional overlay members.
            // Flight is predicted, while hit tests and scoring remain in fixed_tick alone.
            for (const auto id : members) {
                const auto entity = spawns->entity_for(id);
                auto* projectile = bound->get<ThrownProjectile>(entity);
                if (!projectile || projectile->age >= 45)
                    continue;
                const float dt = static_cast<float>(step.fixed_dt);
                projectile->velocity.y += kThrowGravity * dt;
                projectile->position = projectile->position + projectile->velocity * dt;
                ++projectile->age;
                if (auto* wt = bound->get<ecs::WorldTransform>(entity))
                    wt->value.translation = projectile->position;
                if (auto* local = bound->get<ecs::LocalTransform>(entity))
                    local->value.translation = projectile->position;
            }
        }
        bound->mark_changed<ReplicatedGameState>(state_entity);
        if (auto* transform = bound->get<ecs::WorldTransform>(player))
            transform->value.translation = state->player_position;
        if (auto* local = bound->get<ecs::LocalTransform>(player)) {
            local->value.translation = state->player_position;
            bound->mark_changed<ecs::LocalTransform>(player);
        }
    };
    return hooks;
}

replication::PredictionHooks prediction_hooks(ecs::World& world, ecs::Entity player) {
    return prediction_hooks_impl(world, player, {});
}

app::GameDefinition::RemotePlayProfile::SpawnPredictionProfile::ClientHooks
client_spawn_hooks(ecs::World& world,
                   ecs::Entity player,
                   std::uint32_t incarnation,
                   SpawnRequest request) {
    auto state = std::make_shared<ClientSpawnState>();
    state->world = &world;
    state->incarnation = incarnation;
    state->request = std::move(request);
    app::GameDefinition::RemotePlayProfile::SpawnPredictionProfile::ClientHooks hooks{};
    hooks.prediction = prediction_hooks_impl(world, player, state);
    hooks.spawn.create = [state](SpawnId id) {
        if (state->entity_for(id) == ecs::kNullEntity)
            state->entities.emplace_back(id, state->world->spawn_with(ThrownProjectile{}));
    };
    hooks.spawn.destroy = [state](SpawnId id) {
        const auto it = std::find_if(state->entities.begin(),
                                     state->entities.end(),
                                     [id](const auto& member) { return member.first == id; });
        if (it == state->entities.end())
            return;
        state->world->despawn(it->second);
        state->entities.erase(it);
    };
    hooks.spawn.adopt = [state](SpawnId provisional, SpawnId authoritative) {
        for (auto& [id, entity] : state->entities)
            if (id == provisional) {
                id = authoritative;
                return;
            }
    };
    hooks.entity_for = [state](SpawnId id) { return state->entity_for(id); };
    hooks.is_live = [state](SpawnId id) {
        const auto* projectile = state->world->get<ThrownProjectile>(state->entity_for(id));
        return projectile && projectile->age < 45;
    };
    return hooks;
}

// ── The interaction group (m20.6b, ADR-0106) ────────────────────────────────────────────────
//
// The crate is predicted while the server says it is near. Server side, the engine's membership
// policy reads `interaction_sample` each tick and captures `capture_crate` into the owner
// checkpoint; client side, `interaction_client_hooks` gives the prediction a physics world.

namespace {

using InteractionProfile = app::GameDefinition::RemotePlayProfile::InteractionProfile;

void append_f32(std::vector<std::byte>& bytes, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(std::byte{static_cast<std::uint8_t>(bits >> shift)});
}

std::vector<std::byte> encode_crate(const physics::BodyState& body) {
    std::vector<std::byte> bytes;
    bytes.reserve(kCrateStateBytes);
    for (const float value : {body.position.x,
                              body.position.y,
                              body.position.z,
                              body.orientation.x,
                              body.orientation.y,
                              body.orientation.z,
                              body.orientation.w,
                              body.linear_velocity.x,
                              body.linear_velocity.y,
                              body.linear_velocity.z,
                              body.angular_velocity.x,
                              body.angular_velocity.y,
                              body.angular_velocity.z})
        append_f32(bytes, value);
    return bytes;
}

std::optional<physics::BodyState> decode_crate(std::span<const std::byte> bytes) {
    if (bytes.size() != kCrateStateBytes)
        return std::nullopt;
    const auto read = [&](std::size_t index) {
        std::uint32_t bits = 0;
        for (unsigned i = 0; i < 4; ++i)
            bits |= std::to_integer<std::uint32_t>(bytes[index * 4 + i]) << (8 * i);
        return std::bit_cast<float>(bits);
    };
    physics::BodyState body{};
    body.position = {read(0), read(1), read(2)};
    body.orientation = {read(3), read(4), read(5), read(6)};
    body.linear_velocity = {read(7), read(8), read(9)};
    body.angular_velocity = {read(10), read(11), read(12)};
    return body;
}

ecs::Entity interaction_object(ecs::World& world) {
    ecs::Entity result{};
    world.query<physics::RigidBody, physics::Collider>().for_each(
        [&](ecs::Entity entity, const physics::RigidBody& body, const physics::Collider& collider) {
            if (body.motion == kDynamic && !collider.sensor)
                result = entity;
        });
    return result;
}

// Distance on the FLOOR PLANE between centres: the capsule's centre sits higher than the crate's,
// and a vertical offset that never changes is not distance the player could close.
replication::InteractionSample
interaction_sample(const ecs::World& world, ecs::Entity player, ecs::Entity object) {
    replication::InteractionSample sample{};
    const auto* player_pose = world.get<ecs::WorldTransform>(player);
    const auto* crate = world.get<CrateBody>(object);
    if (!player_pose || !crate) {
        // No facts: report "far, still, untouched" so a member drifts out through the normal
        // leave rule rather than staying forever on a broken sample.
        sample.distance = std::numeric_limits<float>::max();
        return sample;
    }
    const core::Vec3 offset = crate->position - player_pose->value.translation;
    sample.distance = std::sqrt(offset.x * offset.x + offset.z * offset.z);
    sample.speed = core::length(crate->linear_velocity);
    sample.touching = crate->touching_player != 0;
    return sample;
}

std::vector<std::byte> capture_crate(const ecs::World& world, ecs::Entity object) {
    const auto* crate = world.get<CrateBody>(object);
    if (!crate)
        return {};
    return encode_crate(physics::BodyState{
        crate->position, crate->orientation, crate->linear_velocity, crate->angular_velocity});
}

physics::BodyDesc body_desc(const physics::RigidBody& rb,
                            const physics::Collider& col,
                            const physics::BodyState& state) {
    // PhysicsSync::reconcile's conversion, field for field, so a prediction body and the
    // authoritative one are the same body.
    physics::BodyDesc d{};
    d.motion = static_cast<physics::MotionType>(rb.motion);
    d.shape.type = static_cast<physics::ShapeType>(col.shape_type);
    d.shape.radius = col.radius;
    d.shape.half_extents = {col.half_x, col.half_y, col.half_z};
    d.shape.half_height = col.half_height;
    d.mass = rb.mass;
    d.friction = rb.friction;
    d.restitution = rb.restitution;
    d.linear_damping = rb.linear_damping;
    d.angular_damping = rb.angular_damping;
    d.gravity_factor = rb.gravity_factor;
    d.sensor = col.sensor;
    d.position = state.position;
    d.orientation = state.orientation;
    d.linear_velocity = state.linear_velocity;
    d.angular_velocity = state.angular_velocity;
    return d;
}

// The client's PREDICTION PHYSICS WORLD (ADR-0106 §3): a separate, tiny PhysicsWorld -- floor,
// kinematic player, crate; no sensors, so prediction can never collect a pickup -- because the
// authoritative world can only be stepped whole. Three rules make a replay a pure function of
// (checkpoint, commands):
//
//   1. REBUILT ON EVERY RESTORE. Restoring the crate makes a FRESH world and creates its bodies in
//      a fixed order from the checkpoint bytes. A teleport (`set_body_state`) would keep the
//      warm-start contact cache and the solver state of the timeline the correction just threw
//      away, and replaying from "the same" checkpoint would then depend on what happened before.
//   2. DIRECT KINEMATIC DRIVE. The player body is set with `set_body_state` from the predicted
//      controller state each step -- position, and the velocity the controller already computed
//      as actual displacement / dt (`integrate_player`), which is exactly what PhysicsSync derives
//      on the server. Never `PhysicsSync::push_in`: it differentiates against a private
//      last-pushed pose, so after a restore it would invent a velocity the size of the correction
//      and fire the crate off.
//   3. SLEEPING OFF. A sleeping body is state no checkpoint carries.
//
// Accepted consequence: the server's world keeps a warm cache this one never has, so the two
// crates are not bit-identical even with nothing disturbing them. That residual is measured.
struct InteractionPhysics {
    ecs::World* world = nullptr;
    ecs::Entity player{}; // the predicted player, which also holds its ReplicatedGameState
    replication::PredictedEntityId object{};
    bool rebuild_on_restore = true;
    // The `stale-restore` control and the bytes it restores from.
    bool stale_restore = false;
    std::vector<std::byte> previous_bytes;
    std::unique_ptr<physics::PhysicsWorld> physics;
    physics::BodyId player_body{};
    physics::BodyId crate_body{};
    bool touching = false;

    void rebuild(const physics::BodyState& crate) {
        physics = std::make_unique<physics::PhysicsWorld>();
        physics->set_sleeping_enabled(false);
        physics::BodyState floor{};
        floor.position = {0.0f, -kFloorHalfY, 0.0f};
        (void)physics->create_body(body_desc(kFloorRigidBody, floor_collider(), floor));
        physics::BodyState player_state{};
        if (const auto* state = world->get<ReplicatedGameState>(player)) {
            player_state.position = state->player_position;
            player_state.linear_velocity = state->player_velocity;
        }
        player_body =
            physics->create_body(body_desc(kPlayerRigidBody, player_collider(), player_state));
        crate_body = physics->create_body(body_desc(kCrateRigidBody, crate_collider(), crate));
        touching = false;
    }

    void restore(std::span<const std::byte> bytes) {
        // The control restores from the PREVIOUS restore's bytes: one checkpoint old, so every
        // replay starts from a state the server has already left.
        std::vector<std::byte> current(bytes.begin(), bytes.end());
        const auto crate =
            decode_crate(stale_restore && !previous_bytes.empty() ? previous_bytes : current);
        previous_bytes = std::move(current);
        if (!crate)
            return;
        if (rebuild_on_restore || !physics)
            rebuild(*crate);
        else
            (void)physics->set_body_state(crate_body, *crate); // the control: cache survives
    }

    void step(float dt) {
        const auto* state = world->get<ReplicatedGameState>(player);
        if (!physics || !state)
            return;
        physics::BodyState drive{};
        drive.position = state->player_position;
        drive.linear_velocity = state->player_velocity;
        (void)physics->set_body_state(player_body, drive);
        physics->step(dt);
        touching = false;
        for (const physics::ContactEvent& ev : physics->contact_events())
            if (((ev.a == player_body && ev.b == crate_body) ||
                 (ev.a == crate_body && ev.b == player_body)) &&
                ev.phase != physics::ContactPhase::Ended)
                touching = true;
    }
};

InteractionProfile::ClientHooks interaction_client_hooks(
    ecs::World& world,
    ecs::Entity player,
    replication::PredictedEntityId object,
    app::GameDefinition::RemotePlayProfile::SpawnPredictionProfile::ClientHooks inner,
    InteractionProfile::ClientOptions options) {
    auto state = std::make_shared<InteractionPhysics>();
    state->world = &world;
    state->player = player;
    state->object = object;
    state->rebuild_on_restore = options.rebuild_on_restore;
    state->stale_restore = options.stale_restore;
    InteractionProfile::ClientHooks out{};
    out.hooks = inner;
    auto& prediction = out.hooks.prediction;
    auto& spawn = out.hooks.spawn;
    prediction.capture = [state,
                          next = inner.prediction.capture](replication::PredictedEntityId id) {
        if (!(id == state->object))
            return next(id);
        physics::BodyState body{};
        if (!state->physics || !state->physics->get_body_state(state->crate_body, body))
            return std::vector<std::byte>{};
        return encode_crate(body);
    };
    prediction.restore = [state, next = inner.prediction.restore](
                             replication::PredictedEntityId id, std::span<const std::byte> bytes) {
        if (id == state->object)
            state->restore(bytes);
        else
            next(id, bytes);
    };
    // The crate steps AFTER the player's controller, in the same order the server's fixed_tick
    // uses (move the player, then step physics), and only while the crate is in the set.
    prediction.simulate = [state, next = inner.prediction.simulate](
                              std::span<const replication::PredictedEntityId> members,
                              const replication::InputCommand& command,
                              const replication::PredictionStep& step) {
        next(members, command, step);
        state->touching = false;
        if (std::find(members.begin(), members.end(), state->object) != members.end())
            state->step(static_cast<float>(step.fixed_dt));
    };
    // Joining needs no work of its own: PredictionHooks::restore follows `create` at once and
    // rebuilds the world. Leaving drops the world, so a non-member costs nothing.
    spawn.create = [state, next = inner.spawn.create](replication::PredictedEntityId id) {
        if (!(id == state->object))
            next(id);
    };
    spawn.destroy = [state, next = inner.spawn.destroy](replication::PredictedEntityId id) {
        if (id == state->object) {
            state->physics.reset();
            state->previous_bytes.clear();
            state->touching = false;
        } else {
            next(id);
        }
    };
    out.hooks.entity_for = [state, next = inner.entity_for](replication::PredictedEntityId id) {
        return id == state->object ? ecs::Entity{} : next(id);
    };
    out.hooks.is_live = [state, next = inner.is_live](replication::PredictedEntityId id) {
        return !(id == state->object) && next(id);
    };
    out.object_pose = [state]() -> std::optional<core::Transform> {
        physics::BodyState body{};
        if (!state->physics || !state->physics->get_body_state(state->crate_body, body))
            return std::nullopt;
        core::Transform pose{};
        pose.translation = body.position;
        pose.rotation = body.orientation;
        return pose;
    };
    out.touching = [state] { return state->touching; };
    return out;
}

// The push tape (`--input-tape-push`), in client ticks. Walking is 4 m/s = 1/15 m per tick, +z is
// "toward the camera / away from the crate", which rests at z = -2.5 (join radius 5.117 m, leave
// radius 6.117 m, centre to centre on the floor plane). The sequence is what m20.6c's proof needs
// to see, in this order (arithmetic for the nominal timing; the proof gates what ACTUALLY
// happened):
//
//    1-10   stand            crate is a member from the first server tick (2.5 m < 5.117 m)
//   11-90   retreat 5.33 m   the crate leaves once d > 6.117 m (t ~ 65) -- it has been asleep for
//                            ages, so the 30-tick quiet window is already met
//   91-100  stand
//  101-245  walk back -z     joins at d <= 5.117 m (t ~ 142), touches at d ~ 0.85 m (t ~ 206) and
//                            pushes it ~2.7 m for the last 40 ticks: the crate moves at 4 m/s
//  246-275  retreat 2 m      back to ~3.3 m away, still a member (inside 6.117 m)
//  276-315  stand            the crate rests; KICK #1 lands here, the crate being a member
//  316-405  retreat 6 m      beyond 6.117 m, at rest: leaves (t ~ 358)
//  406-425  stand
//  426-520  walk back -z     KICK #2 lands ~15 ticks before the unkicked join (t ~ 501): the crate
//                            slides toward the player (0.41 m, ~24 ticks at mu 0.5) and the
//                            player's closing speed drags the join inside that window -- a JOIN
//                            WHILE SLIDING
//  521-600  stand            the crate comes to rest as a member
app::ActionState push_tape(std::uint64_t tick) {
    app::ActionState actions{};
    if ((tick >= 11 && tick <= 90) || (tick >= 246 && tick <= 275) || (tick >= 316 && tick <= 405))
        actions.axes[kAxisMoveZ] = 1.0f;
    else if ((tick >= 101 && tick <= 245) || (tick >= 426 && tick <= 520))
        actions.axes[kAxisMoveZ] = -1.0f;
    return actions;
}

} // namespace

// ── The definition ────────────────────────────────────────────────────────────────────────────
//
// Everything the engine needs, as values: a name, a tick rate, the key bindings, and a factory.
// −z is forward, the engine's camera convention, so W walks away from the chase camera rather than
// into it — the same mapping `KeyboardIntent` hard-coded before the engine owned input.
app::GameDefinition definition() {
    app::GameDefinition def{};
    def.name = "hello-game";
    def.tick_hz = 60.0;
    def.input.axes = {
        {platform::Key::W, kAxisMoveZ, -1.0f},
        {platform::Key::S, kAxisMoveZ, +1.0f},
        {platform::Key::A, kAxisMoveX, -1.0f},
        {platform::Key::D, kAxisMoveX, +1.0f},
    };
    def.input.buttons = {{platform::Key::Escape, kButtonQuit},
                         {platform::Key::Space, kButtonThrow}};
    // InputCommand's x/y plane becomes this game's x/z plane: W is negative z, D positive x.
    // Keep held and pressed in the same button positions as the keyboard's Escape binding.
    def.network_input = app::GameDefinition::NetworkInputProfile{
        "hello-game.actions", 1, [](const replication::InputCommand& command) -> app::ActionState {
            app::ActionState state{};
            state.axes[kAxisMoveX] = command.move_x;
            state.axes[kAxisMoveZ] = command.move_y;
            state.held = command.held;
            state.pressed = command.pressed;
            return state;
        }};
    def.create = [] { return std::make_unique<HelloGame>(); };
    // Content (m20.2): the arena is a file, found through the content root — `--content`, else
    // `content/` beside the executable, else (a binary still in its build directory) the source
    // tree's `samples/hello-game/content`. The two RIME_GAME_* paths come from
    // `rime_game_content()` in CMakeLists.txt and are inert once the binary is moved.
    def.register_components = register_components;
    def.remote_play = app::GameDefinition::RemotePlayProfile{
        register_components,
        owned_player,
        prediction_hooks,
        1.0 / 60.0,
        replicated_digest,
        [](const app::ActionState& actions) {
            replication::InputCommand command{};
            command.move_x = actions.axis(kAxisMoveX);
            command.move_y = actions.axis(kAxisMoveZ);
            command.held = actions.held;
            command.pressed = actions.pressed;
            return command;
        },
        [](ecs::World& mirror, ecs::World& prediction, ecs::Entity mirror_player) {
            const auto* wt = mirror.get<ecs::WorldTransform>(mirror_player);
            ReplicatedGameState initial{};
            mirror.query<ReplicatedGameState>().for_each(
                [&](ecs::Entity, const ReplicatedGameState& state) { initial = state; });
            // The controller state lives on a separate entity in the authoritative world. Place
            // its prediction copy on the player so this world still contains exactly one entity.
            return prediction.spawn_with(wt ? *wt : ecs::WorldTransform{}, initial);
        },
        [](const ecs::World& world, ecs::Entity player) {
            core::Transform pose{};
            if (const auto* wt = world.get<ecs::WorldTransform>(player))
                pose = wt->value;
            return pose;
        },
        [](std::uint64_t tick) {
            app::ActionState actions{};
            // Two ticks right, one left: each missed command is visible quickly, and the
            // resulting net drift stays inside the arena for this 300-tick proof.
            actions.axes[kAxisMoveX] = ((tick - 1) % 3) < 2 ? 1.0f : -1.0f;
            return actions;
        },
        {},
        {}};
    def.remote_play->spawn_prediction =
        app::GameDefinition::RemotePlayProfile::SpawnPredictionProfile{
            1,
            [](const ecs::World& world, ecs::Entity entity) {
                const auto* projectile = world.get<ThrownProjectile>(entity);
                return projectile ? encode_projectile(*projectile) : std::vector<std::byte>{};
            },
            client_spawn_hooks,
            [](std::uint64_t tick) {
                app::ActionState actions{};
                actions.axes[kAxisMoveX] = ((tick - 1) % 3) < 2 ? 1.0f : -1.0f;
                if (tick == 12)
                    actions.pressed = 1u << kButtonThrow;
                return actions;
            }};
    def.entry_scene = std::string{kEntryScene};
#if defined(RIME_GAME_CONTENT_DIR) && defined(RIME_GAME_BINARY_DIR)
    def.dev_content_dir = RIME_GAME_CONTENT_DIR;
    def.dev_binary_dir = RIME_GAME_BINARY_DIR;
#endif
    return def;
}

// The shipped executable's definition: `definition()` plus the m20.6 interaction group.
//
// A SEPARATE function rather than folded into `definition()`, because `definition()` is also the
// m20.5 game that existing in-process proofs pin -- one asserts its roster is exactly {player,
// projectile} at epoch 2 -- and an always-on crate member would change that roster, not the code
// it tests. Both processes of a connected run use this one (main.cpp and the net proof), so they
// agree on the folded schema identity.
app::GameDefinition definition_with_interaction() {
    app::GameDefinition def = definition();
    // The interaction group (ADR-0106): join within contact reach plus the distance the player
    // covers in the 64-tick replay budget -- 0.85 + 4 m/s * 64/60 s = 5.12 m -- so the crate is
    // already predicted before the player can reach it; leave 1 m further out, and only after
    // 30 quiet ticks. Contact reach is capsule radius + crate half-extent, centre to centre.
    replication::InteractionParams interaction{};
    interaction.contact_distance = HelloGame::kPlayerRadius + kCrateHalf;
    interaction.player_max_speed = HelloGame::kPlayerSpeed;
    interaction.replay_ticks = 64;
    interaction.fixed_dt = 1.0 / 60.0;
    def.remote_play->interaction = InteractionProfile{1,
                                                      interaction,
                                                      interaction_object,
                                                      interaction_sample,
                                                      capture_crate,
                                                      interaction_client_hooks,
                                                      push_tape,
                                                      HelloGame::kCrateKickImpulse,
                                                      kCrateRigidBody.mass};
    return def;
}

} // namespace hello_game
