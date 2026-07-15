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
// --stress [N] is the M7.13 measure-first capstone: it drops N debris boxes (default 1000) onto a
// floor on the job system and reports the WorldStats instrument panel — peak contact/island load —
// alongside step throughput (body-steps/s), then self-checks the two properties that must survive
// any scale: the pile settles, and two runs hash identically. It is opt-in so the default M7 "done
// when" gate above stays fast; the determinism-at-scale correctness is also gated by the physics
// suite's stats_test (this mode is the human-facing measurement tool).
//
//   build/<preset>/bin/physics_playground            # self-check (silent), exit 0 = M7 "done when"
//   build/<preset>/bin/physics_playground --verbose  # same, with a printed report
//   build/<preset>/bin/physics_playground --stress   # debris-scale load + throughput report
//   build/<preset>/bin/physics_playground --stress 4000   # …with an explicit body count

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/core/jobs/job_system.hpp"
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

// ── Debris-scale stress harness (M7.13) ─────────────────────────────────────────────────────────
// The measure-first capstone: what the physics core does under a debris load, measured. It drives
// PhysicsWorld directly (no ECS/app) so the numbers are the raw solver's, and reads the WorldStats
// instrument panel step() now fills — peak collision and island load — plus wall-clock throughput.
struct StressReport {
    std::size_t dynamic = 0; // dynamic debris bodies (plus one static floor)
    int ticks = 0;
    double step_seconds = 0.0; // wall time of the step loop alone (spawn excluded)
    bool settled = false;      // every dynamic body asleep at the end
    std::uint64_t hash = 0;    // world_hash() — the determinism witness
    physics::WorldStats final_stats;
    // Element-wise peaks over the whole run — the high-water marks a solver optimization targets.
    physics::WorldStats peak;
};

// Drop `count` unit boxes onto a floor as a side×side grid of short stacks and step the pile on the
// job system for a fixed tick budget. `time_it` controls only whether the step loop is wall-clock
// timed (the determinism re-run doesn't need it). Fixed tick count + deterministic authoring ⇒ two
// calls with the same count produce the same world_hash (the check the caller makes).
StressReport run_stress(int count, core::JobSystem& jobs, bool time_it) {
    constexpr int kLayers = 6;          // stack height — deep enough for real vertical contact load
    constexpr float kSpacing = 1.5f;    // grid pitch: unit boxes 0.5 m apart ⇒ stacks stay
                                        // INDEPENDENT (no leaning on a neighbour), so every column
                                        // settles cleanly and sleeps rather than jittering forever
    constexpr int kTicks = 500;         // enough for an independent 6-high stack to settle + sleep
    constexpr float kDt = 1.0f / 60.0f; // the app's fixed tick (ADR-0023)

    physics::PhysicsWorld phys;
    phys.set_job_system(&jobs); // debris is embarrassingly parallel — many independent islands

    // Floor: a big static slab, top surface at y = 0.
    physics::BodyDesc floor;
    floor.motion = physics::MotionType::Static;
    floor.shape = physics::ShapeDesc{physics::ShapeType::Box, 0.0f, {80.0f, 0.5f, 80.0f}, 0.0f};
    floor.position = {0.0f, -0.5f, 0.0f};
    (void)phys.create_body(floor);

    // A side×side grid of kLayers-tall stacks, centred on the origin, dropped a hair above contact.
    const int footprint = (count + kLayers - 1) / kLayers;
    const int side = static_cast<int>(std::sqrt(static_cast<double>(footprint)) + 0.999);
    const float origin = -0.5f * static_cast<float>(side - 1) * kSpacing;
    int made = 0;
    for (int cell = 0; cell < side * side && made < count; ++cell) {
        const float cx = origin + static_cast<float>(cell % side) * kSpacing;
        const float cz = origin + static_cast<float>(cell / side) * kSpacing;
        for (int j = 0; j < kLayers && made < count; ++j) {
            physics::BodyDesc d;
            d.shape = physics::ShapeDesc{physics::ShapeType::Box, 0.0f, {0.5f, 0.5f, 0.5f}, 0.0f};
            d.position = {cx, 0.6f + static_cast<float>(j) * 1.02f, cz};
            // A little air drag: bleeds the tiny residual jitter a sequential-impulse solver leaves
            // in a tall stack so the whole pile crosses the sleep threshold rather than a fraction
            // of columns limit-cycling forever just above it. Debris has drag anyway.
            d.linear_damping = 0.1f;
            d.angular_damping = 0.1f;
            (void)phys.create_body(d);
            ++made;
        }
    }

    StressReport r;
    r.dynamic = static_cast<std::size_t>(made);
    r.ticks = kTicks;

    const auto merge_peak = [](physics::WorldStats& acc, const physics::WorldStats& s) {
        acc.awake_bodies = std::max(acc.awake_bodies, s.awake_bodies);
        acc.broadphase_pairs = std::max(acc.broadphase_pairs, s.broadphase_pairs);
        acc.manifolds = std::max(acc.manifolds, s.manifolds);
        acc.contact_points = std::max(acc.contact_points, s.contact_points);
        acc.islands = std::max(acc.islands, s.islands);
        acc.active_islands = std::max(acc.active_islands, s.active_islands);
        acc.largest_island = std::max(acc.largest_island, s.largest_island);
    };

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kTicks; ++i) {
        phys.step(kDt);
        merge_peak(r.peak, phys.stats());
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (time_it) {
        r.step_seconds = std::chrono::duration<double>(t1 - t0).count();
    }

    r.final_stats = phys.stats();
    r.settled = r.final_stats.awake_bodies == 0;
    r.hash = phys.world_hash();
    return r;
}

// Run the stress harness, print the report, and return an exit code (0 = passed both checks).
int stress_main(int count) {
    core::JobSystem jobs; // default worker count = hardware_concurrency - 1 (+ this thread)
    fmt::print("debris-scale stress harness (M7.13) — {} dynamic bodies on {} worker(s)\n\n",
               count,
               jobs.participant_count());

    const StressReport a = run_stress(count, jobs, /*time_it=*/true);
    const StressReport b = run_stress(count, jobs, /*time_it=*/false); // determinism re-run
    const bool deterministic = a.hash == b.hash;

    const double body_steps = static_cast<double>(a.dynamic) * static_cast<double>(a.ticks);
    const double per_tick_ms = a.ticks > 0 ? a.step_seconds / a.ticks * 1e3 : 0.0;
    const double throughput = a.step_seconds > 0.0 ? body_steps / a.step_seconds : 0.0;

    fmt::print(
        "scene   : {} dynamic boxes + 1 static floor, {} ticks @ 60 Hz\n", a.dynamic, a.ticks);
    fmt::print("timing  : {:.3f} s total, {:.3f} ms/tick, {:.2f}M body-steps/s\n",
               a.step_seconds,
               per_tick_ms,
               throughput / 1e6);
    fmt::print("peak    : {} manifolds, {} contact points, {} islands (largest {}), {} awake\n",
               a.peak.manifolds,
               a.peak.contact_points,
               a.peak.islands,
               a.peak.largest_island,
               a.peak.awake_bodies);
    fmt::print("settled : {} asleep, {} active islands\n",
               a.final_stats.sleeping_bodies,
               a.final_stats.active_islands);
    fmt::print("\nchecks:\n");
    fmt::print("  pile comes to rest (all asleep)     : {}\n", a.settled ? "PASS" : "FAIL");
    fmt::print("  deterministic (two runs, one hash)  : {} ({:#018x} vs {:#018x})\n",
               deterministic ? "PASS" : "FAIL",
               a.hash,
               b.hash);
    const bool ok = a.settled && deterministic;
    fmt::print("\nstress harness: {}\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    bool verbose = false;
    bool stress = false;
    int stress_count = 1000; // a debris-scale default; --stress N overrides
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--verbose" || arg == "--serve") {
            verbose = true; // --serve (live GPU view) reuses the 07/08 render path — a follow-up;
                            // for now it prints the play-by-play so the run is observable.
        } else if (arg == "--stress") {
            stress = true;
            // An optional bare integer immediately after --stress sets the body count.
            if (i + 1 < argc) {
                const std::string_view next = argv[i + 1];
                if (!next.empty() && next.front() != '-') {
                    stress_count = std::atoi(argv[i + 1]);
                    ++i;
                }
            }
        }
        // --headless is the default and needs no flag.
    }

    if (stress) {
        return stress_main(std::max(stress_count, 1));
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
