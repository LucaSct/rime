// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string_view>

#include "play_arm_ik.hpp"
#include "rime/core/hash.hpp"

using namespace rime;

namespace {

core::Vec3 position(const core::Mat4& m) {
    return core::transform_point(m, {});
}

core::Vec3 position(const render::MeshVertex& v) {
    return {v.px, v.py, v.pz};
}

struct Fixture {
    assets::Skeleton skeleton;
    assets::MeshAsset mesh;
    std::array<core::Vec3, 2> poles{{{-1, -1, -0.3f}, {1, -1, -0.3f}}};

    // Two 0.4 m links per arm, plus a finger whose local pose must follow the wrist.
    Fixture() {
        constexpr std::array<std::array<std::string_view, 4>, 2> names{
            {{"L_arm_01", "L_elbow_02", "L_wrist_03", "L_finger"},
             {"R_arm_023", "R_elbow_024", "R_wrist_025", "R_finger"}}};
        for (std::size_t side = 0; side < 2; ++side) {
            const float sign = side == 0 ? -1.0f : 1.0f;
            for (std::size_t k = 0; k < 4; ++k) {
                assets::Joint j;
                j.name_hash = core::fnv1a_64(names[side][k]);
                j.parent = k == 0 ? -1 : static_cast<std::int32_t>(side * 4 + k - 1);
                j.local_bind.translation = k == 0   ? core::Vec3{sign * 0.25f, 0, 0}
                                           : k == 1 ? core::Vec3{sign * 0.24f, 0, -0.32f}
                                           : k == 2 ? core::Vec3{-sign * 0.24f, 0, -0.32f}
                                                    : core::Vec3{0, 0, -0.06f};
                auto world = core::to_matrix(j.local_bind);
                if (j.parent >= 0) {
                    world = core::inverse(
                                skeleton.joints[static_cast<std::size_t>(j.parent)].inverse_bind) *
                            world;
                }
                j.inverse_bind = core::inverse(world);
                skeleton.joints.push_back(j);
            }
        }
        mesh.attribs = assets::VertexAttribs::Position | assets::VertexAttribs::Normal |
                       assets::VertexAttribs::Uv | assets::VertexAttribs::Tangent |
                       assets::VertexAttribs::Joints | assets::VertexAttribs::Weights;
        mesh.vertex_stride = assets::expected_vertex_stride(mesh.attribs);
        mesh.vertex_count = 6;
        mesh.vertices.resize(static_cast<std::size_t>(mesh.vertex_count) * mesh.vertex_stride);
        mesh.indices = {0, 1, 2, 3, 4, 5};
        for (std::size_t side = 0; side < 2; ++side) {
            const auto wrist = position(core::inverse(skeleton.joints[side * 4 + 2].inverse_bind));
            for (std::size_t k = 0; k < 3; ++k) {
                const auto index = side * 3 + k;
                const auto p = wrist + core::Vec3{0.02f * static_cast<float>(k), 0.01f, -0.03f};
                const std::array<float, 12> vertex{
                    {p.x, p.y, p.z, 0, 1, 0, 0.3f, 0.7f, 1, 0, 0, -1}};
                const std::array<std::uint16_t, 4> joints{{static_cast<std::uint16_t>(side * 4 + 2),
                                                           static_cast<std::uint16_t>(side * 4 + 1),
                                                           0,
                                                           0}};
                // Deliberately unnormalized: the imported sum must not scale the vertex.
                const std::array<float, 4> weights{{3, 1, 0, 0}};
                auto* data = mesh.vertices.data() + index * mesh.vertex_stride;
                std::memcpy(data, vertex.data(), sizeof(vertex));
                std::memcpy(data + 48, joints.data(), sizeof(joints));
                std::memcpy(data + 56, weights.data(), sizeof(weights));
            }
        }
    }
};

void check_lengths(const block_play::PlayArmIk& ik) {
    const auto worlds = ik.joint_worlds();
    for (std::size_t side = 0; side < 2; ++side) {
        const auto offset = side * 4;
        CHECK(core::length(position(worlds[offset + 1]) - position(worlds[offset])) ==
              doctest::Approx(0.4f).epsilon(1e-4));
        CHECK(core::length(position(worlds[offset + 2]) - position(worlds[offset + 1])) ==
              doctest::Approx(0.4f).epsilon(1e-4));
    }
}
} // namespace

TEST_CASE("runtime arm IK keeps both palms on a moving grip without moving shoulders") {
    Fixture f;
    block_play::PlayArmIk ik;
    REQUIRE(ik.initialize(f.mesh, f.skeleton));
    const auto grips = ik.bind_wrists();
    for (int step = -8; step <= 8; ++step) {
        const float phase = static_cast<float>(step) * 0.05f;
        const auto gun = core::mat4_translation({phase * 0.15f, phase * 0.08f, phase * 0.1f}) *
                         core::to_mat4(core::quat_from_axis_angle({0, 0, 1}, phase));
        const auto vertices = ik.solve(gun, grips, f.poles);
        REQUIRE(vertices.size() == f.mesh.vertex_count);
        REQUIRE(ik.stats().valid_input);
        CHECK(ik.stats().clamped_targets == 0);
        check_lengths(ik);
        for (std::size_t side = 0; side < 2; ++side) {
            const auto worlds = ik.joint_worlds();
            const auto target = gun * grips[side];
            CHECK(core::length(position(worlds[side * 4 + 2]) - position(target)) < 1e-5f);
            CHECK(ik.stats().wrist_position_error[side] < 1e-5f);
            CHECK(ik.stats().wrist_orientation_error[side] < 1e-3f);
            for (const core::Vec3 basis :
                 {core::Vec3{1, 0, 0}, core::Vec3{0, 1, 0}, core::Vec3{0, 0, 1}}) {
                CHECK(core::length(core::transform_vector(worlds[side * 4 + 2], basis) -
                                   core::transform_vector(target, basis)) < 1e-5f);
            }
            CHECK(core::approx_eq(
                position(worlds[side * 4]), {side == 0 ? -0.25f : 0.25f, 0, 0}, 1e-6f));
            // A finger's authored local rest pose survives wrist rotation.
            CHECK(core::approx_eq(worlds[side * 4 + 3],
                                  target *
                                      core::to_matrix(f.skeleton.joints[side * 4 + 3].local_bind),
                                  1e-5f));
        }
    }
}

TEST_CASE("runtime arm IK deforms weighted vertices and preserves their surface data") {
    Fixture f;
    block_play::PlayArmIk ik;
    REQUIRE(ik.initialize(f.mesh, f.skeleton));
    const auto grips = ik.bind_wrists();
    const auto bind = ik.bind_mesh().vertices;
    const auto out = ik.solve(core::mat4_translation({0, 0.05f, 0.04f}), grips, f.poles);
    REQUIRE(ik.stats().max_vertex_displacement > 0.02f);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto side = i / 3;
        const auto worlds = ik.joint_worlds();
        const auto wrist_palette =
            worlds[side * 4 + 2] * f.skeleton.joints[side * 4 + 2].inverse_bind;
        const auto elbow_palette =
            worlds[side * 4 + 1] * f.skeleton.joints[side * 4 + 1].inverse_bind;
        const auto p = position(bind[i]);
        // An independent barycentric check catches implementations that move only one bone or
        // report solved joints while handing unchanged vertices to the renderer.
        const auto expected = core::transform_point(wrist_palette, p) * 0.75f +
                              core::transform_point(elbow_palette, p) * 0.25f;
        CHECK(core::length(position(out[i]) - expected) < 1e-5f);
        CHECK(core::length(position(out[i]) - p) > 0.01f);
        CHECK(core::length(core::Vec3{out[i].nx, out[i].ny, out[i].nz}) == doctest::Approx(1.0f));
        CHECK(core::length(core::Vec3{out[i].tx, out[i].ty, out[i].tz}) == doctest::Approx(1.0f));
        CHECK(out[i].tw == -1.0f);
        CHECK(out[i].u == bind[i].u);
        CHECK(out[i].v == bind[i].v);
    }
}

TEST_CASE(
    "runtime arm IK preserves mirrored elbow poles and handles degenerate targets explicitly") {
    Fixture f;
    block_play::PlayArmIk ik;
    REQUIRE(ik.initialize(f.mesh, f.skeleton));
    auto grips = ik.bind_wrists();
    (void)ik.solve({}, grips, f.poles);
    auto worlds = ik.joint_worlds();
    CHECK(position(worlds[1]).x < position(worlds[0]).x);
    CHECK(position(worlds[5]).x > position(worlds[4]).x);
    CHECK(position(worlds[1]).y < 0);
    CHECK(position(worlds[5]).y < 0);
    // A wrist at its shoulder and a wrist far outside its reach cannot both be hit without
    // stretching. They must be finite, flagged misses with the original segment lengths.
    grips[0] = core::mat4_translation({-0.25f, 0, 0});
    grips[1] = core::mat4_translation({10, 0, -10});
    const auto out = ik.solve({}, grips, {{{-0.25f, 0, 0}, {10, 0, -10}}});
    CHECK(ik.stats().valid_input);
    CHECK(ik.stats().clamped_targets == 2);
    CHECK(ik.stats().wrist_position_error[1] > 5.0f);
    check_lengths(ik);
    for (const auto& v : out) {
        CHECK(std::isfinite(v.px));
        CHECK(std::isfinite(v.py));
        CHECK(std::isfinite(v.pz));
    }
    // Exact reversal exercises the 180 degree quaternion branch without changing link lengths.
    grips[0] = core::mat4_translation({-0.25f, 0, 0.64f});
    grips[1] = core::mat4_translation({0.25f, 0, 0.64f});
    (void)ik.solve({}, grips, {{{-1, 0, 0}, {1, 0, 0}}});
    CHECK(ik.stats().valid_input);
    check_lengths(ik);
}

TEST_CASE("runtime arm IK rejects corrupt skin data and incomplete rigs") {
    Fixture f;
    block_play::PlayArmIk ik;
    REQUIRE(ik.initialize(f.mesh, f.skeleton));
    SUBCASE("missing arm") {
        f.skeleton.joints[0].name_hash = 0;
    }
    SUBCASE("invalid inverse bind") {
        f.skeleton.joints[0].inverse_bind.m[0] = 0;
    }
    SUBCASE("nonfinite rig") {
        f.skeleton.joints[0].local_bind.translation.x = std::numeric_limits<float>::infinity();
    }
    SUBCASE("missing weights") {
        f.mesh.attribs = assets::VertexAttribs::Position;
    }
    SUBCASE("short blob") {
        f.mesh.vertices.pop_back();
    }
    SUBCASE("out of range joint even at zero weight") {
        const std::uint16_t bad = 65535;
        std::memcpy(f.mesh.vertices.data() + 52, &bad, sizeof(bad));
    }
    SUBCASE("negative weight") {
        const float bad = -1;
        std::memcpy(f.mesh.vertices.data() + 56, &bad, sizeof(bad));
    }
    SUBCASE("nonfinite weight") {
        const float bad = std::numeric_limits<float>::quiet_NaN();
        std::memcpy(f.mesh.vertices.data() + 56, &bad, sizeof(bad));
    }
    SUBCASE("zero weight sum") {
        std::array<float, 4> zero{};
        std::memcpy(f.mesh.vertices.data() + 56, zero.data(), sizeof(zero));
    }
    CHECK_FALSE(ik.initialize(f.mesh, f.skeleton));
    CHECK_FALSE(ik.initialized());
    CHECK(ik.solve({}, {}, f.poles).empty());
}

TEST_CASE("runtime arm IK keeps the last finite pose on invalid runtime input") {
    Fixture f;
    block_play::PlayArmIk ik;
    REQUIRE(ik.initialize(f.mesh, f.skeleton));
    const auto good = ik.solve({}, ik.bind_wrists(), f.poles);
    const auto saved = position(good.front());
    auto bad = core::Mat4{};
    bad.m[12] = std::numeric_limits<float>::quiet_NaN();
    const auto out = ik.solve(bad, ik.bind_wrists(), f.poles);
    CHECK_FALSE(ik.stats().valid_input);
    CHECK(core::approx_eq(position(out.front()), saved));
}
