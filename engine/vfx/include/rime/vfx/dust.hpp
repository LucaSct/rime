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

// Which effect a particle belongs to (m13.1b). Three, because ADR-0035 §5's fx1a names three.
//
// The SIMULATION is what differs — how fast they leave, how long they last, whether they fall or
// rise, whether they grow. What a family LOOKS like (its colour, and one day its atlas frame) is
// the consumer's: this module has no notion of colour, and giving it one would make a deletable
// stub start owning art direction.
enum class Family : std::uint8_t {
    ImpactDust = 0,     // the M8.4 original: a puff at a break that expands, then settles
    LingeringSmoke = 1, // slower, larger, rises, and hangs around long after the dust is gone
    MuzzleFlash = 2,    // a handful of very fast, very short-lived sparks at a gun's muzzle
};

// One billboard. A camera-facing quad of half-size `size` at `position`, drifting along `velocity`,
// `age` seconds into a `lifetime`-second existence (retired at age >= lifetime). The render side
// (`render::FxParticlePass`, m13.1a) reads these and draws additive, fading by the same
// age/lifetime the coverage proxy below uses.
//
// `gravity` and `growth` are PER PARTICLE rather than per field, and that is what lets one field
// hold all three families at once: smoke rising while dust settles in the same puff is the common
// case, not an edge one, and a per-field constant would force a field per family and a draw per
// field.
struct Particle {
    core::Vec3 position{0.0f, 0.0f, 0.0f};
    core::Vec3 velocity{0.0f, 0.0f, 0.0f};
    float size = 0.0f;     // billboard half-size, metres
    float age = 0.0f;      // seconds since birth
    float lifetime = 0.0f; // seconds; retired once age reaches it
    float gravity = 0.0f;  // m/s² applied to velocity.y (NEGATIVE rises — smoke does)
    float growth = 0.0f;   // m/s the half-size grows; smoke expands as it dissipates
    Family family = Family::ImpactDust;
};

// How one family's burst is authored. Every number is cosmetic — tuned to look right, measured
// against nothing — and fx1b's compute sim inherits the shapes rather than the values.
struct EmitParams {
    std::uint32_t count = 48;  // particles at intensity 1, before the budget clamp
    float speed = 1.0f;        // multiplier on the outward-from-centre velocity
    float lift = 0.4f;         // extra upward velocity at birth, m/s
    float gravity = 1.5f;      // m/s² downward; negative to rise
    float min_lifetime = 0.8f; // s
    float max_lifetime = 1.6f; // s
    float min_size = 0.02f;    // m half-size
    float max_size = 0.06f;    // m half-size
    float growth = 0.0f;       // m/s of half-size growth
    Family family = Family::ImpactDust;
};

// The three authored families. Free functions rather than constants so the numbers live beside
// their reasoning in dust.cpp, where a reader looking for "why is smoke like that" will be.
[[nodiscard]] const EmitParams& impact_dust() noexcept;
[[nodiscard]] const EmitParams& lingering_smoke() noexcept;
[[nodiscard]] const EmitParams& muzzle_flash() noexcept;

// A capped pool of dust fed by destruction events. `emit_burst` blooms a puff filling a world-space
// box (a broken part or island's AABB); `simulate` drifts, ages, and retires. Deterministic given
// the same calls — a fixed SplitMix64 stream drives the scatter — so two fields fed the same events
// hold identical particles. The budget is a hard cap: a demolition storm drops the overflow rather
// than growing without bound (m8.5's budget discipline, in miniature).
class ParticleField {
public:
    // Default budget 200 particles (the plan's "~200 CPU billboards"); `seed` fixes the scatter.
    explicit ParticleField(std::uint32_t max_particles = 200,
                           std::uint64_t seed = 0x9E3779B97F4A7C15ull);

    // Bloom a puff for a break of `intensity` (~0..1+; scales particle count and initial speed)
    // filling the world box [bounds_min, bounds_max]. Spawns into the remaining budget and silently
    // drops the rest — never exceeds capacity(). A non-positive intensity or inverted box is a
    // no-op.
    //
    // This spelling emits IMPACT DUST — the M8.4 behaviour, unchanged, so every caller written
    // before families existed still means exactly what it meant.
    void emit_burst(core::Vec3 bounds_min, core::Vec3 bounds_max, float intensity);

    // …and this one names the family. A muzzle flash is the degenerate case worth knowing about:
    // pass a box of zero extent at the muzzle and the sparks all start at one point, which is what
    // a flash is.
    void emit_burst(core::Vec3 bounds_min,
                    core::Vec3 bounds_max,
                    float intensity,
                    const EmitParams& params);

    // Advance every particle by `dt` seconds (drift + a light settling gravity), aging them and
    // retiring those past their lifetime. Order among survivors is preserved (a stable compaction),
    // so particles() stays reproducible.
    void simulate(float dt);

    [[nodiscard]] std::span<const Particle> particles() const noexcept;

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
    std::vector<Particle> particles_;
};

// The M8.4 names, kept so every caller and test written before families existed compiles
// unchanged. `DustField` was accurate while dust was the only family; it stopped being accurate at
// m13.1b, and a class that emits muzzle flashes should not be called a dust field. Renaming the
// type and aliasing the old name costs nothing and keeps the tree honest — which is cheaper than
// the alternative, where the name quietly stops describing the thing.
using DustParticle = Particle;
using DustField = ParticleField;

} // namespace rime::vfx
