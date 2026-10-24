// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <fmt/core.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <memory>

#include "rime/core/jobs/job_system.hpp"
#include "rime/physics/world.hpp"

namespace fixed_contact_world {
namespace physics = rime::physics;
namespace core = rime::core;
using core::Vec3;

// Frozen contact timing: build the same mixed hull/box/compound pile for each process, then
// inspect it repeatedly without integrating. This exercises production broadphase, bounds,
// parallel dispatch, canonical merge and warm-cache reads on identical input every repetition.
// A separate evolving phase prints EVERY step's world_hash, outside the timed region, so a
// faster contact pass cannot hide a changed trajectory behind a single final checkpoint.
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

bool build_world(physics::PhysicsWorld& world) {
    world.set_sleeping_enabled(false);
    const physics::HullId hull = register_hull(world);
    if (!hull.is_valid())
        return false;
    physics::HullInfo hull_info;
    if (!world.hull_info(hull, hull_info) ||
        (std::abs(hull_info.principal_rotation.x) + std::abs(hull_info.principal_rotation.y) +
         std::abs(hull_info.principal_rotation.z)) < 0.01f)
        return false;

    const auto side = static_cast<std::uint32_t>(std::ceil(std::sqrt(448u / 7.0)));
    const float extent = static_cast<float>(side) + 2.0f;
    physics::BodyDesc floor;
    floor.motion = physics::MotionType::Static;
    floor.shape.type = physics::ShapeType::Box;
    floor.shape.half_extents = {extent, 0.5f, extent};
    floor.position = {0.0f, -0.5f, 0.0f};
    if (!world.create_body(floor).is_valid())
        return false;

    const std::array<physics::CompoundChildDesc, 2> children{
        {{physics::ShapeDesc{.type = physics::ShapeType::Box, .half_extents = {0.25f, 0.5f, 0.5f}},
          {-0.25f, 0, 0},
          core::quat_identity()},
         {physics::ShapeDesc{.type = physics::ShapeType::Box, .half_extents = {0.25f, 0.5f, 0.5f}},
          {0.25f, 0, 0},
          core::quat_identity()}}};
    const auto compound = world.register_compound({children});
    if (!compound.is_valid())
        return false;
    std::uint32_t random = 0x51A7E123u;
    constexpr float pitch = 0.995f; // 5 mm overlap between 1 m bodies, near penetration slop.
    for (std::uint32_t i = 0; i < 448u; ++i) {
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
        if (i % 11u == 0) {
            body.shape.type = physics::ShapeType::Compound;
            body.shape.compound = compound;
        }
        body.ccd = i % 7u == 0;
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

inline int run(int argc, char** argv) {
    unsigned workers = 1;
    bool profile = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--profile") {
            profile = true;
            continue;
        }
        if (arg != "--workers" || i + 1 >= argc)
            return 2;
        const std::string_view value = argv[++i];
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), workers);
        if (ec != std::errc{} || end != value.data() + value.size() || workers > 8)
            return 2;
    }
    std::unique_ptr<core::JobSystem> jobs;
    if (workers != 0)
        jobs = std::make_unique<core::JobSystem>(workers);
    physics::PhysicsWorld world;
    if (!build_world(world))
        return 1;
    world.set_job_system(jobs.get());
    world.set_contact_profile_enabled(profile);
    std::vector<physics::Manifold> contacts;
    world.compute_contacts(contacts);
    const auto count = contacts.size();
    std::array<double, 7> times{};
    for (auto& ms : times) {
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 20; ++i)
            world.compute_contacts(contacts);
        ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                 .count() /
             20;
        if (contacts.size() != count)
            return 1;
    }
    std::sort(times.begin(), times.end());
    fmt::print("{{\"workers\":{},\"contacts\":{},\"contact_ms\":{{\"min\":{},\"median\":{},\"max\":"
               "{}}},\"hashes\":[",
               workers,
               count,
               times.front(),
               times[3],
               times.back());
    for (unsigned step = 0; step < 120; ++step) {
        world.step(1.0f / 60.0f);
        fmt::print("{}\"{:016x}\"", step ? "," : "", world.world_hash());
    }
    fmt::print("]}}\n");
    return 0;
}
} // namespace fixed_contact_world
