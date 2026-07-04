// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M4.6 — the ECS proof. M4's "done when" is two claims; this one runnable program shows both.
//
//  (1) 100k+ entities update IN PARALLEL. We build a field of 200k moving entities and step them
//  with
//      a compute-bound integrate kernel, once serially (Query::for_each) and once across all cores
//      (Query::par_for_each via a System Schedule), check the two results agree bit-for-bit, and
//      report the speedup — which climbs toward the core count. Then we run the whole frame
//      (integrate system + transform propagation) to show the scheduler driving real work at scale.
//
//  (2) transforms COMPOSE correctly. We build a small articulated hierarchy (a tank: hull → turret
//  →
//      barrel → muzzle), propagate world transforms down it, and check each equals the
//      hand-composed product of the locals — then move the hull and confirm the whole tank follows.
//
// Exits non-zero if the parallel result diverges from serial or any composition check fails, so it
// doubles as a smoke test. Run it: build/<preset>/bin/ecs_playground

#include <fmt/core.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "rime/core/jobs.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs.hpp"

namespace {

using rime::core::JobSystem;
using rime::core::Transform;
using rime::core::Vec3;
using namespace rime::ecs;

// A per-entity velocity. LocalTransform / WorldTransform / Parent come from the ECS
// (transform.hpp).
struct Velocity {
    Vec3 v;
};

// A deliberately compute-bound step: a swirling vector field integrated over several sub-steps, so
// the loop is arithmetic-bound (parallelism shows) and deterministic (serial and parallel must
// agree bit-for-bit — each entity is independent, so execution order cannot change any result).
void integrate(LocalTransform& lt, Velocity& vel, float dt) {
    Vec3 p = lt.value.translation;
    Vec3 v = vel.v;
    for (int k = 0; k < 24; ++k) {
        v.x += (std::sin(p.y * 0.7f) - 0.1f * v.x) * dt;
        v.y += (std::cos(p.x * 0.7f) - 0.1f * v.y) * dt;
        v.z += (std::sin(p.x * 0.3f + p.y * 0.2f) - 0.1f * v.z) * dt;
        p.x += v.x * dt;
        p.y += v.y * dt;
        p.z += v.z * dt;
    }
    lt.value.translation = p;
    vel.v = v;
}

template <class Clock> double ms_between(Clock a, Clock b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

Transform translate(float x, float y, float z) {
    Transform t;
    t.translation = Vec3{x, y, z};
    return t;
}

// (1) 100k+ entities update in parallel — the headline proof, with a measured speedup.
bool run_parallel_field(JobSystem& jobs) {
    constexpr std::uint32_t kCount = 200'000; // comfortably over M4's 100k bar
    constexpr float kDt = 0.016f;

    World world;
    world.reserve_entities(kCount);
    std::vector<Entity> field;
    field.reserve(kCount);

    // Seed each entity with a spread-out position and a small initial velocity. Snapshot the seed
    // so we can rewind and run the same step twice (serial, then parallel) from an identical state.
    std::vector<LocalTransform> seed_lt;
    std::vector<Velocity> seed_vel;
    seed_lt.reserve(kCount);
    seed_vel.reserve(kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        const float f = static_cast<float>(i);
        const LocalTransform lt{
            translate(std::sin(f * 0.01f) * 50.0f, std::cos(f * 0.017f) * 50.0f, f * 0.001f)};
        const Velocity vel{Vec3{0.1f, -0.1f, 0.05f}};
        field.push_back(world.spawn_with(lt, vel, WorldTransform{}));
        seed_lt.push_back(lt);
        seed_vel.push_back(vel);
    }

    const auto rewind = [&] {
        for (std::uint32_t i = 0; i < kCount; ++i) {
            *world.get<LocalTransform>(field[i]) = seed_lt[i];
            *world.get<Velocity>(field[i]) = seed_vel[i];
        }
    };

    // --- serial baseline: the same kernel via Query::for_each ---
    rewind();
    const auto s0 = std::chrono::steady_clock::now();
    world.query<LocalTransform, Velocity>().for_each(
        [](LocalTransform& lt, Velocity& vel) { integrate(lt, vel, kDt); });
    const auto s1 = std::chrono::steady_clock::now();
    std::vector<Vec3> serial_out(kCount);
    for (std::uint32_t i = 0; i < kCount; ++i) {
        serial_out[i] = world.get<LocalTransform>(field[i])->value.translation;
    }

    // --- parallel: the identical kernel across all cores via Query::par_for_each ---
    rewind();
    const auto p0 = std::chrono::steady_clock::now();
    world.query<LocalTransform, Velocity>().par_for_each(
        jobs, [](LocalTransform& lt, Velocity& vel) { integrate(lt, vel, kDt); });
    const auto p1 = std::chrono::steady_clock::now();

    bool match = true;
    for (std::uint32_t i = 0; i < kCount; ++i) {
        if (world.get<LocalTransform>(field[i])->value.translation.x != serial_out[i].x ||
            world.get<LocalTransform>(field[i])->value.translation.y != serial_out[i].y ||
            world.get<LocalTransform>(field[i])->value.translation.z != serial_out[i].z) {
            match = false;
            break;
        }
    }

    // Now a realistic frame at scale: an integrate System scheduled by access set, then the
    // transform propagation (this flat field takes propagate_transforms' fully-parallel fast path).
    Schedule schedule;
    schedule.add({"integrate",
                  {signature_of<Velocity>(world), signature_of<LocalTransform>(world)},
                  [](World& w, JobSystem& j, CommandBuffer&) {
                      w.query<LocalTransform, Velocity>().par_for_each(
                          j, [](LocalTransform& lt, Velocity& vel) { integrate(lt, vel, kDt); });
                  }});
    const auto f0 = std::chrono::steady_clock::now();
    schedule.run(world, jobs);
    propagate_transforms(world, jobs);
    const auto f1 = std::chrono::steady_clock::now();

    const double serial_ms = ms_between(s0, s1);
    const double parallel_ms = ms_between(p0, p1);
    const double speedup = parallel_ms > 0.0 ? serial_ms / parallel_ms : 0.0;

    fmt::print("(1) 100k+ entities in parallel\n");
    fmt::print("    entities            : {}\n", kCount);
    fmt::print("    workers + main      : {}\n", jobs.participant_count());
    fmt::print("    integrate serial    : {:.1f} ms\n", serial_ms);
    fmt::print("    integrate parallel  : {:.1f} ms\n", parallel_ms);
    fmt::print("    speedup             : {:.2f}x\n", speedup);
    fmt::print("    serial == parallel  : {}\n", match ? "yes" : "NO");
    fmt::print("    scheduled frame     : {:.1f} ms  (integrate system + transform propagation)\n",
               ms_between(f0, f1));
    return match;
}

// (2) transforms compose correctly — a tank hierarchy, hand-checked.
bool run_transform_hierarchy(JobSystem& jobs) {
    World world;

    const Transform hull_local = translate(20.0f, 0.0f, 0.0f);
    Transform turret_local = translate(0.0f, 1.5f, 0.0f);
    turret_local.rotation = rime::core::quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, 0.7853982f);
    Transform barrel_local = translate(0.0f, 0.0f, 1.0f);
    barrel_local.rotation = rime::core::quat_from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, -0.3f);
    const Transform muzzle_local = translate(0.0f, 0.0f, 3.0f);

    const Entity hull = world.spawn_with(LocalTransform{hull_local}, WorldTransform{});
    const Entity turret =
        world.spawn_with(LocalTransform{turret_local}, WorldTransform{}, Parent{hull});
    const Entity barrel =
        world.spawn_with(LocalTransform{barrel_local}, WorldTransform{}, Parent{turret});
    const Entity muzzle =
        world.spawn_with(LocalTransform{muzzle_local}, WorldTransform{}, Parent{barrel});

    propagate_transforms(world, jobs);

    // Each world transform must equal the product of the locals down the chain (core::Transform's
    // compose operator — the same one propagate_transforms uses, checked here independently).
    const Transform expect_turret = hull_local * turret_local;
    const Transform expect_barrel = expect_turret * barrel_local;
    const Transform expect_muzzle = expect_barrel * muzzle_local;
    const bool composed = approx_eq(world.get<WorldTransform>(hull)->value, hull_local) &&
                          approx_eq(world.get<WorldTransform>(turret)->value, expect_turret) &&
                          approx_eq(world.get<WorldTransform>(barrel)->value, expect_barrel) &&
                          approx_eq(world.get<WorldTransform>(muzzle)->value, expect_muzzle);

    // Move the hull; the whole tank must follow on the next propagate.
    const Vec3 before = world.get<WorldTransform>(muzzle)->value.translation;
    world.get<LocalTransform>(hull)->value.translation.x += 100.0f;
    propagate_transforms(world, jobs);
    const Vec3 after = world.get<WorldTransform>(muzzle)->value.translation;
    const bool followed = std::abs((after.x - before.x) - 100.0f) < 1e-3f &&
                          std::abs(after.y - before.y) < 1e-3f &&
                          std::abs(after.z - before.z) < 1e-3f;

    fmt::print("(2) transforms compose correctly\n");
    fmt::print("    hull → turret → barrel → muzzle\n");
    fmt::print(
        "    muzzle world        : ({:.3f}, {:.3f}, {:.3f})\n", before.x, before.y, before.z);
    fmt::print("    composed == product : {}\n", composed ? "yes" : "NO");
    fmt::print("    hull moved +100x → muzzle world : ({:.3f}, {:.3f}, {:.3f})\n",
               after.x,
               after.y,
               after.z);
    fmt::print("    tank followed hull  : {}\n", followed ? "yes" : "NO");
    return composed && followed;
}

} // namespace

int main() {
    JobSystem jobs;
    fmt::print("Rime ECS playground — the M4 proof (samples/05-ecs-playground)\n");
    fmt::print("  hardware_concurrency : {}\n\n", std::thread::hardware_concurrency());

    const bool parallel_ok = run_parallel_field(jobs);
    fmt::print("\n");
    const bool compose_ok = run_transform_hierarchy(jobs);

    const bool ok = parallel_ok && compose_ok;
    fmt::print("\nM4 \"done when\": {}\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
