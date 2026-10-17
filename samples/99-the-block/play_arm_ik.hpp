// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/assets/mesh_asset.hpp"
#include "rime/assets/skeleton_asset.hpp"
#include "rime/render/mesh.hpp"

namespace rime::block_play {

// Sample-owned CPU deformation: the renderer receives ordinary vertices, and has no knowledge
// of weapon grips. All inputs and outputs are in camera-local metres; left arm is slot zero.
class PlayArmIk {
public:
    struct Stats {
        std::array<float, 2> wrist_position_error{};
        std::array<float, 2> wrist_orientation_error{}; // radians
        std::uint32_t clamped_targets = 0;
        float max_vertex_displacement = 0.0f; // from immutable bind mesh, metres
        bool valid_input = true;
    };

    [[nodiscard]] bool initialize(const rime::assets::MeshAsset& mesh,
                                  const rime::assets::Skeleton& skeleton);
    // Pole POINTS are camera-local, so a moving gun does not drag the shoulders or elbow plane.
    // Bad/non-rigid targets leave the previous finite deformation intact and set valid_input=false.
    [[nodiscard]] std::span<const rime::render::MeshVertex>
    solve(const rime::core::Mat4& camera_from_weapon,
          const std::array<rime::core::Mat4, 2>& weapon_from_wrists,
          const std::array<rime::core::Vec3, 2>& elbow_poles) noexcept;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

    [[nodiscard]] const rime::render::CpuMesh& bind_mesh() const noexcept { return bind_mesh_; }

    [[nodiscard]] const std::array<rime::core::Mat4, 2>& bind_wrists() const noexcept {
        return bind_wrists_;
    }

    [[nodiscard]] std::span<const rime::core::Mat4> joint_worlds() const noexcept { return world_; }

    // Rest-pose anchors let presentation place a torso independently of the moving weapon.
    // Elbow poles choose the bend plane; they cannot move these shoulder joints.
    [[nodiscard]] std::array<rime::core::Vec3, 2> bind_shoulders() const noexcept {
        if (!initialized_)
            return {};
        return {rime::core::transform_point(bind_world_[arms_[0].joints[0]], {}),
                rime::core::transform_point(bind_world_[arms_[1].joints[0]], {})};
    }

private:
    struct Influence {
        std::array<std::uint16_t, 4> joints{};
        std::array<float, 4> weights{};
    };

    struct Arm {
        std::array<std::size_t, 3> joints{}; // shoulder, elbow, wrist: joint heads define lengths
        float upper_length = 0.0f;
        float lower_length = 0.0f;
        rime::core::Vec3 bind_bend{};
    };

    bool initialized_ = false;
    Stats stats_{};
    rime::assets::Skeleton skeleton_;
    rime::render::CpuMesh bind_mesh_;
    std::vector<Influence> influences_;
    std::vector<rime::render::MeshVertex> vertices_;
    std::vector<rime::core::Mat4> bind_world_, local_bind_, world_, palette_;
    std::array<Arm, 2> arms_{};
    std::array<rime::core::Mat4, 2> bind_wrists_{};
};

} // namespace rime::block_play
