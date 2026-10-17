// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>

#include "rime/core/math/transform.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/physics/body.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"

namespace rime::physics {
class PhysicsWorld;
}

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
    // 48 marks is the pool, 20 s is the age at which one expires -- and the pool is the binding
    // limit, not the clock. A held trigger at 600 RPM lays down 10 marks a second, so sustained
    // fire recycles the oldest slot after about 4.8 s and the 20 s figure is only reachable while
    // shooting in bursts. That is the intended trade: a fixed pool that never grows, where the
    // scars you are looking at are always the most recent ones.
    static constexpr std::size_t kImpactSlots = 48;
    static constexpr float kImpactLifetime = 20.0f;
    static constexpr std::size_t kEntityCount = kSlots * 5 + kImpactSlots;

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
              bool hit_surface,
              core::Vec3 normal = {},
              physics::BodyId body = {},
              const physics::PhysicsWorld* physics = nullptr) noexcept;
    void advance(float dt) noexcept;
    // First call binds ONE world and creates kEntityCount entities -- 8 shot slots x (flash,
    // bullet, spark, muzzle light, impact light) plus 48 surface marks, 88 in all -- and every
    // later call only writes components. Nothing is spawned or despawned per shot, so a held
    // trigger at 600 RPM cannot grow GPU residency; an effect that is over is a mesh set to
    // kInvalidMeshId or a light set to zero intensity, not a destroyed entity.
    // Call before rendering either camera; advance() ages a new shot after its first pose.
    void pose(ecs::World& world, const physics::PhysicsWorld* physics = nullptr);

private:
    struct Shot {
        core::Vec3 direction{}, muzzle{}, hit{}, spark{};
        float age = kSparkLifetime;
        bool hit_surface = false;
        bool presented = true;
        std::array<ecs::Entity, 5> entities{};
    };

    struct Impact {
        core::Vec3 local_point{}, local_normal{};
        physics::BodyId body{};
        ecs::Entity entity{};
        float age = kImpactLifetime;
    };

    void show(ecs::World& world,
              ecs::Entity entity,
              render::MeshId mesh,
              const core::Transform& transform);
    render::MeshRegistry& meshes_;
    render::MaterialRegistry& materials_;
    std::array<Shot, kSlots> shots_{};
    std::array<render::MaterialId, 4> material_ids_{};
    std::array<Impact, kImpactSlots> impacts_{};
    render::MeshId burst_ = render::kInvalidMeshId;
    ecs::World* world_ = nullptr; // identity check only; never dereferenced by destruction
    std::size_t next_ = 0;
    std::size_t next_impact_ = 0;
    bool attempted_ = false, ready_ = false;
};

} // namespace rime::block_play
