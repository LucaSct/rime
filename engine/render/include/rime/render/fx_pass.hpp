// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/render_graph.hpp"

// The FX particle draw pass (m13.1a — Track FX brick fx1a, ADR-0035 §5).
//
// `engine/vfx` has simulated deterministic CPU billboards since M8.4 and nothing has ever DRAWN
// them. The M8.6 sample fed the field from destruction events and self-checked its `coverage()`
// proxy — a CPU number standing in for pixels that did not exist. This pass is the pixels, and the
// coverage-delta proof M8.6 deferred lands with it.
//
// ── OFF IS A BYTE-IDENTICAL BASELINE ──────────────────────────────────────────────────────────
//
// ADR-0035 §5 asks for "the `LightingSettings` gate discipline applied to FX", and ADR-0032 §11 is
// what that means: with the toggle off, the frame is not *nearly* the same as before this pass
// existed — it is the same bytes. That is enforced structurally rather than by care: with
// `FxSettings::enabled` false the pass is never declared into the graph at all, so there is no
// pipeline bind, no barrier, and no attachment load to perturb. A pass that ran and drew zero
// particles would be *almost* free and *almost* identical, and "almost" is exactly what a
// regression bridge cannot be built on.
//
// ── WHERE IT SITS IN THE FRAME ────────────────────────────────────────────────────────────────
//
// After the forward-PBR pass, into the same HDR target, before tonemap. Additive blending in HDR
// is the whole reason: a muzzle flash is *radiance*, so it should blow out through the tonemapper
// like any other bright thing, and adding it after tonemap would clamp it to the LDR range and
// make it look like paint. It reads depth (so a puff behind a wall is occluded) and does NOT write
// it (so particles never occlude each other or anything else) — the standard translucent-overlay
// depth policy the RHI's `depth_write = false` exists for.
//
// ── WHAT IS NOT HERE, AND WHY ─────────────────────────────────────────────────────────────────
//
//   * **No sorting.** Additive blending is commutative, so back-to-front ordering buys nothing;
//     this is the one blend mode where the classic transparency problem does not arise. The moment
//     a family wants alpha blending (smoke does, eventually) sorting becomes real work and its own
//     decision.
//   * **No texture atlas.** The billboard's shape is computed in the fragment stage — see
//     particle.frag. Cooking, streaming and binding an FX atlas is its own brick.
//   * **No GPU simulation.** The sim stays on the CPU and deterministic, which is what keeps it
//     replayable and unit-testable with no device. fx1b moves it to compute *if and only if* the
//     work ledger shows the CPU sim binding — ADR-0035 §5 makes that contingent on measurement,
//     not on appetite.
namespace rime::render {

// The FX gate. One struct, hung off the same discipline as LightingSettings, and deliberately a
// separate struct from it: FX and lighting are independent features and a game that wants one
// without the other should not have to reason about a shared toggle block.
struct FxSettings {
    // Off by default. Until a caller opts in, the frame is byte-identical to the pre-fx1a frame —
    // which is the regression bridge every M5/M6/M10 pixel proof stands on.
    bool enabled = false;

    // A global multiplier on every particle's radiance, for authoring and for the proof's negative
    // control. 0 draws the pass with nothing visible, which is NOT the same as `enabled = false`:
    // the pass still runs, still binds, still blends. Keeping the two distinguishable is what lets
    // a test tell "the gate is off" from "the gate is on and the content is dark".
    float intensity = 1.0f;

    // The draw budget. Particles past it are not drawn — the CPU sim has its own cap (vfx's
    // DustField), and this is the render side's independent bound so a mis-sized field cannot
    // turn into an unbounded draw. Exceeding it is counted, never silent.
    std::uint32_t max_particles = 4096;
};

// One billboard, as the GPU reads it. Must match `Particle` in particle.vert.
//
// std430 in a storage buffer, so the layout rules are the plain C ones for these members: two
// vec4s, 32 bytes, no padding surprises. The `static_assert` below is the tripwire if anyone edits
// one side without the other — the same guard passes.hpp puts on its std140 blocks, and worth
// having for the same reason: a silent layout mismatch here draws garbage geometry rather than
// failing.
struct GpuParticle {
    float position_size[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz = world centre, w = half-size (m)
    float color_alpha[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // rgb = linear radiance, a = fade
};

static_assert(sizeof(GpuParticle) == 32, "GpuParticle must match particle.vert's std430 layout");

// What the pass needs to know about the camera to build billboards. Passed rather than derived so
// that the pass and the rest of the frame cannot disagree about which way the camera is facing —
// the same argument gizmo_renderer.hpp's CameraLens makes.
struct FxView {
    core::Mat4 view_proj;               // clip-from-world, the frame's own
    core::Vec3 right{1.0f, 0.0f, 0.0f}; // camera right, world space, unit
    core::Vec3 up{0.0f, 1.0f, 0.0f};    // camera up, world space, unit
};

class FxParticlePass {
public:
    explicit FxParticlePass(rhi::Device& device, std::uint32_t max_particles = 4096);
    ~FxParticlePass();

    FxParticlePass(const FxParticlePass&) = delete;
    FxParticlePass& operator=(const FxParticlePass&) = delete;

    // Declare the pass. **A no-op when `settings.enabled` is false or `particles` is empty** — see
    // the byte-identical note above; the caller does not need to branch, and more importantly a
    // caller that forgets to branch still gets the baseline.
    //
    // `particles` is consumed HERE (copied into the GPU buffer during this call), not at record
    // time, so the caller may let its CPU field change or die immediately afterwards. That is the
    // opposite of the graph's usual defer-to-record-time discipline and it is deliberate: the
    // alternative is holding a span into a simulation that is about to tick.
    void add(RenderGraph& graph,
             RGTexture hdr,
             RGTexture depth,
             std::span<const GpuParticle> particles,
             const FxView& view,
             const FxSettings& settings);

    // Particles dropped because they exceeded `FxSettings::max_particles` or this pass's buffer.
    // Guardrail 5: a draw budget that silently truncates reads exactly like one that never bound.
    [[nodiscard]] std::uint64_t particles_dropped() const noexcept { return dropped_; }

    // Particles actually submitted, cumulative — the vacuity witness. A coverage proof that drew
    // nothing and a coverage proof that drew a dark puff look identical on screen.
    [[nodiscard]] std::uint64_t particles_drawn() const noexcept { return drawn_; }

    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle instances_;
    std::uint32_t capacity_;
    std::uint32_t pending_ = 0; // instances written this frame, read at record time
    std::uint64_t dropped_ = 0;
    std::uint64_t drawn_ = 0;
};

} // namespace rime::render
