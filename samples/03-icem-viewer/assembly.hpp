// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Multi-part assemblies for the ICEM viewer (E1). ICEM emits a big machine as one named STL per
// component (e.g. the ITER-class tokamak: vessel, blanket, TF coils, plasma, divertor, …). This
// loads them as one Assembly of coloured Parts and draws them together, each part independently
// toggle-able and tinted, with an **exploded view** that fans the parts apart so the nested
// internals can be seen.
//
// It reuses the lit mesh pass wholesale: each part is an ordinary GpuMesh, drawn with the shared
// mesh.{vert,frag}. Two push-constant slots that are unused when no field is bound carry the
// per-part data — field_scale.xyz the flat tint, field_bias.xyz the exploded-view offset — flagged
// by cam_pos.w > 1.5 ("assembly mode"); see the branches in mesh.vert / mesh.frag. No new pipeline,
// no RHI change, the push constant stays at its 128-byte budget. See docs/math/assembly.md.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "mesh_render.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/rhi/rhi.hpp"
#include "stl.hpp"

namespace rime::viewer {

// A palette of distinct, balanced tints so up to a dozen parts read apart at a glance. The first
// three are deliberately a clean red / blue / green (one dominant channel each) — the assembly
// render test classifies parts by dominant channel, so keep those first three primary.
[[nodiscard]] inline const std::array<core::Vec3, 12>& part_palette() {
    static const std::array<core::Vec3, 12> kP = {{
        {0.85f, 0.30f, 0.26f}, // red
        {0.28f, 0.50f, 0.85f}, // blue
        {0.34f, 0.74f, 0.40f}, // green
        {0.90f, 0.72f, 0.28f}, // amber
        {0.64f, 0.45f, 0.82f}, // purple
        {0.28f, 0.78f, 0.78f}, // teal
        {0.90f, 0.55f, 0.30f}, // orange
        {0.82f, 0.46f, 0.64f}, // pink
        {0.58f, 0.70f, 0.30f}, // olive
        {0.52f, 0.58f, 0.66f}, // steel
        {0.78f, 0.70f, 0.50f}, // sand
        {0.45f, 0.62f, 0.85f}, // sky
    }};
    return kP;
}

// One component of an assembly: its mesh, a display tint, the world offset it slides to when
// exploded, and whether it is currently shown.
struct Part {
    std::string name;
    CpuMesh mesh;
    core::Vec3 color{0.80f, 0.80f, 0.82f};
    core::Vec3 explode{0.0f, 0.0f, 0.0f}; // world offset applied at explode factor = 1
    bool visible = true;
};

struct Assembly {
    std::vector<Part> parts;
    core::Vec3 center{0.0f, 0.0f, 0.0f}; // bounding-box centre of the whole assembly
    float radius = 1.0f;                 // enclosing-sphere radius of the assembled machine

    [[nodiscard]] std::size_t visible_count() const {
        std::size_t n = 0;
        for (const Part& p : parts)
            if (p.visible)
                ++n;
        return n;
    }
};

// Tidy "tokamak_first_wall.stl" → "first wall": drop the directory + extension, strip a leading
// "<prefix>_" (the machine name ICEM prefixes every part with), and turn '_' into spaces.
[[nodiscard]] inline std::string part_name_from_path(const std::filesystem::path& p) {
    std::string s = p.stem().string();
    const std::size_t us = s.find('_');
    if (us != std::string::npos && us + 1 < s.size())
        s = s.substr(us + 1);
    std::replace(s.begin(), s.end(), '_', ' ');
    return s;
}

// Finish an assembly once its parts' meshes are loaded: compute the overall bounds, give each part
// a palette colour (by load order, so it is stable), and work out its exploded-view offset.
//
// The offset is built to separate *any* assembly. A **radial** term (part centroid − assembly
// centre) pulls scattered parts straight out from the middle. An **axial fan** along +z, ordered by
// part size, is added on top so that even perfectly **concentric** shells — a tokamak's nested tori
// share a centre, so their radial term is ~0 — still spread out: the biggest sinks to one end, the
// smallest rises to the other. At explode factor 1 the fan spans about the assembly's diameter.
inline void finalize_assembly(Assembly& a) {
    if (a.parts.empty())
        return;

    core::Vec3 mn = a.parts.front().mesh.bb_min, mx = a.parts.front().mesh.bb_max;
    for (const Part& p : a.parts) {
        mn.x = std::min(mn.x, p.mesh.bb_min.x);
        mn.y = std::min(mn.y, p.mesh.bb_min.y);
        mn.z = std::min(mn.z, p.mesh.bb_min.z);
        mx.x = std::max(mx.x, p.mesh.bb_max.x);
        mx.y = std::max(mx.y, p.mesh.bb_max.y);
        mx.z = std::max(mx.z, p.mesh.bb_max.z);
    }
    a.center = (mn + mx) * 0.5f;
    a.radius = std::max(0.5f * core::length(mx - mn), 1e-4f);

    // Rank the parts by size (largest first) for the axial fan order.
    std::vector<std::size_t> order(a.parts.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t i, std::size_t j) {
        return a.parts[i].mesh.radius() > a.parts[j].mesh.radius();
    });

    const std::size_t n = a.parts.size();
    const float spacing = (n > 1) ? (2.0f * a.radius) / static_cast<float>(n - 1) : 0.0f;
    for (std::size_t rank = 0; rank < n; ++rank) {
        Part& p = a.parts[order[rank]];
        p.color = part_palette()[order[rank] % part_palette().size()];
        const core::Vec3 radial = p.mesh.center() - a.center;
        const float axial = (static_cast<float>(rank) - 0.5f * static_cast<float>(n - 1)) * spacing;
        p.explode = radial + core::Vec3{0.0f, 0.0f, 1.0f} * axial;
    }
}

// Load every *.stl in `dir` as a part of one assembly (sorted by filename, for determinism).
// nullopt if the directory holds no loadable binary STL.
[[nodiscard]] inline std::optional<Assembly> load_assembly(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return std::nullopt;

    std::vector<fs::path> stls;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".stl")
            stls.push_back(e.path());
    }
    std::sort(stls.begin(), stls.end());

    Assembly a;
    for (const fs::path& path : stls) {
        auto mesh = load_stl_binary_file(path.string());
        if (!mesh)
            continue; // skip the empty/degenerate ones (e.g. a sub-cell-thin part)
        Part part;
        part.name = part_name_from_path(path);
        part.mesh = std::move(*mesh);
        a.parts.push_back(std::move(part));
    }
    if (a.parts.empty())
        return std::nullopt;
    finalize_assembly(a);
    return a;
}

// The enclosing radius once the parts have slid to `factor` of their exploded offsets — what the
// camera frames so the spreading assembly stays in view.
[[nodiscard]] inline float framing_radius(const Assembly& a, float factor) {
    float r = a.radius;
    for (const Part& p : a.parts) {
        if (!p.visible)
            continue;
        const core::Vec3 c = p.mesh.center() + p.explode * factor;
        r = std::max(r, core::length(c - a.center) + p.mesh.radius());
    }
    return std::max(r, 1e-4f);
}

// The per-part push: assembly mode (cam_pos.w = 2), the flat tint in field_scale.xyz, and the
// part's exploded-view offset (scaled by `factor`) in field_bias.xyz — read by mesh.vert /
// mesh.frag.
[[nodiscard]] inline MeshPush
assembly_push(const Part& p, const core::Mat4& view_proj, core::Vec3 eye, float factor) {
    MeshPush m{};
    m.mvp = view_proj;
    m.cam_pos[0] = eye.x;
    m.cam_pos[1] = eye.y;
    m.cam_pos[2] = eye.z;
    m.cam_pos[3] = 2.0f; // > 1.5 → assembly tint + explode mode
    m.clip_plane[0] = m.clip_plane[1] = m.clip_plane[2] = 0.0f;
    m.clip_plane[3] = 1e30f; // the assembly view doesn't cross-section
    m.field_scale[0] = p.color.x;
    m.field_scale[1] = p.color.y;
    m.field_scale[2] = p.color.z;
    m.field_scale[3] = 0.0f;
    m.field_bias[0] = factor * p.explode.x;
    m.field_bias[1] = factor * p.explode.y;
    m.field_bias[2] = factor * p.explode.z;
    m.field_bias[3] = 0.0f;
    return m;
}

// GPU resources for an assembly: one GpuMesh per part (parallel to Assembly::parts). The parts
// share the same pipeline/shaders — clarity over the small cost of separate pipeline objects; the
// M5 render graph will batch this. No field is bound (parts are flat-tinted), so each carries the
// harmless dummy volume.
struct AssemblyGpu {
    std::vector<GpuMesh> parts;
};

[[nodiscard]] inline AssemblyGpu make_assembly_gpu(rhi::Device& device,
                                                   rhi::Format color_format,
                                                   rhi::Format depth_format,
                                                   const Assembly& a,
                                                   const std::uint32_t* vert_spirv,
                                                   std::size_t vert_bytes,
                                                   const std::uint32_t* frag_spirv,
                                                   std::size_t frag_bytes) {
    AssemblyGpu g;
    g.parts.reserve(a.parts.size());
    for (const Part& p : a.parts)
        g.parts.push_back(make_mesh(device,
                                    color_format,
                                    depth_format,
                                    p.mesh,
                                    vert_spirv,
                                    vert_bytes,
                                    frag_spirv,
                                    frag_bytes));
    return g;
}

inline void destroy_assembly_gpu(rhi::Device& device, AssemblyGpu& g) {
    for (GpuMesh& m : g.parts)
        destroy_mesh(device, m);
    g.parts.clear();
}

// Draw every visible part into the already-open rendering scope, each with its tint + exploded
// offset.
inline void draw_assembly(rhi::CommandBuffer& cmd,
                          const AssemblyGpu& g,
                          const Assembly& a,
                          rhi::Extent2D extent,
                          const core::Mat4& view_proj,
                          core::Vec3 eye,
                          float factor) {
    for (std::size_t i = 0; i < a.parts.size() && i < g.parts.size(); ++i) {
        if (!a.parts[i].visible)
            continue;
        draw_mesh(cmd, g.parts[i], extent, assembly_push(a.parts[i], view_proj, eye, factor));
    }
}

// Record one lit frame of the assembly: clear colour + depth, then the visible parts.
inline void record_assembly(rhi::CommandBuffer& cmd,
                            const AssemblyGpu& g,
                            const Assembly& a,
                            rhi::TextureHandle color,
                            rhi::TextureHandle depth,
                            rhi::Extent2D extent,
                            const core::Mat4& view_proj,
                            core::Vec3 eye,
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
    draw_assembly(cmd, g, a, extent, view_proj, eye, factor);
    cmd.end_rendering();
}

// Off-screen assembly render (snapshot + test): build a GpuMesh per part, draw them all
// depth-tested into one square RGBA8 target, read it back.
[[nodiscard]] inline std::vector<std::uint8_t>
render_assembly_offscreen(rhi::Device& device,
                          std::uint32_t size,
                          const Assembly& a,
                          const core::Mat4& view_proj,
                          core::Vec3 eye,
                          float factor,
                          rhi::ClearColor clear,
                          const std::uint32_t* vert_spirv,
                          std::size_t vert_bytes,
                          const std::uint32_t* frag_spirv,
                          std::size_t frag_bytes) {
    using namespace rime::rhi;
    AssemblyGpu g = make_assembly_gpu(device,
                                      Format::RGBA8Unorm,
                                      Format::D32Float,
                                      a,
                                      vert_spirv,
                                      vert_bytes,
                                      frag_spirv,
                                      frag_bytes);

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
    record_assembly(*cmd, g, a, color, depth, {size, size}, view_proj, eye, factor, clear);
    cmd->copy_texture_to_buffer(color, readback);
    device.submit_blocking(*cmd);

    std::vector<std::uint8_t> pixels(byte_count);
    device.read_buffer(readback, pixels.data(), pixels.size(), 0);

    device.destroy(readback);
    device.destroy(depth);
    device.destroy(color);
    destroy_assembly_gpu(device, g);
    return pixels;
}

} // namespace rime::viewer
