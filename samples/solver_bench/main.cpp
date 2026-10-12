// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// solver_bench — what a contact solve COSTS, measured where chaos cannot reach it. GPU-free.
//
// WHY THIS EXISTS. `99-the-block` is the project's physics budget workload, and it cannot price a
// solver change that moves floating-point rounding. Measured 2026-10-12 (docs/perf/README.md): a
// perturbation that is mathematically a NO-OP — normalising an already-unit quaternion inside a
// solver cache — reshaped the collapse enough to fail 2 of that sample's 29 scripted-beat claims,
// and a different non-bit-identical variant failed 2 OTHER claims and moved the WORKLOAD with it,
// 22 % fewer bodies and 31 % fewer broadphase pairs. Fracture is threshold-driven, so the last bit
// decides which parts break, and from there the two runs are simulating different buildings. An A/B
// across them compares two scenes, not two solvers.
//
// THE ONE IDEA. A measurement is comparable only if it starts from a state that is identical under
// any solver. That is STEP 1 from a hard-placed pile: identical initial poses give identical
// broadphase pairs and identical manifolds, by construction, whatever the solver then does. So
// step 1 is the headline here and everything else is context. The step series that follows is
// printed for eyeballing, and diverges as soon as the solver has had one step to disagree.
//
// HOW THE PILE IS BUILT. A lattice of 1 m bodies dropped onto a static floor with a deliberate 5 mm
// overlap — near the penetration slop — so the very first narrowphase yields thousands of contacts
// rather than a settling transient. Nothing settles; nothing sleeps (an island that slept would
// stop being solved); the job system is left unset, because #312 measured that a single connected
// island is the whole solve and island-level parallelism has a 1.000x ceiling on it. 40 % of the
// bodies are sheared convex hulls on purpose: a hull's `inertia_principal` is a non-identity
// rotation where a primitive's is identity, and the world-inertia path is exactly what this bench
// is for — an all-primitive pile would time the wrong code. The build asserts the hull really is
// off-diagonal, and the run asserts step 1's workload is identical across repeats, because a bench
// that silently measures a different pile each time is worse than no bench.
//
// WHAT IT CANNOT DO. It does not make the sample's 29 claims robust, so it prices a
// non-bit-identical solver change without judging whether the collapse that change produces is
// acceptable. That remains a separate decision. And `world_hash` is printed at step 1 and at the
// end precisely so a bit-identical change is VISIBLY bit-identical here too.

#include <fmt/core.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

#include "rime/physics/world.hpp"

namespace {

using rime::core::Vec3;
namespace physics = rime::physics;

struct Options {
    std::uint32_t bodies = 448;
    std::uint32_t steps = 32;
    std::uint32_t repeats = 5;
    std::uint32_t seed = 0x51A7E123u;
    bool json_only = false;
};

struct Step {
    std::uint32_t manifolds = 0;
    std::uint32_t active_islands = 0;
    physics::IslandSolveCost largest{};
};

struct Spread {
    double median = 0.0;
    double min = 0.0;
    double max = 0.0;
};

std::uint32_t next_random(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    return state;
}

float small_offset(std::uint32_t& state) {
    return (static_cast<float>(next_random(state) >> 8u) / 16777215.0f - 0.5f) * 0.002f;
}

physics::HullId register_hull(physics::PhysicsWorld& world) {
    // A sheared cuboid has off-diagonal inertia, hence a non-identity principal rotation.
    std::array<Vec3, 8> vertices{};
    for (std::uint32_t i = 0; i < vertices.size(); ++i) {
        const float x = (i & 1u) != 0 ? 0.5f : -0.5f;
        const float y = (i & 2u) != 0 ? 0.5f : -0.5f;
        const float z = (i & 4u) != 0 ? 0.5f : -0.5f;
        vertices[i] = {x + 0.12f * y, y, z};
    }
    constexpr std::array<std::uint32_t, 6> counts{4, 4, 4, 4, 4, 4};
    constexpr std::array<std::uint32_t, 24> indices{0, 4, 6, 2, 1, 3, 7, 5, 0, 1, 5, 4,
                                                    2, 6, 7, 3, 0, 2, 3, 1, 4, 5, 7, 6};
    return world.register_hull({vertices, counts, indices});
}

bool build_world(physics::PhysicsWorld& world, const Options& options) {
    world.set_sleeping_enabled(false);
    world.set_island_profile_enabled(true);
    // One connected island cannot be split by island-level parallelism; keep the job system unset.
    const physics::HullId hull = register_hull(world);
    if (!hull.is_valid())
        return false;
    physics::HullInfo hull_info;
    if (!world.hull_info(hull, hull_info) ||
        (std::abs(hull_info.principal_rotation.x) + std::abs(hull_info.principal_rotation.y) +
         std::abs(hull_info.principal_rotation.z)) < 0.01f)
        return false;

    const auto side = static_cast<std::uint32_t>(std::ceil(std::sqrt(options.bodies / 7.0)));
    const float extent = static_cast<float>(side) + 2.0f;
    physics::BodyDesc floor;
    floor.motion = physics::MotionType::Static;
    floor.shape.type = physics::ShapeType::Box;
    floor.shape.half_extents = {extent, 0.5f, extent};
    floor.position = {0.0f, -0.5f, 0.0f};
    if (!world.create_body(floor).is_valid())
        return false;

    std::uint32_t random = options.seed;
    constexpr float pitch = 0.995f; // 5 mm overlap between 1 m bodies, near penetration slop.
    for (std::uint32_t i = 0; i < options.bodies; ++i) {
        const std::uint32_t column = i % (side * side);
        const std::uint32_t layer = i / (side * side);
        physics::BodyDesc body;
        if (i % 5u < 2u) {
            body.shape.type = physics::ShapeType::ConvexHull;
            body.shape.hull = hull;
        } else {
            body.shape.type = physics::ShapeType::Box;
            body.shape.half_extents = {0.5f, 0.5f, 0.5f};
        }
        body.position = {
            (static_cast<float>(column % side) - 0.5f * static_cast<float>(side - 1)) * pitch +
                small_offset(random),
            0.5f + static_cast<float>(layer) * pitch + small_offset(random),
            (static_cast<float>(column / side) - 0.5f * static_cast<float>(side - 1)) * pitch +
                small_offset(random)};
        body.orientation =
            rime::core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, small_offset(random));
        if (!world.create_body(body).is_valid())
            return false;
    }
    return true;
}

Spread spread(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    return {values[values.size() / 2], values.front(), values.back()};
}

void print_spread(const char* name, const Spread& s) {
    fmt::print("{}: {:.3f} (min {:.3f}, max {:.3f})\n", name, s.median, s.min, s.max);
}

void json_spread(const char* name, const Spread& s) {
    fmt::print(
        "\"{}\":{{\"median\":{:.6f},\"min\":{:.6f},\"max\":{:.6f}}}", name, s.median, s.min, s.max);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--json-only") {
            options.json_only = true;
            continue;
        }
        if (i + 1 >= argc) {
            fmt::print(stderr, "missing value for {}\n", arg);
            return 1;
        }
        char* end = nullptr;
        const auto value = std::strtoull(argv[++i], &end, 10);
        if (*end != '\0' || (value == 0 && arg != "--seed") || value > UINT32_MAX) {
            fmt::print(stderr, "invalid value for {}\n", arg);
            return 1;
        }
        if (arg == "--bodies")
            options.bodies = static_cast<std::uint32_t>(value);
        else if (arg == "--steps")
            options.steps = static_cast<std::uint32_t>(value);
        else if (arg == "--repeats")
            options.repeats = static_cast<std::uint32_t>(value);
        else if (arg == "--seed")
            options.seed = static_cast<std::uint32_t>(value);
        else {
            fmt::print(stderr, "unknown option: {}\n", arg);
            return 1;
        }
    }

    std::vector<std::vector<Step>> all_series;
    all_series.reserve(options.repeats);
    std::vector<double> solves, velocities, per_constraint, per_iteration;
    std::uint64_t first_hash = 0, last_hash = 0;
    std::uint32_t first_bodies = 0, first_constraints = 0, first_manifolds = 0;
    for (std::uint32_t repeat = 0; repeat < options.repeats; ++repeat) {
        physics::PhysicsWorld world;
        if (!build_world(world, options)) {
            fmt::print(stderr, "solver_bench: construction failed\n");
            return 1;
        }
        std::vector<Step> run_series;
        run_series.reserve(options.steps);
        for (std::uint32_t step = 0; step < options.steps; ++step) {
            world.step(1.0f / 60.0f);
            Step row;
            const auto stats = world.stats();
            row.manifolds = stats.manifolds;
            row.active_islands = stats.active_islands;
            for (const auto& island : world.island_solve_profile_last()) {
                if (island.solve_ns > row.largest.solve_ns)
                    row.largest = island;
            }
            if (step == 0) {
                if (repeat == 0) {
                    first_bodies = row.largest.bodies;
                    first_constraints = row.largest.constraints;
                    first_manifolds = row.manifolds;
                    first_hash = world.world_hash();
                } else if (row.largest.bodies != first_bodies ||
                           row.largest.constraints != first_constraints ||
                           row.manifolds != first_manifolds) {
                    fmt::print(stderr, "solver_bench: step-1 workload changed across repeats\n");
                    return 1;
                }
                const double solve_us = static_cast<double>(row.largest.solve_ns) / 1000.0;
                const double velocity_us = static_cast<double>(row.largest.velocity_ns) / 1000.0;
                solves.push_back(solve_us);
                velocities.push_back(velocity_us);
                per_constraint.push_back(
                    row.largest.constraints != 0 ? velocity_us / row.largest.constraints : 0.0);
                // 8 is kVelocityIterations in the private solver header; change both together.
                per_iteration.push_back(velocity_us / 8.0);
            }
            run_series.push_back(row);
        }
        if (repeat == 0)
            last_hash = world.world_hash();
        all_series.push_back(std::move(run_series));
    }

    const auto& series = all_series.front();
    if (first_bodies < 300 || first_constraints < 1000 || first_manifolds < 1000 ||
        series.front().active_islands != 1) {
        fmt::print(stderr,
                   "solver_bench: missed step-1 target: largest island {} bodies / {} "
                   "constraints, {} manifolds, {} active islands\n",
                   first_bodies,
                   first_constraints,
                   first_manifolds,
                   series.front().active_islands);
        return 1;
    }
    const Spread solve = spread(solves), velocity = spread(velocities);
    const Spread constraint = spread(per_constraint), iteration = spread(per_iteration);
    if (!options.json_only) {
        fmt::print("step 1: {} bodies, {} constraints (manifolds), {} total manifolds\n",
                   first_bodies,
                   first_constraints,
                   first_manifolds);
        print_spread("solve_us", solve);
        print_spread("velocity_us", velocity);
        print_spread("us per constraint", constraint);
        print_spread("us per velocity iteration", iteration);
        fmt::print("step manifolds islands bodies constraints solve_us warm_us velocity_us "
                   "store_us position_us\n");
        for (std::size_t i = 0; i < series.size(); ++i) {
            const auto& s = series[i];
            const auto& c = s.largest;
            fmt::print("{} {} {} {} {} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}\n",
                       i + 1,
                       s.manifolds,
                       s.active_islands,
                       c.bodies,
                       c.constraints,
                       c.solve_ns / 1000.0,
                       c.warm_ns / 1000.0,
                       c.velocity_ns / 1000.0,
                       c.store_ns / 1000.0,
                       c.position_ns / 1000.0);
        }
        fmt::print("world_hash step 1: {:#018x}; last: {:#018x}\n", first_hash, last_hash);
    }
    fmt::print("solver_bench_json: {{\"bodies_requested\":{},\"steps\":{},\"repeats\":{},"
               "\"seed\":{},\"step1\":{{\"bodies\":{},\"constraints\":{},"
               "\"manifolds\":{},\"active_islands\":{},",
               options.bodies,
               options.steps,
               options.repeats,
               options.seed,
               first_bodies,
               first_constraints,
               first_manifolds,
               series.front().active_islands);
    json_spread("solve_us", solve);
    fmt::print(",");
    json_spread("velocity_us", velocity);
    fmt::print(",");
    json_spread("us_per_constraint", constraint);
    fmt::print(",");
    json_spread("us_per_velocity_iteration", iteration);
    fmt::print("}},\"series\":[");
    for (std::size_t i = 0; i < series.size(); ++i) {
        const auto& s = series[i];
        const auto& c = s.largest;
        fmt::print("{}{{\"step\":{},\"manifolds\":{},\"active_islands\":{},"
                   "\"bodies\":{},\"constraints\":{},\"solve_ns\":{},\"warm_ns\":{},"
                   "\"velocity_ns\":{},\"store_ns\":{},\"position_ns\":{}}}",
                   i == 0 ? "" : ",",
                   i + 1,
                   s.manifolds,
                   s.active_islands,
                   c.bodies,
                   c.constraints,
                   c.solve_ns,
                   c.warm_ns,
                   c.velocity_ns,
                   c.store_ns,
                   c.position_ns);
    }
    fmt::print("],\"world_hash_step1\":{},\"world_hash_last\":{}}}\n", first_hash, last_hash);
}
