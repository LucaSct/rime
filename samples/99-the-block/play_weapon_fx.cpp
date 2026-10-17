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

render::CpuMesh case_mesh() {
    // Unit-radius, unit-half-length eight-sided cylinder. The instance scale supplies actual
    // 9 mm case dimensions; flat rings make its motion readable without a texture.
    render::CpuMesh mesh;
    constexpr int sides = 8;
    constexpr float tau = 6.28318530718f;
    for (int i = 0; i < sides; ++i) {
        const float a = tau * static_cast<float>(i) / sides;
        const float x = std::cos(a), y = std::sin(a);
        for (float z : {-1.0f, 1.0f}) {
            render::MeshVertex v{};
            v.px = x;
            v.py = y;
            v.pz = z;
            v.nx = x;
            v.ny = y;
            mesh.vertices.push_back(v);
        }
    }
    for (int i = 0; i < sides; ++i) {
        const auto lo = static_cast<std::uint32_t>(2 * i);
        const auto next = static_cast<std::uint32_t>(2 * ((i + 1) % sides));
        mesh.indices.insert(mesh.indices.end(), {lo, next, lo + 1, lo + 1, next, next + 1});
    }
    for (float z : {-1.0f, 1.0f}) {
        const auto start = static_cast<std::uint32_t>(mesh.vertices.size());
        for (int i = 0; i < sides; ++i) {
            const float a = tau * static_cast<float>(i) / sides;
            render::MeshVertex v{};
            v.px = std::cos(a);
            v.py = std::sin(a);
            v.pz = z;
            v.nx = 0;
            v.ny = 0;
            v.nz = z;
            mesh.vertices.push_back(v);
        }
        for (int i = 1; i + 1 < sides; ++i) {
            if (z > 0)
                mesh.indices.insert(mesh.indices.end(),
                                    {start,
                                     start + static_cast<std::uint32_t>(i),
                                     start + static_cast<std::uint32_t>(i + 1)});
            else
                mesh.indices.insert(mesh.indices.end(),
                                    {start,
                                     start + static_cast<std::uint32_t>(i + 1),
                                     start + static_cast<std::uint32_t>(i)});
        }
    }
    return mesh;
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
    brass_ = meshes_.add(case_mesh(), "weapon-feedback-case");
    // The sphere serves flash, tracer, spark and marks; cases share one low-poly cylinder.
    if (!meshes_.contains(burst_) || !meshes_.contains(brass_))
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
    constexpr float colors[5][3] = {
        {10.0f, 0.18f, 0.02f}, {0, 0, 0}, {8.0f, 0.14f, 0.015f}, {0, 0, 0}, {0, 0, 0}};
    for (std::size_t i = 0; i < material_ids_.size(); ++i) {
        render::PbrMaterialDesc material{};
        const bool brass = i == 4;
        const bool dark = i == 1 || i == 3;
        material.base_color[0] = brass ? .55f : dark ? .012f : .9f;
        material.base_color[1] = brass ? .31f : dark ? .009f : .15f;
        material.base_color[2] = brass ? .065f : dark ? .007f : .015f;
        material.metallic = brass ? .8f : i == 1 ? .7f : 0;
        material.roughness = brass ? .32f : i == 1 ? .35f : 1.0f;
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
    for (auto& casing : cases_) {
        if (casing.age >= kCaseLifetime)
            continue;
        casing.age = std::min(casing.age + dt, kCaseLifetime);
        if (casing.age >= kCaseLifetime || casing.settled)
            continue;
        casing.previous = casing.position;
        casing.position += casing.velocity * dt + core::Vec3{0, -4.905f * dt * dt, 0};
        casing.velocity.y -= 9.81f * dt;
        casing.rotation = core::normalize(core::quat_from_axis_angle({.6f, .3f, .7f}, 9.0f * dt) *
                                          casing.rotation);
    }
    // A shot accepted by the sim just before a slow render still gets one visible presentation.
    // After that, real elapsed time expires effects; no frame-rate-dependent age clamp.
}

void PlayWeaponFx::queue_case() noexcept {
    if (ready_ && queued_cases_ < kCaseSlots)
        ++queued_cases_;
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

void PlayWeaponFx::pose(ecs::World& world,
                        const physics::PhysicsWorld* physics,
                        const ViewPose* view) {
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
        for (auto& casing : cases_)
            casing.entity = world.spawn_with(
                ecs::WorldTransform{}, render::MeshRef{}, render::MaterialRef{material_ids_[4]});
    }
    const float view_dir_length = view ? core::length(view->direction) : 0.0f;
    const bool valid_view = view && finite(view->eye) && finite(view->muzzle) &&
                            finite(view->ejection_point) && finite(view->velocity) &&
                            finite(view->direction) && std::isfinite(view_dir_length) &&
                            view_dir_length > 1e-5f;
    if (valid_view) {
        const auto forward = view->direction / view_dir_length;
        auto right = core::cross(forward, core::Vec3{0, 1, 0});
        right = core::length(right) > 1e-5f ? core::normalize(right) : core::Vec3{1, 0, 0};
        while (queued_cases_ > 0) {
            auto& casing = cases_[next_case_];
            next_case_ = (next_case_ + 1) % kCaseSlots;
            --queued_cases_;
            casing.position = casing.previous = view->ejection_point;
            casing.velocity =
                view->velocity + right * 2.1f + forward * .35f + core::Vec3{0, 2.2f, 0};
            casing.rotation = along_z(forward);
            casing.age = 0.0f;
            casing.bounced = false;
            casing.settled = false;
        }
    }
    for (auto& shot : shots_) {
        core::Transform flash{};
        // Only live muzzle effects follow the latest camera/viewmodel pose. The fallback keeps
        // old callers usable; neither path changes the accepted hit or tracer's world path.
        const auto muzzle = valid_view ? view->muzzle : shot.muzzle;
        const auto muzzle_dir = valid_view ? view->direction / view_dir_length : shot.direction;
        flash.translation = muzzle + muzzle_dir * (valid_view ? .035f : .15f);
        flash.rotation = along_z(muzzle_dir);
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
        const bool forward_path = core::dot(path, shot.direction) > .28f;
        // A short swept streak visualizes the already-resolved hitscan. Its travel time is
        // presentation only: it does not postpone damage, collision, or any ShotEvent.
        // Scale is the sphere radius, so a 30 mm total length needs a 15 mm Z half-extent.
        // Start beyond the live flare and suppress a pellet for a hit too close to the muzzle.
        const float head = std::min(distance, .28f + 800.0f * shot.age);
        const float tail = std::max(.25f, head - .03f);
        tracer.translation = shot.muzzle + direction * ((head + tail) * 0.5f);
        tracer.rotation = along_z(direction);
        tracer.scale = {.003f, .003f, std::max((head - tail) * .5f, .001f)};
        show(world,
             shot.entities[1],
             shot.age < kTracerLifetime && head < distance && forward_path ? burst_
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
                muzzle ? flash.translation : shot.spark + (-shot.direction) * .08f;
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
    for (auto& casing : cases_) {
        const bool active = casing.age < kCaseLifetime;
        if (active && physics && !casing.settled) {
            const auto travel = casing.position - casing.previous;
            const float length = core::length(travel);
            if (length > 1e-5f) {
                physics::RayHit hit{};
                physics::QueryFilter scenery{};
                scenery.dynamics = false; // avoid the shooter's own moving capsule
                if (physics->raycast({casing.previous, travel, length}, hit, scenery)) {
                    casing.position = hit.point + hit.normal * .006f;
                    if (casing.bounced) {
                        // One visible bounce, then rest instead of sinking through the floor.
                        casing.velocity = {};
                        casing.settled = true;
                    } else {
                        const float into = core::dot(casing.velocity, hit.normal);
                        if (into < 0.0f)
                            casing.velocity -= hit.normal * (1.3f * into);
                        casing.velocity *= .55f;
                        casing.bounced = true;
                    }
                }
            }
        }
        core::Transform transform{};
        transform.translation = casing.position;
        transform.rotation = casing.rotation;
        // 9.6 mm diameter x 26 mm length -- the mesh's radius and half-height are both 1, so this
        // scale IS the case's half-dimensions. The old .0095 half-length (19 mm total) read as a
        // pistol case; the held weapon art here is the sniper/rifle pair, so a slightly longer
        // case (still a few cm, not a "giant bullet") reads correctly at ejection distance.
        transform.scale = {.0048f, .0048f, .013f};
        show(world, casing.entity, active ? brass_ : render::kInvalidMeshId, transform);
    }
}

} // namespace rime::block_play
