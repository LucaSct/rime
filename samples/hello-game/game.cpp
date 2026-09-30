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

#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
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
// The CENTRE of the ring, not a point on it. Starting on the ring would put the player inside marker
// 0 at tick zero and collect it for free — which passed the self-check and made the first reported
// collection tick 1, a number that looks like a working trigger and is really a spawn overlap. A
// game's starting position is a rule, and this is the rule: you have to walk.
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
    physics::register_physics_components(world);
    physics_.set_job_system(&ctx.jobs);
    physics_.set_sleeping_enabled(true);

    // The floor: a static slab whose top surface is exactly y = 0, so every other height in this
    // file is a height above the ground and not an offset from a slab's centre.
    {
        core::Transform tf{};
        tf.translation = {0.0f, -kFloorHalfY, 0.0f};
        (void)world.spawn_with(WorldTransform{tf},
                               physics::RigidBody{.motion = kStatic},
                               box_collider(kArenaHalf, kFloorHalfY, kArenaHalf));
    }

    // The player: KINEMATIC, so the game owns its pose and `PhysicsSync::push_in` drives the body
    // from it. Dynamic would mean the solver owns the pose and the game would be fighting it for
    // control of a character — the thing a character controller exists not to do.
    {
        core::Transform tf{};
        tf.translation = kPlayerStart;
        player_ = world.spawn_with(WorldTransform{tf},
                                   physics::RigidBody{.motion = kKinematic},
                                   physics::Collider{.shape_type = kCapsuleShape,
                                                     .radius = kPlayerRadius,
                                                     .half_height = kPlayerHalfHeight});
        player_pos_ = kPlayerStart;
    }

    // The crate: the one DYNAMIC body, and the reason the player is kinematic-with-velocity rather
    // than teleported. Walking into it pushes it.
    {
        core::Transform tf{};
        tf.translation = {0.0f, kCrateHalf, -2.5f};
        crate_ = world.spawn_with(WorldTransform{tf},
                                  physics::RigidBody{.motion = kDynamic, .mass = 6.0f},
                                  box_collider(kCrateHalf, kCrateHalf, kCrateHalf));
    }

    // The five markers: SENSOR spheres. A sensor joins the broadphase and the exact narrowphase like
    // any other body but is skipped by the solver, so it reports overlaps and exchanges no impulse
    // — the player walks through it and the trigger fires.
    //
    // Placed on a ring rather than at random, because a sample's scene is documentation: a reader
    // can see at a glance that five of something is five of something.
    for (int i = 0; i < kPickupCount; ++i) {
        core::Transform tf{};
        tf.translation = marker_position(i);
        const ecs::Entity e = world.spawn_with(WorldTransform{tf},
                                               physics::RigidBody{.motion = kStatic},
                                               physics::Collider{.shape_type = kSphereShape,
                                                                 .radius = kPickupRadius,
                                                                 .sensor = true});
        pickups_.push_back(Pickup{e, physics::BodyId{}, false});
    }
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
    ++ticks_;
}

// Integrate the stored intent into the player's transform, and clamp it to the arena.
//
// The clamp is a game rule rather than a wall, on purpose: an arena fenced with static boxes would
// be more realistic and would also mean a kinematic body pressed into a static one every time the
// player holds a direction, which is a contact the solver cannot resolve (neither body yields) and
// a permanent source of jitter. Games clamp.
void HelloGame::move_player(ecs::World& world, float dt) {
    core::Vec3 dir = intent_.move;
    const float len = core::length(dir);
    // Normalised so holding two keys is not 1.41x faster than one — and guarded, because
    // normalising a zero vector is a division by zero that shows up as a NaN player three ticks
    // later with nothing pointing at the cause.
    if (len > 1e-4f) {
        dir = dir * (1.0f / len);
    } else {
        dir = {0.0f, 0.0f, 0.0f};
    }
    player_pos_ = player_pos_ + dir * (kPlayerSpeed * dt);
    const float limit = kArenaHalf - kPlayerRadius;
    player_pos_.x = std::clamp(player_pos_.x, -limit, limit);
    player_pos_.z = std::clamp(player_pos_.z, -limit, limit);
    player_pos_.y = kPlayerStart.y; // no jumping: this game is flat, and says so
    if (auto* wt = world.get<ecs::WorldTransform>(player_)) {
        wt->value.translation = player_pos_;
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
    for (const physics::TriggerEvent& ev : physics_.trigger_events()) {
        if (ev.phase != physics::ContactPhase::Began) {
            continue; // Persisted/Ended would collect the same marker again, or on the way out
        }
        const bool player_involved = ev.a == player_body_ || ev.b == player_body_;
        if (!player_involved) {
            continue; // a trigger the crate rolled through is not a pickup
        }
        const physics::BodyId other = ev.a == player_body_ ? ev.b : ev.a;
        for (Pickup& p : pickups_) {
            if (p.collected || !(p.body == other)) {
                continue;
            }
            p.collected = true;
            p.collected_tick = ticks_ + 1; // the tick being completed, 1-based
            ++score_;
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
        input.axes[kAxisMoveX] = target.x - player_pos_.x;
        input.axes[kAxisMoveZ] = target.z - player_pos_.z;
        return input;
    }
    input.pressed = 1u << kButtonQuit; // everything collected: ask to stop
    return input;
}

// What a dedicated server and a player must agree on: the rules' state and the one pose the
// SOLVER owns (the crate — the player's pose is the game's own arithmetic, the crate's is physics).
// Not the camera, not anything presentation touched.
std::uint64_t HelloGame::state_digest(const ecs::World& world) const {
    Fnv1a h;
    h.u64(ticks_);
    h.u64(static_cast<std::uint64_t>(score_));
    for (const Pickup& p : pickups_) {
        h.u64(p.collected_tick);
    }
    h.vec3(player_pos_);
    if (const auto* wt = world.get<ecs::WorldTransform>(crate_)) {
        h.vec3(wt->value.translation);
        h.f32(wt->value.rotation.x);
        h.f32(wt->value.rotation.y);
        h.f32(wt->value.rotation.z);
        h.f32(wt->value.rotation.w);
    }
    return h.value();
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

    const render::MeshId floor_mesh = ctx.meshes.add(render::make_plane(kArenaHalf, 12.0f), "floor");
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
    camera_ = world.spawn_with(ecs::WorldTransform{chase_camera(player_pos_)}, render::Camera{});
}

void HelloGame::present_frame(app::PresentFrameContext& ctx) {
    ++presentation_calls_;
    if (auto* wt = ctx.world.get<ecs::WorldTransform>(camera_)) {
        wt->value = chase_camera(player_pos_);
    }
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
    def.create = [] { return std::make_unique<HelloGame>(); };
    return def;
}

} // namespace hello_game
