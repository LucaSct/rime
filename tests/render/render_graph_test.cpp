// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the render graph v0 (M5.4, ADR-0019). Five claims:
//
//  (1) A declared multi-pass frame records and runs correctly END TO END: a raster pass fills a
//      transient, a second raster pass SAMPLES it (the graph must emit the ColorTarget→ShaderRead
//      barrier — the hazard hand-rolled code gets wrong first), and a compute pass feeds a third
//      raster pass the same way. Every output is pixel-verified by readback.
//
//  (2) Culling: a pass whose writes reach no imported/exported resource never executes.
//
//  (3) The transient cache recycles: the same declaration two frames running resolves to the
//      same physical texture (frame 2 allocates nothing).
//
//  (4) Versioning semantics hold: declaration order IS data-flow order — a pass declared before
//      a resource's writer reads the older version (it is not reordered after the writer), and a
//      write whose new version nothing consumes is dead. Execution preserves declared order
//      (with versioned edges it is always a valid topological order).
//
//  (5) Per-pass GPU timings resolve after submission (where the device can timestamp).
//
// Plus a measured (not asserted) compile-overhead figure for a synthetic 100-pass frame — the
// "declare + compile every frame" cost ADR-0019 accepts, recorded per house rule.
//
// GPU-free on lavapipe, reusing the rhi test shaders (fullscreen triangle, push-constant color,
// texel-perfect sampling, compute pattern).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN // the one TU of this exe supplies doctest's main()
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "pattern.comp.spv.h"
#include "pushconst.frag.spv.h"
#include "pushconst.vert.spv.h"
#include "rime/render/render_graph.hpp"
#include "sample_storage.frag.spv.h"

namespace {

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// The recurring cast of GPU objects the passes bind. Built once per test case.
struct Kit {
    rime::rhi::Device& device;
    rime::rhi::ShaderHandle fullscreen_vs; // pushconst.vert: fullscreen triangle, no buffers
    rime::rhi::ShaderHandle color_fs;      // pushconst.frag: flat color from a push constant
    rime::rhi::ShaderHandle sample_fs;     // sample_storage.frag: texel-perfect copy of binding 0
    rime::rhi::ShaderHandle pattern_cs;    // pattern.comp: coordinate pattern via imageStore
    rime::rhi::PipelineHandle fill;        // draw a pushed color
    rime::rhi::PipelineHandle blit;        // draw binding-0's texels 1:1
    rime::rhi::PipelineHandle pattern;     // compute the coordinate pattern
    rime::rhi::SamplerHandle nearest;

    explicit Kit(rime::rhi::Device& dev) : device(dev) {
        using namespace rime::rhi;
        const auto shader =
            [&](ShaderStage stage, const std::uint32_t* spv, std::size_t bytes, const char* name) {
                ShaderDesc sd{};
                sd.stage = stage;
                sd.spirv = spv;
                sd.spirv_size_bytes = bytes;
                sd.debug_name = name;
                return device.create_shader(sd);
            };
        fullscreen_vs = shader(
            ShaderStage::Vertex, pushconst_vert_spv, sizeof(pushconst_vert_spv), "pushconst.vert");
        color_fs = shader(ShaderStage::Fragment,
                          pushconst_frag_spv,
                          sizeof(pushconst_frag_spv),
                          "pushconst.frag");
        sample_fs = shader(ShaderStage::Fragment,
                           sample_storage_frag_spv,
                           sizeof(sample_storage_frag_spv),
                           "sample_storage.frag");
        pattern_cs = shader(
            ShaderStage::Compute, pattern_comp_spv, sizeof(pattern_comp_spv), "pattern.comp");

        GraphicsPipelineDesc fill_pd{};
        fill_pd.vertex_shader = fullscreen_vs;
        fill_pd.fragment_shader = color_fs;
        fill_pd.color_format = Format::RGBA8Unorm;
        fill_pd.cull = CullMode::None;
        fill_pd.push_constant_size = sizeof(float) * 4;
        fill_pd.debug_name = "rg-fill";
        fill = device.create_graphics_pipeline(fill_pd);

        static const BindingDesc kSampled[] = {
            {0, BindingType::CombinedImageSampler, StageMask::Fragment}};
        GraphicsPipelineDesc blit_pd{};
        blit_pd.vertex_shader = fullscreen_vs;
        blit_pd.fragment_shader = sample_fs;
        blit_pd.color_format = Format::RGBA8Unorm;
        blit_pd.cull = CullMode::None;
        blit_pd.bindings = kSampled;
        blit_pd.debug_name = "rg-blit";
        blit = device.create_graphics_pipeline(blit_pd);

        static const BindingDesc kStorage[] = {{0, BindingType::StorageImage, StageMask::Compute}};
        ComputePipelineDesc cpd{};
        cpd.shader = pattern_cs;
        cpd.bindings = kStorage;
        cpd.debug_name = "rg-pattern";
        pattern = device.create_compute_pipeline(cpd);

        SamplerDesc smd{};
        smd.mag_filter = Filter::Nearest;
        smd.min_filter = Filter::Nearest;
        smd.address_mode = AddressMode::ClampToEdge;
        smd.debug_name = "rg-nearest";
        nearest = device.create_sampler(smd);
    }

    ~Kit() {
        device.destroy(nearest);
        device.destroy(pattern);
        device.destroy(blit);
        device.destroy(fill);
        device.destroy(pattern_cs);
        device.destroy(sample_fs);
        device.destroy(color_fs);
        device.destroy(fullscreen_vs);
    }
};

// Read an exported RGBA8 graph texture back to CPU bytes (its own submission, after the graph's).
std::vector<std::uint8_t>
read_back(rime::rhi::Device& device, rime::rhi::TextureHandle tex, std::uint32_t size) {
    using namespace rime::rhi;
    const std::uint64_t bytes = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "rg-readback";
    const BufferHandle rb = device.create_buffer(rbd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(tex, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> px(bytes);
    device.read_buffer(rb, px.data(), px.size(), 0);
    device.destroy(rb);
    return px;
}

} // namespace

TEST_CASE("render graph v0: multi-pass frames, culling, transients, order, timings (M5.4)") {
    using namespace rime::rhi;
    using namespace rime::render;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping render-graph proof");
        return;
    }
    const std::uint32_t size = 32;

    SUBCASE("a raster→raster→(compute→raster) frame runs and every barrier lands") {
        Kit kit(*device);
        RenderGraph graph(*device);
        graph.reset();

        // fill (red) → copied by a sampling pass → exported. The graph must transition `mid`
        // from ColorTarget to ShaderRead between the passes — the make-or-break hazard.
        RGTexture mid = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-mid"});
        RGTexture out = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-out"});
        // pattern (compute) → sampled into a second export: the compute→raster handoff.
        RGTexture pat = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-pattern"});
        RGTexture out2 = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-out2"});

        const RGColorAttachment fill_att[] = {{mid, LoadOp::Clear, StoreOp::Store, {}}};
        graph.add_raster_pass("fill-red", {.colors = fill_att}, [&](CommandBuffer& cmd) {
            cmd.bind_pipeline(kit.fill);
            const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
            cmd.push_constants(red, sizeof(red));
            cmd.draw(3);
        });

        const RGColorAttachment copy_att[] = {{out, LoadOp::Clear, StoreOp::Store, {}}};
        const RGTexture copy_reads[] = {mid};
        graph.add_raster_pass(
            "copy-mid", {.colors = copy_att, .sampled = copy_reads}, [&](CommandBuffer& cmd) {
                cmd.bind_pipeline(kit.blit);
                cmd.bind_texture(0, graph.physical(mid), kit.nearest);
                cmd.draw(3);
            });

        const RGTexture pat_writes[] = {pat};
        graph.add_compute_pass("pattern", {.storage_write = pat_writes}, [&](CommandBuffer& cmd) {
            cmd.bind_compute_pipeline(kit.pattern);
            cmd.bind_storage_image(0, graph.physical(pat));
            cmd.dispatch(size / 8, size / 8, 1);
        });

        const RGColorAttachment out2_att[] = {{out2, LoadOp::Clear, StoreOp::Store, {}}};
        const RGTexture out2_reads[] = {pat};
        graph.add_raster_pass(
            "copy-pattern", {.colors = out2_att, .sampled = out2_reads}, [&](CommandBuffer& cmd) {
                cmd.bind_pipeline(kit.blit);
                cmd.bind_texture(0, graph.physical(pat), kit.nearest);
                cmd.draw(3);
            });

        graph.export_texture(out);
        graph.export_texture(out2);

        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);

        const auto px = read_back(*device, graph.physical(out), size);
        const std::size_t c = (static_cast<std::size_t>(size / 2) * size + size / 2) * 4;
        CHECK(px[c + 0] > 200); // the red survived fill → sample → export
        CHECK(px[c + 1] < 40);

        const auto px2 = read_back(*device, graph.physical(out2), size);
        std::size_t mismatches = 0;
        for (std::uint32_t y = 0; y < size; y += 5) {
            for (std::uint32_t x = 0; x < size; x += 5) {
                const std::size_t at = (static_cast<std::size_t>(y) * size + x) * 4;
                const int checker = (((x / 4) + (y / 4)) % 2) ? 255 : 0;
                if (px2[at + 0] != x || px2[at + 1] != y ||
                    std::abs(static_cast<int>(px2[at + 2]) - checker) > 0)
                    ++mismatches;
            }
        }
        CHECK(mismatches == 0); // the compute pattern crossed compute→raster→export intact

        // (5) timings: one entry per live pass, plausible values, resolvable post-submit.
        const auto timings = graph.resolve_timings(*cmd);
        if (timings.empty()) {
            MESSAGE("device cannot timestamp — timings skipped");
        } else {
            CHECK(timings.size() == 4);
            for (const auto& t : timings)
                CHECK(t.gpu_ms >= 0.0);
        }
    }

    SUBCASE("a pass feeding no output is culled and never executes") {
        Kit kit(*device);
        RenderGraph graph(*device);
        graph.reset();

        RGTexture kept = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-kept"});
        RGTexture dead = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-dead"});

        bool dead_ran = false;
        const RGColorAttachment dead_att[] = {{dead, LoadOp::Clear, StoreOp::Store, {}}};
        graph.add_raster_pass("dead-end", {.colors = dead_att}, [&](CommandBuffer&) {
            dead_ran = true; // nothing reads `dead`, nothing exports it — must never run
        });

        const RGColorAttachment kept_att[] = {{kept, LoadOp::Clear, StoreOp::Store, {}}};
        graph.add_raster_pass("kept", {.colors = kept_att}, [&](CommandBuffer& cmd) {
            cmd.bind_pipeline(kit.fill);
            const float g[4] = {0.0f, 1.0f, 0.0f, 1.0f};
            cmd.push_constants(g, sizeof(g));
            cmd.draw(3);
        });
        graph.export_texture(kept);

        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);

        CHECK_FALSE(dead_ran);
        CHECK(graph.was_culled(0));
        CHECK_FALSE(graph.was_culled(1));
        CHECK(graph.execution_order().size() == 1);
    }

    SUBCASE("the transient cache hands the same physical back next frame") {
        Kit kit(*device);
        RenderGraph graph(*device);

        TextureHandle first_frame{};
        for (int frame = 0; frame < 2; ++frame) {
            graph.reset();
            RGTexture t = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-recycled"});
            const RGColorAttachment att[] = {{t, LoadOp::Clear, StoreOp::Store, {}}};
            graph.add_raster_pass("fill", {.colors = att}, [&](CommandBuffer& cmd) {
                cmd.bind_pipeline(kit.fill);
                const float c[4] = {0.2f, 0.4f, 0.6f, 1.0f};
                cmd.push_constants(c, sizeof(c));
                cmd.draw(3);
            });
            graph.export_texture(t);

            auto cmd = device->begin_commands();
            graph.execute(*cmd);
            device->submit_blocking(*cmd);

            if (frame == 0) {
                first_frame = graph.physical(t);
                CHECK(first_frame.is_valid());
            } else {
                CHECK(graph.physical(t) == first_frame); // recycled, not reallocated
            }
        }
    }

    SUBCASE("versioning semantics: declaration order is data-flow order") {
        // The subtlety worth pinning down in a test: resource VERSIONING means a pass declared
        // before a resource's writer reads the OLDER version — it does not magically reorder
        // after the writer (that would change the frame's declared meaning). Consequences,
        // asserted here: (a) declared order between remaining passes is preserved (with
        // versioned edges, declared order is always a valid topological order — the sort
        // validates and feeds culling, it never surprises); (b) a write whose new version
        // nothing reads is dead and culls, even when an earlier-declared pass read the OLD
        // version of the same resource.
        Kit kit(*device);
        RenderGraph graph(*device);
        graph.reset();

        RGTexture a = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-a"});
        RGTexture b = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-b"});
        RGTexture out = graph.create_texture({{size, size}, Format::RGBA8Unorm, "rg-ord-out"});

        const RGColorAttachment att_out[] = {{out, LoadOp::Clear, StoreOp::Store, {}}};
        const RGTexture reads_b[] = {b};
        graph.add_raster_pass("reads-old-b", // declared FIRST: reads b's initial (v0) contents
                              {.colors = att_out, .sampled = reads_b},
                              [&](CommandBuffer& cmd) {
                                  cmd.bind_pipeline(kit.blit);
                                  cmd.bind_texture(0, graph.physical(b), kit.nearest);
                                  cmd.draw(3);
                              });
        const auto fill_pass = [&](const char* name, const RGColorAttachment(&att)[1]) {
            graph.add_raster_pass(name, {.colors = att}, [&](CommandBuffer& cmd) {
                cmd.bind_pipeline(kit.fill);
                const float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                cmd.push_constants(c, sizeof(c));
                cmd.draw(3);
            });
        };
        const RGColorAttachment att_a[] = {{a, LoadOp::Clear, StoreOp::Store, {}}};
        const RGColorAttachment att_b[] = {{b, LoadOp::Clear, StoreOp::Store, {}}};
        fill_pass("producer-a", att_a);   // live: `a` is exported below
        fill_pass("writes-new-b", att_b); // writes b v1 — which nothing reads ⇒ dead
        graph.export_texture(out);
        graph.export_texture(a);

        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);

        // (b) the v1 writer of b culled; the old-version reader and producer-a survive…
        CHECK(graph.was_culled(2));
        const auto order = graph.execution_order();
        REQUIRE(order.size() == 2);
        // …and (a) in declared order.
        CHECK(order[0] == 0);
        CHECK(order[1] == 1);
    }

    SUBCASE("compile overhead at 100 passes (measured, not asserted)") {
        RenderGraph graph(*device);
        graph.reset();
        // A synthetic chain: pass i fills tex[i] and samples tex[i-1] — maximal dependencies.
        std::vector<RGTexture> texes;
        std::vector<std::array<RGColorAttachment, 1>> atts(100);
        std::vector<std::array<RGTexture, 1>> reads(100);
        for (int i = 0; i < 100; ++i)
            texes.push_back(graph.create_texture({{8, 8}, Format::RGBA8Unorm, "rg-chain"}));
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i) {
            atts[static_cast<std::size_t>(i)] = {RGColorAttachment{
                texes[static_cast<std::size_t>(i)], LoadOp::Clear, StoreOp::Store, {}}};
            RenderGraph::RasterPassDesc desc{};
            desc.colors = atts[static_cast<std::size_t>(i)];
            if (i > 0) {
                reads[static_cast<std::size_t>(i)] = {texes[static_cast<std::size_t>(i - 1)]};
                desc.sampled = reads[static_cast<std::size_t>(i)];
            }
            graph.add_raster_pass("chain", desc, [](CommandBuffer&) {});
        }
        graph.export_texture(texes.back());
        // Compile without recording: execute needs an encoder, so time declaration+compile via a
        // throwaway recording into a fresh command buffer, minus the passes' (empty) bodies.
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        const auto t1 = std::chrono::steady_clock::now();
        device->submit_blocking(*cmd);
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        MESSAGE("declare+compile+record, 100-pass chain: ", ms, " ms");
        CHECK(graph.execution_order().size() == 100);
    }
}
