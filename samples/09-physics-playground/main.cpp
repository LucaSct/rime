// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 09-physics-playground — Milestone 7's "done when": objects fall/collide/stack, raycasts hit, and
// the simulation runs on the job system inside the app's fixed tick. One runnable program shows all
// three, self-checking headless so CI can gate on it (the 05-ecs-playground / 08-gltf-zoo pattern).
//
// The scene: a static floor, a four-box tower, a 3-2-1 box pyramid, and a scatter of spheres — all
// authored as ECS entities (RigidBody + Collider + WorldTransform), bound to physics bodies by
// PhysicsSync, and stepped through app::Application's fixed tick via the per-tick hook. It settles
// and sleeps; then a raycast fired at the tower finds a box and an impulse knocks it out, toppling
// the stack; it scatters and re-sleeps. The self-check asserts: the raycast hit the box we aimed
// at, that box was displaced past a bound, the whole scene came to rest (sleeping), and — the
// headline — running the identical scenario twice yields a bit-identical world_hash (determinism,
// ADR-0026).
//
// GPU-free by construction (physics is CPU-only), so the self-check runs on every CI OS + the
// sanitizers, no device needed. On-screen live viewing (--serve, streamed to 04-remote-view like
// 07/08) reuses the render path and is the documented follow-up; this brick proves the simulation.
//
//   build/<preset>/bin/physics_playground            # self-check (silent), exit 0 = M7 "done when"
//   build/<preset>/bin/physics_playground --verbose  # same, with a printed report

#include <fmt/core.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/physics.hpp"

namespace {

using namespace rime;

// ── Scene authoring helpers: an entity that says "simulate me as this shape"
// ──────────────────────
ecs::Entity spawn_box(ecs::World& w,
                      std::vector<ecs::Entity>& dynamics,
                      core::Vec3 pos,
                      core::Vec3 half,
                      physics::MotionType motion) {
    physics::RigidBody rb;
    rb.motion = static_cast<std::uint32_t>(motion);
    physics::Collider col;
    col.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Box);
    col.half_x = half.x;
    col.half_y = half.y;
    col.half_z = half.z;
    ecs::WorldTransform wt;
    wt.value.translation = pos;
    const ecs::Entity e = w.spawn_with(rb, col, wt);
    if (motion == physics::MotionType::Dynamic) {
        dynamics.push_back(e);
    }
    return e;
}

ecs::Entity
spawn_sphere(ecs::World& w, std::vector<ecs::Entity>& dynamics, core::Vec3 pos, float r) {
    physics::RigidBody rb; // dynamic by default
    physics::Collider col;
    col.shape_type = static_cast<std::uint32_t>(physics::ShapeType::Sphere);
    col.radius = r;
    ecs::WorldTransform wt;
    wt.value.translation = pos;
    const ecs::Entity e = w.spawn_with(rb, col, wt);
    dynamics.push_back(e);
    return e;
}

physics::BodyId body_of(ecs::World& w, ecs::Entity e) {
    return w.get<physics::RigidBodyHandle>(e)->body;
}

struct Scenario {
    bool raycast_hit_target = false;
    float target_displacement = 0.0f;
    bool all_asleep = false;
    std::uint64_t final_hash = 0;
    std::size_t body_count = 0;
};

// Run the whole playground once and report what happened. Deterministic: identical calls produce an
// identical world_hash (the determinism proof runs this twice and compares).
Scenario run_scenario(bool verbose) {
    // Fixed tick counts (not "step until…") so two runs issue byte-identical work — the premise of
    // the determinism check. Generous enough that the scene fully settles and sleeps within them.
    constexpr int kSettleTicks = 200;
    constexpr int kAfterTicks = 600;
    constexpr float kImpulse =
        8.0f; // kg·m/s: knocks a 1 kg box out and topples the tower, but
              // gently enough that the debris stays on the floor and re-sleeps

    app::Application app(app::AppConfig{}); // headless, GPU-free, 60 Hz fixed tick
    physics::PhysicsWorld phys;
    physics::PhysicsSync sync;
    phys.set_job_system(
        &app.jobs()); // the sim runs on the app's job system — "parallel to the frame"

    ecs::World& w = app.world();
    physics::register_physics_components(w);

    std::vector<ecs::Entity> dynamics;

    // Floor: a big static slab whose top surface sits at y = 0.
    spawn_box(w, dynamics, {0.0f, -0.5f, 0.0f}, {50.0f, 0.5f, 50.0f}, physics::MotionType::Static);

    // A four-box tower at the origin (each box is 1×1×1). The second box up is our aim point.
    ecs::Entity target{};
    for (int i = 0; i < 4; ++i) {
        const ecs::Entity e = spawn_box(w,
                                        dynamics,
                                        {0.0f, 0.5f + static_cast<float>(i), 0.0f},
                                        {0.5f, 0.5f, 0.5f},
                                        physics::MotionType::Dynamic);
        if (i == 1) {
            target = e;
        }
    }

    // A 3-2-1 pyramid a few metres away in Z.
    for (int row = 0; row < 3; ++row) {
        const float y = 0.5f + static_cast<float>(row);
        const int count = 3 - row;
        for (int c = 0; c < count; ++c) {
            const float x =
                static_cast<float>(c - (count - 1)) * 1.05f + static_cast<float>(row) * 0.0f;
            spawn_box(w, dynamics, {x, y, 6.0f}, {0.5f, 0.5f, 0.5f}, physics::MotionType::Dynamic);
        }
    }

    // A scatter of spheres resting on the floor off to one side.
    for (int i = 0; i < 3; ++i) {
        spawn_sphere(w, dynamics, {-6.0f + static_cast<float>(i) * 1.4f, 0.5f, -4.0f}, 0.5f);
    }

    // Drive physics inside the fixed tick: reconcile (bind new bodies) → step → write-back.
    app.on_fixed_tick(
        [&](ecs::World& world, double dt) { sync.step(world, phys, static_cast<float>(dt)); });

    // Settle. Each app.step(fixed_dt) runs exactly one tick.
    const double fd = app.fixed_dt();
    for (int i = 0; i < kSettleTicks; ++i) {
        app.step(fd);
    }

    Scenario s;
    const physics::BodyId target_body = body_of(w, target);
    physics::BodyState pre{};
    (void)phys.get_body_state(target_body, pre); // the body is bound and live — always true

    // Fire a hitscan ray at the tower from the +X side, at the target box's height. It should hit
    // the target's +X face; an impulse along the ray knocks it out.
    physics::RayHit hit;
    const bool got =
        phys.raycast(physics::Ray{{10.0f, pre.position.y, 0.0f}, {-1.0f, 0.0f, 0.0f}}, hit);
    s.raycast_hit_target = got && hit.body.index == target_body.index;
    if (got) {
        phys.apply_impulse(hit.body, core::Vec3{-1.0f, 0.0f, 0.0f} * kImpulse, hit.point);
    }

    // Let the toppled tower scatter and everything come back to rest.
    for (int i = 0; i < kAfterTicks; ++i) {
        app.step(fd);
    }

    physics::BodyState post{};
    (void)phys.get_body_state(target_body, post);
    s.target_displacement = core::length(post.position - pre.position);

    s.all_asleep = true;
    for (const ecs::Entity e : dynamics) {
        if (!phys.is_asleep(body_of(w, e))) {
            s.all_asleep = false;
            break;
        }
    }
    s.final_hash = phys.world_hash();
    s.body_count = phys.body_count();

    if (verbose) {
        fmt::print("  bodies                : {} ({} dynamic + 1 static floor)\n",
                   s.body_count,
                   dynamics.size());
        fmt::print("  ticks                 : {} settle + {} after the shot ({} Hz)\n",
                   kSettleTicks,
                   kAfterTicks,
                   static_cast<int>(1.0 / fd));
        fmt::print("  raycast hit the target: {}", s.raycast_hit_target ? "yes" : "NO");
        if (got) {
            fmt::print(" (at {:.2f}, {:.2f}, {:.2f}, dist {:.2f})",
                       hit.point.x,
                       hit.point.y,
                       hit.point.z,
                       hit.distance);
        }
        fmt::print("\n  target box displaced  : {:.3f} m\n", s.target_displacement);
        fmt::print("  all bodies asleep     : {}\n", s.all_asleep ? "yes" : "NO");
        fmt::print("  world_hash            : {:#018x}\n", s.final_hash);
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--verbose" || arg == "--serve") {
            verbose = true; // --serve (live GPU view) reuses the 07/08 render path — a follow-up;
                            // for now it prints the play-by-play so the run is observable.
        }
        // --headless is the default and needs no flag.
    }

    fmt::print("Rime physics playground — the M7 proof (samples/09-physics-playground)\n\n");

    if (verbose) {
        fmt::print("run 1:\n");
    }
    const Scenario a = run_scenario(verbose);
    const Scenario b = run_scenario(false); // a second, independent run for the determinism check
    const bool deterministic = a.final_hash == b.final_hash;

    // M7's "done when": objects fall/collide/stack (the tower/pyramid settle & sleep), raycasts hit
    // (the shot found the box we aimed at), it runs on the job system inside the fixed tick (it
    // did), and the whole thing is deterministic (two runs, one hash).
    const bool fell_and_stacked = a.all_asleep;
    const bool raycast_ok = a.raycast_hit_target;
    const bool knocked = a.target_displacement > 0.5f;
    const bool ok = fell_and_stacked && raycast_ok && knocked && deterministic;

    fmt::print("\nchecks:\n");
    fmt::print("  fall / collide / stack (all asleep) : {}\n", fell_and_stacked ? "PASS" : "FAIL");
    fmt::print("  raycast hits the aimed-at box       : {}\n", raycast_ok ? "PASS" : "FAIL");
    fmt::print("  impulse knocks it out (>0.5 m)      : {}\n", knocked ? "PASS" : "FAIL");
    fmt::print("  deterministic (two runs, one hash)  : {} ({:#018x} vs {:#018x})\n",
               deterministic ? "PASS" : "FAIL",
               a.final_hash,
               b.final_hash);
    fmt::print("\nM7 \"done when\": {}\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
