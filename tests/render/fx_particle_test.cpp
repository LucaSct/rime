// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/fx_pass.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"
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
                                       float clear_depth = 1.0f,
                                       float clear_luminance = 0.0f,
                                       FxParticlePass* following_pass = nullptr) {
    RenderGraph graph(device);
    const RGTexture hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "fx-hdr"});
    const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "fx-depth"});

    // A pass that only clears. The FX pass LOADS its targets (it adds to a shaded frame), so
    // something has to establish them; a no-op lambda with Clear load ops is the cheapest way to
    // say "this is the frame before FX" and it is exactly the baseline the gate is measured
    // against.
    const RGColorAttachment clear_colors[] = {
        {hdr,
         rhi::LoadOp::Clear,
         rhi::StoreOp::Store,
         {clear_luminance, clear_luminance, clear_luminance, 1.0f}}};
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
    if (following_pass)
        following_pass->add(graph, hdr, depth, particles, view.fx, settings);

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

[[nodiscard]] GpuParticle smoke(float z, float r, float g, float b, float a = 1.0f) {
    GpuParticle p{};
    p.position_size[2] = z;
    p.position_size[3] = 1.0f;
    p.color_alpha[0] = r;
    p.color_alpha[1] = g;
    p.color_alpha[2] = b;
    p.color_alpha[3] = a;
    return p;
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

TEST_CASE("smoke alpha attenuates bright background and lights dark background") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass smoke_pass(*device, 8, FxBlendMode::SmokeAlpha);
    FxParticlePass additive_pass(*device, 8);
    FxSettings on{};
    on.enabled = true;
    const GpuParticle black = smoke(0.0f, 0.0f, 0.0f, 0.0f);
    const auto bright = render_frame(*device, smoke_pass, {}, on, 1.0f, 1.0f);
    const auto attenuated = render_frame(*device, smoke_pass, {&black, 1}, on, 1.0f, 1.0f);
    const auto additive = render_frame(*device, additive_pass, {&black, 1}, on, 1.0f, 1.0f);
    CHECK(attenuated.total_radiance < bright.total_radiance);
    CHECK(additive.bytes == bright.bytes);

    const GpuParticle grey = smoke(0.0f, 0.5f, 0.5f, 0.5f);
    const auto dark = render_frame(*device, smoke_pass, {}, on);
    const auto visible = render_frame(*device, smoke_pass, {&grey, 1}, on);
    CHECK(visible.total_radiance > dark.total_radiance);
}

TEST_CASE("smoke sorting is input-order independent and overlapping layers matter") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass pass(*device, 8, FxBlendMode::SmokeAlpha);
    FxSettings on{};
    on.enabled = true;
    const GpuParticle far = smoke(-1.0f, 0.9f, 0.1f, 0.1f);
    const GpuParticle near = smoke(1.0f, 0.1f, 0.1f, 0.9f);
    const std::vector<GpuParticle> front_first{near, far};
    const std::vector<GpuParticle> back_first{far, near};
    const auto ordered = render_frame(*device, pass, front_first, on);
    const auto reversed = render_frame(*device, pass, back_first, on);
    const auto single = render_frame(*device, pass, {&near, 1}, on);
    CHECK(ordered.bytes == reversed.bytes);
    CHECK(ordered.bytes != single.bytes);
}

TEST_CASE("smoke alpha zero, depth occlusion, disabled gate, and zero cap") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass pass(*device, 2, FxBlendMode::SmokeAlpha);
    FxSettings off{};
    FxSettings on{};
    on.enabled = true;
    const GpuParticle grey = smoke(0.0f, 0.4f, 0.4f, 0.4f);
    const GpuParticle zero = smoke(0.0f, 0.4f, 0.4f, 0.4f, 0.0f);
    const auto baseline = render_frame(*device, pass, {}, off, 1.0f, 0.8f);
    CHECK(render_frame(*device, pass, {&grey, 1}, off, 1.0f, 0.8f).bytes == baseline.bytes);
    CHECK(render_frame(*device, pass, {&zero, 1}, on, 1.0f, 0.8f).bytes == baseline.bytes);
    CHECK(render_frame(*device, pass, {&grey, 1}, on, 0.0f, 0.8f).bytes == baseline.bytes);
    on.max_particles = 0;
    const std::vector<GpuParticle> three{grey, grey, grey};
    const auto before = pass.particles_dropped();
    CHECK(render_frame(*device, pass, three, on, 1.0f, 0.8f).bytes == baseline.bytes);
    CHECK(pass.particles_dropped() - before == 3);
    CHECK(pass.capacity() == 2);
}

TEST_CASE("smoke graph declaration is gated and a zero budget counts all particles") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass pass(*device, 2, FxBlendMode::SmokeAlpha);
    RenderGraph graph(*device);
    const RGTexture hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "smoke-graph-hdr"});
    const RGTexture depth =
        graph.create_texture({{kSize, kSize}, kDepthFormat, "smoke-graph-depth"});
    const FxView view = make_view().fx;
    const GpuParticle grey = smoke(0.0f, 0.4f, 0.4f, 0.4f);
    FxSettings off{};
    pass.add(graph, hdr, depth, {&grey, 1}, view, off);
    CHECK(graph.pass_count() == 0);
    FxSettings on{};
    on.enabled = true;
    pass.add(graph, hdr, depth, {}, view, on);
    CHECK(graph.pass_count() == 0);
    on.max_particles = 0;
    pass.add(graph, hdr, depth, {&grey, 1}, view, on);
    CHECK(graph.pass_count() == 0);
    CHECK(pass.particles_dropped() == 1);
    on.max_particles = 2;
    pass.add(graph, hdr, depth, {&grey, 1}, view, on);
    CHECK(graph.pass_count() == 1);
}

TEST_CASE("smoke rejects invalid particles without changing valid pixels") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass pass(*device, 8, FxBlendMode::SmokeAlpha);
    FxSettings on{};
    on.enabled = true;
    const GpuParticle valid = smoke(0.0f, 0.4f, 0.4f, 0.4f);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    GpuParticle bad_position = valid;
    bad_position.position_size[2] = nan;
    GpuParticle bad_size = valid;
    bad_size.position_size[3] = -1.0f;
    GpuParticle bad_color = valid;
    bad_color.color_alpha[3] = nan;
    const auto reference = render_frame(*device, pass, {&valid, 1}, on);
    const std::vector<GpuParticle> mixed{bad_position, valid, bad_size, bad_color};
    const auto before = pass.particles_dropped();
    const auto filtered = render_frame(*device, pass, mixed, on);
    CHECK(filtered.bytes == reference.bytes);
    CHECK(pass.particles_dropped() - before == 3);

    RenderGraph graph(*device);
    const RGTexture hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "invalid-view-hdr"});
    const RGTexture depth =
        graph.create_texture({{kSize, kSize}, kDepthFormat, "invalid-view-depth"});
    FxView bad_view = make_view().fx;
    bad_view.up = bad_view.right;
    const auto before_view = pass.particles_dropped();
    pass.add(graph, hdr, depth, {&valid, 1}, bad_view, on);
    CHECK(graph.pass_count() == 0);
    CHECK(pass.particles_dropped() - before_view == 1);
    on.intensity = nan;
    const auto before_intensity = pass.particles_dropped();
    pass.add(graph, hdr, depth, {&valid, 1}, make_view().fx, on);
    CHECK(graph.pass_count() == 0);
    CHECK(pass.particles_dropped() - before_intensity == 1);
    CHECK_FALSE(pass.set_frames_in_flight(2)); // a prior submitted add owns ring slots
}

TEST_CASE("smoke ring can be configured after a gated add") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass pass(*device, 2, FxBlendMode::SmokeAlpha);
    RenderGraph graph(*device);
    const RGTexture hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "ring-hdr"});
    const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "ring-depth"});
    const GpuParticle grey = smoke(0.0f, 0.4f, 0.4f, 0.4f);
    pass.add(graph, hdr, depth, {&grey, 1}, make_view().fx, FxSettings{});
    CHECK(pass.set_frames_in_flight(9)); // no arbitrary eight-frame ceiling
    CHECK_FALSE(pass.set_frames_in_flight(std::numeric_limits<std::uint32_t>::max()));
}

TEST_CASE("SceneRenderer declares smoke only when enabled, after SSR and before TAA") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const MeshId cube = meshes.add(make_cube(0.5f), "smoke-scene-cube");
    const MaterialId mat = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.5f});
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, Camera{});
    core::Transform tf{};
    tf.translation = {0.0f, 0.0f, -6.0f};
    (void)world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube}, MaterialRef{mat});
    SceneRenderer renderer(*device, meshes, materials);
    LightingSettings lighting{};
    lighting.ssr_enabled = true;
    renderer.set_lighting(lighting);
    const GpuParticle grey = smoke(-2.0f, 0.3f, 0.3f, 0.3f);
    FxSettings on{};
    on.enabled = true;
    on.max_particles = 1;
    const std::vector<GpuParticle> three{grey, grey, grey};
    renderer.set_smoke_particles(three, on);
    RenderGraph enabled(*device);
    const auto output = renderer.render(enabled, world, {kSize, kSize}, true);
    REQUIRE(output.ldr.is_valid());
    std::size_t ssr_index = enabled.pass_count();
    std::size_t smoke_index = enabled.pass_count();
    std::size_t taa_index = enabled.pass_count();
    for (std::size_t i = 0; i < enabled.pass_count(); ++i) {
        const auto& name = enabled.pass_name(static_cast<std::uint32_t>(i));
        if (name == "ssr-resolve")
            ssr_index = i;
        if (name == "fx-smoke")
            smoke_index = i;
        if (name == "taa-resolve")
            taa_index = i;
    }
    CHECK(ssr_index < smoke_index);
    CHECK(smoke_index < taa_index);
    CHECK(renderer.smoke_particles_dropped() == 2);

    renderer.set_smoke_particles({&grey, 1}, FxSettings{});
    RenderGraph disabled(*device);
    (void)renderer.render(disabled, world, {kSize, kSize}, true);
    for (std::size_t i = 0; i < disabled.pass_count(); ++i)
        CHECK(disabled.pass_name(static_cast<std::uint32_t>(i)) != "fx-smoke");
}

TEST_CASE("transient scene lights illuminate without authored components and clear exactly") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    MeshRegistry meshes(*device);
    MaterialRegistry materials;
    const auto cube = meshes.add(make_cube(0.5f), "transient-light-cube");
    const auto material = materials.add({{0.8f, 0.8f, 0.8f, 1.0f}, 0.0f, 0.8f});
    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, Camera{});
    core::Transform pose{};
    pose.translation = {0.0f, 0.0f, -3.0f};
    (void)world.spawn_with(ecs::WorldTransform{pose}, MeshRef{cube}, MaterialRef{material});
    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_temporal_jitter_enabled(false);
    renderer.set_taa_resolve_enabled(false);
    renderer.set_ambient(0.0f, 0.0f, 0.0f);
    const auto frame = [&] {
        RenderGraph graph(*device);
        const auto out = renderer.render(graph, world, {kSize, kSize}, true);
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8);
    };
    const auto off = frame();
    GpuPointLight light{{0.0f, 0.0f, -1.0f, 8.0f}, {8.0f, 2.0f, 0.5f, 0.0f}};
    renderer.set_transient_point_lights({&light, 1});
    const auto on = frame();
    const auto image = decode_hdr(on, kSize, kSize);
    const auto dark = decode_hdr(off, kSize, kSize);
    CHECK(image.luminance(kSize / 2, kSize / 2) > dark.luminance(kSize / 2, kSize / 2) + 0.01f);
    CHECK(extract_scene(world).point_lights.empty());
    renderer.set_transient_point_lights({});
    CHECK(frame() == off);

    auto bad = light;
    bad.position[0] = std::numeric_limits<float>::quiet_NaN();
    std::vector<GpuPointLight> rows(18, light);
    rows.insert(rows.begin(), bad);
    renderer.set_transient_point_lights(rows);
    CHECK(renderer.transient_point_lights_dropped() == 3);
    // Copy ownership: changing the caller's storage after set cannot extinguish accepted rows.
    for (auto& row : rows)
        row.radiance[0] = row.radiance[1] = row.radiance[2] = 0.0f;
    CHECK(frame() != off);
    renderer.set_transient_point_lights({&bad, 1});
    CHECK(renderer.transient_point_lights_dropped() == 4);
    CHECK(frame() == off);
}

TEST_CASE("flame V2 gate, empty input, alpha zero, depth and intensity controls") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass pass(*device, 4, FxBlendMode::Flame);
    FxSettings on{};
    on.enabled = true;
    const GpuParticle flame = smoke(0.0f, 4.0f, 4.0f, 4.0f);
    const auto baseline = render_frame(*device, pass, {}, on, 1.0f, 0.4f);
    CHECK(render_frame(*device, pass, {&flame, 1}, FxSettings{}, 1.0f, 0.4f).bytes ==
          baseline.bytes);
    CHECK(pass.particles_drawn() == 0);
    CHECK(pass.set_frames_in_flight(3)); // off and empty calls must not consume ring slots
    const auto visible = render_frame(*device, pass, {&flame, 1}, on, 1.0f, 0.4f);
    CHECK(visible.total_radiance > baseline.total_radiance + 10.0f);
    CHECK(visible.bytes != baseline.bytes); // sensitivity witness for every null control below
    GpuParticle zero_alpha = flame;
    zero_alpha.color_alpha[3] = 0.0f;
    CHECK(render_frame(*device, pass, {&zero_alpha, 1}, on, 1.0f, 0.4f).bytes == baseline.bytes);
    CHECK(render_frame(*device, pass, {&flame, 1}, on, 0.0f, 0.4f).bytes == baseline.bytes);
    on.intensity = 0.0f;
    const auto drawn_before = pass.particles_drawn();
    CHECK(render_frame(*device, pass, {&flame, 1}, on, 1.0f, 0.4f).bytes == baseline.bytes);
    CHECK(pass.particles_drawn() == drawn_before + 1); // dark still submits
    CHECK_FALSE(pass.set_frames_in_flight(3));
}

TEST_CASE("flame V2 silhouette is tall and tapers toward its tip") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass flame_pass(*device, 4, FxBlendMode::Flame);
    FxParticlePass disc_pass(*device, 4);
    FxSettings on{};
    on.enabled = true;
    const GpuParticle flame = smoke(0.0f, 4.0f, 4.0f, 4.0f);
    const auto frame = render_frame(*device, flame_pass, {&flame, 1}, on);
    const auto disc = render_frame(*device, disc_pass, {&flame, 1}, on);
    REQUIRE(frame.total_radiance > 0.0f);
    const auto image = decode_hdr(frame.bytes, kSize, kSize);
    const auto bounds = [](const HdrImage& img) {
        std::uint32_t min_x = kSize, min_y = kSize, max_x = 0, max_y = 0;
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x)
                if (img.luminance(x, y) > 0.01f) {
                    min_x = std::min(min_x, x);
                    min_y = std::min(min_y, y);
                    max_x = std::max(max_x, x);
                    max_y = std::max(max_y, y);
                }
        return std::pair{max_x - min_x + 1, max_y - min_y + 1};
    };
    const auto [width, height] = bounds(image);
    const auto [disc_width, disc_height] = bounds(decode_hdr(disc.bytes, kSize, kSize));
    CHECK(height > width * 2);
    CHECK(height > disc_height);
    CHECK(width < disc_width); // switching back to a radial disc must falsify the shape proof
    const auto row_width = [&](float world_y) {
        const auto row = static_cast<std::uint32_t>(
            project(make_view().view_proj, {0.0f, world_y, 0.0f}, kSize).y);
        std::uint32_t pixels = 0;
        for (std::uint32_t x = 0; x < kSize; ++x)
            pixels += image.luminance(x, row) > 0.01f ? 1u : 0u;
        return pixels;
    };
    const auto root_width = row_width(-0.6f);
    const auto tip_width = row_width(0.8f);
    CHECK(tip_width > 0);
    CHECK(root_width > tip_width * 2);

    // Two layers still add when the front layer was submitted first: depth must stay read-only.
    GpuParticle near = flame;
    near.position_size[2] = 1.0f;
    GpuParticle far = flame;
    far.position_size[2] = -1.0f;
    const float near_radiance = render_frame(*device, flame_pass, {&near, 1}, on).total_radiance;
    const float far_radiance = render_frame(*device, flame_pass, {&far, 1}, on).total_radiance;
    const std::vector<GpuParticle> layers{near, far};
    const float both = render_frame(*device, flame_pass, layers, on).total_radiance;
    CHECK(std::abs(both - near_radiance - far_radiance) <
          (near_radiance + far_radiance) * 0.005f); // margin for HDR half-float accumulation
}

TEST_CASE("flame V2 admission counts invalid and over-budget rows and names its pass") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass pass(*device, 2, FxBlendMode::Flame);
    FxSettings on{};
    on.enabled = true;
    const GpuParticle valid = smoke(0.0f, 4.0f, 4.0f, 4.0f);
    GpuParticle bad_position = valid;
    bad_position.position_size[0] = std::numeric_limits<float>::infinity();
    GpuParticle bad_size = valid;
    bad_size.position_size[3] = -1.0f;
    GpuParticle bad_color = valid;
    bad_color.color_alpha[3] = std::numeric_limits<float>::quiet_NaN();
    const auto reference = render_frame(*device, pass, {&valid, 1}, on);
    const std::vector<GpuParticle> mixed{bad_position, valid, bad_size, bad_color, valid};
    on.max_particles = 1;
    const auto before = pass.particles_dropped();
    CHECK(render_frame(*device, pass, mixed, on).bytes == reference.bytes);
    CHECK(pass.particles_dropped() - before == 4);
    CHECK(pass.capacity() == 2);

    RenderGraph graph(*device);
    const RGTexture hdr = graph.create_texture({{kSize, kSize}, kHdrFormat, "flame-admit-hdr"});
    const RGTexture depth =
        graph.create_texture({{kSize, kSize}, kDepthFormat, "flame-admit-depth"});
    pass.add(graph, hdr, depth, {&valid, 1}, make_view().fx, FxSettings{});
    CHECK(graph.pass_count() == 0);
    pass.add(graph, hdr, depth, {}, make_view().fx, on);
    CHECK(graph.pass_count() == 0);
    on.max_particles = 0;
    const auto before_zero = pass.particles_dropped();
    pass.add(graph, hdr, depth, mixed, make_view().fx, on);
    CHECK(graph.pass_count() == 0);
    CHECK(pass.particles_dropped() - before_zero == mixed.size());
    on.max_particles = 4;
    FxView bad_view = make_view().fx;
    bad_view.up = bad_view.right;
    const auto before_view = pass.particles_dropped();
    pass.add(graph, hdr, depth, {&valid, 1}, bad_view, on);
    CHECK(graph.pass_count() == 0);
    CHECK(pass.particles_dropped() - before_view == 1);
    on.intensity = std::numeric_limits<float>::quiet_NaN();
    const auto before_intensity = pass.particles_dropped();
    pass.add(graph, hdr, depth, {&valid, 1}, make_view().fx, on);
    CHECK(graph.pass_count() == 0);
    CHECK(pass.particles_dropped() - before_intensity == 1);
    on.intensity = 1.0f;
    const auto before_cap = pass.particles_dropped();
    const std::vector<GpuParticle> three{valid, valid, valid};
    pass.add(graph, hdr, depth, three, make_view().fx, on);
    REQUIRE(graph.pass_count() == 1);
    CHECK(graph.pass_name(0) == "fx-flame");
    CHECK(pass.particles_dropped() - before_cap == 1); // pass capacity independently bounds draw
}

TEST_CASE("smoke V2 density is deterministic and breaks radial symmetry with black occlusion") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass pass(*device, 4, FxBlendMode::SmokeAlpha);
    FxSettings on{};
    on.enabled = true;
    const GpuParticle grey = smoke(0.0f, 0.5f, 0.5f, 0.5f);
    const auto frame = render_frame(*device, pass, {&grey, 1}, on);
    CHECK(render_frame(*device, pass, {&grey, 1}, on).bytes == frame.bytes);
    const auto image = decode_hdr(frame.bytes, kSize, kSize);
    float asymmetry = 0.0f;
    for (std::uint32_t y = 0; y < kSize; ++y)
        for (std::uint32_t x = 0; x < kSize; ++x)
            asymmetry += std::abs(image.luminance(x, y) - image.luminance(kSize - 1 - x, y));
    REQUIRE(frame.total_radiance > 0.0f);
    // A radial puff has equal samples at equal radii. This control measures density breakup,
    // without pinning a noise pattern or depending on any particular driver's sine precision.
    CHECK(asymmetry > frame.total_radiance * 0.02f);

    const auto bright = render_frame(*device, pass, {}, on, 1.0f, 1.0f);
    on.intensity = 0.0f;
    const auto dark_smoke = render_frame(*device, pass, {&grey, 1}, on, 1.0f, 1.0f);
    CHECK(dark_smoke.total_radiance < bright.total_radiance);
    CHECK(dark_smoke.bytes != bright.bytes); // zero light must retain smoke opacity
}

TEST_CASE("flame V2 preserves occluder depth for a following translucent pass") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        return;
    }
    FxParticlePass flame_pass(*device, 4, FxBlendMode::Flame);
    FxParticlePass smoke_pass(*device, 4, FxBlendMode::SmokeAlpha);
    FxSettings on{};
    on.enabled = true;
    const GpuParticle source = smoke(0.0f, 4.0f, 4.0f, 4.0f);
    const auto baseline = render_frame(*device, flame_pass, {}, on, 0.0f, 0.4f);
    const auto occluded =
        render_frame(*device, flame_pass, {&source, 1}, on, 0.0f, 0.4f, &smoke_pass);
    CHECK(occluded.bytes == baseline.bytes);
    const auto visible =
        render_frame(*device, flame_pass, {&source, 1}, on, 1.0f, 0.4f, &smoke_pass);
    CHECK(visible.total_radiance > baseline.total_radiance);
    CHECK(flame_pass.particles_drawn() == 2);
    CHECK(smoke_pass.particles_drawn() == 2); // both draws submitted; depth rejected hidden pixels
}
