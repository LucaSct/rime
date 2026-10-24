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
#include <memory>
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

physics::Collider box_collider(float hx, float hy, float hz) {
    return physics::Collider{.shape_type = kBoxShape, .half_x = hx, .half_y = hy, .half_z = hz};
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
        player_ = world.spawn_with(ecs::LocalTransform{tf},
                                   WorldTransform{tf},
                                   physics::RigidBody{.motion = kKinematic},
                                   physics::Collider{.shape_type = kCapsuleShape,
                                                     .radius = kPlayerRadius,
                                                     .half_height = kPlayerHalfHeight});
    }

    // The crate: the one DYNAMIC body, and the reason the player is kinematic-with-velocity rather
    // than teleported. Walking into it pushes it.
    {
        core::Transform tf{};
        tf.translation = {0.0f, kCrateHalf, -2.5f};
        crate_ = world.spawn_with(ecs::LocalTransform{tf},
                                  WorldTransform{tf},
                                  physics::RigidBody{.motion = kDynamic, .mass = 6.0f},
                                  box_collider(kCrateHalf, kCrateHalf, kCrateHalf));
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
    move_player(ctx.world, step);
    sync_.step(ctx.world, physics_, step);
    // Bind the markers' body ids once the bridge has created them. It cannot be done in `setup`:
    // `RigidBodyHandle` is added by `reconcile`, which first runs inside this call.
    bind_bodies(ctx.world);
    collect(ctx.world);
    if (auto* replicated = state()) {
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
}

void HelloGame::present_frame(app::PresentFrameContext& ctx) {
    ++presentation_calls_;
    if (auto* wt = ctx.world.get<ecs::WorldTransform>(camera_)) {
        wt->value = chase_camera(player_position());
    }
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
    (void)world.spawn_with(ecs::LocalTransform{tf},
                           physics::RigidBody{.motion = kStatic},
                           box_collider(kArenaHalf, kFloorHalfY, kArenaHalf));
}

void register_components(ecs::World& world) {
    ecs::register_transform_components(world);
    physics::register_physics_components(world);
    (void)world.register_component<ReplicatedGameState>();
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

replication::PredictionHooks prediction_hooks(ecs::World& world, ecs::Entity player) {
    ecs::Entity state_entity{};
    world.query<ReplicatedGameState>().for_each(
        [&](ecs::Entity entity, const ReplicatedGameState&) { state_entity = entity; });
    auto* bound = &world;
    replication::PredictionHooks hooks{};
    hooks.capture = [bound, state_entity](replication::PredictedEntityId) {
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
    hooks.restore = [bound, state_entity, player](replication::PredictedEntityId,
                                                  std::span<const std::byte> bytes) {
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
    // The locomotion controller has no RNG or hidden timer. Keep an explicit empty group so
    // later interaction prediction cannot silently inherit this assumption.
    hooks.capture_group = [] { return std::vector<std::byte>{}; };
    hooks.restore_group = [](std::span<const std::byte>) {};
    hooks.simulate = [bound, state_entity, player](std::span<const replication::PredictedEntityId>,
                                                   const replication::InputCommand& command,
                                                   const replication::PredictionStep& step) {
        auto* state = bound->get<ReplicatedGameState>(state_entity);
        if (!state)
            return;
        app::ActionState actions{};
        actions.axes[kAxisMoveX] = command.move_x;
        actions.axes[kAxisMoveZ] = command.move_y;
        integrate_player(*state, intent_from(actions), static_cast<float>(step.fixed_dt));
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
    def.input.buttons = {{platform::Key::Escape, kButtonQuit}};
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
        register_components, owned_player, prediction_hooks, 1.0 / 60.0, replicated_digest};
    def.entry_scene = std::string{kEntryScene};
#if defined(RIME_GAME_CONTENT_DIR) && defined(RIME_GAME_BINARY_DIR)
    def.dev_content_dir = RIME_GAME_CONTENT_DIR;
    def.dev_binary_dir = RIME_GAME_BINARY_DIR;
#endif
    return def;
}

} // namespace hello_game
