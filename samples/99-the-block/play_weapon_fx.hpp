// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>

#include "rime/core/math/transform.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"

namespace rime::ecs {
class World;
}

namespace rime::block_play {

// Presentation of ACCEPTED shots, not a projectile/damage simulation. Ordinary world geometry
// is seen by both parent and optical cameras. The world owns one fixed pool for its lifetime.
class PlayWeaponFx {
public:
    static constexpr std::size_t kSlots = 8;
    static constexpr float kFlashLifetime = 0.04f;
    static constexpr float kTracerLifetime = 0.06f;
    static constexpr float kSparkLifetime = 0.12f;

    PlayWeaponFx(rhi::Device&,
                 render::MeshRegistry& meshes,
                 render::MaterialRegistry& materials) noexcept;
    PlayWeaponFx(const PlayWeaponFx&) = delete;
    PlayWeaponFx& operator=(const PlayWeaponFx&) = delete;
    // One attempt only: two meshes and three materials, with no per-shot residency changes.
    [[nodiscard]] bool initialize();
    void fire(core::Vec3 eye,
              core::Vec3 direction,
              core::Vec3 muzzle,
              core::Vec3 hit,
              bool hit_surface) noexcept;
    void advance(float dt) noexcept;
    // First call binds ONE world and creates 24 entities. Later calls only write components.
    // Call before rendering either camera; advance() ages a new shot after its first pose.
    void pose(ecs::World& world);

private:
    struct Shot {
        core::Vec3 direction{}, muzzle{}, hit{}, spark{};
        float age = kSparkLifetime;
        bool hit_surface = false;
        bool presented = true;
        std::array<ecs::Entity, 3> entities{};
    };

    void show(ecs::World& world,
              ecs::Entity entity,
              render::MeshId mesh,
              const core::Transform& transform);
    render::MeshRegistry& meshes_;
    render::MaterialRegistry& materials_;
    std::array<Shot, kSlots> shots_{};
    std::array<render::MaterialId, 3> material_ids_{};
    render::MeshId burst_ = render::kInvalidMeshId, segment_ = render::kInvalidMeshId;
    ecs::World* world_ = nullptr; // identity check only; never dereferenced by destruction
    std::size_t next_ = 0;
    bool attempted_ = false, ready_ = false;
};

} // namespace rime::block_play
