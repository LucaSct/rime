// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/core/math/vec.hpp"

// engine/vfx — the destruction dust STUB (M8.4). A deliberately small, deletable CPU particle field
// that turns destruction events into visible feedback: when a part breaks or an island detaches, a
// puff of billboard dust blooms at the break and drifts away. It is a STUB in the honest sense —
// the real GPU-driven FX system (track fx1) replaces this whole module, and the actual additive
// draw pass + its coverage-delta pixel proof land with the M8.6 sample where a device/render path
// exists (the same GPU-free discipline M8.2/M8.3 followed). What lives here is the SIMULATION:
// spawn, drift, age, retire, capped at a fixed budget — deterministic, so it is unit-testable on
// lavapipe/CI with no device, and so the M8.6 pass has real, reproducible data to draw.
//
// Removable feature module (guardrail 2): depends on core only, and nothing depends on it. It is
// not wired to destruction — the fan-out glue (a DestructionEvent → emit_burst) lives in the
// consumer, so destruction never learns vfx exists.
namespace rime::vfx {

// One dust billboard. A camera-facing quad of half-size `size` at `position`, drifting along
// `velocity`, `age` seconds into a `lifetime`-second existence (retired at age >= lifetime). The
// render side (m8.6) reads these and draws additive, fading by the same age/lifetime the coverage
// proxy below uses.
struct DustParticle {
    core::Vec3 position{0.0f, 0.0f, 0.0f};
    core::Vec3 velocity{0.0f, 0.0f, 0.0f};
    float size = 0.0f;     // billboard half-size, metres
    float age = 0.0f;      // seconds since birth
    float lifetime = 0.0f; // seconds; retired once age reaches it
};

// A capped pool of dust fed by destruction events. `emit_burst` blooms a puff filling a world-space
// box (a broken part or island's AABB); `simulate` drifts, ages, and retires. Deterministic given
// the same calls — a fixed SplitMix64 stream drives the scatter — so two fields fed the same events
// hold identical particles. The budget is a hard cap: a demolition storm drops the overflow rather
// than growing without bound (m8.5's budget discipline, in miniature).
class DustField {
public:
    // Default budget 200 particles (the plan's "~200 CPU billboards"); `seed` fixes the scatter.
    explicit DustField(std::uint32_t max_particles = 200,
                       std::uint64_t seed = 0x9E3779B97F4A7C15ull);

    // Bloom a puff for a break of `intensity` (~0..1+; scales particle count and initial speed)
    // filling the world box [bounds_min, bounds_max]. Spawns into the remaining budget and silently
    // drops the rest — never exceeds capacity(). A non-positive intensity or inverted box is a
    // no-op.
    void emit_burst(core::Vec3 bounds_min, core::Vec3 bounds_max, float intensity);

    // Advance every particle by `dt` seconds (drift + a light settling gravity), aging them and
    // retiring those past their lifetime. Order among survivors is preserved (a stable compaction),
    // so particles() stays reproducible.
    void simulate(float dt);

    [[nodiscard]] std::span<const DustParticle> particles() const noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return particles_.size(); }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return max_; }

    // A cheap coverage proxy: Σ size²·alpha over live particles, alpha = 1 − age/lifetime. This is
    // the scalar the m8.6 GPU pass's coverage-delta pixel test confirms on screen — it JUMPS on a
    // burst (fresh particles at alpha≈1) and DECAYS to 0 as the puff ages out.
    // Screen-space-agnostic (no camera here), it is the CPU witness the visual feedback exists and
    // then fades.
    [[nodiscard]] float coverage() const noexcept;

    void clear() noexcept;

private:
    // SplitMix64 → a uniform float in [0, 1). A tiny, well-distributed, fully deterministic stream
    // (the same generator the fracture cook uses on the Rust side) — reproducibility, not crypto.
    [[nodiscard]] float next_unit() noexcept;

    std::uint64_t rng_;
    std::uint32_t max_;
    std::vector<DustParticle> particles_;
};

} // namespace rime::vfx
