// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The ID-buffer picker's implementation (M9.6). See scene_picker.hpp for the technique; the notes
// here are about the moving parts: the push-constant draw loop, the shifted viewport, and the
// ticket-based readback.

#include "rime/render/scene_picker.hpp"

#include <cstring>
#include <utility>

#include "pick_id.frag.spv.h"
#include "pick_id.vert.spv.h"

namespace rime::render {

namespace {

// The per-draw push-constant block, mirroring pick_id.vert/.frag: a column-major MVP then the
// 1-based draw id. A flat float[16] — NOT a core::Mat4 member — because Mat4 is alignas(16), which
// would pad this struct to 80 bytes and push 12 bytes of garbage past the declared range; the
// static_assert is the tripwire. 68 bytes sits comfortably under the 128-byte push floor every
// Vulkan device guarantees.
struct DrawPush {
    float mvp[16];
    std::uint32_t id; // 1-based: 0 is the cleared "nothing" sentinel
};

static_assert(sizeof(DrawPush) == 68, "DrawPush must match the shaders' push_constant block");

} // namespace

ScenePicker::ScenePicker(rhi::Device& device, const MeshRegistry& meshes)
    : device_(device), meshes_(meshes), graph_(device) {
    rhi::ShaderDesc vs{};
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = pick_id_vert_spv;
    vs.spirv_size_bytes = sizeof(pick_id_vert_spv);
    vs.debug_name = "pick_id.vert";
    vertex_shader_ = device.create_shader(vs);

    rhi::ShaderDesc fs{};
    fs.stage = rhi::ShaderStage::Fragment;
    fs.spirv = pick_id_frag_spv;
    fs.spirv_size_bytes = sizeof(pick_id_frag_spv);
    fs.debug_name = "pick_id.frag";
    fragment_shader_ = device.create_shader(fs);

    // The pick pipeline mirrors the forward pass's visibility decisions — back-face culling and a
    // Less depth test — so what is pickable is exactly what is visible. The vertex layout is the
    // full registry vertex with only position consumed (the depth pre-pass's shared-layout trick).
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.vertex_layout.stride = MeshRegistry::vertex_stride();
    pd.vertex_layout.attributes = MeshRegistry::vertex_attributes();
    pd.color_format = rhi::Format::R32Uint;
    pd.cull = rhi::CullMode::Back;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_compare = rhi::CompareOp::Less;
    pd.depth_format = kDepthFormat;
    pd.push_constant_size = sizeof(DrawPush);
    pd.debug_name = "pick-id";
    pipeline_ = device.create_graphics_pipeline(pd);

    // The persistent 4-byte readback. The 1×1 render targets are NOT owned here: they are graph
    // transients (created in begin_pick, recycled by the graph's desc-keyed cache), so the graph
    // knows their extent — which is what its automatic viewport/scissor preset and usage
    // accumulation key off. Only the buffer must persist, because the CPU reads it after the
    // submission completes, past the graph's next reset.
    rhi::BufferDesc rbd{};
    rbd.size = sizeof(std::uint32_t);
    rbd.usage = rhi::BufferUsage::TransferDst;
    rbd.memory = rhi::MemoryUsage::GpuToCpu;
    rbd.debug_name = "pick-readback";
    readback_ = device.create_buffer(rbd);
}

ScenePicker::~ScenePicker() {
    // Drain before destroy (the s1.1 contract): an in-flight pick's copy still references the
    // targets and readback buffer; freeing under it would be a use-after-free on the GPU timeline.
    if (pending_) {
        device_.wait(ticket_);
    }
    device_.destroy(readback_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

void ScenePicker::begin_pick(ecs::World& world,
                             rhi::Extent2D extent,
                             std::int32_t x,
                             std::int32_t y) {
    if (pending_) {
        // Defensive latest-wins (documented in the header): finish and discard the stale pick so
        // its slot is free. The editor host queues instead of hitting this.
        device_.wait(ticket_);
        pending_ = false;
        ticket_ = {};
    }
    pending_ = true;
    immediate_miss_ = true; // until a submission actually goes out
    ticket_ = {};

    if (x < 0 || y < 0 || extent.width == 0 || extent.height == 0 ||
        x >= static_cast<std::int32_t>(extent.width) ||
        y >= static_cast<std::int32_t>(extent.height)) {
        return; // outside the rendered view: nothing to hit, answered without the GPU
    }

    ExtractedScene scene = extract_scene(world);
    if (!scene.camera.found || scene.draws.empty()) {
        return; // nothing rendered ⇒ nothing pickable
    }
    pick_entities_ = std::move(scene.draw_entities);

    // The same clip-from-world the SceneRenderer builds (scene_renderer.cpp) — the pick must see
    // the scene through the viewport's exact lens or clicks would land beside their pixels.
    const float aspect =
        static_cast<float>(extent.width) / static_cast<float>(extent.height); // height > 0 above
    const core::Mat4 view_proj =
        core::perspective(scene.camera.fov_y, aspect, scene.camera.z_near, scene.camera.z_far) *
        scene.camera.view;

    // Fold MVP + id per draw. Computed here (not in the λ) so the pass body is pure recording.
    std::vector<DrawPush> pushes(scene.draws.size());
    for (std::size_t i = 0; i < scene.draws.size(); ++i) {
        const core::Mat4 mvp = view_proj * scene.draws[i].model;
        std::memcpy(pushes[i].mvp, mvp.m, sizeof(pushes[i].mvp));
        pushes[i].id = static_cast<std::uint32_t>(i) + 1; // 0 is "nothing"
    }

    // Declare the one-pass frame. The 1×1 targets are graph TRANSIENTS: the first pick allocates
    // them, every later pick recycles them from the graph's desc-keyed cache — and because the
    // graph knows their extent, its automatic viewport/scissor preset is well-formed (an imported
    // raw handle has no extent the graph could preset from). Exporting the id target keeps its
    // producer from being culled and adds TransferSrc for the readback copy below.
    graph_.reset();
    const RGTexture id_rt = graph_.create_texture({{1, 1}, rhi::Format::R32Uint, "pick-id"});
    const RGTexture depth_rt = graph_.create_texture({{1, 1}, kDepthFormat, "pick-depth"});
    graph_.export_texture(id_rt);

    // Clear = {0,0,0,0}: all-zero bits are the same clear through the float-shaped ClearColor as
    // through a uint one (see the header), so the integer target starts at the "nothing" id.
    const RGColorAttachment colors[] = {
        {id_rt, rhi::LoadOp::Clear, rhi::StoreOp::Store, {0.0f, 0.0f, 0.0f, 0.0f}}};
    const RGDepthAttachment depth_att{
        depth_rt, rhi::LoadOp::Clear, rhi::StoreOp::DontCare, 1.0f, 0, false};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.depth = &depth_att;

    // The λ owns its data by value (moved) — execute() runs inside this call today, but a stored
    // pass body referencing dead locals is exactly the bug a later refactor would trip over.
    const auto fx = static_cast<float>(x);
    const auto fy = static_cast<float>(y);
    const auto fw = static_cast<float>(extent.width);
    const auto fh = static_cast<float>(extent.height);
    graph_.add_raster_pass(
        "pick-id",
        desc,
        [this, fx, fy, fw, fh, draws = std::move(scene.draws), pushes = std::move(pushes)](
            rhi::CommandBuffer& cmd) {
            cmd.bind_pipeline(pipeline_);
            // The viewport shift: a full W×H viewport positioned at (−x, −y) maps NDC exactly as
            // the real frame does, but slides pixel (x, y) onto the 1×1 target at (0, 0). Negative
            // viewport origins are core Vulkan (viewportBoundsRange reaches well past −W). The
            // scissor pins rasterization to the one texel that exists.
            cmd.set_viewport({-fx, -fy, fw, fh, 0.0f, 1.0f});
            cmd.set_scissor({0, 0, 1, 1});
            for (std::size_t i = 0; i < draws.size(); ++i) {
                const GpuMesh& mesh = meshes_.get(draws[i].mesh);
                cmd.bind_vertex_buffer(mesh.vertices);
                cmd.bind_index_buffer(mesh.indices, rhi::IndexType::Uint32);
                cmd.push_constants(&pushes[i], sizeof(DrawPush));
                cmd.draw_indexed(mesh.index_count);
            }
        });

    // Record render + readback into one submission and hand it to the GPU WITHOUT waiting (the
    // async seam). try_resolve() polls the ticket; the host's frame loop never stalls on a click.
    // physical() is valid for the exported target after execute(), and the cached transient it
    // names survives until the NEXT reset() — by which time this submission has been waited out
    // (try_resolve or the destructor), so the copy never races the texture's reuse.
    auto cmd = device_.begin_commands();
    graph_.execute(*cmd);
    cmd->copy_texture_to_buffer(graph_.physical(id_rt), readback_);
    ticket_ = device_.submit(std::move(cmd));
    immediate_miss_ = false;
}

std::optional<ecs::Entity> ScenePicker::try_resolve() {
    if (!pending_) {
        return std::nullopt; // nothing was begun
    }
    if (immediate_miss_) {
        pending_ = false;
        return ecs::kNullEntity;
    }
    if (!device_.is_complete(ticket_)) {
        return std::nullopt; // still on the GPU — ask again next frame
    }
    std::uint32_t id = 0;
    device_.read_buffer(readback_, &id, sizeof(id));
    pending_ = false;
    ticket_ = {};
    if (id == 0 || id > pick_entities_.size()) {
        return ecs::kNullEntity; // empty space (or an impossible id — treat as a miss, not UB)
    }
    return pick_entities_[id - 1];
}

} // namespace rime::render
