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

// At intensity 1, a burst spawns this many particles (before the budget clamp). "~200 billboards"
// total across a few simultaneous breaks (the plan's figure) falls out of this with the 200 cap.
constexpr std::uint32_t kParticlesPerBurst = 48;

// Dust drifts up and out of the break, then settles gently — a light gravity, not the physics
// world's 9.81 (this is cosmetic haze, meant to hang and fade, not fall like rubble).
constexpr float kDustGravity = 1.5f; // m/s², downward

constexpr float kMinLifetime = 0.8f; // s
constexpr float kMaxLifetime = 1.6f; // s
constexpr float kMinSize = 0.02f;    // m half-size
constexpr float kMaxSize = 0.06f;    // m half-size

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

DustField::DustField(std::uint32_t max_particles, std::uint64_t seed)
    : rng_(seed), max_(max_particles) {
    particles_.reserve(max_particles);
}

float DustField::next_unit() noexcept {
    // Top 24 bits → a float in [0, 1) with 2^-24 resolution (the standard trick: a 24-bit mantissa
    // is all a float can hold, so drawing more bits would be wasted).
    return static_cast<float>(splitmix64(rng_) >> 40) * (1.0f / 16777216.0f);
}

void DustField::emit_burst(core::Vec3 bounds_min, core::Vec3 bounds_max, float intensity) {
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
        static_cast<std::uint32_t>(static_cast<float>(kParticlesPerBurst) * clamped_intensity);
    const std::uint32_t room = max_ - static_cast<std::uint32_t>(particles_.size());
    wanted = std::min(wanted, room);

    for (std::uint32_t i = 0; i < wanted; ++i) {
        DustParticle p;
        // Position: uniformly inside the break's box.
        p.position = core::Vec3{bounds_min.x + next_unit() * (bounds_max.x - bounds_min.x),
                                bounds_min.y + next_unit() * (bounds_max.y - bounds_min.y),
                                bounds_min.z + next_unit() * (bounds_max.z - bounds_min.z)};
        // Velocity: outward from the centre (so the puff expands) plus a puff of lift, scaled by
        // intensity. next_unit()·2−1 maps [0,1) to [−1,1) for a signed jitter.
        const core::Vec3 out = p.position - center;
        const float speed = 0.5f + 1.5f * clamped_intensity;
        p.velocity = core::Vec3{out.x * speed + (next_unit() * 2.0f - 1.0f) * 0.3f,
                                std::abs(out.y) * speed + 0.4f + next_unit() * 0.6f,
                                out.z * speed + (next_unit() * 2.0f - 1.0f) * 0.3f};
        p.size = kMinSize + next_unit() * (kMaxSize - kMinSize);
        p.age = 0.0f;
        p.lifetime = kMinLifetime + next_unit() * (kMaxLifetime - kMinLifetime);
        particles_.push_back(p);
    }
}

void DustField::simulate(float dt) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < particles_.size(); ++read) {
        DustParticle p = particles_[read];
        p.age += dt;
        if (p.age >= p.lifetime) {
            continue; // retired — dropped by not copying it forward
        }
        p.velocity.y -= kDustGravity * dt;
        p.position = p.position + p.velocity * dt;
        particles_[write++] = p; // stable compaction: survivors keep their relative order
    }
    particles_.resize(write);
}

std::span<const DustParticle> DustField::particles() const noexcept {
    return {particles_.data(), particles_.size()};
}

float DustField::coverage() const noexcept {
    float sum = 0.0f;
    for (const DustParticle& p : particles_) {
        const float alpha = p.lifetime > 0.0f ? std::max(0.0f, 1.0f - p.age / p.lifetime) : 0.0f;
        sum += p.size * p.size * alpha;
    }
    return sum;
}

void DustField::clear() noexcept {
    particles_.clear();
}

} // namespace rime::vfx
