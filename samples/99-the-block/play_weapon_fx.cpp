// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "play_weapon_fx.hpp"

#include <algorithm>
#include <cmath>

#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/physics/world.hpp"
#include "rime/render/components.hpp"
#include "rime/rhi/rhi.hpp"
#include "scar_reanchor.hpp"

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

constexpr std::array<core::Vec3, 7> kSurfaceColors{{{.30f, .29f, .27f},
                                                    {.34f, .12f, .065f},
                                                    {.55f, .51f, .44f},
                                                    {.16f, .085f, .035f},
                                                    {.8f, .35f, .06f},
                                                    {.20f, .36f, .44f},
                                                    {.24f, .12f, .045f}}};

float random01(std::uint32_t& seed) {
    // Counter-seeded xorshift: replaying accepted shots reproduces their cosmetic spray without
    // consuming the simulation RNG or depending on render frame rate.
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return static_cast<float>(seed & 0xffffu) / 65535.0f;
}

render::CpuMesh decal_quad() {
    render::CpuMesh mesh;
    mesh.vertices = {{-.01f, -.01f, 0, 0, 0, 1, 0, 0},
                     {.01f, -.01f, 0, 0, 0, 1, 1, 0},
                     {.01f, .01f, 0, 0, 0, 1, 1, 1},
                     {-.01f, .01f, 0, 0, 0, 1, 0, 1}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    render::compute_tangents(mesh);
    return mesh;
}

render::CpuMesh flash_star() {
    // Muzzle flash is a flat star: 8 petals of uneven length with a short valley between each,
    // because a perfect disc reads as a sticker while a ragged star reads as escaping gas. It is
    // emitted double-sided (both windings) so it needs no culling exemption from the material.
    render::CpuMesh mesh;
    mesh.vertices.push_back({0, 0, 0, 0, 0, 1, .5f, .5f});
    constexpr unsigned petals = 8;
    for (unsigned i = 0; i < petals * 2; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f / petals;
        const float tip = .75f + .25f * std::sin(static_cast<float>(i / 2) * 2.7f + 1.3f);
        const float r = (i % 2 == 0 ? tip : .30f) * .01f;
        mesh.vertices.push_back({std::cos(a) * r, std::sin(a) * r, 0, 0, 0, 1, 0, 0});
    }
    for (unsigned i = 0; i < petals * 2; ++i) {
        const unsigned a = 1 + i, b = 1 + (i + 1) % (petals * 2);
        mesh.indices.insert(mesh.indices.end(), {0, a, b, 0, b, a});
    }
    return mesh;
}

render::CpuMesh splash_ring() {
    render::CpuMesh mesh;
    for (unsigned i = 0; i <= 32; ++i) {
        const float angle = static_cast<float>(i) * 6.2831853f / 32;
        for (const float radius : {.008f, .01f})
            mesh.vertices.push_back(
                {std::cos(angle) * radius, std::sin(angle) * radius, 0, 0, 0, 1, 0, 0});
        if (i != 0) {
            const auto b = i * 2;
            mesh.indices.insert(mesh.indices.end(), {b - 2, b, b - 1, b - 1, b, b + 1});
        }
    }
    return mesh;
}

render::CpuMesh case_mesh() {
    // Bake metre dimensions into the mesh, keeping its instance transform rigid. A tiny scale
    // determinant trips the normal-matrix inverse's absolute singularity guard in debug builds,
    // even though the shape itself is valid. Geometry size must not depend on assertion mode.
    render::CpuMesh mesh;
    constexpr float radius = .0048f, half_length = .013f;
    constexpr int sides = 8;
    constexpr float tau = 6.28318530718f;
    for (int i = 0; i < sides; ++i) {
        const float a = tau * static_cast<float>(i) / sides;
        const float x = std::cos(a), y = std::sin(a);
        for (float z : {-1.0f, 1.0f}) {
            render::MeshVertex v{};
            v.px = x * radius;
            v.py = y * radius;
            v.pz = z * half_length;
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
            v.px = std::cos(a) * radius;
            v.py = std::sin(a) * radius;
            v.pz = z * half_length;
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

PlayWeaponFx::PlayWeaponFx(rhi::Device& device,
                           render::MeshRegistry& meshes,
                           render::MaterialRegistry& materials) noexcept
    : device_(device), meshes_(meshes), materials_(materials) {}

PlayWeaponFx::~PlayWeaponFx() {
    if (scar_texture_.is_valid())
        device_.destroy(scar_texture_);
}

bool PlayWeaponFx::initialize() {
    if (attempted_)
        return ready_;
    attempted_ = true;
    // Low-poly solids work with the existing opaque/emissive path. They need no texture, alpha
    // blending, transient light, shader, or asset cook. Registry ownership covers GPU retirement.
    // One centimetre is the shared sphere's base radius; scales express centimetres below.
    // This keeps millimetre-wide tracers above the inverse's determinant guard as well.
    burst_ = meshes_.add(render::make_uv_sphere(.01f, 4, 8), "weapon-feedback-burst");
    brass_ = meshes_.add(case_mesh(), "weapon-feedback-case");
    // The sphere still serves flash/tracer/spray; cases share one low-poly cylinder. Marks now
    // use an alpha-tested planar quad, so their thickness cannot silhouette as a sphere blob.
    scar_ = meshes_.add(decal_quad(), "weapon-scar-quad");
    ring_ = meshes_.add(splash_ring(), "weapon-water-ring");
    star_ = meshes_.add(flash_star(), "weapon-flash-star");
    if (!meshes_.contains(burst_) || !meshes_.contains(brass_) || !meshes_.contains(scar_) ||
        !meshes_.contains(ring_) || !meshes_.contains(star_))
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
    // A tiny procedural coverage/albedo stamp supplies a dark crater and pale chipped rim.
    // Alpha testing uses the existing material path; the transparent corners leave the wall
    // visible, and a 2 mm normal offset prevents z-fighting without adding physical thickness.
    // Crater stamp, alpha-tested. Three zones, because a bullet hole reads as a hole only when
    // something is darker than the wall AND something is lighter around it: a near-black core
    // (the void), a charcoal-to-grey scorched band, and a pale, ragged chipped rim where the
    // surface skin spalled off and exposed fresh, lighter material. The rim's outer edge is
    // broken by hashed noise so it never reads as a printed circle, and the texels carry a
    // radial gradient so downsampling at 15 m still leaves a dark centre. Everything is a pure
    // function of the texel, so the stamp is deterministic.
    constexpr unsigned texels = 128;
    std::array<std::uint8_t, texels * texels * 4> pixels{};
    const auto hash = [](unsigned x, unsigned y) {
        std::uint32_t h = x * 374761393u + y * 668265263u;
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 0xffu) / 255.0f;
    };
    for (unsigned y = 0; y < texels; ++y)
        for (unsigned x = 0; x < texels; ++x) {
            const float u = (static_cast<float>(x) + .5f) / texels * 2 - 1;
            const float v = (static_cast<float>(y) + .5f) / texels * 2 - 1;
            const float angle = std::atan2(v, u);
            const float edge = .80f + .07f * std::sin(angle * 7) + .05f * std::cos(angle * 11) +
                               .06f * (hash(x / 4, y / 4) - .5f);
            const float radius = std::sqrt(u * u + v * v) / edge;
            float shade;
            if (radius < .5f)
                shade = 2 + 12 * radius;
            else if (radius < .62f)
                shade = 14 + 90 * (radius - .5f) / .12f;
            else
                shade = 200 + 50 * hash(x, y) - 40 * (radius - .62f);
            const auto byte = static_cast<std::uint8_t>(std::clamp(shade, 0.0f, 255.0f));
            const unsigned at = (y * texels + x) * 4;
            // Slight warm bias on the pale rim only (spalled masonry is buff, not blue-grey).
            const bool rim = radius >= .62f;
            pixels[at] = rim ? static_cast<std::uint8_t>(std::min(byte + 14, 255)) : byte;
            pixels[at + 1] = byte;
            pixels[at + 2] = rim ? static_cast<std::uint8_t>(byte * 85 / 100) : byte;
            pixels[at + 3] = radius < 1 ? 255 : 0;
        }
    rhi::TextureDesc texture{};
    texture.extent = {texels, texels};
    texture.format = rhi::Format::RGBA8Srgb;
    texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    texture.debug_name = "weapon-scar-mask";
    scar_texture_ = device_.create_texture(texture);
    if (!scar_texture_.is_valid())
        return false;
    device_.write_texture(scar_texture_, pixels.data(), pixels.size());
    constexpr float colors[6][3] = {{7.0f, 0.9f, 0.05f},
                                    {5, 1.1f, .20f},
                                    {3.2f, 1.7f, .6f},
                                    {0, 0, 0},
                                    {0, 0, 0},
                                    {18.0f, 13.0f, 4.5f}};
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
        if (i == 2) {
            material.base_color[0] = .55f;
            material.base_color[1] = .50f;
            material.base_color[2] = .42f;
        }
        if (i == 3) {
            material.base_color[0] = material.base_color[1] = material.base_color[2] = 1;
            material.base_color_texture = scar_texture_;
            material.alpha_cutoff = .5f;
            material.clamp_uv = true;
        }
        if (i == 5) {
            material.base_color[0] = .9f;
            material.base_color[1] = .8f;
            material.base_color[2] = .5f;
        }
        material_ids_[i] = materials_.add(material);
    }
    for (std::size_t i = 0; i < surface_materials_.size(); ++i) {
        render::PbrMaterialDesc desc{};
        const auto color = kSurfaceColors[i];
        desc.base_color[0] = color.x;
        desc.base_color[1] = color.y;
        desc.base_color[2] = color.z;
        desc.roughness = i == static_cast<std::size_t>(Surface::Water) ? .18f : .95f;
        if (i == static_cast<std::size_t>(Surface::Metal)) {
            desc.emissive[0] = 7;
            desc.emissive[1] = 1.2f;
            desc.emissive[2] = .12f;
        }
        surface_materials_[i] = materials_.add(desc);
    }
    for (std::size_t i = 0; i < kSlots; ++i)
        tracer_materials_[i] =
            i == 0 ? material_ids_[1] : materials_.add(materials_.get(material_ids_[1]));
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
                        const physics::PhysicsWorld* physics,
                        Surface surface,
                        bool tracer) noexcept {
    const float length = core::length(direction);
    const float distance = core::length(hit - muzzle);
    if (!ready_ || !finite(eye) || !finite(direction) || !finite(muzzle) || !finite(hit) ||
        !std::isfinite(length) || length < 1e-5f || !std::isfinite(distance))
        return;
    std::uint32_t seed = ++sequence_ * 0x9e3779b9u;
    auto& shot = shots_[next_];
    next_ = (next_ + 1) % kSlots; // overwrite the oldest feedback, never grow a particle collection
    shot.direction = direction / length;
    shot.muzzle = muzzle;
    shot.hit = hit;
    const auto toward_eye = core::normalize(eye - hit);
    // Raised impact feedback remains distinct from the persistent, surface-aligned scar.
    shot.spark = hit + (finite(toward_eye) ? toward_eye : -shot.direction) * 0.015f;
    shot.flash_roll = random01(seed) * 6.2831853f;
    shot.flash_scale = (.75f + random01(seed) * .5f) * flash_size_;
    shot.hit_surface = hit_surface;
    shot.surface = surface;
    shot.tracer = tracer;
    const float normal_size = core::length(normal);
    shot.normal = finite(normal) && normal_size > .5f ? normal / normal_size : -shot.direction;
    if (hit_surface) {
        const auto color = kSurfaceColors[static_cast<std::size_t>(surface)];
        const auto tangent = core::rotate(along_z(shot.normal), core::Vec3{1, 0, 0});
        const auto bitangent = core::cross(shot.normal, tangent);
        for (unsigned i = 0; i < (surface == Surface::Metal ? 14u : 9u); ++i) {
            auto& fragment = fragments_[next_fragment_];
            next_fragment_ = (next_fragment_ + 1) % kFragmentSlots;
            const float angle = random01(seed) * 6.2831853f;
            const float speed = surface == Surface::Metal ? 6.0f : 2.4f;
            fragment.origin = hit + shot.normal * .018f;
            fragment.velocity = shot.normal * (.6f + random01(seed) * speed) +
                                (tangent * std::cos(angle) + bitangent * std::sin(angle)) *
                                    (.4f + random01(seed) * speed);
            if (surface == Surface::Water)
                fragment.velocity = shot.normal * (2.0f + random01(seed) * 1.8f) +
                                    (tangent * std::cos(angle) + bitangent * std::sin(angle)) * .3f;
            fragment.surface = surface;
            fragment.age = 0;
            fragment.life = surface == Surface::Metal ? .22f : .6f;
            fragment.size = surface == Surface::Metal ? .005f : .016f + random01(seed) * .02f;
        }
        if (surface != Surface::Metal && surface != Surface::Water)
            for (unsigned i = 0; i < kDustPerHit; ++i) {
                auto& dust = dust_[next_dust_];
                next_dust_ = (next_dust_ + 1) % kDustSlots;
                dust.origin = hit + shot.normal * .04f;
                // Billow: mostly along the hit normal (soil kicks straight up, which for a floor
                // is its normal), with a sideways spread so the cloud mushrooms.
                dust.velocity = shot.normal * (.7f + random01(seed) * 1.3f) +
                                tangent * ((random01(seed) - .5f) * 1.6f) +
                                bitangent * ((random01(seed) - .5f) * 1.6f);
                // Dust is pulverised material, i.e. paler than the intact surface; lift the
                // albedo toward grey so dark brick still yields a visible reddish puff.
                const float lift = surface == Surface::Soil ? .05f : .10f;
                dust.color = {std::min(color.x * 1.6f + lift, .9f),
                              std::min(color.y * 1.5f + lift * .8f, .85f),
                              std::min(color.z * 1.4f + lift * .6f, .8f)};
                dust.life = kDustLifetime;
                dust.age = -random01(seed) * .05f; // staggered start so puffs are not one blob
                dust.size = .07f + random01(seed) * .07f;
            }
    }
    if (tracer) {
        // A tiny grey puff of propellant gas just ahead of the muzzle. Only on tracer rounds so
        // a burst does not bury the screen in smoke; it shares the dust ring with a short life.
        auto& puff = dust_[next_dust_];
        next_dust_ = (next_dust_ + 1) % kDustSlots;
        puff.origin = muzzle + shot.direction * .35f;
        puff.velocity = shot.direction * .6f;
        puff.color = {.62f, .62f, .6f};
        puff.age = 0;
        puff.life = .55f;
        puff.size = .035f * flash_size_;
    }
    shot.age = 0.0f;
    shot.presented = false;
    physics::BodyState state{};
    const float normal_length = core::length(normal);
    if (hit_surface && surface != Surface::Water && surface != Surface::Soil && physics &&
        finite(normal) && normal_length > .5f && physics->get_body_state(body, state)) {
        // Surface marks live in the hit body's frame, so a moving target carries its scar.
        // Generational BodyId lookup notices when that collider is fractured/replaced; pose()
        // then re-anchors the scar to whatever solid piece now holds that spot.
        auto& impact = impacts_[next_impact_];
        next_impact_ = (next_impact_ + 1) % kImpactSlots;
        const auto inverse = core::conjugate(state.orientation);
        impact.local_point = core::rotate(inverse, hit - state.position);
        impact.local_normal = core::rotate(inverse, normal / normal_length);
        impact.body = body;
        impact.world_point = hit;
        impact.world_normal = normal / normal_length;
        impact.age = 0;
        impact.spin = (random01(seed) - .5f) * 1.2f;
    }
}

void PlayWeaponFx::advance(float dt) noexcept {
    if (!std::isfinite(dt) || dt <= 0.0f)
        return;
    exposure_ = std::min(dt, 1.0f / 30.0f);
    for (auto& shot : shots_)
        if (shot.presented)
            shot.age = std::min(shot.age + dt, kTracerLifetime);
    for (auto& fragment : fragments_)
        fragment.age = std::min(fragment.age + dt, fragment.life);
    for (auto& dust : dust_)
        dust.age = std::min(dust.age + dt, dust.life);
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

void PlayWeaponFx::append_smoke(std::vector<render::GpuParticle>& particles) const {
    // Analytic drag and expansion turn five soft billboards into a dust puff. Age is bounded,
    // no collision queries or engine VFX state are needed, and opacity retires smoothly.
    for (const auto& dust : dust_) {
        if (dust.age >= dust.life)
            continue;
        const float t = std::max(dust.age, 0.0f);
        const auto position = dust.origin + dust.velocity * ((1 - std::exp(-2.2f * t)) / 2.2f) +
                              core::Vec3{0, .12f * t, 0};
        render::GpuParticle p{};
        p.position_size[0] = position.x;
        p.position_size[1] = position.y;
        p.position_size[2] = position.z;
        p.position_size[3] = dust.size + t * .26f;
        p.color_alpha[0] = dust.color.x;
        p.color_alpha[1] = dust.color.y;
        p.color_alpha[2] = dust.color.z;
        // Ease in over the first 60 ms (no pop), then fade linearly to nothing at kDustLifetime.
        p.color_alpha[3] = .42f * (1 - t / dust.life) * std::min(1.0f, .35f + t / .06f);
        particles.push_back(p);
    }
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
            shot.entities[5] = world.spawn_with(
                ecs::WorldTransform{}, render::MeshRef{}, render::MaterialRef{material_ids_[5]});
        for (auto& shot : shots_)
            for (std::size_t part = 3; part < 5; ++part) {
                render::PointLight light{};
                light.intensity = 0;
                shot.entities[part] = world.spawn_with(ecs::WorldTransform{}, light);
            }
        for (auto& impact : impacts_)
            impact.entity = world.spawn_with(
                ecs::WorldTransform{}, render::MeshRef{}, render::MaterialRef{material_ids_[3]});
        for (auto& fragment : fragments_)
            fragment.entity = world.spawn_with(ecs::WorldTransform{},
                                               render::MeshRef{},
                                               render::MaterialRef{surface_materials_[0]});
        for (std::size_t i = 0; i < kSlots; ++i)
            world.get<render::MaterialRef>(shots_[i].entities[1])->material = tracer_materials_[i];
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
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        auto& shot = shots_[slot];
        core::Transform flash{};
        // Only live muzzle effects follow the latest camera/viewmodel pose. The fallback keeps
        // old callers usable; neither path changes the accepted hit or tracer's world path.
        const auto muzzle = valid_view ? view->muzzle : shot.muzzle;
        const auto muzzle_dir = valid_view ? view->direction / view_dir_length : shot.direction;
        flash.translation = muzzle + muzzle_dir * (valid_view ? .035f : .15f);
        flash.rotation =
            along_z(muzzle_dir) * core::quat_from_axis_angle({0, 0, 1}, shot.flash_roll);
        const float flare = 1.0f - std::min(shot.age / kFlashLifetime, 1.0f);
        const float star = shot.flash_scale * (13.0f + 11.0f * flare);
        flash.scale = {star, star, 1.0f};
        show(world,
             shot.entities[0],
             shot.age < kFlashLifetime ? star_ : render::kInvalidMeshId,
             flash);
        core::Transform core_t = flash;
        core_t.translation = flash.translation + muzzle_dir * .005f;
        core_t.scale = core::Vec3{1, 1, 1} * (2.2f * shot.flash_scale * (.4f + .6f * flare));
        show(world,
             shot.entities[5],
             shot.age < kFlashLifetime ? burst_ : render::kInvalidMeshId,
             core_t);

        core::Transform tracer{};
        const auto path = shot.hit - shot.muzzle;
        const float distance = core::length(path);
        const auto direction = distance > 1e-5f ? path / distance : shot.direction;
        const bool forward_path = core::dot(path, shot.direction) > .28f;
        // Exposure-length swept segment: intersect the travelled interval with the accepted
        // ray, rather than drawing a 3 cm pellet that jumps 13 m between 60 Hz frames. The
        // last interval stays visible even when its head has reached the target; damage is
        // still immediate hitscan. A per-slot emissive material fades without changing peers.
        const float head = std::min(distance, .28f + 800.0f * shot.age);
        const float tail = std::max(.25f, .28f + 800.0f * (shot.age - exposure_));
        tracer.translation = shot.muzzle + direction * ((head + tail) * 0.5f);
        tracer.rotation = along_z(direction);
        tracer.scale = {.35f, .35f, std::max((head - tail) * 50.0f, .1f)};
        auto glow = materials_.get(tracer_materials_[slot]);
        const float fade = std::max(0.0f, 1 - shot.age / kTracerLifetime);
        glow.emissive[0] = 5 * fade;
        glow.emissive[1] = 1.1f * fade;
        glow.emissive[2] = .20f * fade;
        materials_.update(tracer_materials_[slot], glow);
        show(world,
             shot.entities[1],
             shot.tracer && shot.age < kTracerLifetime && tail < head && forward_path
                 ? burst_
                 : render::kInvalidMeshId,
             tracer);

        core::Transform spark{};
        spark.translation = shot.spark;
        spark.rotation = along_z(shot.normal);
        const bool water = shot.surface == Surface::Water;
        const bool metal = shot.surface == Surface::Metal;
        const float size = .012f + .035f * (1 - std::min(shot.age / kSparkLifetime, 1.0f));
        spark.scale = water ? core::Vec3{5 + shot.age * 90, 5 + shot.age * 90, 1}
                            : core::Vec3{size * 100, size * 100, .25f};
        auto* spark_mat = world.get<render::MaterialRef>(shot.entities[2]);
        spark_mat->material =
            water || metal || shot.surface == Surface::Soil || shot.surface == Surface::Wood
                ? surface_materials_[static_cast<std::size_t>(shot.surface)]
                : material_ids_[2];
        world.mark_changed<render::MaterialRef>(shot.entities[2]);
        show(world,
             shot.entities[2],
             shot.hit_surface && shot.age < (water ? kTracerLifetime : .06f)
                 ? (water ? ring_ : burst_)
                 : render::kInvalidMeshId,
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
            // The muzzle light is a one-frame flicker (~16 ms); a longer one floods the whole wall.
            const float life = muzzle ? .016f : kSparkLifetime;
            const float fade = 1 - std::min(shot.age / life, 1.0f);
            // LEAD THE LIGHT AHEAD OF THE MUZZLE. The first-person forearms are ~0.4-0.6 m from
            // the flash, and an inverse-square light that close is ~50x brighter on the sleeve
            // than on a wall 3 m away. The arm cloth is near-black with a normal-mapped wrinkle
            // relief, so only the wrinkle crests facing the light clear the tone-mapper's knee:
            // hard orange islands on black cloth that read as burnt-through holes (measured at
            // 1080p on the 3060: the islands appear with the muzzle light and vanish without it).
            // Moving the light 1.2 m down the bore (less when a wall is nearer, so a close wall
            // is still lit from in front of it) cuts the sleeve's share ~18x. The intensity is
            // rescaled by (new/old distance to the hit)^2 so the lit wall keeps its brightness.
            const float to_hit =
                shot.hit_surface ? core::length(shot.hit - flash.translation) : 0.0f;
            const float lead =
                muzzle ? (shot.hit_surface ? std::min(1.2f, .4f * to_hit) : 1.2f) : 0.0f;
            const float rescale = muzzle && shot.hit_surface && to_hit > 1e-3f
                                      ? (to_hit - lead) * (to_hit - lead) / (to_hit * to_hit)
                                      : 1.0f;
            transform->value.translation = muzzle ? flash.translation + muzzle_dir * lead
                                                  : shot.spark + (-shot.direction) * .08f;
            light->color_r = 1;
            light->color_g = .14f;
            light->color_b = .018f;
            light->radius = muzzle ? 3.0f - lead : 1.0f;
            light->intensity = (muzzle || (shot.hit_surface && metal))
                                   ? (muzzle ? 12.0f * rescale : 1.5f) * fade
                                   : 0;
            world.mark_changed<ecs::WorldTransform>(shot.entities[part]);
            world.mark_changed<render::PointLight>(shot.entities[part]);
        }
        shot.presented = true;
    }
    // RE-ANCHOR, DON'T DROP (see scar_reanchor.hpp). A scar whose body vanished (the wall
    // fractured and its collider was replaced) probes the world once at its last known pose:
    // a body there inherits the scar, nothing there means the piece left and the scar goes.
    // Work is capped per frame; scars past the cap are simply retried next frame, still hidden
    // until then (a one-frame blink is cheaper than a hitch after a building-sized collapse).
    std::size_t reanchor_budget = kReanchorsPerFrame;
    for (auto& impact : impacts_) {
        physics::BodyState state{};
        bool active = false;
        if (impact.age < kImpactLifetime && physics) {
            active = physics->get_body_state(impact.body, state);
            if (!active && reanchor_budget > 0) {
                --reanchor_budget;
                ScarProbeHit found{};
                const bool anchored = find_scar_anchor(
                    impact.world_point,
                    impact.world_normal,
                    [&](core::Vec3 origin, core::Vec3 dir, float reach, ScarProbeHit& out) {
                        physics::RayHit ray_hit{};
                        if (!physics->raycast({origin, dir, reach}, ray_hit))
                            return false;
                        out = {ray_hit.body, ray_hit.point};
                        return true;
                    },
                    found);
                physics::BodyState fresh{};
                if (anchored && physics->get_body_state(found.body, fresh)) {
                    const auto inverse = core::conjugate(fresh.orientation);
                    impact.local_point = core::rotate(inverse, impact.world_point - fresh.position);
                    impact.local_normal = core::rotate(inverse, impact.world_normal);
                    impact.body = found.body;
                    state = fresh;
                    active = true;
                } else {
                    impact.age = kImpactLifetime; // the piece it was on left: retire the scar
                }
            }
        }
        core::Transform scar{};
        if (active) {
            const auto normal = core::rotate(state.orientation, impact.local_normal);
            impact.world_normal = normal;
            impact.world_point =
                state.position + core::rotate(state.orientation, impact.local_point);
            scar.translation = impact.world_point + normal * .002f;
            scar.rotation = along_z(normal) * core::quat_from_axis_angle({0, 0, 1}, impact.spin);
            // Quad is 2 cm; x10 gives a 20 cm stamp whose crater (0.5 x edge) is ~8 cm and whose
            // pale rim reaches ~16 cm: readable at 15 m (about 20 px at 1280x720).
            scar.scale = {10.0f, 10.0f, 1.0f};
        }
        show(world, impact.entity, active ? scar_ : render::kInvalidMeshId, scar);
    }
    for (auto& fragment : fragments_) {
        const bool active = fragment.age < fragment.life;
        core::Transform transform{};
        if (active) {
            const float t = fragment.age;
            transform.translation =
                fragment.origin + fragment.velocity * t + core::Vec3{0, -4.905f * t * t, 0};
            transform.rotation = along_z(core::normalize(fragment.velocity));
            const float fade = 1 - t / fragment.life;
            const float size = fragment.size * 100 * std::max(fade, .15f);
            transform.scale = {size,
                               size,
                               fragment.surface == Surface::Metal   ? size * 8
                               : fragment.surface == Surface::Water ? size * 4
                                                                    : size};
            auto* material = world.get<render::MaterialRef>(fragment.entity);
            material->material = surface_materials_[static_cast<std::size_t>(fragment.surface)];
            world.mark_changed<render::MaterialRef>(fragment.entity);
        }
        show(world, fragment.entity, active ? burst_ : render::kInvalidMeshId, transform);
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
        // The shared mesh already contains the 9.6 mm diameter and 26 mm length.
        show(world, casing.entity, active ? brass_ : render::kInvalidMeshId, transform);
    }
}

} // namespace rime::block_play
