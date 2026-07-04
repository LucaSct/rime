// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Procedural mesh primitives + the GPU registry (M5.5). The primitives carry their derivations in
// comments — they are the geometry the shading proofs assert against, so their normals/uvs being
// analytically exact is the point.

#include "rime/render/mesh.hpp"

#include <array>
#include <cmath>

#include "rime/core/diagnostics/log.hpp"
#include "rime/core/math/scalar.hpp"

namespace rime::render {

CpuMesh make_plane(float half_extent, float uv_tiles) {
    // Two triangles over ±half_extent at y = 0, +y normal. uv (0,0) at (-x,-z) growing to
    // (uv_tiles, uv_tiles) at (+x,+z): a uv_tiles > 1 floor repeats its texture, which is what a
    // mip/anisotropy demo wants underfoot.
    CpuMesh m;
    const float e = half_extent;
    const float t = uv_tiles;
    m.vertices = {
        {-e, 0.0f, -e, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {+e, 0.0f, -e, 0.0f, 1.0f, 0.0f, t, 0.0f},
        {+e, 0.0f, +e, 0.0f, 1.0f, 0.0f, t, t},
        {-e, 0.0f, +e, 0.0f, 1.0f, 0.0f, 0.0f, t},
    };
    // Counter-clockwise seen from +y (looking down the normal): 0→2→1, 0→3→2.
    m.indices = {0, 2, 1, 0, 3, 2};
    return m;
}

CpuMesh make_cube(float half_extent) {
    // 4 vertices per face (24 total) so each face keeps its own flat normal — sharing corner
    // vertices would force the rasterizer to interpolate normals across the edge and shade the
    // cube like a blob. Faces are laid out (+x, -x, +y, -y, +z, -z); each quad's vertices wind
    // counter-clockwise when seen from OUTSIDE along its normal.
    CpuMesh m;
    const float e = half_extent;

    struct Face {
        std::array<float, 3> normal;
        std::array<std::array<float, 3>, 4> corners; // CCW from outside
    };

    const Face faces[6] = {
        {{+1, 0, 0}, {{{+e, -e, -e}, {+e, +e, -e}, {+e, +e, +e}, {+e, -e, +e}}}},
        {{-1, 0, 0}, {{{-e, -e, +e}, {-e, +e, +e}, {-e, +e, -e}, {-e, -e, -e}}}},
        {{0, +1, 0}, {{{-e, +e, -e}, {-e, +e, +e}, {+e, +e, +e}, {+e, +e, -e}}}},
        {{0, -1, 0}, {{{-e, -e, +e}, {-e, -e, -e}, {+e, -e, -e}, {+e, -e, +e}}}},
        {{0, 0, +1}, {{{-e, -e, +e}, {+e, -e, +e}, {+e, +e, +e}, {-e, +e, +e}}}},
        {{0, 0, -1}, {{{+e, -e, -e}, {-e, -e, -e}, {-e, +e, -e}, {+e, +e, -e}}}},
    };
    const float uvs[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (const Face& f : faces) {
        const std::uint32_t base = static_cast<std::uint32_t>(m.vertices.size());
        for (int i = 0; i < 4; ++i) {
            MeshVertex v;
            v.px = f.corners[static_cast<std::size_t>(i)][0];
            v.py = f.corners[static_cast<std::size_t>(i)][1];
            v.pz = f.corners[static_cast<std::size_t>(i)][2];
            v.nx = f.normal[0];
            v.ny = f.normal[1];
            v.nz = f.normal[2];
            v.u = uvs[i][0];
            v.v = uvs[i][1];
            m.vertices.push_back(v);
        }
        // Quad → two CCW triangles: (0,1,2) and (0,2,3).
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return m;
}

CpuMesh make_uv_sphere(float radius, std::uint32_t rings, std::uint32_t segments) {
    // Latitude/longitude parameterization (see mesh.hpp): rows of vertices at θ = r·π/rings for
    // r = 0..rings (poles included as degenerate rows — simple and correct; pole fans render
    // fine), columns at φ = s·2π/segments for s = 0..segments (the s = segments column duplicates
    // s = 0 with u = 1.0 so texture coordinates wrap without a seam-crossing interpolation).
    CpuMesh m;
    m.vertices.reserve(static_cast<std::size_t>(rings + 1) * (segments + 1));
    for (std::uint32_t r = 0; r <= rings; ++r) {
        const float theta = core::kPi * static_cast<float>(r) / static_cast<float>(rings);
        const float st = std::sin(theta), ct = std::cos(theta);
        for (std::uint32_t s = 0; s <= segments; ++s) {
            const float phi =
                2.0f * core::kPi * static_cast<float>(s) / static_cast<float>(segments);
            const float sp = std::sin(phi), cp = std::cos(phi);
            MeshVertex v;
            // Unit direction — for a sphere the normal IS the direction; position is r·n.
            v.nx = st * cp;
            v.ny = ct;
            v.nz = st * sp;
            v.px = radius * v.nx;
            v.py = radius * v.ny;
            v.pz = radius * v.nz;
            v.u = static_cast<float>(s) / static_cast<float>(segments);
            v.v = static_cast<float>(r) / static_cast<float>(rings);
            m.vertices.push_back(v);
        }
    }
    const std::uint32_t stride = segments + 1;
    for (std::uint32_t r = 0; r < rings; ++r) {
        for (std::uint32_t s = 0; s < segments; ++s) {
            const std::uint32_t a = r * stride + s; //   a---b     (θ grows downward,
            const std::uint32_t b = a + 1;          //   |   |      φ grows rightward)
            const std::uint32_t c = a + stride;     //   c---d
            const std::uint32_t d = c + 1;
            // CCW seen from outside: with x = sinθcosφ, z = sinθsinφ and +y up, going a→c→b (and
            // b→c→d) winds counter-clockwise for an outside viewer. Degenerate pole triangles
            // (zero area) are harmless and keep the loop branch-free.
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    return m;
}

MeshRegistry::~MeshRegistry() {
    for (const GpuMesh& g : meshes_) {
        device_.destroy(g.indices);
        device_.destroy(g.vertices);
    }
}

MeshId MeshRegistry::add(const CpuMesh& mesh, std::string_view debug_name) {
    if (mesh.vertices.empty() || mesh.indices.empty()) {
        RIME_ERROR("render: MeshRegistry::add('{}') with empty geometry", debug_name);
        return kInvalidMeshId;
    }
    GpuMesh g;
    rhi::BufferDesc vbd{};
    vbd.size = mesh.vertices.size() * sizeof(MeshVertex);
    vbd.usage = rhi::BufferUsage::Vertex;
    vbd.memory = rhi::MemoryUsage::CpuToGpu;
    vbd.initial_data = mesh.vertices.data();
    vbd.debug_name = debug_name;
    g.vertices = device_.create_buffer(vbd);

    rhi::BufferDesc ibd{};
    ibd.size = mesh.indices.size() * sizeof(std::uint32_t);
    ibd.usage = rhi::BufferUsage::Index;
    ibd.memory = rhi::MemoryUsage::CpuToGpu;
    ibd.initial_data = mesh.indices.data();
    ibd.debug_name = debug_name;
    g.indices = device_.create_buffer(ibd);

    if (!g.vertices.is_valid() || !g.indices.is_valid()) {
        RIME_ERROR("render: MeshRegistry::add('{}') buffer creation failed", debug_name);
        device_.destroy(g.indices);
        device_.destroy(g.vertices);
        return kInvalidMeshId;
    }
    g.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    meshes_.push_back(g);
    return static_cast<MeshId>(meshes_.size() - 1);
}

std::span<const rhi::VertexAttribute> MeshRegistry::vertex_attributes() noexcept {
    // location 0 = position, 1 = normal, 2 = uv — the contract every mesh-drawing shader follows.
    static const rhi::VertexAttribute kAttrs[] = {
        {0, rhi::Format::RGB32Float, offsetof(MeshVertex, px)},
        {1, rhi::Format::RGB32Float, offsetof(MeshVertex, nx)},
        {2, rhi::Format::RG32Float, offsetof(MeshVertex, u)},
    };
    return kAttrs;
}

} // namespace rime::render
