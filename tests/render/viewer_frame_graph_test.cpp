// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M5.9 — the render graph's DOGFOOD acceptance (ADR-0016 rule 4, test-only per the M5 plan). The
// ICEM viewer (Frostlens, samples/03-icem-viewer) draws its cross-section frame as a hand-recorded
// sequence inside ONE begin/end_rendering scope (cap.hpp::record_section): a lit, clip-planed part;
// a stencil pass that MARKS where the cut plane lies inside the solid; a solid CAP that fills
// exactly those pixels; and — over the top — an alpha-tested UI overlay. This test re-expresses the
// very same frame as FOUR separate rime::render::RenderGraph passes sharing ONE colour target and
// ONE D32FloatS8 depth+stencil target, and checks the pixels still meet the viewer tests'
// expectations. It is the proof that the graph's resource model covers what a real application asks
// of it:
//
//   * a depth+STENCIL attachment as a graph transient — D32FloatS8, its TextureUsage::DepthStencil
//     accumulated from the declared DepthTarget accesses (RGTextureDesc carries no usage field);
//   * LOAD / keep semantics ACROSS pass boundaries — the stencil the "cut-mark" pass writes must
//     survive into a LATER, separate begin_rendering (the "cap" pass) for its stencil test to see
//     it, and the colour each pass accumulates must survive to the next (mesh → cap → UI) instead
//     of being re-cleared;
//   * a read_only depth attachment — the cap TESTS the stencil but writes neither depth nor
//     stencil, which the compiler orders as read-after-write, not another write.
//
// A failure here is loud, not subtle: a dropped stencil ⇒ the cap fills nothing ⇒ the cut face is
// identical with and without it; a re-cleared colour ⇒ the overlay pass wipes the 3-D scene. The
// test reuses the viewer's OWN shaders and resource helpers (make_mesh / make_cap /
// make_ui_renderer), so only the frame COMPOSITION — the graph — is under test, not new rendering
// code. Offscreen + readback, GPU-free on lavapipe in CI. (main() lives in render_graph_test.cpp.)

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "rime/render/orbit_camera.hpp"
#include "rime/render/render_graph.hpp"

// The viewer's own headers + shaders. samples/03-icem-viewer is on this target's include path (see
// tests/render/CMakeLists.txt) and its shaders are compiled to these *.spv.h — so the dogfood
// really eats the viewer's food.
#include "cap.frag.spv.h"
#include "cap.hpp"
#include "cap.vert.spv.h"
#include "capmark.frag.spv.h"
#include "mesh.frag.spv.h"
#include "mesh.vert.spv.h"
#include "mesh_render.hpp"
#include "stl.hpp"
#include "ui.frag.spv.h"
#include "ui.hpp"
#include "ui.vert.spv.h"
#include "ui_render.hpp"

namespace {

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// The z-ramp field the viewer's colormap reads: value 0 at the bottom (k=0), 1 at the top; validity
// 1 everywhere; a 2×2×2 RGBA32F volume. The very fixture the B2b cap proof uses, so the cut face
// shows the same cold-blue → hot-red slice and the same pixel expectations transfer.
std::vector<float> z_ramp_field() {
    std::vector<float> vol(2 * 2 * 2 * 4, 0.0f);
    for (int k = 0; k < 2; ++k)
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) {
                const std::size_t g = static_cast<std::size_t>(i) + 2 * (j + 2 * k);
                vol[g * 4 + 0] = (k == 0) ? 0.0f : 1.0f; // value
                vol[g * 4 + 1] = 1.0f;                   // validity
                vol[g * 4 + 3] = 1.0f;
            }
    return vol;
}

// Render the Frostlens cross-section frame as a render graph, offscreen, and read it back as RGBA8.
// `do_cap` adds the stencil-mark + solid-cap passes; `do_ui` adds the alpha-tested overlay pass.
// `order_out`, when non-null, receives the compiled pass names in execution order.
std::vector<std::uint8_t> render_frostlens(rime::rhi::Device& device,
                                           std::uint32_t size,
                                           bool do_cap,
                                           bool do_ui,
                                           std::vector<std::string>* order_out = nullptr) {
    using namespace rime::rhi;
    using namespace rime::render;
    namespace vw = rime::viewer;
    namespace vui = rime::viewer::ui;

    // ── The scene: a unit cube cut open on its +x half, framed by the orbit camera, coloured by
    // the z-ramp field. Identical setup to tests/rhi/cap_offscreen_test so the same pixel
    // expectations hold — only the RECORDING path (a graph, not one hand-rolled pass) is different.
    // ──
    const vw::CpuMesh cube = vw::make_unit_cube(); // x,y,z ∈ [-1, 1]
    const float r = cube.radius();
    const std::vector<float> field = z_ramp_field();

    OrbitCamera cam;
    cam.frame(cube.center(), cube.radius(), 1.0f);
    cam.yaw = rime::core::radians(35.0f);
    cam.pitch = rime::core::radians(20.0f);

    vw::MeshPush mp{};
    mp.mvp = cam.view_proj(1.0f);
    const auto eye = cam.eye();
    mp.cam_pos[0] = eye.x;
    mp.cam_pos[1] = eye.y;
    mp.cam_pos[2] = eye.z;
    mp.cam_pos[3] = 1.0f;
    mp.clip_plane[0] = 1.0f; // normal +x
    mp.clip_plane[1] = 0.0f;
    mp.clip_plane[2] = 0.0f;
    mp.clip_plane[3] = 0.0f; // discard x > 0 — the cut face is the plane x = 0
    for (int c = 0; c < 3; ++c) {
        mp.field_scale[c] = 0.25f;
        mp.field_bias[c] = 0.5f;
    }
    mp.field_scale[3] = 0.0f; // vmin
    mp.field_bias[3] = 1.0f;  // vmax

    vw::CapPush cp{};
    cp.mvp = mp.mvp;
    for (int c = 0; c < 3; ++c) {
        cp.field_scale[c] = mp.field_scale[c];
        cp.field_bias[c] = mp.field_bias[c];
    }
    cp.field_scale[3] = 0.0f;
    cp.field_bias[3] = 1.0f;
    cp.cap_rect[0] = -r; // x-plane → the two in-plane axes are y and z
    cp.cap_rect[1] = r;
    cp.cap_rect[2] = -r;
    cp.cap_rect[3] = r;
    cp.cap_meta[0] = 0.0f; // plane offset
    cp.cap_meta[1] = 0.0f; // axis = x

    // ── GPU resources, built by the viewer's own helpers. D32FloatS8 so the depth target carries
    // stencil; the cap re-uses the mesh's field volume for the slice colour. ──
    const vw::GpuMesh mesh = vw::make_mesh(device,
                                           Format::RGBA8Unorm,
                                           Format::D32FloatS8,
                                           cube,
                                           mesh_vert_spv,
                                           sizeof(mesh_vert_spv),
                                           mesh_frag_spv,
                                           sizeof(mesh_frag_spv),
                                           field.data(),
                                           2,
                                           2,
                                           2);
    const vw::Cap cap = vw::make_cap(device,
                                     Format::RGBA8Unorm,
                                     Format::D32FloatS8,
                                     mesh_vert_spv,
                                     sizeof(mesh_vert_spv),
                                     capmark_frag_spv,
                                     sizeof(capmark_frag_spv),
                                     cap_vert_spv,
                                     sizeof(cap_vert_spv),
                                     cap_frag_spv,
                                     sizeof(cap_frag_spv));

    // The UI overlay: a small corner panel + label, built once into a streamed vertex batch. Only
    // built when asked, so the `do_ui = false` frames record no overlay pass at all.
    vui::UiRenderer uir{};
    if (do_ui) {
        uir = vui::make_ui_renderer(device,
                                    Format::RGBA8Unorm,
                                    ui_vert_spv,
                                    sizeof(ui_vert_spv),
                                    ui_frag_spv,
                                    sizeof(ui_frag_spv),
                                    /*max_vertices=*/4096);
        vui::Ui ui;
        ui.begin(static_cast<float>(size), static_cast<float>(size), -1.0f, -1.0f, false);
        ui.panel(6, 6, 110, 44); // top-left corner, clear of the cut at the frame centre
        ui.label("ICEM");
        ui.end();
        vui::upload_ui(device, uir, ui);
    }

    // ── The frame, declared as a graph. One colour target (exported → read back) and one shared
    // depth+stencil target (a plain transient); the pass ORDER and every barrier fall out of the
    // declared attachment accesses. All four passes reference the SAME `ds` handle, so the compiler
    // hands them one physical D32FloatS8 image — the sharing under test. ──
    RenderGraph graph(device);
    graph.reset();
    RGTexture color = graph.create_texture({{size, size}, Format::RGBA8Unorm, "frostlens-color"});
    RGTexture ds =
        graph.create_texture({{size, size}, Format::D32FloatS8, "frostlens-depth-stencil"});
    graph.export_texture(color);

    const Extent2D extent{size, size};
    const ClearColor clear{0.05f, 0.05f, 0.06f, 1.0f};

    // Pass 1 — lit mesh. The ONLY pass that clears: it establishes the colour, the depth, AND the
    // (zeroed) stencil the rest of the frame loads and builds on.
    const RGColorAttachment mesh_color[] = {{color, LoadOp::Clear, StoreOp::Store, clear}};
    RGDepthAttachment mesh_ds{};
    mesh_ds.texture = ds;
    mesh_ds.load = LoadOp::Clear; // clears depth (1.0) AND stencil (0)
    mesh_ds.store = StoreOp::Store;
    mesh_ds.clear_depth = 1.0f;
    mesh_ds.clear_stencil = 0;
    {
        RenderGraph::RasterPassDesc d{};
        d.colors = mesh_color;
        d.depth = &mesh_ds;
        graph.add_raster_pass(
            "mesh", d, [&](CommandBuffer& cmd) { vw::draw_mesh(cmd, mesh, extent, mp); });
    }

    // Passes 2 & 3 carry the cross-section. Both KEEP the colour built so far (LoadOp::Load) — a
    // re-clear here would erase the lit part.
    const RGColorAttachment keep_color[] = {{color, LoadOp::Load, StoreOp::Store, {}}};

    // Pass 2 — cut-mark: re-draw the part flipping the stencil parity bit where the plane is inside
    // the solid (colour + depth writes masked by the pipeline). It LOADS the depth+stencil and
    // STORES it so the mark reaches the cap pass; it WRITES stencil, so read_only stays false.
    RGDepthAttachment mark_ds{};
    mark_ds.texture = ds;
    mark_ds.load = LoadOp::Load;
    mark_ds.store = StoreOp::Store;
    if (do_cap) {
        RenderGraph::RasterPassDesc d{};
        d.colors = keep_color;
        d.depth = &mark_ds;
        graph.add_raster_pass("cut-mark", d, [&](CommandBuffer& cmd) {
            cmd.bind_pipeline(cap.mark_pipeline);
            cmd.push_constants(&mp,
                               sizeof(vw::MeshPush)); // the clip plane, reused from the mesh push
            cmd.bind_vertex_buffer(mesh.vbuf, 0);
            cmd.draw(mesh.vertex_count);
        });
    }

    // Pass 3 — solid cap: a plane quad kept only where the mark left stencil parity odd. It only
    // TESTS the stencil, so the depth attachment is read_only — the compiler makes this a
    // read-after-write on cut-mark, not another write. This is the pass the whole test hinges on:
    // its stencil `== 1` test can only match if the graph preserved the mark across the
    // begin_rendering boundary.
    RGDepthAttachment cap_ds{};
    cap_ds.texture = ds;
    cap_ds.load = LoadOp::Load;
    cap_ds.store = StoreOp::DontCare;
    cap_ds.read_only = true;
    if (do_cap) {
        RenderGraph::RasterPassDesc d{};
        d.colors = keep_color;
        d.depth = &cap_ds;
        graph.add_raster_pass("cap", d, [&](CommandBuffer& cmd) {
            cmd.bind_pipeline(cap.cap_pipeline);
            cmd.bind_texture(0, mesh.field_tex, mesh.field_sampler); // field volume → slice colour
            cmd.push_constants(&cp, sizeof(vw::CapPush));
            cmd.draw(6); // two triangles spanning the cut plane
        });
    }

    // Pass 4 — UI overlay: alpha-tested widgets over the finished scene. Colour-ONLY (the UI
    // pipeline declares no depth/stencil format, so it must bind no depth attachment), LoadOp::Load
    // so it paints ON TOP of the 3-D image instead of clearing it — the other half of load/keep.
    if (do_ui && uir.vertex_count > 0) {
        RenderGraph::RasterPassDesc d{};
        d.colors = keep_color;
        graph.add_raster_pass("ui", d, [&](CommandBuffer& cmd) {
            cmd.bind_pipeline(uir.pipeline);
            cmd.bind_texture(0, uir.atlas, uir.sampler);
            vui::UiPush push{};
            push.screen[0] = static_cast<float>(size);
            push.screen[1] = static_cast<float>(size);
            cmd.push_constants(&push, sizeof(vui::UiPush));
            cmd.bind_vertex_buffer(uir.vbuf, 0);
            cmd.draw(uir.vertex_count);
        });
    }

    // ── Compile + record + submit, then copy the exported colour to a readback buffer (in the same
    // command buffer — the graph leaves the colour in a color-attachment layout the copy
    // transitions from). ──
    BufferDesc rbd{};
    rbd.size = static_cast<std::uint64_t>(size) * size * 4;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "frostlens-readback";
    const BufferHandle readback = device.create_buffer(rbd);

    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    cmd->copy_texture_to_buffer(graph.physical(color), readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rbd.size));
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    if (order_out != nullptr) {
        order_out->clear();
        for (const std::uint32_t pi : graph.execution_order())
            order_out->emplace_back(graph.pass_name(pi));
    }

    device.destroy(readback);
    if (do_ui)
        vui::destroy_ui_renderer(device, uir);
    vw::destroy_cap(device, cap);
    vw::destroy_mesh(device, mesh);
    return pixels;
}

} // namespace

TEST_CASE("M5.9 dogfood: the viewer's cross-section frame as a render graph "
          "(shared depth+stencil, load/keep)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the viewer-frame graph proof");
        return;
    }

    // constexpr so the addressing lambda can use it WITHOUT capturing it — a plain `[size]` capture
    // is flagged unused by Apple Clang's -Wunused-lambda-capture (the value is a compile-time
    // constant, so no capture is needed), and this build treats warnings as errors.
    constexpr std::uint32_t size = 128;
    const auto at = [](const std::vector<std::uint8_t>& px, std::uint32_t x, std::uint32_t y) {
        return &px[(static_cast<std::size_t>(y) * size + x) * 4];
    };

    // Three renders of the same scene differing only in which passes run: the full frame (with its
    // pass order captured), the same 3-D frame without the UI overlay, and the frame without the
    // cross-section cap. The DIFFERENCES between them isolate each graph claim.
    std::vector<std::string> order;
    const std::vector<std::uint8_t> full = render_frostlens(*device, size, true, true, &order);
    const std::vector<std::uint8_t> capped = render_frostlens(*device, size, true, false);
    const std::vector<std::uint8_t> bare = render_frostlens(*device, size, false, false);
    REQUIRE(full.size() == static_cast<std::size_t>(size) * size * 4);

    // ── (0) The graph derived the one legal order from the shared-attachment dependencies, and
    // culled nothing: mesh → cut-mark → cap → ui. The colour LoadOp::Load chain plus the
    // depth-stencil write→write→read chain admit no other schedule. ──
    REQUIRE(order.size() == 4);
    CHECK(order[0] == "mesh");
    CHECK(order[1] == "cut-mark");
    CHECK(order[2] == "cap");
    CHECK(order[3] == "ui");
    MESSAGE(
        "compiled pass order: ", order[0], " -> ", order[1], " -> ", order[2], " -> ", order[3]);

    // ── (1) THE claim: a stencil written in one pass survives into a later, SEPARATE
    // begin_rendering. The centre pixel sits on the x=0 cut; with the cap it is the flat field
    // slice, without it the lit interior wall behind — they must differ. Were the stencil lost
    // across the pass boundary, the cap's `== 1` test would match nothing and the two frames would
    // be identical here. (Same assertion as tests/rhi/cap_offscreen_test — now across graph
    // passes.) ──
    const std::uint8_t* capc = at(capped, size / 2, size / 2);
    const std::uint8_t* barec = at(bare, size / 2, size / 2);
    const int dr = capc[0] - barec[0], dg = capc[1] - barec[1], db = capc[2] - barec[2];
    CHECK((dr * dr + dg * dg + db * db) > 200); // a clear colour change at the cut

    // ── (2) The cut face shows the field colormap — cold (blue) and hot (red) both present, and no
    // collapsed-normal black holes — so the cap pass really sampled the field volume through the
    // graph (the same expectation the B2b proof asserts). ──
    std::size_t blue = 0, red = 0, black = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &capped[static_cast<std::size_t>(i) * 4];
        if (p[0] < 8 && p[1] < 8 && p[2] < 8)
            ++black;
        const float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        if (lum <= 40.0f)
            continue;
        if (p[2] > p[0] + 25 && p[2] > p[1])
            ++blue;
        else if (p[0] > p[1] + 25 && p[0] > p[2] + 25)
            ++red;
    }
    CHECK(blue > 20);
    CHECK(red > 10);
    CHECK(black == 0);

    // ── (3) The lit part rendered at all — depth-tested 3-D through the graph — so the uncapped
    // frame is far from empty (brighter than the {13,13,15} clear across a big patch). ──
    std::size_t lit = 0;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        const std::uint8_t* p = &bare[static_cast<std::size_t>(i) * 4];
        if (p[0] > 25 || p[1] > 25 || p[2] > 25)
            ++lit;
    }
    CHECK(lit > 1000);

    // ── (4) The UI overlay is a final colour-LOAD pass: it must paint its panel yet KEEP the scene
    // everywhere it doesn't cover. `full` and `capped` are the same frame ± the UI pass, so the
    // only pixels that may differ live inside the overlay's corner panel. A substantial change
    // there proves the overlay drew; ZERO change outside it proves LoadOp::Load preserved the 3-D
    // image (a re-clear would have wiped it). ──
    std::size_t overlay = 0, stray = 0;
    for (std::uint32_t y = 0; y < size; ++y)
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::uint8_t* a = at(full, x, y);
            const std::uint8_t* b = at(capped, x, y);
            const int e0 = a[0] - b[0], e1 = a[1] - b[1], e2 = a[2] - b[2];
            if (e0 * e0 + e1 * e1 + e2 * e2 > 100) {
                ++overlay;
                const bool in_panel =
                    x >= 4 && x <= 118 && y >= 4 && y <= 52; // panel (6,6)–(116,50)
                if (!in_panel)
                    ++stray;
            }
        }
    CHECK(overlay > 800); // the panel + label left a clear mark
    CHECK(stray == 0);    // and ONLY inside the panel — the scene under/around it is byte-identical
}
