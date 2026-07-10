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
#include "rime/core/math/vec.hpp"

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
    compute_tangents(m);
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
    compute_tangents(m);
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
            // Winding: a→b→c (and b→d→c) so each triangle's geometric normal (edge cross product)
            // points the SAME way as its vertices' outward normals — "counter-clockwise seen from
            // outside", the exact criterion make_cube uses and the scene_layer winding proof
            // asserts. Getting this backwards makes back-face culling discard the visible surface
            // and shade the sphere's interior (dark, inside-out) — the M5.6 bug this comment now
            // guards against. Degenerate pole triangles (zero area) are harmless and keep the loop
            // branch-free.
            m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
        }
    }
    compute_tangents(m);
    return m;
}

void compute_tangents(CpuMesh& mesh) {
    // Per-face tangent accumulation (Lengyel's method): each triangle contributes its constant
    // ∂p/∂u and ∂p/∂v — solved from the position edges and their UV deltas (docs/math/tangent-space.md
    // §2) — to its three vertices, so a shared vertex averages its faces, the same smoothing that
    // makes vertex normals smooth. This is the procedural-mesh twin of the cooker's MikkTSpace pass.
    const std::size_t n = mesh.vertices.size();
    std::vector<core::Vec3> tan_u(n, core::Vec3{0.0f, 0.0f, 0.0f}); // ∂p/∂u accumulation
    std::vector<core::Vec3> tan_v(n, core::Vec3{0.0f, 0.0f, 0.0f}); // ∂p/∂v accumulation

    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::uint32_t i0 = mesh.indices[t];
        const std::uint32_t i1 = mesh.indices[t + 1];
        const std::uint32_t i2 = mesh.indices[t + 2];
        const MeshVertex& v0 = mesh.vertices[i0];
        const MeshVertex& v1 = mesh.vertices[i1];
        const MeshVertex& v2 = mesh.vertices[i2];

        const core::Vec3 e1{v1.px - v0.px, v1.py - v0.py, v1.pz - v0.pz};
        const core::Vec3 e2{v2.px - v0.px, v2.py - v0.py, v2.pz - v0.pz};
        const float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        const float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;

        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-12f)
            continue; // degenerate UV (e.g. a sphere's collapsed pole triangle) contributes nothing
        const float r = 1.0f / det;
        const core::Vec3 su = (e1 * dv2 - e2 * dv1) * r; // ∂p/∂u
        const core::Vec3 sv = (e2 * du1 - e1 * du2) * r; // ∂p/∂v
        for (const std::uint32_t i : {i0, i1, i2}) {
            tan_u[i] = tan_u[i] + su;
            tan_v[i] = tan_v[i] + sv;
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        MeshVertex& vert = mesh.vertices[i];
        const core::Vec3 nrm = core::normalize(core::Vec3{vert.nx, vert.ny, vert.nz});
        core::Vec3 tangent = tan_u[i] - nrm * core::dot(nrm, tan_u[i]); // Gram-Schmidt vs. the normal
        // A vertex no triangle tangented (unreferenced, or fully degenerate UVs) has no meaningful
        // ∂p/∂u; pick any axis perpendicular to the normal so the basis stays finite and orthonormal.
        if (core::length_squared(tangent) < 1e-12f) {
            const core::Vec3 axis =
                std::fabs(nrm.x) < 0.9f ? core::Vec3{1, 0, 0} : core::Vec3{0, 1, 0};
            tangent = axis - nrm * core::dot(nrm, axis);
        }
        tangent = core::normalize(tangent);
        // Handedness: the sign that makes the shader's w·cross(N,T) reproduce the accumulated
        // bitangent ∂p/∂v — the mirrored-UV case docs/math/tangent-space.md §4 pins.
        const float handed = core::dot(core::cross(nrm, tangent), tan_v[i]) < 0.0f ? -1.0f : 1.0f;
        vert.tx = tangent.x;
        vert.ty = tangent.y;
        vert.tz = tangent.z;
        vert.tw = handed;
    }
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
    // location 0 = position, 1 = normal, 2 = uv, 3 = tangent (xyz + handedness w) — the contract
    // every mesh-drawing shader follows (the depth pre-pass consumes only position; the rest are
    // legal-but-unused there).
    static const rhi::VertexAttribute kAttrs[] = {
        {0, rhi::Format::RGB32Float, offsetof(MeshVertex, px)},
        {1, rhi::Format::RGB32Float, offsetof(MeshVertex, nx)},
        {2, rhi::Format::RG32Float, offsetof(MeshVertex, u)},
        {3, rhi::Format::RGBA32Float, offsetof(MeshVertex, tx)},
    };
    return kAttrs;
}

} // namespace rime::render
