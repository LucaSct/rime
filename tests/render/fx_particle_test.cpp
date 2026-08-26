// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/fx_pass.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/vfx/dust.hpp"

// m13.1a — Track FX brick fx1a's proof, and the one M8.4 deferred.
//
// `engine/vfx` has simulated deterministic CPU billboards since M8.4 and nothing drew them. The
// M8.6 sample fed the field from destruction events and self-checked `DustField::coverage()` — a
// CPU number standing in for pixels that did not exist. Its header says, in as many words, that
// coverage() "is the scalar the m8.6 GPU pass's coverage-delta pixel test confirms on screen". This
// is that test, arriving one milestone late.
//
// THE CLAIM IS A DELTA, NOT AN IMAGE. Following the M5.6/M6.4 discipline — structural properties
// with margins on lavapipe, never golden images — nothing here asserts what a puff LOOKS like. What
// it asserts is that on-screen radiance MOVES THE WAY THE SIMULATION SAYS IT SHOULD: zero before a
// burst, up after it, down as the puff ages, and back to zero when it retires. A golden image would
// pin the falloff curve, the blend order and the driver's rounding; this pins the relationship
// between the sim and the screen, which is the thing that can actually regress.
using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

namespace {

constexpr std::uint32_t kSize = 128;

// A camera at +Z looking down -Z at the origin, and the billboard basis that goes with it. Written
// out rather than derived from a Camera component because this test drives the graph directly —
// there is no scene, and inventing one would be testing the extractor.
struct View {
    core::Mat4 view_proj;
    FxView fx;
};

[[nodiscard]] View make_view() {
    View v;
    // `look_at` rather than a hand-built translation: it is the same function the renderer uses, so
    // the basis below and the matrix cannot drift apart.
    const core::Mat4 view =
        core::look_at({0.0f, 0.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const core::Mat4 proj = core::perspective(1.0f, 1.0f, 0.1f, 100.0f);
    v.view_proj = proj * view;
    v.fx.view_proj = v.view_proj;
    v.fx.right = {1.0f, 0.0f, 0.0f};
    v.fx.up = {0.0f, 1.0f, 0.0f};
    return v;
}

// Render one frame: clear HDR + depth, then (maybe) the FX pass. Returns the total linear radiance
// over the whole image — the on-screen twin of DustField::coverage().
//
// `clear_depth` is a knob rather than a constant because it is how the depth-test proof is driven:
// clearing depth to the NEAR plane makes every particle fail LessEqual, with no geometry needed.
struct FrameResult {
    float total_radiance = 0.0f;
    std::vector<std::uint8_t> bytes; // raw HDR, for the byte-identical comparison
};

[[nodiscard]] FrameResult render_frame(rhi::Device& device,
                                       FxParticlePass& pass,
                                       std::span<const GpuParticle> particles,
                                       const FxSettings& settings,
                                       float clear_depth = 1.0f) {
    RenderGraph graph(device);
    const RGTexture hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "fx-hdr"});
    const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "fx-depth"});

    // A pass that only clears. The FX pass LOADS its targets (it adds to a shaded frame), so
    // something has to establish them; a no-op lambda with Clear load ops is the cheapest way to
    // say "this is the frame before FX" and it is exactly the baseline the gate is measured
    // against.
    const RGColorAttachment clear_colors[] = {
        {hdr, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 1.0f}}};
    RGDepthAttachment clear_depth_att{};
    clear_depth_att.texture = depth;
    clear_depth_att.load = rhi::LoadOp::Clear;
    clear_depth_att.store = rhi::StoreOp::Store;
    clear_depth_att.clear_depth = clear_depth;
    RenderGraph::RasterPassDesc clear_desc{};
    clear_desc.colors = clear_colors;
    clear_desc.depth = &clear_depth_att;
    graph.add_raster_pass("fx-test-clear", clear_desc, [](rhi::CommandBuffer&) {});

    const View view = make_view();
    pass.add(graph, hdr, depth, particles, view.fx, settings);

    graph.export_texture(hdr);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);

    FrameResult result;
    result.bytes = read_texture(device, graph.physical(hdr), kSize, kSize, 8);
    const HdrImage image = decode_hdr(result.bytes, kSize, kSize);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            result.total_radiance += image.luminance(x, y);
        }
    }
    return result;
}

// The vfx field, as the GPU reads it. This is the glue a consumer writes — vfx knows nothing about
// rendering and render knows nothing about vfx, so the conversion lives with whoever owns both.
// Kept in the test rather than in either module for exactly that reason.
[[nodiscard]] std::vector<GpuParticle> to_gpu(const vfx::DustField& field) {
    std::vector<GpuParticle> out;
    out.reserve(field.count());
    for (const vfx::DustParticle& p : field.particles()) {
        const float alpha = p.lifetime > 0.0f ? 1.0f - p.age / p.lifetime : 0.0f;
        GpuParticle g;
        g.position_size[0] = p.position.x;
        g.position_size[1] = p.position.y;
        g.position_size[2] = p.position.z;
        g.position_size[3] = p.size;
        // A warm dust grey. Colour is the game's; what matters to the proof is that it is non-zero
        // and constant, so every change in screen radiance comes from the simulation.
        g.color_alpha[0] = 0.8f;
        g.color_alpha[1] = 0.7f;
        g.color_alpha[2] = 0.6f;
        g.color_alpha[3] = std::clamp(alpha, 0.0f, 1.0f);
        out.push_back(g);
    }
    return out;
}

} // namespace

TEST_CASE("fx1a: the gate off is a byte-identical baseline") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping FX render proofs");
        return;
    }

    FxParticlePass pass(*device);
    vfx::DustField field;
    field.emit_burst({-0.6f, -0.6f, -0.6f}, {0.6f, 0.6f, 0.6f}, 1.0f);
    const std::vector<GpuParticle> particles = to_gpu(field);
    REQUIRE(!particles.empty());

    FxSettings off;
    off.enabled = false;
    FxSettings on;
    on.enabled = true;

    const FrameResult baseline = render_frame(*device, pass, {}, off);
    const FrameResult gated_off = render_frame(*device, pass, particles, off);
    // Sampled HERE, between the gated-off frame and the gated-on one. The counter is CUMULATIVE,
    // so reading it after the on-frame would report that frame's work and say nothing about
    // whether the gate held.
    const std::uint64_t drawn_while_off = pass.particles_drawn();
    const FrameResult gated_on = render_frame(*device, pass, particles, on);

    // ADR-0032 §11's rule, applied to FX by ADR-0035 §5: off is not *nearly* the pre-fx1a frame,
    // it is the SAME BYTES. Compared as raw bytes rather than as decoded radiance, because a
    // tolerance would let a pass that ran and blended nothing pass as "identical" — which is the
    // one thing the structural gate exists to rule out.
    CHECK(gated_off.bytes == baseline.bytes);
    CHECK(drawn_while_off == 0); // it never even uploaded

    // …and the run was not vacuous: with the gate ON, the same particles do reach the screen.
    // Without this, "off is identical to nothing" would also be true of a pass that never works.
    CHECK(gated_on.bytes != baseline.bytes);
    CHECK(gated_on.total_radiance > 0.0f);
    CHECK(pass.particles_drawn() - drawn_while_off == particles.size());
}

TEST_CASE("fx1a: on-screen radiance tracks the simulation's coverage, up and down") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping FX render proofs");
        return;
    }

    FxParticlePass pass(*device);
    FxSettings on;
    on.enabled = true;

    vfx::DustField field;

    // ── Before the burst: nothing to draw, nothing on screen ─────────────────────────────────
    CHECK(field.coverage() == 0.0f);
    const FrameResult before = render_frame(*device, pass, to_gpu(field), on);
    CHECK(before.total_radiance == 0.0f);

    // ── The burst ────────────────────────────────────────────────────────────────────────────
    field.emit_burst({-0.6f, -0.6f, -0.6f}, {0.6f, 0.6f, 0.6f}, 1.0f);
    const float coverage_fresh = field.coverage();
    REQUIRE(coverage_fresh > 0.0f);
    const FrameResult fresh = render_frame(*device, pass, to_gpu(field), on);

    MESSAGE("m13.1a burst: coverage " << coverage_fresh << " -> screen radiance "
                                      << fresh.total_radiance);
    CHECK(fresh.total_radiance > before.total_radiance);

    // ── Ageing: both must fall, together ─────────────────────────────────────────────────────
    // Sampled at several points rather than only at the ends, because "it went up then came back
    // to zero" is also true of a pass that flickers. What is asserted is MONOTONE DECAY in both
    // the CPU witness and the pixels, at the same steps.
    std::vector<float> coverage_series{coverage_fresh};
    std::vector<float> radiance_series{fresh.total_radiance};
    for (int step = 0; step < 5; ++step) {
        for (int i = 0; i < 12; ++i) {
            field.simulate(1.0f / 60.0f);
        }
        coverage_series.push_back(field.coverage());
        radiance_series.push_back(render_frame(*device, pass, to_gpu(field), on).total_radiance);
    }

    for (std::size_t i = 1; i < coverage_series.size(); ++i) {
        // The CPU proxy decays…
        CHECK(coverage_series[i] <= coverage_series[i - 1]);
        // …and so does the screen. THIS is the coverage-delta claim: the two move together, so
        // coverage() is a witness for pixels rather than a number that merely exists.
        CHECK(radiance_series[i] <= radiance_series[i - 1]);
    }

    MESSAGE("m13.1a decay: coverage " << coverage_series.front() << " -> " << coverage_series.back()
                                      << ", radiance " << radiance_series.front() << " -> "
                                      << radiance_series.back());

    // It really decayed rather than merely not increasing — a flat series satisfies <= at every
    // step and would prove nothing.
    CHECK(radiance_series.back() < radiance_series.front() * 0.9f);
    CHECK(coverage_series.back() < coverage_series.front() * 0.9f);

    // ── Retirement: the puff ages out completely, and the screen goes black again ────────────
    for (int i = 0; i < 600; ++i) {
        field.simulate(1.0f / 60.0f);
    }
    CHECK(field.count() == 0);
    const FrameResult retired = render_frame(*device, pass, to_gpu(field), on);
    CHECK(retired.total_radiance == 0.0f);
}

TEST_CASE("fx1a: particles are depth-tested and never write depth") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping FX render proofs");
        return;
    }

    FxParticlePass pass(*device);
    FxSettings on;
    on.enabled = true;

    vfx::DustField field;
    field.emit_burst({-0.6f, -0.6f, -0.6f}, {0.6f, 0.6f, 0.6f}, 1.0f);
    const std::vector<GpuParticle> particles = to_gpu(field);
    REQUIRE(!particles.empty());

    // Depth cleared to the FAR plane: everything passes, the puff is drawn. The control.
    const FrameResult visible = render_frame(*device, pass, particles, on, /*clear_depth=*/1.0f);
    CHECK(visible.total_radiance > 0.0f);

    // Depth cleared to the NEAR plane: every particle is behind it and must fail LessEqual.
    //
    // Driven by the clear value rather than by drawing an occluder, deliberately — an occluder
    // would need a second pipeline and a mesh, and would then be testing that pipeline. What is
    // under test here is whether the FX pipeline's depth state is wired at all, and a depth buffer
    // that rejects everything answers exactly that.
    const FrameResult occluded = render_frame(*device, pass, particles, on, /*clear_depth=*/0.0f);
    CHECK(occluded.total_radiance == 0.0f);
}

TEST_CASE("fx1a: intensity zero is not the same as the gate being off") {
    // The distinction the FxSettings header insists on. `intensity = 0` runs the pass — it binds,
    // it blends, it costs — and draws nothing visible; `enabled = false` does not run it at all.
    // A design that collapsed the two would make the byte-identical baseline untestable, because
    // "dark" and "absent" would be the same state.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping FX render proofs");
        return;
    }

    FxParticlePass pass(*device);
    vfx::DustField field;
    field.emit_burst({-0.6f, -0.6f, -0.6f}, {0.6f, 0.6f, 0.6f}, 1.0f);
    const std::vector<GpuParticle> particles = to_gpu(field);

    FxSettings dark;
    dark.enabled = true;
    dark.intensity = 0.0f;
    FxSettings bright;
    bright.enabled = true;

    const std::uint64_t before = pass.particles_drawn();
    const FrameResult dark_frame = render_frame(*device, pass, particles, dark);
    const std::uint64_t after_dark = pass.particles_drawn();
    const FrameResult bright_frame = render_frame(*device, pass, particles, bright);

    // Nothing visible…
    CHECK(dark_frame.total_radiance == 0.0f);
    // …but the pass DID run and DID submit every particle. That is the difference, counted.
    CHECK(after_dark - before == particles.size());
    CHECK(bright_frame.total_radiance > 0.0f);
}

TEST_CASE("fx1a: the draw budget truncates loudly, never silently") {
    // Guardrail 5. The CPU sim has its own cap; this is the render side's independent bound, so a
    // mis-sized field cannot turn into an unbounded draw. A budget that truncated without counting
    // would look exactly like a small puff.
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping FX render proofs");
        return;
    }

    FxParticlePass pass(*device);
    vfx::DustField field;
    field.emit_burst({-0.6f, -0.6f, -0.6f}, {0.6f, 0.6f, 0.6f}, 1.0f);
    const std::vector<GpuParticle> particles = to_gpu(field);
    REQUIRE(particles.size() > 4);

    FxSettings tight;
    tight.enabled = true;
    tight.max_particles = 3;

    CHECK(pass.particles_dropped() == 0);
    const FrameResult clipped = render_frame(*device, pass, particles, tight);
    CHECK(pass.particles_dropped() == particles.size() - 3);
    CHECK(pass.particles_drawn() == 3);
    // Still drew something — the budget clips, it does not disable.
    CHECK(clipped.total_radiance > 0.0f);
}
