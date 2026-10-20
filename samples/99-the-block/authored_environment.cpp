// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "authored_environment.hpp"

#include <algorithm>
#include <cmath>

#include "play_effects.hpp"
#include "rime/ecs/query.hpp"
#include "rime/vfx_scene/components.hpp"

namespace rime::block_play {
static_assert(AuthoredEnvironment::legacy_smoke_capacity == PlayEffects::kParticleCapacity,
              "authored/legacy composition must track the existing scripted pool bound");

AuthoredEnvironment::~AuthoredEnvironment() {
    report(stderr); // one end-of-session receipt, never a per-render log
}

bool AuthoredEnvironment::tick(ecs::World& world, std::uint64_t tick, double dt) {
    // Independent cosmetic consumers: a malformed thermal graph cannot suppress an otherwise
    // valid fire. Each consumer owns its refusal/duplicate-tick guard and observable counters.
    // Validate in the caller's double precision before narrowing: 0.250000001 must not round
    // down into an accepted FX duration while the thermal consumer correctly refuses it.
    const bool valid_dt = std::isfinite(dt) && dt > 0 && dt <= .25;
    const bool fx = effects_.tick(world, tick, valid_dt ? static_cast<float>(dt) : -1.0f);
    const bool thermal = thermal_.tick(world, tick, dt);
    return fx && thermal;
}

const AuthoredEnvironment::Snapshot&
AuthoredEnvironment::prepare(ecs::World& world, std::span<const render::GpuParticle> legacy) {
    effects_.reconcile(world);
    thermal_.reconcile(world);
    snapshot_ = {};
    world.query<vfx_scene::SmokeEmitter>().for_each([&](const vfx_scene::SmokeEmitter& c) {
        snapshot_.enabled_sources += c.enabled ? 1u : 0u;
    });
    world.query<vfx_scene::FireEmitter>().for_each(
        [&](const vfx_scene::FireEmitter& c) { snapshot_.enabled_sources += c.enabled ? 1u : 0u; });
    const bool authored_smoke = effects_.source_count() != 0 || effects_.field().count() != 0;
    snapshot_.override_smoke = authored_smoke || owned_smoke_;
    owned_smoke_ = authored_smoke;
    if (snapshot_.override_smoke) {
        const std::size_t count = std::min(legacy.size(), legacy_smoke_capacity);
        std::copy_n(legacy.begin(), count, snapshot_.smoke.begin());
        snapshot_.smoke_count = count;
        legacy_dropped_ += legacy.size() - count;
        for (const auto& particle : effects_.field().particles()) {
            const auto appearance = vfx_scene::smoke_appearance(particle, effects_.lights());
            snapshot_.smoke[snapshot_.smoke_count++] = {
                {particle.position.x, particle.position.y, particle.position.z, particle.size},
                {appearance.color.x, appearance.color.y, appearance.color.z, appearance.opacity}};
        }
    }
    for (const auto& flame : effects_.render_flames())
        snapshot_.flames[snapshot_.flame_count++] = {
            {flame.position.x, flame.position.y, flame.position.z, flame.half_size},
            // A flame-only alpha tag selects the broad core without changing the shared GPU ABI.
            {flame.color.x,
             flame.color.y,
             flame.color.z,
             flame.opacity + (flame.core ? 2.0f : 0.0f)}};
    std::size_t fire_index = 0;
    for (const auto& light : effects_.lights()) {
        // The scene light borrows the same smoothly moving tongue amplitudes as the flame.
        // Small modulation and centimetre-scale drift give ground illumination life without
        // an independent random flicker that could flash when the visible flame is quiet.
        const auto tongues = effects_.flames();
        const auto& root = tongues[fire_index * 3];
        const auto& side = tongues[fire_index * 3 + 1];
        const float modulation =
            std::clamp(0.86f + 0.15f * root.opacity + 0.12f * side.opacity, 0.86f, 1.12f);
        const core::Vec3 light_position{
            light.position.x + 0.16f * (side.position.x - light.position.x),
            light.position.y + 0.28f,
            light.position.z + 0.16f * (side.position.z - light.position.z)};
        // Raising the emitter slightly gives the ground a useful incidence angle; a bounded
        // wider radius lights the fuel surround instead of a tiny point underneath it. Radiance
        // is presentation-only so the authored intensity and smoke lighting remain unchanged.
        const float radius = std::min(8.0f, light.radius * 2.5f);
        const float radiance = light.intensity * modulation * 4.0f;
        snapshot_.lights[snapshot_.light_count++] = {
            {light_position.x, light_position.y, light_position.z, radius},
            {light.color.x * radiance, light.color.y * radiance, light.color.z * radiance, 0}};
        ++fire_index;
    }

    // Validation and committed-state mapping belong to the shared thermal adapter. Do not clamp
    // bad authored energy, or allocate 266 textures merely because an inert component exists.
    snapshot_.surfaces = fluids_render::phase_surfaces(world, thermal_);
    if (snapshot_.surfaces.count != 0 && device_ && !detail_)
        detail_ = std::make_unique<fluids_render::PhaseDetail>(*device_);
    if (detail_ && snapshot_.surfaces.count != 0)
        snapshot_.surfaces = detail_->surfaces(world, thermal_);
    return snapshot_;
}

void AuthoredEnvironment::publish(ecs::World& world,
                                  std::span<const render::GpuParticle> legacy,
                                  render::SceneRenderer& main,
                                  render::SceneRenderer* scope) {
    (void)prepare(world, legacy);
    apply_to(main);
    if (scope)
        apply_to(*scope);
}

void AuthoredEnvironment::apply_to(render::SceneRenderer& renderer) const {
    if (snapshot_.override_smoke) {
        render::FxSettings smoke{};
        smoke.enabled = snapshot_.smoke_count != 0;
        smoke.max_particles = smoke_capacity;
        renderer.set_smoke_particles(snapshot_.smoke_span(), smoke);
    }
    render::FxSettings flame{};
    flame.enabled = snapshot_.flame_count != 0;
    flame.max_particles = vfx_scene::Effects::max_flames;
    renderer.set_flame_particles({snapshot_.flames.data(), snapshot_.flame_count}, flame);
    renderer.set_transient_point_lights({snapshot_.lights.data(), snapshot_.light_count});
    (void)renderer.set_surface_appearances(snapshot_.surfaces.span());
}

void AuthoredEnvironment::reset() {
    effects_.reset();
    thermal_.reset();
    const bool must_clear = owned_smoke_ || snapshot_.override_smoke;
    snapshot_ = {};
    snapshot_.override_smoke = must_clear;
    owned_smoke_ = must_clear;
    // Retain immutable detail storage: renderers may still hold copied borrowed texture handles.
    // A subsequent publication clears their rows; teardown releases the atlas after renderers.
}

void AuthoredEnvironment::report(std::FILE* output) const {
    std::fprintf(output,
                 "99-the-block: authored environment (cosmetic) enabled_sources=%zu sources=%zu "
                 "smoke=%zu flames=%zu "
                 "lights=%zu phase_surfaces=%zu phase_cells=%zu thermal_status=%u atlas=%u "
                 "fx_ticks=%llu thermal_ticks=%llu invalid_source_visits=%llu "
                 "invalid_thermal_builds=%llu thermal_refusals=%llu\n",
                 snapshot_.enabled_sources,
                 effects_.source_count(),
                 effects_.field().count(),
                 snapshot_.flame_count,
                 snapshot_.light_count,
                 snapshot_.surfaces.count,
                 thermal_.entities().size(),
                 static_cast<unsigned>(thermal_.status()),
                 detail_ready() ? 1u : 0u,
                 static_cast<unsigned long long>(effects_.stats().accepted_ticks),
                 static_cast<unsigned long long>(thermal_.stats().accepted_ticks),
                 static_cast<unsigned long long>(effects_.stats().invalid_sources),
                 static_cast<unsigned long long>(thermal_.stats().invalid_builds),
                 static_cast<unsigned long long>(thermal_.stats().refused_ticks));
}
} // namespace rime::block_play
