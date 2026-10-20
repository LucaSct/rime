// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

#include "rime/core/math/transform.hpp"
#include "rime/ecs/entity.hpp"
#include "rime/physics/body.hpp"
#include "rime/render/fx_pass.hpp"
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
    enum class Surface : std::uint8_t { Concrete, Brick, Plaster, Soil, Metal, Water, Wood, Count };
    static constexpr std::size_t kSlots = 8;
    static constexpr float kFlashLifetime = 0.04f;
    static constexpr float kTracerLifetime = 0.18f;
    static constexpr float kSparkLifetime = 0.12f;
    // 48 marks is the pool, 20 s is the age at which one expires -- and the pool is the binding
    // limit, not the clock. A held trigger at 600 RPM lays down 10 marks a second, so sustained
    // fire recycles the oldest slot after about 4.8 s and the 20 s figure is only reachable while
    // shooting in bursts. That is the intended trade: a fixed pool that never grows, where the
    // scars you are looking at are always the most recent ones.
    static constexpr std::size_t kImpactSlots = 48;
    static constexpr float kImpactLifetime = 20.0f;
    static constexpr std::size_t kCaseSlots = 32;
    static constexpr float kCaseLifetime = 3.0f;
    static constexpr std::size_t kFragmentSlots = 96;
    // Each masonry hit puffs kDustPerHit billboards that live kDustLifetime seconds, so 192 slots
    // hold the last ~16 hits -- ample for a burst at 600 RPM (about 1.5 s of fire).
    static constexpr std::size_t kDustSlots = 200;
    static constexpr unsigned kDustPerHit = 12;
    static constexpr float kDustLifetime = 1.4f;
    static constexpr std::size_t kEntityCount =
        kSlots * 6 + kImpactSlots + kCaseSlots + kFragmentSlots;

    struct ViewPose {
        core::Vec3 eye{};
        core::Vec3 direction{0, 0, -1};
        core::Vec3 muzzle{};
        core::Vec3 ejection_point{};
        core::Vec3 velocity{};
    };

    PlayWeaponFx(rhi::Device&,
                 render::MeshRegistry& meshes,
                 render::MaterialRegistry& materials) noexcept;
    ~PlayWeaponFx();
    PlayWeaponFx(const PlayWeaponFx&) = delete;
    PlayWeaponFx& operator=(const PlayWeaponFx&) = delete;
    // One attempt only: burst and brass meshes plus fixed materials; no per-shot uploads.
    [[nodiscard]] bool initialize();
    void fire(core::Vec3 eye,
              core::Vec3 direction,
              core::Vec3 muzzle,
              core::Vec3 hit,
              bool hit_surface,
              core::Vec3 normal = {},
              physics::BodyId body = {},
              const physics::PhysicsWorld* physics = nullptr,
              Surface surface = Surface::Concrete,
              bool tracer = true) noexcept;

    // Muzzle-flash size multiplier for subsequent shots (1 = rifle; the caller sets ~1.8 for a
    // sniper rifle's larger brake). fire() has no weapon argument, so this is the seam.
    // Per-weapon flash scale, indexed like PlayViewmodel::Weapon (SCAR, M4, MSR). The MSR's
    // long muzzle brake throws a visibly bigger flash than the carbines, so it gets 1.8x.
    static constexpr std::array<float, 3> kWeaponFlashSize{1.0f, 1.0f, 1.8f};

    void set_flash_size(float scale) noexcept { flash_size_ = std::clamp(scale, .25f, 4.0f); }

    void advance(float dt) noexcept;
    void append_smoke(std::vector<render::GpuParticle>& particles) const;
    // Queue a spent case for the next pose. The bounded queue is also valid before the first
    // pose; it creates no entities. The caller chooses extraction timing (shot or bolt cycle).
    void queue_case() noexcept;
    // First call binds one world and creates kEntityCount entities: shot parts, marks,
    // cases and fragments. Later calls only write components. Provide the camera/viewmodel pose
    // AFTER posing the viewmodel. Flash and muzzle light follow it for their short lifetime;
    // accepted shot paths and impacts remain fixed in world/body space.
    void pose(ecs::World& world,
              const physics::PhysicsWorld* physics = nullptr,
              const ViewPose* view = nullptr);

private:
    struct Shot {
        core::Vec3 direction{}, muzzle{}, hit{}, spark{}, normal{0, 1, 0};
        Surface surface = Surface::Concrete;
        bool tracer = true;
        float age = kTracerLifetime;
        bool hit_surface = false;
        bool presented = true;
        std::array<ecs::Entity, 6> entities{};
        float flash_roll = 0, flash_scale = 1;
    };

    struct Impact {
        core::Vec3 local_point{}, local_normal{};
        physics::BodyId body{};
        ecs::Entity entity{};
        float age = kImpactLifetime;
        float spin = 0;
    };

    struct Case {
        core::Vec3 position{}, previous{}, velocity{};
        core::Quat rotation = core::quat_identity();
        ecs::Entity entity{};
        float age = kCaseLifetime;
        bool bounced = false;
        bool settled = false;
    };

    struct Fragment {
        core::Vec3 origin{}, velocity{};
        float age = .6f, life = .6f, size = .01f;
        Surface surface = Surface::Concrete;
        ecs::Entity entity{};
    };

    struct Dust {
        core::Vec3 origin{}, velocity{}, color{};
        float age = kDustLifetime, size = .08f, life = kDustLifetime;
    };

    void show(ecs::World& world,
              ecs::Entity entity,
              render::MeshId mesh,
              const core::Transform& transform);
    rhi::Device& device_;
    rhi::TextureHandle scar_texture_{};
    render::MeshRegistry& meshes_;
    render::MaterialRegistry& materials_;
    std::array<Shot, kSlots> shots_{};
    std::array<render::MaterialId, 6> material_ids_{};
    std::array<Impact, kImpactSlots> impacts_{};
    std::array<Case, kCaseSlots> cases_{};
    std::array<Fragment, kFragmentSlots> fragments_{};
    std::array<Dust, kDustSlots> dust_{};
    std::array<render::MaterialId, static_cast<std::size_t>(Surface::Count)> surface_materials_{};
    std::array<render::MaterialId, kSlots> tracer_materials_{};
    render::MeshId scar_ = render::kInvalidMeshId;
    render::MeshId ring_ = render::kInvalidMeshId;
    render::MeshId star_ = render::kInvalidMeshId;
    float flash_size_ = 1.0f;
    float exposure_ = 1.0f / 60.0f;
    std::uint32_t sequence_ = 0;
    std::size_t next_fragment_ = 0, next_dust_ = 0;
    render::MeshId burst_ = render::kInvalidMeshId;
    render::MeshId brass_ = render::kInvalidMeshId;
    ecs::World* world_ = nullptr; // identity check only; never dereferenced by destruction
    std::size_t next_ = 0;
    std::size_t next_impact_ = 0;
    std::size_t next_case_ = 0;
    std::size_t queued_cases_ = 0;
    bool attempted_ = false, ready_ = false;
};

} // namespace rime::block_play
