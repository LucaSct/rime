// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 06-render-graph — the render graph, demonstrated (M5.8). A small procedural PBR scene (a row of
// metallic×roughness spheres on a checkered floor, one point light) is drawn through the M5.6 pass
// library — depth pre-pass → forward PBR into HDR → tonemap — declared on a render::RenderGraph via
// the SceneRenderer. The sample prints the graph's COMPILED PASS ORDER and PER-PASS GPU TIME, then
// shows the milestone's "adding a pass is easy" claim: with --vignette, one extra post pass is
// spliced onto the frame in ~10 lines of C++ (see add_vignette_pass below) and the graph re-derives
// the order and the barrier for it automatically.
//
// Headless by default (this is a lavapipe/CI-friendly proof): it renders off-screen, reads the
// result back, self-checks that the scene is genuinely lit, and returns non-zero on failure — so it
// doubles as a smoke test. A windowed presentation is the ADR-0023 §4 seam (07-first-light on a
// display); this sample is about the GRAPH.
//
// Run it:   build/dev/bin/render_graph_sample
//           build/dev/bin/render_graph_sample --vignette
//           build/dev/bin/render_graph_sample --ppm out.ppm

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "rime/core/math/quat.hpp"
#include "rime/core/math/scalar.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/rhi.hpp"

// The demo post pass's shaders (rime_add_shaders, ADR-0008 offline compile).
#include "fullscreen.vert.spv.h"
#include "vignette.frag.spv.h"

namespace {

using namespace rime;
using render::RenderGraph;
using render::RGColorAttachment;
using render::RGTexture;

constexpr std::uint32_t kWidth = 640;
constexpr std::uint32_t kHeight = 480;

// Build the procedural scene into `world`: five metallic×roughness spheres in a row on a checkered
// floor, a point light up-front, and a camera looking at them. Pure scene-layer / ECS calls — the
// same registries and components tests and 07-first-light use.
void build_scene(render::MeshRegistry& meshes,
                 render::MaterialRegistry& materials,
                 ecs::World& world) {
    using ecs::WorldTransform;
    render::register_render_components(world);

    const render::MeshId sphere = meshes.add(render::make_uv_sphere(0.7f, 32, 64), "sphere");
    const render::MeshId floor = meshes.add(render::make_plane(6.0f, 6.0f), "floor");

    // A 2×2 checker so the floor shows the texture path (sRGB base-color map).
    // (Kept tiny and procedural — the M6 asset pipeline brings real textures.)
    constexpr std::uint32_t kMetal = 5;
    for (std::uint32_t i = 0; i < kMetal; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kMetal - 1); // 0..1 roughness
        render::PbrMaterialDesc m{};
        m.base_color[0] = 0.9f;
        m.base_color[1] = 0.75f;
        m.base_color[2] = 0.4f; // gold-ish
        m.metallic = 1.0f;
        m.roughness = 0.05f + 0.9f * t;
        const render::MaterialId mat = materials.add(m);

        core::Transform tf{};
        tf.translation = {(static_cast<float>(i) - 2.0f) * 1.6f, 0.7f, 0.0f};
        (void)world.spawn_with(
            WorldTransform{tf}, render::MeshRef{sphere}, render::MaterialRef{mat});
    }

    render::PbrMaterialDesc floor_mat{};
    floor_mat.base_color[0] = 0.5f;
    floor_mat.base_color[1] = 0.5f;
    floor_mat.base_color[2] = 0.55f;
    floor_mat.metallic = 0.0f;
    floor_mat.roughness = 0.6f;
    const render::MaterialId floor_id = materials.add(floor_mat);
    (void)world.spawn_with(WorldTransform{}, render::MeshRef{floor}, render::MaterialRef{floor_id});

    // Camera: up and back, pitched down a little to take in the spheres and the floor.
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 2.2f, 7.5f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -core::radians(15.0f));
    (void)world.spawn_with(WorldTransform{cam_tf}, render::Camera{});

    // One warm point light, up-front-right.
    core::Transform light_tf{};
    light_tf.translation = {3.5f, 5.0f, 4.0f};
    (void)world.spawn_with(WorldTransform{light_tf},
                           render::PointLight{1.0f, 0.95f, 0.85f, 55.0f, 40.0f});
}

// The vignette pipeline (created once): fullscreen triangle + the vignette fragment shader, one
// combined-image-sampler binding, writing the sample's LDR format.
struct PostPipeline {
    rhi::ShaderHandle vs, fs;
    rhi::PipelineHandle pipeline;
    rhi::SamplerHandle sampler;
};

PostPipeline make_post_pipeline(rhi::Device& device) {
    PostPipeline pp;
    rhi::ShaderDesc vsd{};
    vsd.stage = rhi::ShaderStage::Vertex;
    vsd.spirv = fullscreen_vert_spv;
    vsd.spirv_size_bytes = sizeof(fullscreen_vert_spv);
    vsd.debug_name = "fullscreen.vert";
    pp.vs = device.create_shader(vsd);
    rhi::ShaderDesc fsd{};
    fsd.stage = rhi::ShaderStage::Fragment;
    fsd.spirv = vignette_frag_spv;
    fsd.spirv_size_bytes = sizeof(vignette_frag_spv);
    fsd.debug_name = "vignette.frag";
    pp.fs = device.create_shader(fsd);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}};
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = pp.vs;
    pd.fragment_shader = pp.fs;
    pd.color_format = render::kLdrFormat;
    pd.cull = rhi::CullMode::None;
    pd.bindings = bindings;
    pd.debug_name = "vignette";
    pp.pipeline = device.create_graphics_pipeline(pd);

    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Nearest;
    sd.min_filter = rhi::Filter::Nearest;
    sd.address_mode = rhi::AddressMode::ClampToEdge;
    pp.sampler = device.create_sampler(sd);
    return pp;
}

// ── "Adding a pass is easy" ──────────────────────────────────────────────────────────────────────
// This is the whole demonstration: given the tonemapped image `src`, declare ONE more pass that
// samples it and writes a vignetted copy, and hand back the new handle. The graph sees `src` in the
// pass's `sampled` set, so it orders this pass after tonemap and inserts the read-after-write
// barrier on its own — no manual synchronization, no pipeline-order bookkeeping.
RGTexture
add_vignette_pass(RenderGraph& graph, const PostPipeline& pp, RGTexture src, rhi::Extent2D extent) {
    const RGTexture dst = graph.create_texture({extent, render::kLdrFormat, "vignette-out"});
    const RGColorAttachment color{dst, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}};
    const RGTexture sampled[] = {src};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = {&color, 1};
    desc.sampled = sampled;
    graph.add_raster_pass("vignette", desc, [&pp, &graph, src](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pp.pipeline);
        cmd.bind_texture(0, graph.physical(src), pp.sampler);
        cmd.draw(3);
    });
    graph.export_texture(dst);
    return dst;
}

// Read an RGBA8 texture back to CPU bytes (the tests/rhi pattern).
std::vector<std::uint8_t>
read_rgba8(rhi::Device& device, rhi::TextureHandle tex, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 4;
    rhi::BufferDesc bd{};
    bd.size = bytes;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle rb = device.create_buffer(bd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(tex, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

// Self-check: the scene must be genuinely LIT — a fair fraction of pixels clearly brighter than the
// black background, and a few bright highlights on the metal. Catches "rendered nothing / all
// black" without pinning driver-specific colors. Returns true on pass.
bool scene_is_lit(const std::vector<std::uint8_t>& px) {
    std::uint64_t lit = 0, bright = 0;
    const std::size_t n = px.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const int r = px[i * 4 + 0], g = px[i * 4 + 1], b = px[i * 4 + 2];
        const int lum = (r + g + b) / 3;
        if (lum > 30)
            ++lit;
        if (lum > 180)
            ++bright;
    }
    std::printf("  self-check: %llu lit / %llu bright of %zu px\n",
                static_cast<unsigned long long>(lit),
                static_cast<unsigned long long>(bright),
                n);
    return lit > n / 50 && bright > 20; // at least ~2% lit and a real highlight somewhere
}

void write_ppm(const char* path,
               const std::vector<std::uint8_t>& px,
               std::uint32_t w,
               std::uint32_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
        std::fwrite(&px[i * 4], 1, 3, f);
    std::fclose(f);
    std::printf("  wrote %s\n", path);
}

int run(bool vignette, const char* ppm) {
    rhi::DeviceDesc dd{};
    dd.app_name = "06-render-graph";
    auto device = rhi::create_device(dd);
    if (!device) {
        std::fprintf(stderr, "06-render-graph: no Vulkan device (need a driver or lavapipe)\n");
        return std::getenv("RIME_REQUIRE_VULKAN") ? 1 : 0; // absent GPU: a skip, unless required
    }
    std::printf("06-render-graph: rendering on '%s' (%ux%u)%s\n",
                device->adapter().name.c_str(),
                kWidth,
                kHeight,
                vignette ? ", + vignette post pass" : "");

    render::MeshRegistry meshes(*device);
    render::MaterialRegistry materials;
    ecs::World world;
    build_scene(meshes, materials, world);

    render::SceneRenderer renderer(*device, meshes, materials);
    RenderGraph graph(*device);
    const PostPipeline post = make_post_pipeline(*device);

    // Declare + execute the frame.
    graph.reset();
    const render::SceneRenderer::Output out =
        renderer.render(graph, world, {kWidth, kHeight}, true);
    if (!out.ldr.is_valid()) {
        std::fprintf(stderr, "06-render-graph: scene produced no output (no active camera?)\n");
        return 1;
    }
    RGTexture final_tex = out.ldr;
    if (vignette)
        final_tex = add_vignette_pass(graph, post, out.ldr, {kWidth, kHeight});

    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);

    // Report what the graph compiled: the surviving passes in execution order, each with its GPU
    // time — the graph's introspection the samples and tools read.
    std::printf("06-render-graph: compiled %zu passes:\n", graph.execution_order().size());
    const auto timings = graph.resolve_timings(*cmd);
    if (!timings.empty()) {
        for (const auto& t : timings)
            std::printf(
                "  [%-14.*s] %.3f ms\n", static_cast<int>(t.name.size()), t.name.data(), t.gpu_ms);
    } else {
        for (const std::uint32_t p : graph.execution_order())
            std::printf("  [%s]\n", graph.pass_name(p).c_str());
        std::printf("  (per-pass GPU timing unavailable on this device)\n");
    }

    const std::vector<std::uint8_t> px =
        read_rgba8(*device, graph.physical(final_tex), kWidth, kHeight);
    const bool ok = scene_is_lit(px);
    if (ppm)
        write_ppm(ppm, px, kWidth, kHeight);

    // Tidy up the sample-owned GPU objects (the registries/renderer clean up their own).
    device->destroy(post.pipeline);
    device->destroy(post.sampler);
    device->destroy(post.vs);
    device->destroy(post.fs);

    std::printf("06-render-graph: %s\n",
                ok ? "scene is lit — graph frame OK." : "FAILED self-check.");
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    bool vignette = false;
    const char* ppm = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--vignette")
            vignette = true;
        else if (a == "--ppm" && i + 1 < argc)
            ppm = argv[++i];
        else if (a == "--headless") {
            // default; accepted for symmetry with the other samples
        }
    }
    return run(vignette, ppm);
}
