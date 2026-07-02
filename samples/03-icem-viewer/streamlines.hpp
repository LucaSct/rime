// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/rhi/rhi.hpp"

#include "field.hpp"
#include "mesh_render.hpp"

// Streamlines of a computed 3-D velocity field (D·V): seed points across the inlet and integrate the
// velocity with RK4 to trace the flow, colouring each line by speed. The field is ICEM's potential-flow
// velocity (a vec3 .icef); the integration is on the CPU (trilinear sampling of the loaded volume), the
// lines drawn as a GPU LineList. See docs/math/streamlines.md.
namespace rime::viewer {

namespace detail {

// Trilinear sample of a VectorField's RGBA volume at a world point → (vx, vy, vz, validity).
inline std::array<float, 4> sample_field(const VectorField& vf, float wx, float wy, float wz) {
    const int nx = static_cast<int>(vf.nx), ny = static_cast<int>(vf.ny), nz = static_cast<int>(vf.nz);
    // world → continuous texel coordinate: g = (world·scale + bias)·dim − 0.5 (texel centres).
    float g[3] = {(wx * vf.scale[0] + vf.bias[0]) * nx - 0.5f,
                  (wy * vf.scale[1] + vf.bias[1]) * ny - 0.5f,
                  (wz * vf.scale[2] + vf.bias[2]) * nz - 0.5f};
    const int dim[3] = {nx, ny, nz};
    int lo[3];
    float fr[3];
    for (int c = 0; c < 3; ++c) {
        if (g[c] < 0.0f) g[c] = 0.0f;
        if (g[c] > dim[c] - 1.0f) g[c] = static_cast<float>(dim[c] - 1);
        lo[c] = static_cast<int>(std::floor(g[c]));
        if (lo[c] > dim[c] - 2) lo[c] = (dim[c] >= 2) ? dim[c] - 2 : 0;
        fr[c] = g[c] - static_cast<float>(lo[c]);
    }
    const auto idx = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(i) + static_cast<std::size_t>(nx) *
                (static_cast<std::size_t>(j) + static_cast<std::size_t>(ny) * k)) * 4;
    };
    std::array<float, 4> out{0, 0, 0, 0};
    for (int ch = 0; ch < 4; ++ch) {
        const int x1 = (nx >= 2) ? lo[0] + 1 : lo[0];
        const int y1 = (ny >= 2) ? lo[1] + 1 : lo[1];
        const int z1 = (nz >= 2) ? lo[2] + 1 : lo[2];
        const auto v = [&](int i, int j, int k) { return vf.rgba[idx(i, j, k) + ch]; };
        const float c00 = v(lo[0], lo[1], lo[2]) * (1 - fr[0]) + v(x1, lo[1], lo[2]) * fr[0];
        const float c10 = v(lo[0], y1, lo[2]) * (1 - fr[0]) + v(x1, y1, lo[2]) * fr[0];
        const float c01 = v(lo[0], lo[1], z1) * (1 - fr[0]) + v(x1, lo[1], z1) * fr[0];
        const float c11 = v(lo[0], y1, z1) * (1 - fr[0]) + v(x1, y1, z1) * fr[0];
        const float c0 = c00 * (1 - fr[1]) + c10 * fr[1];
        const float c1 = c01 * (1 - fr[1]) + c11 * fr[1];
        out[ch] = c0 * (1 - fr[2]) + c1 * fr[2];
    }
    return out;
}

// Trilinear sample of a ScalarField's volume at a world point → (value, validity). Mirrors
// sample_field (the vector sampler) but reads the scalar's R = value, G = validity channels. Used to
// colour a streamline by an *auxiliary* field carried on the same grid as the velocity — for the
// engine cut-away that auxiliary field is the computed Mach number, so the lines read true Mach
// rather than raw speed.
inline std::array<float, 2> sample_scalar(const ScalarField& sf, float wx, float wy, float wz) {
    const int nx = static_cast<int>(sf.nx), ny = static_cast<int>(sf.ny), nz = static_cast<int>(sf.nz);
    float g[3] = {(wx * sf.scale[0] + sf.bias[0]) * nx - 0.5f,
                  (wy * sf.scale[1] + sf.bias[1]) * ny - 0.5f,
                  (wz * sf.scale[2] + sf.bias[2]) * nz - 0.5f};
    const int dim[3] = {nx, ny, nz};
    int lo[3];
    float fr[3];
    for (int c = 0; c < 3; ++c) {
        if (g[c] < 0.0f) g[c] = 0.0f;
        if (g[c] > dim[c] - 1.0f) g[c] = static_cast<float>(dim[c] - 1);
        lo[c] = static_cast<int>(std::floor(g[c]));
        if (lo[c] > dim[c] - 2) lo[c] = (dim[c] >= 2) ? dim[c] - 2 : 0;
        fr[c] = g[c] - static_cast<float>(lo[c]);
    }
    const auto idx = [&](int i, int j, int k) {
        return (static_cast<std::size_t>(i) + static_cast<std::size_t>(nx) *
                (static_cast<std::size_t>(j) + static_cast<std::size_t>(ny) * k)) * 4;
    };
    std::array<float, 2> out{0, 0};
    for (int ch = 0; ch < 2; ++ch) { // 0 = value, 1 = validity
        const int x1 = (nx >= 2) ? lo[0] + 1 : lo[0];
        const int y1 = (ny >= 2) ? lo[1] + 1 : lo[1];
        const int z1 = (nz >= 2) ? lo[2] + 1 : lo[2];
        const auto v = [&](int i, int j, int k) { return sf.rgba[idx(i, j, k) + ch]; };
        const float c00 = v(lo[0], lo[1], lo[2]) * (1 - fr[0]) + v(x1, lo[1], lo[2]) * fr[0];
        const float c10 = v(lo[0], y1, lo[2]) * (1 - fr[0]) + v(x1, y1, lo[2]) * fr[0];
        const float c01 = v(lo[0], lo[1], z1) * (1 - fr[0]) + v(x1, lo[1], z1) * fr[0];
        const float c11 = v(lo[0], y1, z1) * (1 - fr[0]) + v(x1, y1, z1) * fr[0];
        const float c0 = c00 * (1 - fr[1]) + c10 * fr[1];
        const float c1 = c01 * (1 - fr[1]) + c11 * fr[1];
        out[ch] = c0 * (1 - fr[2]) + c1 * fr[2];
    }
    return out;
}

} // namespace detail

// Trace streamlines and pack them as a LineList of vec4 vertices (xyz = world position, w = colour
// coordinate in [0,1]). Seeds are a grid across the inlet (the low-z face of the field volume); each
// is integrated downstream by arc-length RK4 of the normalized velocity, so the points are evenly
// spaced regardless of local speed. A line stops when it leaves the solid (validity < 0.5) or the
// velocity vanishes.
//
// The colour coordinate is, by default, the local speed |u|/vmag_max (the plain --flow look). When an
// auxiliary scalar `color` (on the same grid) and a positive `color_ref` are supplied, the lines are
// instead coloured by that scalar / color_ref — the engine cut-away passes the computed Mach field
// and a Mach reference shared across the core and bypass ducts, so both read on one Mach scale.
[[nodiscard]] inline std::vector<float> build_streamlines(const VectorField& vf,
                                                          const ScalarField* color = nullptr,
                                                          float color_ref = 0.0f, int n_seed = 18,
                                                          int max_steps = 800) {
    std::vector<float> verts;
    if (!vf.usable() || vf.scale[0] == 0.0f) return verts;
    const float h = 1.0f / (vf.scale[0] * static_cast<float>(vf.nx)); // cell size (scale = 1/(h·dim))
    const float ds = 0.6f * h;                                        // arc-length step

    const auto world_of = [&](float u, float v, float w, float p[3]) {
        p[0] = (u - vf.bias[0]) / vf.scale[0];
        p[1] = (v - vf.bias[1]) / vf.scale[1];
        p[2] = (w - vf.bias[2]) / vf.scale[2];
    };
    // A point is in the field's domain iff its texel coordinate lands in [0,1]^3. The sampler *clamps*
    // outside the volume, so without this a line that reaches an OUTFLOW face (a valid edge — exactly
    // what the engine's open core/bypass nozzles are) would keep integrating the clamped edge velocity
    // in a straight line until max_steps, shooting far past the geometry. Stopping at the domain bound
    // ends each line at the duct exit instead.
    const auto in_domain = [&](const float p[3]) {
        for (int c = 0; c < 3; ++c) {
            const float t = p[c] * vf.scale[c] + vf.bias[c];
            if (t < -0.01f || t > 1.01f) return false;
        }
        return true;
    };
    // Unit flow direction at p (returns false if outside the fluid or stagnant).
    const auto dir = [&](const float p[3], float g[3]) {
        const auto r = detail::sample_field(vf, p[0], p[1], p[2]);
        const float s = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (r[3] < 0.5f || s < 1e-12f) return false;
        g[0] = r[0] / s;
        g[1] = r[1] / s;
        g[2] = r[2] / s;
        return true;
    };

    for (int su = 0; su < n_seed; ++su) {
        for (int sv = 0; sv < n_seed; ++sv) {
            const float u = 0.08f + 0.84f * su / std::max(1, n_seed - 1);
            const float v = 0.08f + 0.84f * sv / std::max(1, n_seed - 1);
            float p[3];
            world_of(u, v, 1.0f / static_cast<float>(vf.nz), p); // just inside the inlet face
            if (detail::sample_field(vf, p[0], p[1], p[2])[3] < 0.5f) continue; // seed not in the fluid

            for (int step = 0; step < max_steps; ++step) {
                float k1[3], k2[3], k3[3], k4[3];
                if (!dir(p, k1)) break;
                const float p2[3] = {p[0] + 0.5f * ds * k1[0], p[1] + 0.5f * ds * k1[1], p[2] + 0.5f * ds * k1[2]};
                if (!dir(p2, k2)) break;
                const float p3[3] = {p[0] + 0.5f * ds * k2[0], p[1] + 0.5f * ds * k2[1], p[2] + 0.5f * ds * k2[2]};
                if (!dir(p3, k3)) break;
                const float p4[3] = {p[0] + ds * k3[0], p[1] + ds * k3[1], p[2] + ds * k3[2]};
                if (!dir(p4, k4)) break;
                float pn[3];
                for (int c = 0; c < 3; ++c)
                    pn[c] = p[c] + (ds / 6.0f) * (k1[c] + 2.0f * k2[c] + 2.0f * k3[c] + k4[c]);

                // Colour coordinate at p: the auxiliary scalar (e.g. Mach) / color_ref when supplied,
                // else the local speed |u|/vmag_max. Clamped to the colormap's [0,1] domain.
                float w;
                if (color && color_ref > 0.0f) {
                    w = detail::sample_scalar(*color, p[0], p[1], p[2])[0] / color_ref;
                } else {
                    const auto r = detail::sample_field(vf, p[0], p[1], p[2]);
                    w = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]) / vf.vmag_max;
                }
                const float t = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
                // LineList segment p → pn, both vertices coloured by the field at p.
                verts.insert(verts.end(), {p[0], p[1], p[2], t, pn[0], pn[1], pn[2], t});
                p[0] = pn[0];
                p[1] = pn[1];
                p[2] = pn[2];
                if (!in_domain(p) || detail::sample_field(vf, p[0], p[1], p[2])[3] < 0.5f)
                    break; // left the domain or the fluid
            }
        }
    }
    return verts;
}

struct StreamlineView {
    rhi::ShaderHandle vsh;
    rhi::ShaderHandle fsh;
    rhi::PipelineHandle pipeline;
    rhi::BufferHandle vbuf;
    std::uint32_t vertex_count = 0;
};

// Build the line pipeline + upload the streamline vertices (vec4: xyz position, w speed). `verts` is the
// flat output of build_streamlines (8 floats per LineList segment).
[[nodiscard]] inline StreamlineView make_streamlines(rhi::Device& device,
                                                     rhi::Format color_format,
                                                     rhi::Format depth_format,
                                                     const std::uint32_t* vert_spirv,
                                                     std::size_t vert_bytes,
                                                     const std::uint32_t* frag_spirv,
                                                     std::size_t frag_bytes,
                                                     const std::vector<float>& verts) {
    using namespace rime::rhi;
    StreamlineView s;
    s.vertex_count = static_cast<std::uint32_t>(verts.size() / 4);

    BufferDesc vbd{};
    vbd.size = std::max<std::uint64_t>(verts.size() * sizeof(float), 16);
    vbd.usage = BufferUsage::Vertex;
    vbd.memory = MemoryUsage::CpuToGpu;
    vbd.initial_data = verts.empty() ? nullptr : verts.data();
    vbd.debug_name = "icem-streamlines";
    s.vbuf = device.create_buffer(vbd);

    ShaderDesc vsd{};
    vsd.stage = ShaderStage::Vertex;
    vsd.spirv = vert_spirv;
    vsd.spirv_size_bytes = vert_bytes;
    vsd.debug_name = "streamline.vert";
    s.vsh = device.create_shader(vsd);

    ShaderDesc fsd{};
    fsd.stage = ShaderStage::Fragment;
    fsd.spirv = frag_spirv;
    fsd.spirv_size_bytes = frag_bytes;
    fsd.debug_name = "streamline.frag";
    s.fsh = device.create_shader(fsd);

    const VertexAttribute attrs[] = {{0, Format::RGBA32Float, 0}}; // xyz position + w speed
    GraphicsPipelineDesc pd{};
    pd.vertex_shader = s.vsh;
    pd.fragment_shader = s.fsh;
    pd.vertex_layout.stride = 4 * sizeof(float);
    pd.vertex_layout.attributes = attrs;
    pd.color_format = color_format;
    pd.topology = PrimitiveTopology::LineList;
    pd.cull = CullMode::None;
    pd.depth_test = true;
    pd.depth_write = true;
    pd.depth_format = depth_format;
    pd.push_constant_size = sizeof(MeshPush);
    pd.debug_name = "icem-streamline-pipeline";
    s.pipeline = device.create_graphics_pipeline(pd);
    return s;
}

inline void destroy_streamlines(rhi::Device& device, const StreamlineView& s) {
    device.destroy(s.pipeline);
    device.destroy(s.fsh);
    device.destroy(s.vsh);
    device.destroy(s.vbuf);
}

// Draw a prepared streamline view into an ALREADY-OPEN rendering scope (mirrors draw_assembly), so the
// lines can be composited with other geometry that shares the camera and depth buffer. The engine
// cut-away (Bview) draws the cut-away assembly and then the core/bypass streamlines into one pass, so
// the lines depth-test against the metal — visible in the opened half, occluded by the solid behind.
inline void draw_streamlines(rhi::CommandBuffer& cmd,
                             const StreamlineView& s,
                             rhi::Extent2D extent,
                             const MeshPush& push) {
    using namespace rime::rhi;
    cmd.bind_pipeline(s.pipeline);
    cmd.push_constants(&push, sizeof(MeshPush));
    cmd.bind_vertex_buffer(s.vbuf, 0);
    Viewport vp{};
    vp.width = static_cast<float>(extent.width);
    vp.height = static_cast<float>(extent.height);
    vp.max_depth = 1.0f;
    cmd.set_viewport(vp);
    Rect2D sc{};
    sc.width = extent.width;
    sc.height = extent.height;
    cmd.set_scissor(sc);
    if (s.vertex_count > 0) cmd.draw(s.vertex_count);
}

inline void record_streamlines(rhi::CommandBuffer& cmd,
                               const StreamlineView& s,
                               rhi::TextureHandle color,
                               rhi::TextureHandle depth,
                               rhi::Extent2D extent,
                               const MeshPush& push,
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
    draw_streamlines(cmd, s, extent, push);
    cmd.end_rendering();
}

// Off-screen streamline render (snapshot + test): build lines from the field, draw them depth-tested.
[[nodiscard]] inline std::vector<std::uint8_t> render_streamlines_offscreen(rhi::Device& device,
                                                                            std::uint32_t size,
                                                                            const VectorField& vf,
                                                                            const MeshPush& push,
                                                                            rhi::ClearColor clear,
                                                                            const std::uint32_t* vert_spirv,
                                                                            std::size_t vert_bytes,
                                                                            const std::uint32_t* frag_spirv,
                                                                            std::size_t frag_bytes) {
    using namespace rime::rhi;
    const std::vector<float> verts = build_streamlines(vf);
    const StreamlineView s = make_streamlines(device, Format::RGBA8Unorm, Format::D32Float, vert_spirv,
                                              vert_bytes, frag_spirv, frag_bytes, verts);

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
    record_streamlines(*cmd, s, color, depth, {size, size}, push, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(depth);
    device.destroy(color);
    destroy_streamlines(device, s);
    return pixels;
}

} // namespace rime::viewer
