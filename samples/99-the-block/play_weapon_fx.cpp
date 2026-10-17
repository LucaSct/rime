// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_weapon_fx.hpp"

#include <algorithm>
#include <cmath>

#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"

namespace rime::block_play {
namespace {
bool finite(core::Vec3 p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

core::Quat along_z(core::Vec3 unit) {
    // Shortest-arc rotation from local +Z onto the segment. Handle the antipodal case where
    // cross(+Z, direction) vanishes but the required rotation is still a half turn.
    if (unit.z < -0.999999f)
        return core::quat_from_axis_angle({0, 1, 0}, 3.14159265359f);
    return core::normalize(core::Quat{-unit.y, unit.x, 0.0f, 1.0f + unit.z});
}
} // namespace

PlayWeaponFx::PlayWeaponFx(rhi::Device&,
                           render::MeshRegistry& meshes,
                           render::MaterialRegistry& materials) noexcept
    : meshes_(meshes), materials_(materials) {}

bool PlayWeaponFx::initialize() {
    if (attempted_)
        return ready_;
    attempted_ = true;
    // Low-poly solids work with the existing opaque/emissive path. They need no texture, alpha
    // blending, transient light, shader, or asset cook. Registry ownership covers GPU retirement.
    burst_ = meshes_.add(render::make_uv_sphere(1.0f, 4, 8), "weapon-feedback-burst");
    segment_ = meshes_.add(render::make_cube(0.5f), "weapon-feedback-segment");
    if (!meshes_.contains(burst_) || !meshes_.contains(segment_))
        return false;
    constexpr float colors[3][3] = {{24.0f, 10.0f, 2.0f}, {10.0f, 5.0f, 0.5f}, {18.0f, 9.0f, 1.0f}};
    for (std::size_t i = 0; i < material_ids_.size(); ++i) {
        render::PbrMaterialDesc material{};
        material.base_color[0] = 0.9f;
        material.base_color[1] = 0.5f;
        material.base_color[2] = 0.08f;
        material.roughness = 1.0f;
        std::copy(std::begin(colors[i]), std::end(colors[i]), material.emissive);
        material_ids_[i] = materials_.add(material);
    }
    ready_ = true;
    return true;
}

void PlayWeaponFx::fire(core::Vec3 eye,
                        core::Vec3 direction,
                        core::Vec3 muzzle,
                        core::Vec3 hit,
                        bool hit_surface) noexcept {
    const float length = core::length(direction);
    const float distance = core::length(hit - muzzle);
    if (!ready_ || !finite(eye) || !finite(direction) || !finite(muzzle) || !finite(hit) ||
        !std::isfinite(length) || length < 1e-5f || !std::isfinite(distance))
        return;
    auto& shot = shots_[next_];
    next_ = (next_ + 1) % kSlots; // overwrite the oldest feedback, never grow a particle collection
    shot.direction = direction / length;
    shot.muzzle = muzzle;
    shot.hit = hit;
    const auto toward_eye = core::normalize(eye - hit);
    // No surface normal is supplied: this is a raised luminous spark, not a decal or a scar.
    shot.spark = hit + (finite(toward_eye) ? toward_eye : -shot.direction) * 0.015f;
    shot.hit_surface = hit_surface;
    shot.age = 0.0f;
    shot.presented = false;
}

void PlayWeaponFx::advance(float dt) noexcept {
    if (!std::isfinite(dt) || dt <= 0.0f)
        return;
    for (auto& shot : shots_)
        if (shot.presented)
            shot.age = std::min(shot.age + dt, kSparkLifetime);
    // A shot accepted by the sim just before a slow render still gets one visible presentation.
    // After that, real elapsed time expires effects; no frame-rate-dependent age clamp.
}

void PlayWeaponFx::show(ecs::World& world,
                        ecs::Entity entity,
                        render::MeshId mesh,
                        const core::Transform& transform) {
    if (auto* ref = world.get<render::MeshRef>(entity)) {
        if (ref->mesh != mesh) {
            ref->mesh = mesh;
            world.mark_changed<render::MeshRef>(entity);
        }
    }
    if (mesh != render::kInvalidMeshId)
        if (auto* pose = world.get<ecs::WorldTransform>(entity)) {
            pose->value = transform;
            world.mark_changed<ecs::WorldTransform>(entity);
        }
}

void PlayWeaponFx::pose(ecs::World& world) {
    if (!ready_ || (world_ && world_ != &world))
        return;
    if (!world_) {
        world_ = &world;
        for (auto& shot : shots_)
            for (std::size_t part = 0; part < shot.entities.size(); ++part)
                shot.entities[part] = world.spawn_with(ecs::WorldTransform{},
                                                       render::MeshRef{},
                                                       render::MaterialRef{material_ids_[part]});
    }
    for (auto& shot : shots_) {
        core::Transform flash{};
        flash.translation = shot.muzzle + shot.direction * 0.07f;
        flash.rotation = along_z(shot.direction);
        const float flare = 1.0f - std::min(shot.age / kFlashLifetime, 1.0f);
        flash.scale = {0.03f + 0.045f * flare, 0.03f + 0.045f * flare, 0.12f * flare + 0.03f};
        show(world,
             shot.entities[0],
             shot.age < kFlashLifetime ? burst_ : render::kInvalidMeshId,
             flash);

        core::Transform tracer{};
        const auto path = shot.hit - shot.muzzle;
        const float distance = core::length(path);
        const auto direction = distance > 1e-5f ? path / distance : shot.direction;
        // A short swept streak visualizes the already-resolved hitscan. Its travel time is
        // presentation only: it does not postpone damage, collision, or any ShotEvent.
        const float head = std::min(distance, 2.0f + distance * shot.age / kTracerLifetime);
        const float tail = std::max(0.0f, head - 2.0f);
        tracer.translation = shot.muzzle + direction * ((head + tail) * 0.5f);
        tracer.rotation = along_z(direction);
        tracer.scale = {0.012f, 0.012f, std::max(head - tail, 0.001f)};
        show(world,
             shot.entities[1],
             shot.age < kTracerLifetime && distance > 0.01f ? segment_ : render::kInvalidMeshId,
             tracer);

        core::Transform spark{};
        spark.translation = shot.spark;
        spark.rotation = along_z(-shot.direction);
        const float size = 0.015f + 0.065f * (1.0f - shot.age / kSparkLifetime);
        spark.scale = {size, size, size * 1.5f};
        show(world,
             shot.entities[2],
             shot.hit_surface && shot.age < kSparkLifetime ? burst_ : render::kInvalidMeshId,
             spark);
        shot.presented = true;
    }
}

} // namespace rime::block_play
