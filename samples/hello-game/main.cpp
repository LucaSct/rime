// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// hello-game — the smallest thing in this repository that is a *game* rather than a demonstration.
//
// Walk a character around a floor, touch five markers, push a crate out of the way, win. That is
// the whole design, and the smallness is the feature: two milestones point at this sample for two
// different reasons and both of them want it small.
//
//   * ADR-0038 §"what's missing" named it as **m15.7, the on-ramp** — the sample a newcomer reads
//     first. Every other sample here demonstrates a subsystem (a triangle, a render graph, a
//     physics scene); none of them shows the shape of a game. It was the first item on that ADR's
//     cut list and it was duly cut, so this is a debt being paid rather than new scope.
//   * ADR-0046 §1 then made it a **prerequisite of M20**, the shipped game, for a sharper reason.
//     That ADR's own "riskiest assumption, recorded because it is load-bearing" is that a game's
//     play loop can be lifted out of its sample `main` into an engine-owned `GameDefinition`
//     without rewriting the game. The block's loop is ~3000 lines of sample code and is the wrong
//     first subject for finding that out. This is the right one.
//
// **So the structure here is the point, not just the gameplay.** Everything the game *is* lives in
// `HelloGame` below: its scene, its rules, its state, and three methods —
// `setup`/`apply_intent`/`fixed_tick`. Everything about *running* it — argument parsing, the
// `app::Application`, the window, the renderer, the self-check — lives in the host code at the
// bottom and touches the game only through those three methods. Nothing in `HelloGame` knows
// whether it is being rendered, and nothing in the host knows the rules.
//
// That line is where M20's `GameDefinition` will be cut. If lifting `HelloGame` into the engine
// turns out to need the rules to know about a device, a window or a frame, then ADR-0046's
// assumption is false and we find out here, in 500 lines, instead of in the block.
//
// **Input is an `Intent`, never a keystroke.** `HelloGame` is handed a direction and a quit flag;
// the mapping from keys to that intent is the host's job (`intent_from_events`). This is not
// tidiness — it is what lets the same rules run from a keyboard, from a scripted sequence, and
// (Track H) from a browser's DataChannel, with no branch inside the game. The self-check relies on
// exactly that: it drives the game from a fixed script and gets a bit-reproducible result.
//
// **The pickups are real trigger volumes**, not a distance check. `Collider::sensor` was reflected
// and honoured by nothing until m15.6 made it fire `TriggerEvent`s; a sample that faked the overlap
// with a `length(a - b) < r` test would quietly stop exercising the feature it looks like it uses.
// The player is a KINEMATIC capsule, so `PhysicsSync::push_in` drives it from the transform the
// game writes — which is also why the crate is *pushed* at walking speed instead of being fired
// across the floor by a teleport-induced penetration.
//
//   build/<preset>/bin/hello_game                # the self-check: GPU-free, silent, exit 0
//   build/<preset>/bin/hello_game --verbose      # the same, with a report
//   build/<preset>/bin/hello_game --headless     # render it off-screen (lavapipe / CI)
//   build/<preset>/bin/hello_game --windowed     # play it: WASD to move, Esc to quit

#include <fmt/core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/physics.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/device.hpp"

namespace {

using namespace rime;

// ── What the game is told, per tick ───────────────────────────────────────────────────────────
//
// A direction on the floor plane and a wish to stop. Deliberately NOT a key state, a mouse delta or
// an event span: those are facts about a keyboard, and the game must not have an opinion about
// keyboards. A browser sending `{"x":1,"z":0}` over a DataChannel produces the same Intent, and the
// scripted self-check below produces it with no input device in the process at all.
//
// `move` is not normalised here. Normalising is a game rule (it is what stops diagonal movement
// being 1.41x faster), so the game does it, and a host cannot get it wrong on the game's behalf.
struct Intent {
    core::Vec3 move{0.0f, 0.0f, 0.0f};
    bool quit = false;
};

// ── The game ──────────────────────────────────────────────────────────────────────────────────
//
// The candidate shape for M20's engine-owned `GameDefinition`. Three methods, no device, no window,
// no frame: `setup` builds the world, `apply_intent` receives the player's wish, `fixed_tick`
// advances the simulation by one fixed step and applies the rules.
class HelloGame {
public:
    // Tuning, all in one place so a reader can change the feel without reading the logic.
    static constexpr float kPlayerSpeed = 4.0f;      // m/s on the floor
    static constexpr float kPlayerRadius = 0.35f;    // capsule radius
    static constexpr float kPlayerHalfHeight = 0.5f; // capsule half-height (excluding caps)
    static constexpr float kPickupRadius = 0.6f;     // the trigger sphere the player must touch
    static constexpr int kPickupCount = 5;
    static constexpr float kRingRadius = 5.0f; // the markers sit on this circle about the origin

    // Where marker `index` is. Public because the scripted player steers at it, and steering at a
    // hard-coded coordinate instead would mean moving a marker silently turns the self-check into
    // one that walks past it — a test that no longer tests what its name says.
    [[nodiscard]] static core::Vec3 marker_position(int index) {
        constexpr float kTwoPi = 6.28318531f;
        const float angle = kTwoPi * static_cast<float>(index) / static_cast<float>(kPickupCount);
        return {kRingRadius * std::sin(angle), kPickupRadius + 0.1f, kRingRadius * std::cos(angle)};
    }

    // Build the scene. Spawns entities with physics AND render components: the render ones are
    // inert when nobody renders (no device means no SceneRenderer means nothing reads them), which
    // is what lets one scene serve the GPU-free self-check and the windowed run without a branch.
    //
    // `meshes`/`materials` are optional because the GPU-free path has no registries to add to. When
    // they are absent the entities simply carry no MeshRef — the rules never look.
    void setup(ecs::World& world,
               physics::PhysicsWorld& physics,
               render::MeshRegistry* meshes = nullptr,
               render::MaterialRegistry* materials = nullptr) {
        using ecs::WorldTransform;
        physics::register_physics_components(world);
        render::register_render_components(world);
        physics.set_sleeping_enabled(true);

        std::optional<render::MeshId> floor_mesh;
        std::optional<render::MeshId> crate_mesh;
        std::optional<render::MeshId> marker_mesh;
        std::optional<render::MeshId> player_mesh;
        std::optional<render::MaterialId> floor_mat;
        std::optional<render::MaterialId> crate_mat;
        std::optional<render::MaterialId> marker_mat;
        std::optional<render::MaterialId> player_mat;
        if (meshes != nullptr && materials != nullptr) {
            floor_mesh = meshes->add(render::make_plane(kArenaHalf, 12.0f), "floor");
            crate_mesh = meshes->add(render::make_cube(kCrateHalf), "crate");
            marker_mesh = meshes->add(render::make_uv_sphere(kPickupRadius, 16, 24), "marker");
            // The player is drawn as a sphere rather than a capsule because `make_capsule` does not
            // exist and inventing one in a sample would be an engine change in `samples/` clothing
            // — exactly what ADR-0038 forbids of this milestone's diff. The COLLIDER is a real
            // capsule; only the silhouette is approximate, and the header says so rather than the
            // reader having to notice.
            player_mesh = meshes->add(render::make_uv_sphere(kPlayerRadius, 16, 24), "player");

            render::PbrMaterialDesc m{};
            m.base_color[0] = 0.32f;
            m.base_color[1] = 0.34f;
            m.base_color[2] = 0.38f;
            m.roughness = 0.8f;
            floor_mat = materials->add(m);
            m.base_color[0] = 0.62f;
            m.base_color[1] = 0.42f;
            m.base_color[2] = 0.22f;
            m.roughness = 0.65f;
            crate_mat = materials->add(m);
            // Vivid green, and the choice is the TEST's rather than the art's: the headless check
            // below identifies the markers by hue, so their colour has to be one nothing else in
            // the scene can be mistaken for. Gold was the first choice and the brown crate
            // satisfied it — the falsification caught that, which is the entire reason to run one.
            m.base_color[0] = 0.10f;
            m.base_color[1] = 0.92f;
            m.base_color[2] = 0.28f;
            m.metallic = 0.0f;
            m.roughness = 0.3f;
            marker_mat = materials->add(m);
            m.base_color[0] = 0.20f;
            m.base_color[1] = 0.55f;
            m.base_color[2] = 0.85f;
            m.metallic = 0.0f;
            m.roughness = 0.4f;
            player_mat = materials->add(m);
        }

        // The floor: a static slab whose top surface is exactly y = 0, so every other height in
        // this file is a height above the ground and not an offset from a slab's centre.
        {
            core::Transform tf{};
            tf.translation = {0.0f, -kFloorHalfY, 0.0f};
            const ecs::Entity e =
                world.spawn_with(WorldTransform{tf},
                                 physics::RigidBody{.motion = kStatic},
                                 box_collider(kArenaHalf, kFloorHalfY, kArenaHalf));
            if (floor_mesh) {
                // The visual plane sits at y = 0 — the slab's TOP — while the collider is centred
                // half a slab lower. Two entities rather than one because a plane mesh and a box
                // collider do not share a centre, and scaling one to fit the other is how a sample
                // grows a subtle offset that every later reader has to re-derive.
                core::Transform vis{};
                (void)world.spawn_with(WorldTransform{vis},
                                       render::MeshRef{*floor_mesh},
                                       render::MaterialRef{*floor_mat});
            }
            (void)e;
        }

        // The player: KINEMATIC, so the game owns its pose and `PhysicsSync::push_in` drives the
        // body from it. Dynamic would mean the solver owns the pose and the game would be fighting
        // it for control of a character — the thing a character controller exists not to do.
        {
            core::Transform tf{};
            tf.translation = kPlayerStart;
            player_ = world.spawn_with(WorldTransform{tf},
                                       physics::RigidBody{.motion = kKinematic},
                                       physics::Collider{.shape_type = kCapsuleShape,
                                                         .radius = kPlayerRadius,
                                                         .half_height = kPlayerHalfHeight});
            if (player_mesh) {
                world.add_component(player_, render::MeshRef{*player_mesh});
                world.add_component(player_, render::MaterialRef{*player_mat});
            }
            player_pos_ = kPlayerStart;
        }

        // The crate: the one DYNAMIC body, and the reason the player is kinematic-with-velocity
        // rather than teleported. Walking into it pushes it.
        {
            core::Transform tf{};
            tf.translation = {0.0f, kCrateHalf, -2.5f};
            crate_ = world.spawn_with(WorldTransform{tf},
                                      physics::RigidBody{.motion = kDynamic, .mass = 6.0f},
                                      box_collider(kCrateHalf, kCrateHalf, kCrateHalf));
            if (crate_mesh) {
                world.add_component(crate_, render::MeshRef{*crate_mesh});
                world.add_component(crate_, render::MaterialRef{*crate_mat});
            }
        }

        // The five markers: SENSOR spheres. A sensor joins the broadphase and the exact narrowphase
        // like any other body but is skipped by the solver, so it reports overlaps and exchanges no
        // impulse — the player walks through it and the trigger fires.
        //
        // Placed on a ring rather than at random, because a sample's scene is documentation: a
        // reader can see at a glance that five of something is five of something.
        for (int i = 0; i < kPickupCount; ++i) {
            core::Transform tf{};
            tf.translation = marker_position(i);
            const ecs::Entity e = world.spawn_with(WorldTransform{tf},
                                                   physics::RigidBody{.motion = kStatic},
                                                   physics::Collider{.shape_type = kSphereShape,
                                                                     .radius = kPickupRadius,
                                                                     .sensor = true});
            if (marker_mesh) {
                world.add_component(e, render::MeshRef{*marker_mesh});
                world.add_component(e, render::MaterialRef{*marker_mat});
            }
            pickups_.push_back(Pickup{e, physics::BodyId{}, false});
        }

        // The sun. Aiming a light is rotating its entity (its forward is −z), the same convention a
        // camera looks down, so this is a light tilted down and to the side.
        {
            core::Transform tf{};
            tf.rotation = core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.6f) *
                                          core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.05f));
            (void)world.spawn_with(WorldTransform{tf},
                                   render::DirectionalLight{0.98f, 0.96f, 0.90f, 3.2f});
        }

        camera_ = world.spawn_with(WorldTransform{core::Transform{}}, render::Camera{});
    }

    // Receive the player's wish for the next tick. Stored rather than acted on, because input
    // arrives per FRAME and the simulation advances per TICK: a frame may run zero ticks or eight,
    // and applying a move the moment it arrives would make the player's speed depend on the frame
    // rate. This is the same split `FrameContext::frame_dt` versus `Application::fixed_dt` exists
    // for, applied to input.
    void apply_intent(const Intent& intent) {
        intent_ = intent;
        if (intent.quit) {
            quit_ = true;
        }
    }

    // One fixed simulation step, and the rules.
    //
    // Order matters and is the canonical one (`docs/design/simulation-tick.md`): move the player
    // (the game owns a kinematic pose), then let the bridge push that pose in and step, then read
    // what the step reported. Reading triggers before the step would read the previous tick's.
    void fixed_tick(ecs::World& world,
                    physics::PhysicsWorld& physics,
                    physics::PhysicsSync& sync,
                    double dt) {
        const float step = static_cast<float>(dt);
        move_player(world, step);
        sync.step(world, physics, step);
        // Bind the markers' body ids once the bridge has created them. It cannot be done in
        // `setup`: `RigidBodyHandle` is added by `reconcile`, which first runs inside this call.
        bind_bodies(world);
        collect(world, physics);
        ++ticks_;
    }

    // ── What the host may ask about the game ──────────────────────────────────────────────────

    [[nodiscard]] int score() const noexcept { return score_; }

    [[nodiscard]] bool won() const noexcept { return score_ == kPickupCount; }

    [[nodiscard]] bool quit_requested() const noexcept { return quit_; }

    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }

    [[nodiscard]] ecs::Entity camera() const noexcept { return camera_; }

    [[nodiscard]] core::Vec3 player_position() const noexcept { return player_pos_; }

    // The tick the Nth marker was collected on, or 0 for one still out there. The self-check
    // compares these between two runs: a scalar score would also match if the pickups were
    // collected in a different order on a different thread count, and that is exactly the failure a
    // determinism proof is for.
    [[nodiscard]] std::uint64_t collected_on(std::size_t index) const {
        return pickups_[index].collected_tick;
    }

private:
    struct Pickup {
        ecs::Entity entity{};
        physics::BodyId body{};
        bool collected = false;
        std::uint64_t collected_tick = 0;
    };

    // Component field values spelled as constants, because `RigidBody::motion` is a `std::uint32_t`
    // rather than the enum (the reflected components stay POD) and `RigidBody{.motion = 0}` at a
    // call site says nothing at all.
    static constexpr std::uint32_t kStatic =
        static_cast<std::uint32_t>(physics::MotionType::Static);
    static constexpr std::uint32_t kKinematic =
        static_cast<std::uint32_t>(physics::MotionType::Kinematic);
    static constexpr std::uint32_t kDynamic =
        static_cast<std::uint32_t>(physics::MotionType::Dynamic);
    static constexpr std::uint32_t kSphereShape =
        static_cast<std::uint32_t>(physics::ShapeType::Sphere);
    static constexpr std::uint32_t kBoxShape = static_cast<std::uint32_t>(physics::ShapeType::Box);
    static constexpr std::uint32_t kCapsuleShape =
        static_cast<std::uint32_t>(physics::ShapeType::Capsule);

    static constexpr float kArenaHalf = 9.0f;
    static constexpr float kFloorHalfY = 0.5f;
    static constexpr float kCrateHalf = 0.5f;
    // The CENTRE of the ring, not a point on it. Starting on the ring would put the player inside
    // marker 0 at tick zero and collect it for free — which passed the self-check and made the
    // first reported collection tick 1, a number that looks like a working trigger and is really a
    // spawn overlap. A game's starting position is a rule, and this is the rule: you have to walk.
    static constexpr core::Vec3 kPlayerStart{0.0f, kPlayerRadius + kPlayerHalfHeight, 0.0f};

    static physics::Collider box_collider(float hx, float hy, float hz) {
        return physics::Collider{.shape_type = kBoxShape, .half_x = hx, .half_y = hy, .half_z = hz};
    }

    // Integrate the stored intent into the player's transform, and clamp it to the arena.
    //
    // The clamp is a game rule rather than a wall, on purpose: an arena fenced with static boxes
    // would be more realistic and would also mean a kinematic body pressed into a static one every
    // time the player holds a direction, which is a contact the solver cannot resolve (neither body
    // yields) and a permanent source of jitter. Games clamp.
    void move_player(ecs::World& world, float dt) {
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

    void bind_bodies(ecs::World& world) {
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
    // consumer has to check both — the header says so, and a consumer that checks only `b` works
    // until the day body indices happen to come out the other way round.
    void collect(ecs::World& world, physics::PhysicsWorld& physics) {
        for (const physics::TriggerEvent& ev : physics.trigger_events()) {
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
                // Despawning takes the RigidBodyHandle with it, which is precisely why the bridge
                // keeps its own entity↔body roster: next tick's `reconcile` destroys the orphaned
                // sensor body from that roster rather than failing to find it in a query.
                world.despawn(p.entity);
            }
        }
    }

    Intent intent_{};
    ecs::Entity player_{};
    ecs::Entity crate_{};
    ecs::Entity camera_{};
    physics::BodyId player_body_{};
    core::Vec3 player_pos_{};
    std::vector<Pickup> pickups_;
    bool bound_ = false;
    bool quit_ = false;
    int score_ = 0;
    std::uint64_t ticks_ = 0;
};

// ── Host: keys to Intent ──────────────────────────────────────────────────────────────────────
//
// Held-key state rather than per-event impulses, because "W is down" is a fact about the whole tick
// and a KeyDown event is a fact about one instant. Auto-repeat is ignored for the same reason: a
// repeat is not a second press, and counting it would make a held key accelerate.
class KeyboardIntent {
public:
    Intent update(std::span<const platform::Event> events) {
        for (const platform::Event& e : events) {
            const bool down = e.type == platform::EventType::KeyDown;
            if (!down && e.type != platform::EventType::KeyUp) {
                if (e.type == platform::EventType::WindowClose) {
                    quit_ = true;
                }
                continue;
            }
            switch (e.key.key) {
                case platform::Key::W:
                    forward_ = down;
                    break;
                case platform::Key::S:
                    back_ = down;
                    break;
                case platform::Key::A:
                    left_ = down;
                    break;
                case platform::Key::D:
                    right_ = down;
                    break;
                case platform::Key::Escape:
                    quit_ = quit_ || down;
                    break;
                default:
                    break;
            }
        }
        // −z is forward, the engine's camera convention, so W walks away from the default camera
        // rather than into it.
        Intent intent{};
        intent.move.z = (back_ ? 1.0f : 0.0f) - (forward_ ? 1.0f : 0.0f);
        intent.move.x = (right_ ? 1.0f : 0.0f) - (left_ ? 1.0f : 0.0f);
        intent.quit = quit_;
        return intent;
    }

private:
    bool forward_ = false, back_ = false, left_ = false, right_ = false, quit_ = false;
};

// ── Host: a scripted player ───────────────────────────────────────────────────────────────────
//
// The self-check's input device. It walks the ring the markers are on by steering toward each in
// turn — a fixed, device-free, frame-rate-free sequence of Intents.
//
// **This is the reason `Intent` exists.** A game that read `platform::Event` directly could only be
// tested by synthesising key events, which tests the key mapping and the rules together and cannot
// tell you which one broke. Here the script is the same kind of input a browser will be.
class ScriptedPlayer {
public:
    explicit ScriptedPlayer(const HelloGame& game) : game_(game) {}

    [[nodiscard]] Intent next() const {
        // Steer at the next uncollected marker on the ring, asking the GAME where that marker is
        // rather than repeating its coordinates here. A script carrying its own copy of the scene
        // keeps passing after the scene moves, which is the shape of a test that has stopped
        // testing.
        for (int i = 0; i < HelloGame::kPickupCount; ++i) {
            if (game_.collected_on(static_cast<std::size_t>(i)) != 0) {
                continue;
            }
            const core::Vec3 target = HelloGame::marker_position(i);
            const core::Vec3 here = game_.player_position();
            Intent intent{};
            intent.move = {target.x - here.x, 0.0f, target.z - here.z};
            return intent;
        }
        return Intent{.move = {}, .quit = true}; // everything collected: ask to stop
    }

private:
    const HelloGame& game_;
};

// ── Host: the GPU-free self-check ─────────────────────────────────────────────────────────────

struct RunResult {
    bool won = false;
    int score = 0;
    std::uint64_t ticks = 0;
    std::uint64_t collected_on[HelloGame::kPickupCount]{};
};

// Play the scripted game to its end, GPU-free. `tick_budget` bounds it so a rule that never
// completes fails as a timeout rather than hanging CI.
RunResult play_scripted(int tick_budget, unsigned workers) {
    app::AppConfig config{};
    config.worker_threads = workers;
    app::Application app(config);
    physics::PhysicsWorld physics;
    physics::PhysicsSync sync;
    physics.set_job_system(&app.jobs());

    HelloGame game;
    game.setup(app.world(), physics);
    const ScriptedPlayer script(game);

    app.on_fixed_tick([&](ecs::World& world, double dt) {
        game.apply_intent(script.next());
        game.fixed_tick(world, physics, sync, dt);
    });

    const double fd = app.fixed_dt();
    for (int i = 0; i < tick_budget && !game.won() && !game.quit_requested(); ++i) {
        app.step(fd); // exactly one tick per call when the frame dt IS the fixed dt
    }

    RunResult result{};
    result.won = game.won();
    result.score = game.score();
    result.ticks = game.ticks();
    for (int i = 0; i < HelloGame::kPickupCount; ++i) {
        result.collected_on[i] = game.collected_on(static_cast<std::size_t>(i));
    }
    return result;
}

int run_selftest(bool verbose) {
    constexpr int kBudget = 2000; // ~33 s of game time at 60 Hz; the script needs a few hundred

    const RunResult first = play_scripted(kBudget, 0);
    if (!first.won) {
        fmt::print(stderr,
                   "hello-game: FAILED — the scripted player collected {} of {} in {} ticks\n",
                   first.score,
                   HelloGame::kPickupCount,
                   first.ticks);
        return 1;
    }

    // Determinism, the same property every other sim proof in this repository asserts (ADR-0026):
    // the identical scenario on a DIFFERENT worker-thread count must reach the identical result.
    // Same-thread-count repetition would pass on a racy tick; a different count is what makes the
    // claim mean anything.
    const RunResult second = play_scripted(kBudget, 1);
    if (second.ticks != first.ticks || second.score != first.score) {
        fmt::print(stderr,
                   "hello-game: FAILED — nondeterministic: {} ticks / {} score vs {} / {}\n",
                   second.ticks,
                   second.score,
                   first.ticks,
                   first.score);
        return 1;
    }
    for (int i = 0; i < HelloGame::kPickupCount; ++i) {
        if (second.collected_on[i] != first.collected_on[i]) {
            fmt::print(stderr,
                       "hello-game: FAILED — marker {} collected on tick {} vs {}\n",
                       i,
                       second.collected_on[i],
                       first.collected_on[i]);
            return 1;
        }
    }

    if (verbose) {
        fmt::print("hello-game self-check\n");
        fmt::print("  markers collected : {} of {}\n", first.score, HelloGame::kPickupCount);
        fmt::print("  ticks to win      : {}\n", first.ticks);
        fmt::print("  collection ticks  : ");
        for (int i = 0; i < HelloGame::kPickupCount; ++i) {
            fmt::print("{}{}", i == 0 ? "" : ", ", first.collected_on[i]);
        }
        fmt::print("\n  determinism       : identical on 1 worker and on {}\n",
                   "hardware_concurrency-1");
        fmt::print("  PASS\n");
    }
    return 0;
}

// ── Host: the rendered run ────────────────────────────────────────────────────────────────────

// Read an LDR target back to CPU memory (the 06/07 pattern). Only the headless self-check does
// this; it is a full pipeline stall and has no business in a frame anyone is watching.
std::vector<std::uint8_t>
read_rgba8(rhi::Device& device, rhi::TextureHandle tex, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 4;
    rhi::BufferDesc bd{};
    bd.size = bytes;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle rb = device.create_buffer(bd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(tex, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

// Two STRUCTURAL claims about the rendered frame, not a golden image (the house rule: properties
// the scene guarantees, checked with margins, so it survives a driver's rounding).
//
// **This exists because a render smoke test that only checks the exit code passes on a black
// frame.** That is not hypothetical here: the render graph culls passes whose outputs nothing
// consumes, and m18.3d spent real time on a measured pass that had been culled for 119 of 120
// frames while every number still looked plausible. So the check is on the PIXELS.
//
//   1. the scene is lit — a floor and a sun mean most of the frame is not black;
//   2. a marker is visible — the pickups are the only GREEN-DOMINANT thing in the scene (the floor
//   is
//      blue-grey, the crate brown, the player blue), so green-dominant pixels are proof the trigger
//      volumes are actually drawn. A lit floor alone would satisfy claim 1 and say nothing about
//      the game. The discriminator is strict — green must beat both other channels by a wide margin
//      — because the first version of this check asked for "gold" and the brown crate passed it.
bool frame_shows_the_game(const std::vector<std::uint8_t>& px, bool verbose) {
    std::uint64_t lit = 0;
    std::uint64_t marker = 0;
    const std::size_t n = px.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const int r = px[i * 4 + 0];
        const int g = px[i * 4 + 1];
        const int b = px[i * 4 + 2];
        if ((r + g + b) / 3 > 20) {
            ++lit;
        }
        if (g > 110 && g > r + 60 && g > b + 60) {
            ++marker;
        }
    }
    const bool ok = lit > n / 20 && marker > 40;
    if (verbose || !ok) {
        fmt::print("  frame check: {} lit / {} marker of {} px -> {}\n",
                   lit,
                   marker,
                   n,
                   ok ? "ok" : "FAILED");
    }
    return ok;
}

core::Transform chase_camera(core::Vec3 target) {
    // A fixed-angle chase camera: behind and above the player, looking down at it. Fixed rather
    // than free because this sample is about the game loop, and a free camera is a second input
    // system competing for the reader's attention (07-first-light already demonstrates one).
    const core::Quat q = core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.0f) *
                                         core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.55f));
    const core::Vec3 back = core::rotate(q, {0.0f, 0.0f, 1.0f});
    core::Transform t{};
    t.rotation = q;
    t.translation = target + back * 11.0f;
    return t;
}

int run_rendered(bool windowed, int frames) {
    app::AppConfig config{};
    config.gpu = true;
    config.render_extent = {1280, 720};
    config.windowed = windowed;
    config.window_title = "Rime — hello-game (WASD, Esc)";
    app::Application app(config);
    if (app.device() == nullptr) {
        // No Vulkan here. An honest skip, not a failure: CI runs this on machines with no device,
        // and RIME_REQUIRE_VULKAN is how a machine that SHOULD have one says so.
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            fmt::print(stderr, "hello-game: no Vulkan device and RIME_REQUIRE_VULKAN is set\n");
            return 1;
        }
        fmt::print("hello-game: no Vulkan device — skipping the rendered run\n");
        return 0;
    }

    physics::PhysicsWorld physics;
    physics::PhysicsSync sync;
    physics.set_job_system(&app.jobs());

    render::MeshRegistry meshes(*app.device());
    render::MaterialRegistry materials;
    render::SceneRenderer renderer(*app.device(), meshes, materials);
    renderer.set_frames_in_flight(app.frames_in_flight());
    renderer.set_ambient(0.04f, 0.045f, 0.06f);

    HelloGame game;
    game.setup(app.world(), physics, &meshes, &materials);

    render::RGTexture last_ldr{};
    KeyboardIntent keys;
    // Off-screen there is nobody to press a key, so the scripted player drives it — the same script
    // the self-check uses. One render path, two input sources, which is the whole reason the game
    // takes an Intent.
    const ScriptedPlayer script(game);

    app.on_fixed_tick(
        [&](ecs::World& world, double dt) { game.fixed_tick(world, physics, sync, dt); });

    app.on_render([&](app::FrameContext& ctx) {
        // The intent is read here, in the frame callback, because that is where `ctx.input` lives —
        // so it is applied by the NEXT frame's ticks, one frame later. That lag is inherent to
        // reading input per frame and simulating per tick, and naming it beats pretending
        // otherwise; at 60 Hz it is 16 ms and this game is not a shooter. The scripted paths have
        // no such lag, because they read the intent inside the tick, where a script can always be
        // asked.
        game.apply_intent(windowed ? keys.update(ctx.input) : script.next());
        if (auto* wt = ctx.world.get<ecs::WorldTransform>(game.camera())) {
            wt->value = chase_camera(game.player_position());
        }
        last_ldr = renderer.render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
        ctx.present = last_ldr;
        if (game.quit_requested() || game.won()) {
            app.request_quit();
        }
    });

    if (windowed) {
        const std::uint64_t ran = app.run();
        fmt::print("hello-game: {} frames, {} of {} markers{}\n",
                   ran,
                   game.score(),
                   HelloGame::kPickupCount,
                   game.won() ? " — you win" : "");
    } else {
        app.run_frames(frames);
        // `run_frames` guarantees the GPU is idle on return, so the last frame's graph handles are
        // still valid and this readback is reading a finished image rather than one mid-flight.
        const std::vector<std::uint8_t> px = read_rgba8(*app.device(),
                                                        app.graph()->physical(last_ldr),
                                                        config.render_extent.width,
                                                        config.render_extent.height);
        const bool ok = frame_shows_the_game(px, true);
        fmt::print("hello-game: rendered {} frames, {} of {} markers collected — {}\n",
                   frames,
                   game.score(),
                   HelloGame::kPickupCount,
                   ok ? "the game is on screen" : "FAILED the frame check");
        return ok ? 0 : 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    bool headless = false;
    bool windowed = false;
    int frames = 240;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--windowed") {
            windowed = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else {
            fmt::print(stderr,
                       "usage: hello_game [--verbose] [--headless [--frames N]] [--windowed]\n");
            return 2;
        }
    }
    if (windowed || headless) {
        return run_rendered(windowed, frames);
    }
    return run_selftest(verbose);
}
