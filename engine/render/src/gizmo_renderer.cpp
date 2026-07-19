// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The gizmo renderer's implementation (m9.6 Part B). See gizmo_renderer.hpp for the architecture;
// the notes here are about the geometry (unit handles built once, drawn per-axis so each carries
// its own colour) and the overlay pass (load the finished LDR, no depth — always-on-top).

#include "rime/render/gizmo_renderer.hpp"

#include <cmath>
#include <cstring>
#include <vector>

#include "gizmo.frag.spv.h"
#include "gizmo.vert.spv.h"
#include "rime/ecs/transform.hpp"
#include "rime/render/components.hpp"
#include "rime/render/scene_renderer.hpp"

namespace rime::render {

namespace {

// The per-draw push-constant block, mirroring gizmo.vert/.frag: a column-major MVP then a flat
// RGBA colour. Flat arrays — not core::Mat4/Vec members — for the same reason as ScenePicker's
// DrawPush: alignas padding would push garbage past the declared range. 80 bytes, comfortably
// under the 128-byte push floor.
struct GizmoPush {
    float mvp[16];
    float color[4];
};

static_assert(sizeof(GizmoPush) == 80, "GizmoPush must match the shaders' push_constant block");

// Axis palette: the near-universal editor convention (X red, Y green, Z blue) plus a yellow
// highlight for the hovered/active axis — chosen to be unmistakable against both the scene and
// the other two axes, and to survive LZ4/AV1 streaming legibly.
constexpr float kAxisColor[3][4] = {
    {0.90f, 0.15f, 0.15f, 1.0f}, // X
    {0.15f, 0.80f, 0.15f, 1.0f}, // Y
    {0.20f, 0.40f, 0.95f, 1.0f}, // Z
};
constexpr float kHighlightColor[4] = {1.0f, 0.85f, 0.10f, 1.0f};

// The selection tint: a translucent blue-white glow alpha-blended over the shaded mesh. Alpha is
// the whole design: strong enough to read as "selected" at a glance, weak enough that the
// material underneath stays recognisable while editing it.
constexpr float kTintColor[4] = {0.35f, 0.60f, 1.00f, 0.35f};

// Unit handle proportions (scaled per frame by the screen-constant factor). The shaft runs the
// full unit length; decorations sit near the tip.
constexpr float kArrowBack = 0.85f;  // arrowhead flare re-joins the shaft here
constexpr float kArrowFlare = 0.06f; // flare half-width
constexpr float kCubeCenter = 0.90f; // scale handle's cube centre along the axis
constexpr float kCubeHalf = 0.05f;   // scale cube half-extent
constexpr int kRingSegments = 48;    // rotate ring tessellation (line segments per ring)

// The unit axis frame: direction + the two perpendiculars that span its normal plane. Ordered so
// axis i's ring lies in the plane of the OTHER two axes.
constexpr core::Vec3 kAxisDir[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
constexpr core::Vec3 kAxisPerp1[3] = {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}};
constexpr core::Vec3 kAxisPerp2[3] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}};

// Line-vertex emission: 3 floats per vertex, 2 vertices per segment (LineList).
void emit(std::vector<float>& out, core::Vec3 a, core::Vec3 b) {
    out.insert(out.end(), {a.x, a.y, a.z, b.x, b.y, b.z});
}

// The line pipeline's vertex layout: a bare position. Static storage — VertexLayout holds a span.
constexpr rhi::VertexAttribute kLineAttrs[] = {{0, rhi::Format::RGB32Float, 0}};

} // namespace

CameraLens compute_camera_lens(ecs::World& world, rhi::Extent2D extent) {
    CameraLens lens;
    if (extent.width == 0 || extent.height == 0) {
        return lens; // no viewport, no lens
    }
    // The extraction already solves "which camera, and what is its view matrix" with the engine's
    // conventions (first active camera, looking down local −z); reusing it is what guarantees the
    // lens equals the renderer's — the exactness the editor's drag math depends on.
    const ExtractedScene scene = extract_scene(world);
    if (!scene.camera.found) {
        return lens;
    }
    const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
    lens.view_proj =
        core::perspective(scene.camera.fov_y, aspect, scene.camera.z_near, scene.camera.z_far) *
        scene.camera.view;
    lens.inv_view_proj = core::inverse(lens.view_proj);
    lens.eye = {scene.camera.position[0], scene.camera.position[1], scene.camera.position[2]};
    lens.fov_y = scene.camera.fov_y;
    lens.found = true;
    return lens;
}

GizmoRenderer::GizmoRenderer(rhi::Device& device, const MeshRegistry& meshes)
    : device_(device), meshes_(meshes) {
    rhi::ShaderDesc vs{};
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = gizmo_vert_spv;
    vs.spirv_size_bytes = sizeof(gizmo_vert_spv);
    vs.debug_name = "gizmo.vert";
    vertex_shader_ = device.create_shader(vs);

    rhi::ShaderDesc fs{};
    fs.stage = rhi::ShaderStage::Fragment;
    fs.spirv = gizmo_frag_spv;
    fs.spirv_size_bytes = sizeof(gizmo_frag_spv);
    fs.debug_name = "gizmo.frag";
    fragment_shader_ = device.create_shader(fs);

    // Handle pipeline: line lists, no depth (always-on-top), no blending — handles are opaque UI.
    rhi::GraphicsPipelineDesc lp{};
    lp.vertex_shader = vertex_shader_;
    lp.fragment_shader = fragment_shader_;
    lp.vertex_layout.stride = 3 * sizeof(float);
    lp.vertex_layout.attributes = kLineAttrs;
    lp.color_format = kLdrFormat;
    lp.topology = rhi::PrimitiveTopology::LineList;
    lp.cull = rhi::CullMode::None;
    lp.push_constant_size = sizeof(GizmoPush);
    lp.debug_name = "gizmo-lines";
    line_pipeline_ = device.create_graphics_pipeline(lp);

    // Tint pipeline: re-draw a registry mesh flat with alpha blending, no depth. Only position is
    // consumed — the depth pre-pass's shared-layout trick, so any registry mesh binds unchanged.
    // Back-face culling halves the overdraw; a concave mesh can still blend a few front layers,
    // which reads as a slightly stronger tint there — acceptable for a selection cue (noted).
    rhi::GraphicsPipelineDesc tp{};
    tp.vertex_shader = vertex_shader_;
    tp.fragment_shader = fragment_shader_;
    tp.vertex_layout.stride = MeshRegistry::vertex_stride();
    tp.vertex_layout.attributes = MeshRegistry::vertex_attributes();
    tp.color_format = kLdrFormat;
    tp.cull = rhi::CullMode::Back;
    tp.blend = rhi::BlendMode::Alpha;
    tp.push_constant_size = sizeof(GizmoPush);
    tp.debug_name = "gizmo-tint";
    tint_pipeline_ = device.create_graphics_pipeline(tp);

    // Build the unit handle geometry for all three modes into ONE static buffer, with per-axis
    // ranges so each axis draws separately (its own colour). Unit-sized on purpose: the per-frame
    // screen-constant scale lives in the model matrix, not the vertices.
    std::vector<float> verts;
    for (int a = 0; a < 3; ++a) {
        const core::Vec3 d = kAxisDir[a];
        const core::Vec3 p1 = kAxisPerp1[a];
        const core::Vec3 p2 = kAxisPerp2[a];

        // Translate: shaft to the tip + a four-line arrowhead flare (legible at 1-px line width,
        // where a solid cone would rasterize to a blob).
        const std::size_t t_first = verts.size() / 3;
        emit(verts, {0, 0, 0}, d);
        const core::Vec3 back = d * kArrowBack;
        emit(verts, d, back + p1 * kArrowFlare);
        emit(verts, d, back - p1 * kArrowFlare);
        emit(verts, d, back + p2 * kArrowFlare);
        emit(verts, d, back - p2 * kArrowFlare);
        translate_[a] = {static_cast<std::uint32_t>(t_first),
                         static_cast<std::uint32_t>(verts.size() / 3 - t_first)};

        // Rotate: a unit ring in the axis' normal plane (per-axis rings, the ratified model).
        const std::size_t r_first = verts.size() / 3;
        for (int s = 0; s < kRingSegments; ++s) {
            const float a0 = 2.0f * 3.14159265358979323846f * static_cast<float>(s) /
                             static_cast<float>(kRingSegments);
            const float a1 = 2.0f * 3.14159265358979323846f * static_cast<float>(s + 1) /
                             static_cast<float>(kRingSegments);
            emit(verts,
                 p1 * std::cos(a0) + p2 * std::sin(a0),
                 p1 * std::cos(a1) + p2 * std::sin(a1));
        }
        rotate_[a] = {static_cast<std::uint32_t>(r_first),
                      static_cast<std::uint32_t>(verts.size() / 3 - r_first)};

        // Scale: shaft to the cube, then the cube's 12 wireframe edges centred on the axis.
        const std::size_t s_first = verts.size() / 3;
        const core::Vec3 c = d * kCubeCenter;
        emit(verts, {0, 0, 0}, c - d * kCubeHalf);
        const core::Vec3 e0 = d * kCubeHalf;
        const core::Vec3 e1 = p1 * kCubeHalf;
        const core::Vec3 e2 = p2 * kCubeHalf;
        // The 8 corners: c ± e0 ± e1 ± e2; edges connect corners differing in ONE sign.
        for (int i = 0; i < 8; ++i) {
            const float s0 = (i & 1) != 0 ? 1.0f : -1.0f;
            const float s1 = (i & 2) != 0 ? 1.0f : -1.0f;
            const float s2 = (i & 4) != 0 ? 1.0f : -1.0f;
            const core::Vec3 corner = c + e0 * s0 + e1 * s1 + e2 * s2;
            // Emit each edge once: only towards the +1 neighbour on each unset bit.
            if (s0 < 0.0f) {
                emit(verts, corner, corner + e0 * 2.0f);
            }
            if (s1 < 0.0f) {
                emit(verts, corner, corner + e1 * 2.0f);
            }
            if (s2 < 0.0f) {
                emit(verts, corner, corner + e2 * 2.0f);
            }
        }
        scale_[a] = {static_cast<std::uint32_t>(s_first),
                     static_cast<std::uint32_t>(verts.size() / 3 - s_first)};
    }

    rhi::BufferDesc vbd{};
    vbd.size = verts.size() * sizeof(float);
    vbd.usage = rhi::BufferUsage::Vertex;
    vbd.memory = rhi::MemoryUsage::CpuToGpu;
    vbd.initial_data = verts.data();
    vbd.debug_name = "gizmo-handles";
    vertices_ = device.create_buffer(vbd);
}

GizmoRenderer::~GizmoRenderer() {
    device_.destroy(vertices_);
    device_.destroy(tint_pipeline_);
    device_.destroy(line_pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

void GizmoRenderer::declare(RenderGraph& graph,
                            ecs::World& world,
                            RGTexture target,
                            const CameraLens& lens,
                            const GizmoSelection& sel) {
    if (!lens.found || sel.mode == GizmoMode::None || !target.is_valid() ||
        !world.is_alive(sel.entity)) {
        return;
    }
    const auto* wt = world.get<ecs::WorldTransform>(sel.entity);
    if (wt == nullptr) {
        return; // nothing placeable to gizmo (no world pose)
    }

    // Screen-constant scale: at distance d the viewport's half-height covers d·tan(fov/2) world
    // units, so a gizmo of world size d·tan(fov/2)·f spans the fraction f of the half-height at
    // ANY distance (docs/math/gizmos.md). The floor guards the eye-on-entity degenerate case.
    const core::Vec3 pos = wt->value.translation;
    const float dist = core::length(pos - lens.eye);
    const float s = std::max(dist * std::tan(lens.fov_y * 0.5f) * kGizmoScreenFraction, 1.0e-4f);

    // World-aligned handles: translation + uniform scale only, never the entity's rotation (v1's
    // "global space" choice — the header's rationale).
    const core::Mat4 gizmo_mvp =
        lens.view_proj * core::mat4_translation(pos) * core::mat4_scaling({s, s, s});

    // Fold the pushes now so the pass body is pure recording (the ScenePicker discipline). Slot 0
    // is the tint; 1..3 are the axes, highlight applied where the wire said so.
    GizmoPush pushes[4] = {};
    const core::Mat4 tint_mvp = lens.view_proj * core::to_matrix(wt->value);
    std::memcpy(pushes[0].mvp, tint_mvp.m, sizeof(pushes[0].mvp));
    std::memcpy(pushes[0].color, kTintColor, sizeof(kTintColor));
    for (int a = 0; a < 3; ++a) {
        std::memcpy(pushes[a + 1].mvp, gizmo_mvp.m, sizeof(pushes[a + 1].mvp));
        const bool highlighted = static_cast<int>(sel.axis) == a + 1;
        std::memcpy(pushes[a + 1].color,
                    highlighted ? kHighlightColor : kAxisColor[a],
                    sizeof(kHighlightColor));
    }

    // The tint re-draws the selected mesh, when there is one; handles draw regardless (a camera
    // or light entity has a pose worth dragging but no mesh worth tinting).
    GpuMesh tint_mesh{};
    const auto* mesh_ref = world.get<MeshRef>(sel.entity);
    const bool tint =
        mesh_ref != nullptr && mesh_ref->mesh != kInvalidMeshId && mesh_ref->mesh < meshes_.size();
    if (tint) {
        tint_mesh = meshes_.get(mesh_ref->mesh);
    }

    const Range* ranges = sel.mode == GizmoMode::Translate ? translate_
                          : sel.mode == GizmoMode::Rotate  ? rotate_
                                                           : scale_;
    Range axis_ranges[3] = {ranges[0], ranges[1], ranges[2]};

    // One overlay pass over the finished frame: LoadOp::Load keeps the shaded pixels, no depth
    // attachment means nothing occludes the overlay — always-on-top by construction, not by a
    // cleared depth trick.
    const RGColorAttachment colors[] = {{target, rhi::LoadOp::Load, rhi::StoreOp::Store, {}}};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;

    struct DrawData { // captured by value: the λ must not reference dead locals
        GizmoPush pushes[4];
        Range ranges[3];
        GpuMesh mesh;
        bool tint;
    } data{};

    std::memcpy(data.pushes, pushes, sizeof(pushes));
    std::memcpy(data.ranges, axis_ranges, sizeof(axis_ranges));
    data.mesh = tint_mesh;
    data.tint = tint;
    graph.add_raster_pass("editor-gizmo", desc, [this, data](rhi::CommandBuffer& cmd) {
        if (data.tint) {
            cmd.bind_pipeline(tint_pipeline_);
            cmd.bind_vertex_buffer(data.mesh.vertices);
            cmd.bind_index_buffer(data.mesh.indices, rhi::IndexType::Uint32);
            cmd.push_constants(&data.pushes[0], sizeof(GizmoPush));
            cmd.draw_indexed(data.mesh.index_count);
        }
        cmd.bind_pipeline(line_pipeline_);
        cmd.bind_vertex_buffer(vertices_);
        for (int a = 0; a < 3; ++a) {
            cmd.push_constants(&data.pushes[a + 1], sizeof(GizmoPush));
            cmd.draw(data.ranges[a].count, 1, data.ranges[a].first);
        }
    });
}

} // namespace rime::render
