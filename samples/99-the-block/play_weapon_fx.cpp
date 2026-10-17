// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_weapon_fx.hpp"

#include <algorithm>
#include <cmath>

#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/world.hpp"
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
    // One mesh serves every effect. A low-band uv sphere scales into all four shapes we need:
    // a round flash, an elongated bullet, a spark, and a disc-shaped surface mark. The cube that
    // used to draw the streak is gone with it -- a bullet reads as a stretched ellipsoid, and an
    // unreferenced mesh in the registry is just VRAM a reader has to account for.
    if (!meshes_.contains(burst_))
        return false;
    // These are LINEAR RADIANCE values fed straight to the ACES fit in tonemap.frag -- there is no
    // exposure system yet, so the curve is the only thing between these numbers and the screen.
    // That makes "more orange" a counter-intuitive edit: ACES saturates hard, and at the old
    // {38, 5, 0.6} BOTH red and green clipped to 1.0, so the flash rendered as #FFFDD8 -- white --
    // and pushing red higher while dropping green changed nothing a viewer could see. The hue has
    // to be placed where the curve can still represent it. Solving the fit backwards for a target
    // of sRGB (1.0, 0.55, 0.15) gives green 0.18 and blue 0.02; red stays well above 1 so the core
    // still blows out and blooms, which is what reads as heat. Re-derive these if an exposure
    // system lands, because every one of them is relative to a curve with no exposure in front.
    constexpr float colors[4][3] = {
        {10.0f, 0.18f, 0.02f}, {0, 0, 0}, {8.0f, 0.14f, 0.015f}, {0, 0, 0}};
    for (std::size_t i = 0; i < material_ids_.size(); ++i) {
        render::PbrMaterialDesc material{};
        const bool dark = i == 1 || i == 3;
        material.base_color[0] = dark ? .012f : .9f;
        material.base_color[1] = dark ? .009f : .15f;
        material.base_color[2] = dark ? .007f : .015f;
        material.metallic = i == 1 ? .7f : 0;
        material.roughness = i == 1 ? .35f : 1.0f;
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
                        bool hit_surface,
                        core::Vec3 normal,
                        physics::BodyId body,
                        const physics::PhysicsWorld* physics) noexcept {
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
    physics::BodyState state{};
    const float normal_length = core::length(normal);
    if (hit_surface && physics && finite(normal) && normal_length > .5f &&
        physics->get_body_state(body, state)) {
        // Surface marks live in the hit body's frame, so a moving target carries its scar.
        // Generational BodyId lookup removes it when that collider is fractured/replaced.
        auto& impact = impacts_[next_impact_];
        next_impact_ = (next_impact_ + 1) % kImpactSlots;
        const auto inverse = core::conjugate(state.orientation);
        impact.local_point = core::rotate(inverse, hit - state.position);
        impact.local_normal = core::rotate(inverse, normal / normal_length);
        impact.body = body;
        impact.age = 0;
    }
}

void PlayWeaponFx::advance(float dt) noexcept {
    if (!std::isfinite(dt) || dt <= 0.0f)
        return;
    for (auto& shot : shots_)
        if (shot.presented)
            shot.age = std::min(shot.age + dt, kSparkLifetime);
    for (auto& impact : impacts_)
        impact.age = std::min(impact.age + dt, kImpactLifetime);
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

void PlayWeaponFx::pose(ecs::World& world, const physics::PhysicsWorld* physics) {
    if (!ready_ || (world_ && world_ != &world))
        return;
    if (!world_) {
        world_ = &world;
        for (auto& shot : shots_)
            for (std::size_t part = 0; part < 3; ++part)
                shot.entities[part] = world.spawn_with(ecs::WorldTransform{},
                                                       render::MeshRef{},
                                                       render::MaterialRef{material_ids_[part]});
        for (auto& shot : shots_)
            for (std::size_t part = 3; part < 5; ++part) {
                render::PointLight light{};
                light.intensity = 0;
                shot.entities[part] = world.spawn_with(ecs::WorldTransform{}, light);
            }
        for (auto& impact : impacts_)
            impact.entity = world.spawn_with(
                ecs::WorldTransform{}, render::MeshRef{}, render::MaterialRef{material_ids_[3]});
    }
    for (auto& shot : shots_) {
        core::Transform flash{};
        // 0.30 m, not the 0.07 m this used to use. The sphere is scaled in RADIUS, so the old
        // 0.095 radius put its near face at -0.025 m -- behind the eye -- and the camera sat
        // inside the flash, which is why it filled the frame as an octagon (you were looking at
        // the inside of a 4x8 uv sphere). The muzzle is roughly at the eye in the environment
        // smoke, which has no viewmodel to push it forward, so the offset has to carry it clear
        // on its own rather than relying on a weapon being in the way.
        flash.translation = shot.muzzle + shot.direction * 0.15f;
        flash.rotation = along_z(shot.direction);
        const float flare = 1.0f - std::min(shot.age / kFlashLifetime, 1.0f);
        flash.scale = {0.02f + 0.03f * flare, 0.02f + 0.03f * flare, 0.06f * flare + 0.02f};
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
        // The bullet starts at 0.40 m, clear of the flash. Those two numbers are coupled: the
        // flash sits at 0.15 m with a ~0.05 m radius, so it ends at 0.20, and a bullet spawning
        // at 0.23 (the old 0.35 head minus its 0.12 length) was drawn INSIDE it -- a dark,
        // non-emissive metallic body occluding the middle of the flare, which read as a hole
        // punched in the muzzle fire. Keep the gap if either number moves.
        const float head = std::min(distance, .40f + 800.0f * shot.age);
        const float tail = std::max(0.0f, head - .12f);
        tracer.translation = shot.muzzle + direction * ((head + tail) * 0.5f);
        tracer.rotation = along_z(direction);
        tracer.scale = {0.016f, 0.016f, std::max(head - tail, 0.001f)};
        show(world,
             shot.entities[1],
             shot.age < kTracerLifetime && head < distance && distance > 0.01f
                 ? burst_
                 : render::kInvalidMeshId,
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
        // Short-lived inverse-square lights illuminate nearby walls and enter the normal HDR
        // reflection path. Point lights avoid allocating extra shadow maps for every round.
        for (std::size_t part = 3; part < 5; ++part) {
            auto* light = world.get<render::PointLight>(shot.entities[part]);
            auto* transform = world.get<ecs::WorldTransform>(shot.entities[part]);
            // show() above guards its lookups and so does this: a slot whose spawn did not take
            // should drop its light, not take the frame down with it.
            if (!light || !transform)
                continue;
            const bool muzzle = part == 3;
            const float life = muzzle ? kFlashLifetime : kSparkLifetime;
            const float fade = 1 - std::min(shot.age / life, 1.0f);
            transform->value.translation =
                muzzle ? shot.muzzle : shot.spark + (-shot.direction) * .08f;
            light->color_r = 1;
            light->color_g = .14f;
            light->color_b = .018f;
            light->radius = muzzle ? 4.0f : 2.0f;
            light->intensity = (muzzle || shot.hit_surface) ? (muzzle ? 35.0f : 18.0f) * fade : 0;
            world.mark_changed<ecs::WorldTransform>(shot.entities[part]);
            world.mark_changed<render::PointLight>(shot.entities[part]);
        }
        shot.presented = true;
    }
    for (auto& impact : impacts_) {
        physics::BodyState state{};
        const bool active =
            impact.age < kImpactLifetime && physics && physics->get_body_state(impact.body, state);
        core::Transform scar{};
        if (active) {
            const auto normal = core::rotate(state.orientation, impact.local_normal);
            scar.translation = state.position +
                               core::rotate(state.orientation, impact.local_point) + normal * .004f;
            scar.rotation = along_z(normal);
            scar.scale = {.045f, .045f, .006f};
        }
        show(world, impact.entity, active ? burst_ : render::kInvalidMeshId, scar);
    }
}

} // namespace rime::block_play
