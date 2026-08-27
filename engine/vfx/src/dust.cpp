// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/vfx/dust.hpp"

#include <algorithm>
#include <cmath>

// The dust stub's simulation. Everything is deterministic — a SplitMix64 stream scatters the
// particles, and simulate() is a plain per-particle integration — so the field is a pure function
// of its emit_burst/simulate call sequence. The numbers (per-burst count, sizes, speeds, lifetimes)
// are tuned to LOOK like settling concrete dust, not measured against anything; fx1 discards them.
namespace rime::vfx {

namespace {

// SplitMix64: advance the state, avalanche-mix it to 64 well-distributed bits. Same generator as
// the fracture cook (Rust side) — chosen for being tiny, seedable, and identical across platforms.
[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

} // namespace

// ── The three families (m13.1b, ADR-0035 §5) ─────────────────────────────────────────────────
//
// Every number here is cosmetic: tuned to look right, measured against nothing. What is NOT
// arbitrary is that the three are distinguishable in ways a test can name — lifetime, direction of
// gravity, and growth — because "three families" is only a real feature if they behave differently,
// and a proof that three identically-parameterised puffs all work proves one family three times.

const EmitParams& impact_dust() noexcept {
    // The M8.4 original, unchanged: a puff at a break that expands outward, lifts a little, then
    // settles under a light gravity — cosmetic haze meant to hang and fade, not to fall like
    // rubble, so nothing here is 9.81.
    static const EmitParams kParams{/*count=*/48,
                                    /*speed=*/1.0f,
                                    /*lift=*/0.4f,
                                    /*gravity=*/1.5f,
                                    /*min_lifetime=*/0.8f,
                                    /*max_lifetime=*/1.6f,
                                    /*min_size=*/0.02f,
                                    /*max_size=*/0.06f,
                                    /*growth=*/0.0f,
                                    Family::ImpactDust};
    return kParams;
}

const EmitParams& lingering_smoke() noexcept {
    // Fewer, slower, bigger, and it RISES — a negative gravity, which is the whole visual
    // difference between smoke and dust. It also GROWS: a dissipating cloud gets larger and
    // fainter at once, and without growth a long-lived puff just fades in place, which reads as a
    // bug rather than as smoke. Lives ~5x as long as dust, which is what "lingering" means.
    static const EmitParams kParams{/*count=*/18,
                                    /*speed=*/0.35f,
                                    /*lift=*/0.5f,
                                    /*gravity=*/-0.6f,
                                    /*min_lifetime=*/4.0f,
                                    /*max_lifetime=*/7.0f,
                                    /*min_size=*/0.10f,
                                    /*max_size=*/0.22f,
                                    /*growth=*/0.09f,
                                    Family::LingeringSmoke};
    return kParams;
}

const EmitParams& muzzle_flash() noexcept {
    // A handful of very fast, very short-lived sparks. Under a tenth of a second, because a flash
    // that outlives the shot reads as a flare; and no gravity at all, because nothing falls in
    // 60 ms and pretending otherwise costs an integration for an effect nobody can see.
    static const EmitParams kParams{/*count=*/10,
                                    /*speed=*/6.0f,
                                    /*lift=*/0.0f,
                                    /*gravity=*/0.0f,
                                    /*min_lifetime=*/0.04f,
                                    /*max_lifetime=*/0.09f,
                                    /*min_size=*/0.03f,
                                    /*max_size=*/0.07f,
                                    /*growth=*/0.0f,
                                    Family::MuzzleFlash};
    return kParams;
}

ParticleField::ParticleField(std::uint32_t max_particles, std::uint64_t seed)
    : rng_(seed), max_(max_particles) {
    particles_.reserve(max_particles);
}

float ParticleField::next_unit() noexcept {
    // Top 24 bits → a float in [0, 1) with 2^-24 resolution (the standard trick: a 24-bit mantissa
    // is all a float can hold, so drawing more bits would be wasted).
    return static_cast<float>(splitmix64(rng_) >> 40) * (1.0f / 16777216.0f);
}

void ParticleField::emit_burst(core::Vec3 bounds_min, core::Vec3 bounds_max, float intensity) {
    emit_burst(bounds_min, bounds_max, intensity, impact_dust());
}

void ParticleField::emit_burst(core::Vec3 bounds_min,
                               core::Vec3 bounds_max,
                               float intensity,
                               const EmitParams& params) {
    if (!(intensity > 0.0f)) {
        return;
    }
    if (bounds_max.x < bounds_min.x || bounds_max.y < bounds_min.y || bounds_max.z < bounds_min.z) {
        return; // inverted/empty box — nothing to fill
    }
    const core::Vec3 center{0.5f * (bounds_min.x + bounds_max.x),
                            0.5f * (bounds_min.y + bounds_max.y),
                            0.5f * (bounds_min.z + bounds_max.z)};

    const float clamped_intensity =
        std::min(intensity, 4.0f); // one break can't drain the whole pool
    std::uint32_t wanted =
        static_cast<std::uint32_t>(static_cast<float>(params.count) * clamped_intensity);
    const std::uint32_t room = max_ - static_cast<std::uint32_t>(particles_.size());
    wanted = std::min(wanted, room);

    for (std::uint32_t i = 0; i < wanted; ++i) {
        Particle p;
        // Position: uniformly inside the break's box.
        p.position = core::Vec3{bounds_min.x + next_unit() * (bounds_max.x - bounds_min.x),
                                bounds_min.y + next_unit() * (bounds_max.y - bounds_min.y),
                                bounds_min.z + next_unit() * (bounds_max.z - bounds_min.z)};
        // Velocity: outward from the centre (so the puff expands) plus a puff of lift, scaled by
        // intensity. next_unit()·2−1 maps [0,1) to [−1,1) for a signed jitter.
        const core::Vec3 out = p.position - center;
        const float speed = (0.5f + 1.5f * clamped_intensity) * params.speed;
        p.velocity = core::Vec3{out.x * speed + (next_unit() * 2.0f - 1.0f) * 0.3f * params.speed,
                                std::abs(out.y) * speed + params.lift + next_unit() * 0.6f,
                                out.z * speed + (next_unit() * 2.0f - 1.0f) * 0.3f * params.speed};
        p.size = params.min_size + next_unit() * (params.max_size - params.min_size);
        p.age = 0.0f;
        p.lifetime =
            params.min_lifetime + next_unit() * (params.max_lifetime - params.min_lifetime);
        p.gravity = params.gravity;
        p.growth = params.growth;
        p.family = params.family;
        particles_.push_back(p);
    }
}

void ParticleField::simulate(float dt) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < particles_.size(); ++read) {
        Particle p = particles_[read];
        p.age += dt;
        if (p.age >= p.lifetime) {
            continue; // retired — dropped by not copying it forward
        }
        // PER-PARTICLE gravity and growth, which is what lets one field hold three families at
        // once: a negative gravity rises (smoke), zero neither rises nor falls (a flash lives 60 ms
        // and would not visibly move anyway), and growth expands a puff as it dissipates.
        p.velocity.y -= p.gravity * dt;
        p.position = p.position + p.velocity * dt;
        p.size += p.growth * dt;
        particles_[write++] = p; // stable compaction: survivors keep their relative order
    }
    particles_.resize(write);
}

std::span<const Particle> ParticleField::particles() const noexcept {
    return {particles_.data(), particles_.size()};
}

float ParticleField::coverage() const noexcept {
    float sum = 0.0f;
    for (const Particle& p : particles_) {
        const float alpha = p.lifetime > 0.0f ? std::max(0.0f, 1.0f - p.age / p.lifetime) : 0.0f;
        sum += p.size * p.size * alpha;
    }
    return sum;
}

void ParticleField::clear() noexcept {
    particles_.clear();
}

} // namespace rime::vfx
