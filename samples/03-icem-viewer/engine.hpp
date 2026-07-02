// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The engine cut-away (Bview): ICEM's computed geared turbofan shown as the coloured, sectioned
// assembly AND its computed flow at once. ICEM's `engine` command emits the machine as one named STL
// per component (engine_<part>.stl, the fan/booster/HPC/combustor/HPT/LPT/nozzles/shafts/nacelle …)
// plus two compressible-throughflow fields — engine_core.icef and engine_bypass.icef, each carrying a
// vec3 `velocity` and a scalar `mach` on the same node grid. This module fuses the two existing views:
//
//   * the multi-part **assembly** (E1) — every part tinted + toggle-able + explodable — but now with a
//     real **cut-away** clip plane through the engine axis so the gas path is laid open; and
//   * **streamlines** (D) traced through *both* ducts, coloured not by raw speed but by the computed
//     **Mach number**, on one Mach scale shared by core and bypass so the two streams read together.
//
// Both are drawn into one rendering scope: they share the camera and the depth buffer, so the metal
// that the cut leaves occludes the flow behind it while the opened half reveals it — a true cut-away.
// No new pipeline, shader or RHI surface: the assembly reuses mesh.{vert,frag} (whose clip the cut-away
// switches on), the streamlines reuse streamline.{vert,frag} (the Mach value rides the same w channel
// the speed colouring used). See docs/math/engine-view.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "assembly.hpp"
#include "field.hpp"
#include "mesh_render.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/rhi/rhi.hpp"
#include "streamlines.hpp"

namespace rime::viewer {

// The whole engine prepared on the CPU: the assembly, the two traced + Mach-coloured streamline vertex
// sets (LineList vec4 = world xyz + Mach/mach_max), and the shared Mach scale the legend annotates.
struct EngineScene {
    Assembly assembly;
    std::vector<float> core_lines;   // engine_core.icef streamline verts (empty if none)
    std::vector<float> bypass_lines; // engine_bypass.icef streamline verts (empty if none)
    float mach_max = 0.0f;           // top of the shared Mach colour scale (max over both ducts)
    bool has_flow = false;           // at least one duct loaded (the parts still show without flow)
};

// The mesh-shader clip plane that removes the half-space where dot(N, p) > w (see mesh.frag). `on` =
// false yields the disabled plane (N = 0, w = +big: nothing is clipped). axis ∈ {0,1,2}, sign ±1
// chooses which half to cut, offset is the plane position in world units. This is exactly the encoding
// make_push uses for the single-mesh cross-section, lifted out so the engine view can drive it per
// frame from a UI toggle.
[[nodiscard]] inline std::array<float, 4>
make_clip_plane(bool on, int axis, float sign, float offset) {
    if (!on)
        return {0.0f, 0.0f, 0.0f, 1e30f};
    std::array<float, 4> p{0.0f, 0.0f, 0.0f, 0.0f};
    p[static_cast<std::size_t>(axis)] = sign;
    p[3] = sign * offset;
    return p;
}

// Load the engine ICEM emits from `dir`: every engine_*.stl as the assembly, and the two .icef ducts
// as velocity (to integrate) + Mach (to colour). The streamlines are traced here so the GPU layer (and
// the off-screen test) just upload a vertex list. Returns nullopt only if the assembly itself is empty
// — the flow is optional, so a missing/unreadable .icef just omits that duct's lines; the parts draw.
[[nodiscard]] inline std::optional<EngineScene>
load_engine(const std::string& dir, int n_seed = 26) {
    std::optional<Assembly> a = load_assembly(dir);
    if (!a)
        return std::nullopt;

    EngineScene s;
    s.assembly = std::move(*a);

    namespace fs = std::filesystem;
    const std::string core_path = (fs::path(dir) / "engine_core.icef").string();
    const std::string byp_path = (fs::path(dir) / "engine_bypass.icef").string();
    const std::optional<VectorField> core_v = load_icef_vector(core_path, "velocity");
    const std::optional<ScalarField> core_m = load_icef_scalar(core_path, "mach");
    const std::optional<VectorField> byp_v = load_icef_vector(byp_path, "velocity");
    const std::optional<ScalarField> byp_m = load_icef_scalar(byp_path, "mach");

    // One Mach scale for both ducts: normalize every streamline's colour by the larger of the two
    // ducts' peak Mach, so a single legend reads both and the cold bypass vs the hot core are
    // directly comparable.
    if (core_m)
        s.mach_max = std::max(s.mach_max, core_m->vmax);
    if (byp_m)
        s.mach_max = std::max(s.mach_max, byp_m->vmax);
    if (s.mach_max <= 0.0f)
        s.mach_max = 1.0f;

    if (core_v && core_m) {
        s.core_lines = build_streamlines(*core_v, &*core_m, s.mach_max, n_seed);
        s.has_flow = true;
    }
    if (byp_v && byp_m) {
        s.bypass_lines = build_streamlines(*byp_v, &*byp_m, s.mach_max, n_seed);
        s.has_flow = true;
    }
    return s;
}

// The per-part cut-away push: the assembly tint + exploded-view push (cam_pos.w = 2), with the clip
// plane switched on. The mesh shader applies the clip first and unconditionally, independent of the
// assembly tint/explode branch, so sectioning and tinting compose on the same draw.
[[nodiscard]] inline MeshPush engine_part_push(const Part& p,
                                               const core::Mat4& view_proj,
                                               core::Vec3 eye,
                                               float factor,
                                               const std::array<float, 4>& clip) {
    MeshPush m = assembly_push(p, view_proj, eye, factor);
    m.clip_plane[0] = clip[0];
    m.clip_plane[1] = clip[1];
    m.clip_plane[2] = clip[2];
    m.clip_plane[3] = clip[3];
    return m;
}

// The line push for the streamline pass: only the camera matrix is read by streamline.vert (the lines
// are pre-coloured), but a full MeshPush is sent to match the pipeline's 128-byte push block.
[[nodiscard]] inline MeshPush engine_line_push(const core::Mat4& view_proj, core::Vec3 eye) {
    MeshPush p{};
    p.mvp = view_proj;
    p.cam_pos[0] = eye.x;
    p.cam_pos[1] = eye.y;
    p.cam_pos[2] = eye.z;
    p.cam_pos[3] = 1.0f;
    return p;
}

// GPU resources for the engine view: the assembly's per-part meshes plus a streamline buffer per duct.
struct EngineGpu {
    AssemblyGpu assembly;
    StreamlineView core;
    StreamlineView bypass;
    bool has_core = false;
    bool has_bypass = false;
};

[[nodiscard]] inline EngineGpu make_engine_gpu(rhi::Device& device,
                                               rhi::Format color_format,
                                               rhi::Format depth_format,
                                               const EngineScene& s,
                                               const std::uint32_t* mesh_vert_spirv,
                                               std::size_t mesh_vert_bytes,
                                               const std::uint32_t* mesh_frag_spirv,
                                               std::size_t mesh_frag_bytes,
                                               const std::uint32_t* line_vert_spirv,
                                               std::size_t line_vert_bytes,
                                               const std::uint32_t* line_frag_spirv,
                                               std::size_t line_frag_bytes) {
    EngineGpu g;
    g.assembly = make_assembly_gpu(device, color_format, depth_format, s.assembly, mesh_vert_spirv,
                                   mesh_vert_bytes, mesh_frag_spirv, mesh_frag_bytes);
    if (!s.core_lines.empty()) {
        g.core = make_streamlines(device, color_format, depth_format, line_vert_spirv,
                                  line_vert_bytes, line_frag_spirv, line_frag_bytes, s.core_lines);
        g.has_core = true;
    }
    if (!s.bypass_lines.empty()) {
        g.bypass = make_streamlines(device, color_format, depth_format, line_vert_spirv,
                                    line_vert_bytes, line_frag_spirv, line_frag_bytes,
                                    s.bypass_lines);
        g.has_bypass = true;
    }
    return g;
}

inline void destroy_engine_gpu(rhi::Device& device, EngineGpu& g) {
    if (g.has_bypass)
        destroy_streamlines(device, g.bypass);
    if (g.has_core)
        destroy_streamlines(device, g.core);
    destroy_assembly_gpu(device, g.assembly);
    g.has_core = g.has_bypass = false;
}

// Record one engine frame into one rendering scope: clear, draw the cut-away assembly (each visible
// part tinted, exploded and sectioned by `clip`), then both Mach-coloured streamline sets. Sharing the
// scope means one depth buffer governs both, so the remaining metal occludes the flow behind it while
// the cut-open half reveals it.
inline void record_engine(rhi::CommandBuffer& cmd,
                          const EngineGpu& g,
                          const EngineScene& s,
                          rhi::TextureHandle color,
                          rhi::TextureHandle depth,
                          rhi::Extent2D extent,
                          const core::Mat4& view_proj,
                          core::Vec3 eye,
                          const std::array<float, 4>& clip,
                          float factor,
                          rhi::ClearColor clear) {
    using namespace rime::rhi;
    RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = LoadOp::Clear;
    ri.color.store_op = StoreOp::Store;
    ri.color.clear = clear;
    DepthStencilAttachment da{};
    da.target = depth;
    da.load_op = LoadOp::Clear;
    da.clear_depth = 1.0f;
    ri.depth_stencil = da;

    cmd.begin_rendering(ri);
    for (std::size_t i = 0; i < s.assembly.parts.size() && i < g.assembly.parts.size(); ++i) {
        if (!s.assembly.parts[i].visible)
            continue;
        draw_mesh(cmd, g.assembly.parts[i], extent,
                  engine_part_push(s.assembly.parts[i], view_proj, eye, factor, clip));
    }
    const MeshPush line = engine_line_push(view_proj, eye);
    if (g.has_core)
        draw_streamlines(cmd, g.core, extent, line);
    if (g.has_bypass)
        draw_streamlines(cmd, g.bypass, extent, line);
    cmd.end_rendering();
}

// Off-screen engine cut-away (snapshot + test): build the GPU resources, draw the sectioned assembly +
// Mach streamlines into one square RGBA8 target, read it back.
[[nodiscard]] inline std::vector<std::uint8_t>
render_engine_offscreen(rhi::Device& device,
                        std::uint32_t size,
                        const EngineScene& s,
                        const core::Mat4& view_proj,
                        core::Vec3 eye,
                        const std::array<float, 4>& clip,
                        float factor,
                        rhi::ClearColor clear,
                        const std::uint32_t* mesh_vert_spirv,
                        std::size_t mesh_vert_bytes,
                        const std::uint32_t* mesh_frag_spirv,
                        std::size_t mesh_frag_bytes,
                        const std::uint32_t* line_vert_spirv,
                        std::size_t line_vert_bytes,
                        const std::uint32_t* line_frag_spirv,
                        std::size_t line_frag_bytes) {
    using namespace rime::rhi;
    EngineGpu g = make_engine_gpu(device, Format::RGBA8Unorm, Format::D32Float, s, mesh_vert_spirv,
                                  mesh_vert_bytes, mesh_frag_spirv, mesh_frag_bytes,
                                  line_vert_spirv, line_vert_bytes, line_frag_spirv,
                                  line_frag_bytes);

    TextureDesc ctd{};
    ctd.extent = {size, size};
    ctd.format = Format::RGBA8Unorm;
    ctd.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
    const TextureHandle color = device.create_texture(ctd);
    TextureDesc dtd{};
    dtd.extent = {size, size};
    dtd.format = Format::D32Float;
    dtd.usage = TextureUsage::DepthStencil;
    const TextureHandle depth = device.create_texture(dtd);

    const std::uint64_t byte_count = static_cast<std::uint64_t>(size) * size * 4;
    BufferDesc rbd{};
    rbd.size = byte_count;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    const BufferHandle readback = device.create_buffer(rbd);

    auto cmd = device.begin_commands();
    record_engine(*cmd, g, s, color, depth, {size, size}, view_proj, eye, clip, factor, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(depth);
    device.destroy(color);
    destroy_engine_gpu(device, g);
    return pixels;
}

} // namespace rime::viewer
