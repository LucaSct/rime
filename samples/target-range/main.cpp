// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// target-range — Milestone 15's "done when" (m15.8, ADR-0038): **the platform proof**.
//
//   a small game that is NOT the block is authored through the editor and runs on the engine,
//   with no engine or editor source changed to support it
//
// The load-bearing words are "no engine or editor source changed". Every earlier sample is a
// demonstration the engine was built to support; this one is a game the engine did not know about,
// and the proof is what its own diff does NOT contain. If making a target range work had needed a
// field, a flag or a special case in `engine/` or `tools/`, the platform claim would be false and
// this file could not exist in this form.
//
// **The proof is the game's OWN component.** `targetrange::Target` below is declared here,
// reflected here, registered here, and written into `target_range.rscene` by name. The engine's
// serializer loads it without knowing what a target is, because
// `worldkit::register_engine_components` names the engine's components and explicitly stops there —
// "a game calls this and then registers its own on top"
// (`engine/worldkit/include/rime/worldkit/profile.hpp:45-48`). A strict load is used on purpose:
// `LoadOptions::allow_unknown_components` would let the file's `targetrange::Target` records be
// silently skipped and the range would come up with zero targets and pass nothing. So the
// self-check asserts `skipped_components == 0` — the engine did not merely tolerate the game's
// data, it round-tripped it.
//
// **The targets come from the FILE, not from this code.** Nothing here says "six". The win
// condition is "every Target in the loaded world is down", so moving, adding or deleting a crate in
// the editor changes the game with no rebuild. That is the difference between a game that reads a
// scene and a game that regenerates its own level and ignores the file it was handed — the
// distinction M14's round trip exists to catch, and the reason this sample prints a placement
// digest.
//
// **A shot is a real raycast into the physics world**, not a bookkeeping decision. It can MISS: if
// the ray reaches a crate the shooter was not aiming at, or the floor, the shot is counted as a
// miss and the target stays up. A "hit detector" that cannot miss proves nothing, so the counters
// below distinguish hits, misses and shots blocked by another target — the last of which is a real
// event on a range whose crates can be knocked into each other's line of fire.
//
//   build/<preset>/bin/target_range --emit-scene samples/target-range/target_range.rscene
//   build/<preset>/bin/target_range --scene <file.rscene>            # the self-check, GPU-free
//   build/<preset>/bin/target_range --scene <file> --verbose         # ...with a report
//   build/<preset>/bin/target_range --scene <file> --digest          # placement digest only
//   build/<preset>/bin/target_range --scene <file> --headless        # render it off-screen

#include <fmt/core.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/core/reflect.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/reflect.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/physics.hpp"
#include "rime/render/components.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/device.hpp"
#include "rime/scene/derive_transforms.hpp"
#include "rime/scene/scene_format.hpp"
#include "rime/worldkit/profile.hpp"
#include "target.hpp"

namespace {

using namespace rime;

constexpr float kFloorHalfY = 0.5f;
constexpr float kCrateHalf = 0.4f;
constexpr float kRangeZ = -9.0f; // how far downrange the crates stand
constexpr int kTicksBetweenShots = 12;
constexpr float kShotRange = 60.0f;

std::uint32_t motion_of(physics::MotionType m) {
    return static_cast<std::uint32_t>(m);
}

// ── The authored scene, as a generator ────────────────────────────────────────────────────────
//
// `--emit-scene` writes `target_range.rscene`, and the committed file is this function's output.
// The same shape M14 established: `rime-blockgen` writes the block's scene, the editor opens and
// changes it, the game runs whatever file it is handed. A generator is how content gets its first
// draft; the editor is how it gets changed. Writing the `.rscene` by hand is possible (the format
// is human-diffable and says so) but would mean hand-computing every component's type hash.
void author_scene(ecs::World& world) {
    using ecs::LocalTransform;

    // The camera the shooter fires from. Its pose is authored, so moving the camera in the editor
    // changes the firing line — which is why a shot can be blocked by geometry rather than always
    // connecting.
    {
        core::Transform tf{};
        tf.translation = {0.0f, 1.25f, 6.0f};
        (void)world.spawn_with(LocalTransform{tf}, render::Camera{});
    }
    // The sun, tilted down the range.
    {
        core::Transform tf{};
        tf.rotation = core::normalize(core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, 0.25f) *
                                      core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.95f));
        (void)world.spawn_with(LocalTransform{tf},
                               render::DirectionalLight{1.0f, 0.97f, 0.92f, 3.4f});
    }
    // The floor: one static slab whose top face is y = 0.
    {
        core::Transform tf{};
        tf.translation = {0.0f, -kFloorHalfY, 0.0f};
        (void)world.spawn_with(
            LocalTransform{tf},
            physics::RigidBody{.motion = motion_of(physics::MotionType::Static)},
            physics::Collider{.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Box),
                              .half_x = 24.0f,
                              .half_y = kFloorHalfY,
                              .half_z = 24.0f});
    }
    // Five crates in a line across the range, all with a clear shot from the authored camera.
    // Spread wide rather than stacked, so the default range has no target hiding behind another — a
    // blocked shot should be something the editor can CREATE, not the shipped state.
    constexpr int kCrates = 5;
    for (int i = 0; i < kCrates; ++i) {
        const float x = (static_cast<float>(i) - (kCrates - 1) * 0.5f) * 2.4f;
        core::Transform tf{};
        tf.translation = {x, kCrateHalf, kRangeZ};
        (void)world.spawn_with(
            LocalTransform{tf},
            physics::RigidBody{.motion = motion_of(physics::MotionType::Dynamic), .mass = 4.0f},
            physics::Collider{.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Box),
                              .half_x = kCrateHalf,
                              .half_y = kCrateHalf,
                              .half_z = kCrateHalf},
            // The game's own component, authored per crate. The centre crate is worth more.
            targetrange::Target{.points = i == kCrates / 2 ? 50 : 10, .required = true});
    }
}

// ── The game ──────────────────────────────────────────────────────────────────────────────────
//
// Same division hello-game established: the rules know nothing about devices, windows or frames.
// The difference is that this game's world arrives from a FILE rather than from its own `setup`.
class TargetRange {
public:
    struct Counters {
        int targets = 0;
        int down = 0;
        std::int32_t score = 0;
        int shots = 0;
        int hits = 0;
        int missed_world = 0;   // the ray hit the floor or nothing: a genuine miss
        int missed_blocked = 0; // the ray hit a DIFFERENT target first
        int no_aim = 0;         // a shot with nothing left to aim at; must stay 0 before the win
    };

    // Adopt a world somebody else loaded. The game does not read files: the host does, so the same
    // rules run on a generated scene, on an editor-saved one, and (later) on one a server sent.
    void adopt(ecs::World& world, physics::PhysicsWorld& physics, physics::PhysicsSync& sync) {
        // Bind bodies first: the raycast answers with a BodyId and the rules need to know which
        // entity that is. One reconcile is enough — every body in an authored scene exists already.
        sync.reconcile(world, physics);
        world.query<targetrange::Target, physics::RigidBodyHandle>().for_each(
            [this](targetrange::Target& t, physics::RigidBodyHandle& h) {
                targets_.push_back(Bound{h.body, &t});
                if (!t.down) {
                    ++counters_.targets;
                }
            });
        // Sorted by body index so the shooter's choice of target is independent of ECS archetype
        // iteration order. Without this the firing ORDER could differ between two runs of the same
        // scene, and a determinism proof over shot-by-shot results would be measuring the wrong
        // thing.
        std::sort(targets_.begin(), targets_.end(), [](const Bound& a, const Bound& b) {
            return a.body.index < b.body.index;
        });
        camera_ = find_camera(world);
    }

    void fixed_tick(ecs::World& world,
                    physics::PhysicsWorld& physics,
                    physics::PhysicsSync& sync,
                    double dt) {
        sync.step(world, physics, static_cast<float>(dt));
        if (ticks_ % kTicksBetweenShots == 0 && !won()) {
            shoot(world, physics);
        }
        ++ticks_;
    }

    [[nodiscard]] bool won() const noexcept {
        return counters_.targets > 0 && counters_.down == counters_.targets;
    }

    [[nodiscard]] const Counters& counters() const noexcept { return counters_; }

    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }

    [[nodiscard]] ecs::Entity camera() const noexcept { return camera_; }

private:
    struct Bound {
        physics::BodyId body;
        targetrange::Target* target;
    };

    static ecs::Entity find_camera(ecs::World& world) {
        ecs::Entity found{};
        world.query<render::Camera, ecs::WorldTransform>().for_each(
            [&found, &world](ecs::Entity e, render::Camera&, ecs::WorldTransform&) {
                if (found == ecs::Entity{}) {
                    found = e;
                }
                (void)world;
            });
        return found;
    }

    // Fire one shot at the nearest target still standing.
    //
    // The ray is cast into the real physics world and the result is BELIEVED — including when it
    // says something else was in the way. Aiming is the game's job; deciding what was hit is the
    // engine's, and conflating the two is how a hitscan weapon becomes a guarantee.
    void shoot(ecs::World& world, physics::PhysicsWorld& physics) {
        const auto* cam = world.get<ecs::WorldTransform>(camera_);
        if (cam == nullptr) {
            return;
        }
        const core::Vec3 eye = cam->value.translation;

        const Bound* aim = nullptr;
        float best = 0.0f;
        for (const Bound& b : targets_) {
            if (b.target->down) {
                continue;
            }
            const auto* wt = world.get<ecs::WorldTransform>(entity_of(world, b.body));
            if (wt == nullptr) {
                continue;
            }
            const float d = core::length(wt->value.translation - eye);
            if (aim == nullptr || d < best) {
                aim = &b;
                best = d;
            }
        }
        if (aim == nullptr) {
            ++counters_.no_aim;
            return;
        }

        const auto* target_wt = world.get<ecs::WorldTransform>(entity_of(world, aim->body));
        physics::Ray ray{};
        ray.origin = eye;
        ray.direction = target_wt->value.translation - eye;
        ray.max_distance = kShotRange;

        ++counters_.shots;
        physics::RayHit hit{};
        if (!physics.raycast(ray, hit)) {
            ++counters_.missed_world;
            return;
        }
        if (hit.body.index != aim->body.index) {
            // Something else was in the line of fire. If it was another target, that is a blocked
            // shot on a crowded range; otherwise the floor or a wall ate it. Counted separately
            // because the two mean different things to whoever authored the scene.
            if (target_for(hit.body) != nullptr) {
                ++counters_.missed_blocked;
            } else {
                ++counters_.missed_world;
            }
            return;
        }
        aim->target->down = true;
        counters_.score += aim->target->points;
        ++counters_.down;
        ++counters_.hits;
        // Knock it over rather than deleting it: the crate stays in the world, so a later shot can
        // still be blocked by it, and the scene can be saved mid-range with the state intact.
        physics.apply_central_impulse(aim->body,
                                      core::Vec3{0.0f, 0.35f, -1.0f} * aim->target->knock);
    }

    [[nodiscard]] targetrange::Target* target_for(physics::BodyId body) const {
        for (const Bound& b : targets_) {
            if (b.body.index == body.index) {
                return b.target;
            }
        }
        return nullptr;
    }

    static ecs::Entity entity_of(ecs::World& world, physics::BodyId body) {
        ecs::Entity found{};
        world.query<physics::RigidBodyHandle, ecs::WorldTransform>().for_each(
            [&](ecs::Entity e, physics::RigidBodyHandle& h, ecs::WorldTransform&) {
                if (h.body.index == body.index) {
                    found = e;
                }
            });
        return found;
    }

    std::vector<Bound> targets_;
    ecs::Entity camera_{};
    Counters counters_{};
    std::uint64_t ticks_ = 0;
};

// ── Host ──────────────────────────────────────────────────────────────────────────────────────

// Register the engine's components and then the game's own, in that fixed order. The order is
// shared with every other Rime game on purpose (`profile.hpp:50-52`): `component_schema_hash` goes
// into the net driver's config and two peers that registered different sets refuse to connect.
std::size_t register_all(ecs::World& world) {
    const std::size_t engine = worldkit::register_engine_components(world);
    targetrange::register_content_components(world);
    return engine;
}

// A digest over the authored state this game depends on: every target's placement and points, plus
// the camera's pose and lens.
//
// **This is what makes "the game ran the file the editor saved" a checkable claim rather than a
// hopeful one.** Two runs whose digests differ are provably running different scenes; a game that
// regenerated its own level and ignored the file would print the same digest for both. Quantised to
// a millimetre so a float round-trip through the text format cannot flip it, which would make the
// digest a test of the serializer's printf precision instead of a test of the scene.
// The camera is in here for a reason beyond completeness: it is WHERE YOU SHOOT FROM, so its pose
// is as much a part of this range as the crates are — move it and shots start being blocked by the
// floor. It also happens to be what `editor --smoke` edits (it bumps the first scalar leaf it
// finds, which in this scene is the camera's `fov_y`), and a digest that could not see the editor's
// edit would make the round-trip test unfalsifiable. That is a coincidence worth stating rather
// than a design.
std::uint64_t authored_digest(ecs::World& world) {
    std::vector<std::uint64_t> per_target;
    world.query<targetrange::Target, ecs::WorldTransform>().for_each(
        [&per_target](targetrange::Target& t, ecs::WorldTransform& wt) {
            const auto q = [](float v) {
                return static_cast<std::int64_t>(std::llround(static_cast<double>(v) * 1000.0));
            };
            std::uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
            const std::int64_t parts[4] = {q(wt.value.translation.x),
                                           q(wt.value.translation.y),
                                           q(wt.value.translation.z),
                                           static_cast<std::int64_t>(t.points)};
            for (std::int64_t p : parts) {
                h ^= static_cast<std::uint64_t>(p);
                h *= 1099511628211ull;
            }
            per_target.push_back(h);
        });
    // Order-independent combine: the digest must describe the SET of placements, not the order the
    // ECS happened to iterate them in. An order-sensitive digest would differ between two runs of
    // the same file whenever an archetype layout changed, which is a false alarm that trains you to
    // ignore the alarm.
    std::sort(per_target.begin(), per_target.end());
    std::uint64_t digest = 1469598103934665603ull;
    for (std::uint64_t h : per_target) {
        digest ^= h;
        digest *= 1099511628211ull;
    }
    world.query<render::Camera, ecs::WorldTransform>().for_each(
        [&digest](render::Camera& cam, ecs::WorldTransform& wt) {
            const auto q = [](float v) {
                return static_cast<std::int64_t>(std::llround(static_cast<double>(v) * 1000.0));
            };
            const std::int64_t parts[4] = {q(wt.value.translation.x),
                                           q(wt.value.translation.y),
                                           q(wt.value.translation.z),
                                           q(cam.fov_y)};
            for (std::int64_t pp : parts) {
                digest ^= static_cast<std::uint64_t>(pp);
                digest *= 1099511628211ull;
            }
        });
    return digest;
}

struct RunResult {
    bool loaded = false;
    std::string load_error;
    std::size_t entities = 0;
    std::size_t skipped_components = 0;
    std::uint64_t digest = 0;
    TargetRange::Counters counters{};
    std::uint64_t ticks = 0;
    bool won = false;
};

RunResult play(const std::filesystem::path& scene, int tick_budget, unsigned workers) {
    RunResult result{};
    app::AppConfig config{};
    config.worker_threads = workers;
    app::Application app(config);
    register_all(app.world());

    // STRICT load: no `allow_unknown_components`. A lenient load would skip this game's own
    // `targetrange::Target` records if the engine could not match them and come up with an empty
    // range that passes nothing — the exact failure this milestone is supposed to detect.
    const scene::LoadReport report = scene::load_scene_file(app.world(), scene);
    result.loaded = report.ok;
    result.load_error = report.error;
    result.entities = report.entities;
    result.skipped_components = report.skipped_components;
    if (!report.ok) {
        return result;
    }
    // WorldTransform is DERIVED and is not written by a load, so nothing has a world pose until
    // this runs. Skipping it leaves every crate at the origin and the range silently unwinnable.
    scene::derive_world_transforms(app.world(), app.jobs());
    result.digest = authored_digest(app.world());

    physics::PhysicsWorld physics;
    physics::PhysicsSync sync;
    physics.set_job_system(&app.jobs());

    TargetRange game;
    game.adopt(app.world(), physics, sync);
    app.on_fixed_tick(
        [&](ecs::World& world, double dt) { game.fixed_tick(world, physics, sync, dt); });

    const double fd = app.fixed_dt();
    for (int i = 0; i < tick_budget && !game.won(); ++i) {
        app.step(fd);
    }
    result.counters = game.counters();
    result.ticks = game.ticks();
    result.won = game.won();
    return result;
}

int run_selftest(const std::filesystem::path& scene, bool verbose) {
    constexpr int kBudget = 4000;

    const RunResult first = play(scene, kBudget, 0);
    if (!first.loaded) {
        fmt::print(
            stderr, "target-range: FAILED to load {}: {}\n", scene.string(), first.load_error);
        return 1;
    }
    // A belt-and-braces guard, described honestly: on a STRICT load this can never fire, because an
    // unregistered type makes the load itself fail ("unknown component type 'targetrange::Target'
    // (hash 0x…) — is it registered?", measured by deleting the registration). The real assertion
    // for the platform claim is the `targets == 0` check below plus that failing load. This one
    // exists so that if anybody ever adds `allow_unknown_components` here for convenience, the
    // range stops passing silently with its own data thrown away.
    if (first.skipped_components != 0) {
        fmt::print(stderr,
                   "target-range: FAILED — {} component records skipped; the engine did not "
                   "understand this game's own components\n",
                   first.skipped_components);
        return 1;
    }
    if (first.counters.targets == 0) {
        fmt::print(stderr,
                   "target-range: FAILED — the scene declared no targets, so winning is vacuous\n");
        return 1;
    }
    if (!first.won) {
        fmt::print(stderr,
                   "target-range: FAILED — {} of {} targets down after {} ticks "
                   "({} shots, {} world misses, {} blocked)\n",
                   first.counters.down,
                   first.counters.targets,
                   first.ticks,
                   first.counters.shots,
                   first.counters.missed_world,
                   first.counters.missed_blocked);
        return 1;
    }
    if (first.counters.no_aim != 0) {
        fmt::print(stderr,
                   "target-range: FAILED — {} shots had nothing to aim at\n",
                   first.counters.no_aim);
        return 1;
    }

    // Determinism on a different worker count, as every other sim proof here asserts (ADR-0026).
    const RunResult second = play(scene, kBudget, 1);
    if (second.ticks != first.ticks || second.counters.score != first.counters.score ||
        second.counters.hits != first.counters.hits ||
        second.counters.missed_world != first.counters.missed_world ||
        second.counters.missed_blocked != first.counters.missed_blocked ||
        second.digest != first.digest) {
        fmt::print(stderr,
                   "target-range: FAILED — nondeterministic: ticks {}/{} score {}/{} hits {}/{} "
                   "digest {:#018x}/{:#018x}\n",
                   second.ticks,
                   first.ticks,
                   second.counters.score,
                   first.counters.score,
                   second.counters.hits,
                   first.counters.hits,
                   second.digest,
                   first.digest);
        return 1;
    }

    if (verbose) {
        fmt::print("target-range — M15's platform proof\n");
        fmt::print("  scene              : {}\n", scene.string());
        fmt::print("  entities loaded    : {}\n", first.entities);
        fmt::print("  skipped components : {}  (strict load: an unknown type would have FAILED)\n",
                   first.skipped_components);
        fmt::print("  targets (from file): {}\n", first.counters.targets);
        fmt::print("  authored digest    : {:#018x}\n", first.digest);
        fmt::print("  shots / hits       : {} / {}\n", first.counters.shots, first.counters.hits);
        fmt::print("  misses world/block : {} / {}\n",
                   first.counters.missed_world,
                   first.counters.missed_blocked);
        fmt::print("  score              : {}\n", first.counters.score);
        fmt::print("  ticks to clear     : {}\n", first.ticks);
        fmt::print("  determinism        : identical on 1 worker and on hardware_concurrency-1\n");
        fmt::print("  PASS\n");
    }
    return 0;
}

int run_digest(const std::filesystem::path& scene) {
    app::Application app(app::AppConfig{});
    register_all(app.world());
    const scene::LoadReport report = scene::load_scene_file(app.world(), scene);
    if (!report.ok) {
        fmt::print(stderr, "target-range: FAILED to load {}: {}\n", scene.string(), report.error);
        return 1;
    }
    scene::derive_world_transforms(app.world(), app.jobs());
    fmt::print("{:#018x}\n", authored_digest(app.world()));
    return 0;
}

int run_emit(const std::filesystem::path& out) {
    app::Application app(app::AppConfig{});
    register_all(app.world());
    author_scene(app.world());
    scene::derive_world_transforms(app.world(), app.jobs());
    if (!scene::save_scene_file(app.world(), out)) {
        fmt::print(stderr, "target-range: could not write {}\n", out.string());
        return 1;
    }
    fmt::print("target-range: wrote {}\n", out.string());
    return 0;
}

// The rendered run. Meshes and materials are built here rather than authored, because a `.rscene`
// names a mesh by content id and this sample deliberately ships no cooked assets — that is M16's
// subject, not M15's. The SCENE is authored; the look is code. Saying so is better than implying
// the crates' appearance came out of the editor too.
int run_rendered(const std::filesystem::path& scene, int frames) {
    app::AppConfig config{};
    config.gpu = true;
    config.render_extent = {1280, 720};
    app::Application app(config);
    if (app.device() == nullptr) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            fmt::print(stderr, "target-range: no Vulkan device and RIME_REQUIRE_VULKAN is set\n");
            return 1;
        }
        fmt::print("target-range: no Vulkan device — skipping the rendered run\n");
        return 0;
    }
    register_all(app.world());
    const scene::LoadReport report = scene::load_scene_file(app.world(), scene);
    if (!report.ok) {
        fmt::print(stderr, "target-range: FAILED to load {}: {}\n", scene.string(), report.error);
        return 1;
    }
    scene::derive_world_transforms(app.world(), app.jobs());

    physics::PhysicsWorld physics;
    physics::PhysicsSync sync;
    physics.set_job_system(&app.jobs());
    render::MeshRegistry meshes(*app.device());
    render::MaterialRegistry materials;
    render::SceneRenderer renderer(*app.device(), meshes, materials);
    renderer.set_frames_in_flight(app.frames_in_flight());
    renderer.set_ambient(0.05f, 0.05f, 0.06f);

    const render::MeshId crate = meshes.add(render::make_cube(kCrateHalf), "crate");
    const render::MeshId ground = meshes.add(render::make_plane(24.0f, 24.0f), "ground");
    render::PbrMaterialDesc crate_mat{};
    crate_mat.base_color[0] = 0.70f;
    crate_mat.base_color[1] = 0.45f;
    crate_mat.base_color[2] = 0.20f;
    crate_mat.roughness = 0.6f;
    const render::MaterialId crate_material = materials.add(crate_mat);
    render::PbrMaterialDesc ground_mat{};
    ground_mat.base_color[0] = 0.28f;
    ground_mat.base_color[1] = 0.30f;
    ground_mat.base_color[2] = 0.33f;
    ground_mat.roughness = 0.9f;
    const render::MaterialId ground_material = materials.add(ground_mat);

    // Give the loaded targets a look. The scene said WHERE and WHAT; this says how it draws.
    app.world().query<targetrange::Target, ecs::WorldTransform>().for_each(
        [&](ecs::Entity e, targetrange::Target&, ecs::WorldTransform&) {
            app.world().add_component(e, render::MeshRef{crate});
            app.world().add_component(e, render::MaterialRef{crate_material});
        });
    {
        core::Transform tf{};
        (void)app.world().spawn_with(
            ecs::WorldTransform{tf}, render::MeshRef{ground}, render::MaterialRef{ground_material});
    }

    TargetRange game;
    game.adopt(app.world(), physics, sync);
    app.on_fixed_tick(
        [&](ecs::World& world, double dt) { game.fixed_tick(world, physics, sync, dt); });
    app.on_render([&](app::FrameContext& ctx) {
        ctx.present = renderer.render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
    });
    app.run_frames(frames);
    fmt::print("target-range: rendered {} frames, {} of {} targets down, score {}\n",
               frames,
               game.counters().down,
               game.counters().targets,
               game.counters().score);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path scene;
    std::filesystem::path emit;
    bool verbose = false;
    bool digest_only = false;
    bool headless = false;
    int frames = 240;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--scene" && i + 1 < argc) {
            scene = argv[++i];
        } else if (arg == "--emit-scene" && i + 1 < argc) {
            emit = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--digest") {
            digest_only = true;
        } else if (arg == "--headless") {
            headless = true;
        } else if (arg == "--frames" && i + 1 < argc) {
            frames = std::atoi(argv[++i]);
        } else {
            fmt::print(stderr,
                       "usage: target_range --emit-scene <out.rscene>\n"
                       "       target_range --scene <in.rscene> [--verbose|--digest|--headless]\n");
            return 2;
        }
    }
    if (!emit.empty()) {
        return run_emit(emit);
    }
    if (scene.empty()) {
        fmt::print(stderr, "target-range: --scene <file.rscene> is required\n");
        return 2;
    }
    if (digest_only) {
        return run_digest(scene);
    }
    if (headless) {
        return run_rendered(scene, frames);
    }
    return run_selftest(scene, verbose);
}
