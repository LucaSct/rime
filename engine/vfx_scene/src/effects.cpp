// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/vfx_scene/effects.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <tuple>

#include "rime/core/math/transform.hpp"
#include "rime/ecs/query.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/vfx_scene/components.hpp"

namespace rime::vfx_scene {
namespace {
bool finite(core::Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool equal(core::Vec3 a, core::Vec3 b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
} // namespace

SmokeAppearance smoke_appearance(const vfx::Particle& particle,
                                 std::span<const vfx::FireLightContribution> lights) noexcept {
    if (!finite(particle.position) || !std::isfinite(particle.age) ||
        !std::isfinite(particle.lifetime) || particle.lifetime <= 0.0f || particle.age < 0.0f ||
        particle.age >= particle.lifetime)
        return {};
    const auto smooth = [](float x) {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    };
    const float life = particle.age / particle.lifetime;
    SmokeAppearance result{{0.24f, 0.27f, 0.30f},
                           0.38f * smooth(particle.age / 0.15f) * smooth((1.0f - life) / 0.8f)};
    // A bounded single-scattering approximation, not a volume-lighting claim. The same fire
    // rows illuminate meshes through normal scene extraction; smoke only borrows their falloff.
    for (const auto& light : lights.first(std::min(lights.size(), Effects::max_sources))) {
        if (!finite(light.position) || !finite(light.color) || !std::isfinite(light.intensity) ||
            !std::isfinite(light.radius) || light.radius <= 0.0f || light.intensity <= 0.0f)
            continue;
        const core::Vec3 d = particle.position - light.position;
        const double distance_squared = double(d.x) * d.x + double(d.y) * d.y + double(d.z) * d.z;
        const double radius_squared = double(light.radius) * light.radius;
        const float attenuation =
            static_cast<float>(std::max(0.0, 1.0 - distance_squared / radius_squared));
        const float incident = 0.35f * std::min(light.intensity, 4.0f) * attenuation * attenuation;
        result.color.x += std::clamp(light.color.x, 0.0f, 1.0f) * incident;
        result.color.y += std::clamp(light.color.y, 0.0f, 1.0f) * incident;
        result.color.z += std::clamp(light.color.z, 0.0f, 1.0f) * incident;
    }
    result.color = {std::min(result.color.x, 1.5f),
                    std::min(result.color.y, 1.5f),
                    std::min(result.color.z, 1.5f)};
    return result;
}

Effects::Effects() {
    sources_.reserve(max_sources);
    next_sources_.reserve(max_sources);
}

void Effects::reconcile(ecs::World& world) {
    std::array<Description, max_sources> admitted{};
    std::size_t count = 0;
    const auto key = [](const Description& d) {
        return std::tuple{d.entity.index, d.entity.generation, d.fire};
    };
    const auto gather = [&](ecs::Entity e, const auto& component, bool fire) {
        if (!component.enabled)
            return;
        const auto* pose = world.get<ecs::WorldTransform>(e);
        const core::Vec3 half{component.half_x, component.half_y, component.half_z};
        if (!pose || !finite(half) || half.x < 0.0f || half.y < 0.0f || half.z < 0.0f ||
            !std::isfinite(component.particles_per_second) ||
            component.particles_per_second <= 0.0f || component.particles_per_second > 1024.0f ||
            !std::isfinite(component.intensity) || component.intensity < 0.0f ||
            component.intensity > 4.0f) {
            ++stats_.invalid_sources;
            return;
        }
        Description d{e, fire, {}, {}, component.particles_per_second, component.intensity};
        // Transforming every corner handles rotation, parent poses and negative scale without
        // pretending a local half-extent is already a world-space box. Reject overflow before
        // any emitter sees it; the established particle seam also requires representable bounds.
        for (unsigned corner = 0; corner < 8; ++corner) {
            const core::Vec3 p = core::transform_point(pose->value,
                                                       {corner & 1 ? half.x : -half.x,
                                                        corner & 2 ? half.y : -half.y,
                                                        corner & 4 ? half.z : -half.z});
            if (!finite(p)) {
                ++stats_.invalid_sources;
                return;
            }
            if (corner == 0) {
                d.lo = d.hi = p;
            } else {
                d.lo = {std::min(d.lo.x, p.x), std::min(d.lo.y, p.y), std::min(d.lo.z, p.z)};
                d.hi = {std::max(d.hi.x, p.x), std::max(d.hi.y, p.y), std::max(d.hi.z, p.z)};
            }
        }
        if (!finite(d.hi - d.lo) || !finite((d.lo + d.hi) * 0.5f)) {
            ++stats_.invalid_sources;
            return;
        }
        // Keep only the lowest bounded keys while scanning. Collect-then-sort would allocate
        // memory proportional to a malformed scene's source count before applying the cap.
        std::size_t at = 0;
        while (at < count && key(admitted[at]) < key(d))
            ++at;
        if (count == max_sources) {
            ++stats_.source_cap_drops;
            if (at == max_sources)
                return;
        }
        const std::size_t end = std::min(count, max_sources - 1);
        for (std::size_t i = end; i > at; --i)
            admitted[i] = admitted[i - 1];
        admitted[at] = d;
        count = std::min(count + 1, max_sources);
    };
    world.query<SmokeEmitter>().for_each(
        [&](ecs::Entity e, const SmokeEmitter& c) { gather(e, c, false); });
    world.query<FireEmitter>().for_each(
        [&](ecs::Entity e, const FireEmitter& c) { gather(e, c, true); });
    next_sources_.clear();
    for (const auto& old : sources_) {
        const bool retained =
            std::any_of(admitted.begin(), admitted.begin() + count, [&](const Description& d) {
                return key(old.description) == key(d) && old.description.rate == d.rate;
            });
        if (!retained)
            stats_.discarded_source_particles += old.description.fire
                                                     ? old.fire.smoke_stats().deferred_particles
                                                     : old.plume.stats().deferred_particles;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto& d = admitted[i];
        const auto old = std::find_if(sources_.begin(), sources_.end(), [&](const Source& s) {
            return key(s.description) == key(d);
        });
        if (old == sources_.end() || old->description.rate != d.rate) {
            next_sources_.emplace_back(d);
        } else {
            next_sources_.push_back(std::move(*old));
            auto& kept = next_sources_.back();
            if (d.fire && (!equal(kept.description.lo, d.lo) || !equal(kept.description.hi, d.hi) ||
                           kept.description.intensity != d.intensity))
                kept.light_current = false;
            // A moving source must retain fractional emission debt. Resetting its whole fire
            // adapter on every pose edit would prevent a 18/s source at 60 Hz ever emitting one
            // particle. Only extinguish deliberately discards that debt.
            if (d.intensity == 0.0f) {
                stats_.discarded_source_particles +=
                    d.fire ? kept.fire.smoke_stats().deferred_particles
                           : kept.plume.stats().deferred_particles;
                kept.fire.reset();
                kept.plume.reset();
                kept.light_current = false;
            }
            kept.description = d;
        }
    }
    sources_.swap(next_sources_);
    next_sources_.clear();
    collect_lights();
}

bool Effects::tick(ecs::World& world, std::uint64_t tick, float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f || dt > 0.25f || (has_tick_ && tick <= last_tick_)) {
        ++stats_.rejected_ticks;
        return false;
    }
    reconcile(world);
    field_.simulate(dt);
    for (auto& source : sources_) {
        const auto& d = source.description;
        const auto before = d.fire ? source.fire.smoke_stats() : source.plume.stats();
        bool ok = true;
        if (d.fire) {
            const std::uint64_t source_key =
                (std::uint64_t{d.entity.index} << 32) | d.entity.generation;
            ok = source.fire.consume(field_, {source_key, tick, d.lo, d.hi, d.intensity}, dt);
            source.light_current = ok;
        } else if (d.intensity == 0.0f) {
            source.plume.reset();
        } else {
            ok = source.plume.update(field_, d.lo, d.hi, dt, d.rate, d.intensity);
        }
        if (!ok)
            ++stats_.rejected_emissions;
        const auto& after = d.fire ? source.fire.smoke_stats() : source.plume.stats();
        stats_.emission_budget_dropped_particles +=
            after.dropped_particles - before.dropped_particles;
    }
    // Commit presentation time only after admitting the tick. Reconcile/render/Pause and a
    // refused tick cannot animate flame tongues or advance the smoke's age.
    presentation_seconds_ += dt;
    collect_lights();
    has_tick_ = true;
    if (light_count_ != 0)
        ++stats_.lit_ticks;
    last_tick_ = tick;
    ++stats_.accepted_ticks;
    return true;
}

void Effects::collect_lights() {
    light_count_ = 0;
    flame_count_ = 0;
    stats_.deferred_particles = 0;
    for (const auto& source : sources_) {
        stats_.deferred_particles += source.description.fire
                                         ? source.fire.smoke_stats().deferred_particles
                                         : source.plume.stats().deferred_particles;
        if (source.description.fire && source.light_current)
            if (const auto light = source.fire.light()) {
                lights_[light_count_++] = *light;
                const auto& d = source.description;
                const core::Vec3 extent = d.hi - d.lo;
                const float size =
                    std::clamp(0.20f + 0.35f * std::max(extent.x, extent.z), 0.12f, 1.2f) *
                    std::sqrt(d.intensity);
                // Three offset tongues share a source, instead of emitting one flame for every
                // smoke particle. The hard 16-source admission bounds this at 48 sprites. Phase
                // uses accepted simulation duration and stable source identity, never frame time.
                //
                // Why the motion looks like fire rather than a breathing sprite: real flames are
                // a few independent buoyant lobes that surge and retire at unrelated rates, not
                // one body pulsing in lockstep. Each tongue therefore sums two sinusoids whose
                // frequencies are different INTEGERS of the 8 s loop. Integer harmonics return to
                // their start value and slope exactly when the loop wraps, so there is no seam,
                // and unrelated harmonics rarely align, which reads as irregular without any
                // random stream. Tongue 0 is the anchored, luminous root (small amplitude, slow);
                // the two side tongues are faster and swing further, so they rise and retire
                // around a steadier core. Everything is a smooth function of the loop angle:
                // no sawtooth, no per-frame noise, no teleporting.
                const std::uint32_t id = d.entity.index * 37u + d.entity.generation * 17u;
                const float tau = 2.0f * std::numbers::pi_v<float>;
                const float seed = static_cast<float>(id % 101u) / 101.0f;
                const float seed_b = static_cast<float>((id * 29u + 13u) % 97u) / 97.0f;
                const float time =
                    static_cast<float>(std::fmod(presentation_seconds_, 8.0)) * (tau / 8.0f);
                // {surge harmonics A/B, sway harmonics A/B} per tongue. A tongue's surge and
                // sway use different harmonics so its height and lean do not move in step.
                static constexpr float kSurgeA[3] = {4.0f, 7.0f, 6.0f};
                static constexpr float kSurgeB[3] = {9.0f, 13.0f, 11.0f};
                static constexpr float kSwayA[3] = {3.0f, 5.0f, 4.0f};
                static constexpr float kSwayB[3] = {8.0f, 10.0f, 9.0f};
                for (unsigned tongue = 0; tongue < 3; ++tongue) {
                    const bool root = tongue == 0;
                    const float offset = float(tongue) * 2.1f;
                    // Surge in [-1, 1]; `lobe` is the same signal remapped to [0, 1].
                    const float surge =
                        0.65f * std::sin(time * kSurgeA[tongue] + seed * tau + offset) +
                        0.35f * std::sin(time * kSurgeB[tongue] + seed_b * tau + offset * 1.7f);
                    const float lobe = 0.5f + 0.5f * surge;
                    const float width =
                        size * (root ? 1.0f : 0.7f) * (1.0f + (root ? 0.10f : 0.26f) * surge);
                    // Lift is quadratic in the lobe: a tongue spends most of its time low and
                    // only occasionally darts up, which is how a flame tip behaves.
                    const float lift = size * (root ? 0.05f : 0.30f) * lobe * lobe;
                    const float sway_a = time * kSwayA[tongue] + seed_b * tau + offset;
                    const float sway_b = time * kSwayB[tongue] + seed * tau;
                    const float lean = size * (root ? 0.10f : 0.26f);
                    const core::Vec3 position{light->position.x + lean * (0.8f * std::sin(sway_a) +
                                                                          0.4f * std::sin(sway_b)),
                                              d.lo.y + width * 1.35f + lift,
                                              light->position.z + lean * (0.7f * std::cos(sway_a) +
                                                                          0.3f * std::cos(sway_b))};
                    // The root stays bright (anchored radiance); a side tongue dims as it
                    // retires and flares as it rises. Heat shifts the hue between warm orange and
                    // pale yellow-white without leaving the established palette.
                    const float heat = root ? 0.9f + 0.1f * lobe : 0.7f + 0.3f * lobe;
                    const float body = root ? 0.85f + 0.15f * lobe : 0.45f + 0.55f * lobe;
                    flames_[flame_count_++] = {
                        position,
                        width,
                        {2.8f, 2.2f * (0.88f + 0.12f * heat), 1.4f * (0.75f + 0.25f * heat)},
                        std::clamp(std::min(d.intensity, 1.0f) * (root ? 0.65f : 0.45f) * body,
                                   0.0f,
                                   1.0f)};
                }
            }
    }
}

void Effects::reset() {
    stats_.discarded_source_particles += stats_.deferred_particles;
    sources_.clear();
    next_sources_.clear();
    field_.clear();
    light_count_ = 0;
    flame_count_ = 0;
    presentation_seconds_ = 0.0;
    stats_.deferred_particles = 0;
    has_tick_ = false;
    last_tick_ = 0;
}

} // namespace rime::vfx_scene
