// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Procedural mesh primitives + the GPU registry (M5.5). The primitives carry their derivations in
// comments — they are the geometry the shading proofs assert against, so their normals/uvs being
// analytically exact is the point.

#include "rime/render/mesh.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "rime/assets/mesh_asset.hpp"
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
    return make_box(core::Vec3{half_extent, half_extent, half_extent});
}

CpuMesh make_box(const core::Vec3& half_extents) {
    // 4 vertices per face (24 total) so each face keeps its own flat normal — sharing corner
    // vertices would force the rasterizer to interpolate normals across the edge and shade the
    // box like a blob. Faces are laid out (+x, -x, +y, -y, +z, -z); each quad's vertices wind
    // counter-clockwise when seen from OUTSIDE along its normal.
    CpuMesh m;

    struct Face {
        std::array<float, 3> normal;
        std::array<std::array<float, 3>, 4> corners; // CCW from outside
    };

    const float x = half_extents.x;
    const float y = half_extents.y;
    const float z = half_extents.z;
    const Face faces[6] = {
        {{+1, 0, 0}, {{{+x, -y, -z}, {+x, +y, -z}, {+x, +y, +z}, {+x, -y, +z}}}},
        {{-1, 0, 0}, {{{-x, -y, +z}, {-x, +y, +z}, {-x, +y, -z}, {-x, -y, -z}}}},
        {{0, +1, 0}, {{{-x, +y, -z}, {-x, +y, +z}, {+x, +y, +z}, {+x, +y, -z}}}},
        {{0, -1, 0}, {{{-x, -y, +z}, {-x, -y, -z}, {+x, -y, -z}, {+x, -y, +z}}}},
        {{0, 0, +1}, {{{-x, -y, +z}, {+x, -y, +z}, {+x, +y, +z}, {-x, +y, +z}}}},
        {{0, 0, -1}, {{{+x, -y, -z}, {-x, -y, -z}, {-x, +y, -z}, {+x, +y, -z}}}},
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
    // ∂p/∂u and ∂p/∂v — solved from the position edges and their UV deltas
    // (docs/math/tangent-space.md §2) — to its three vertices, so a shared vertex averages its
    // faces, the same smoothing that makes vertex normals smooth. This is the procedural-mesh twin
    // of the cooker's MikkTSpace pass.
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
        core::Vec3 tangent =
            tan_u[i] - nrm * core::dot(nrm, tan_u[i]); // Gram-Schmidt vs. the normal
        // A vertex no triangle tangented (unreferenced, or fully degenerate UVs) has no meaningful
        // ∂p/∂u; pick any axis perpendicular to the normal so the basis stays finite and
        // orthonormal.
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
    // The local bounds, from the vertices we are about to upload — the only moment the CPU has
    // them all in hand (m13.2a).
    g.local_min = {mesh.vertices[0].px, mesh.vertices[0].py, mesh.vertices[0].pz};
    g.local_max = g.local_min;
    for (const MeshVertex& v : mesh.vertices) {
        g.local_min.x = std::min(g.local_min.x, v.px);
        g.local_min.y = std::min(g.local_min.y, v.py);
        g.local_min.z = std::min(g.local_min.z, v.pz);
        g.local_max.x = std::max(g.local_max.x, v.px);
        g.local_max.y = std::max(g.local_max.y, v.py);
        g.local_max.z = std::max(g.local_max.z, v.pz);
    }

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

    // The submesh table (m16.2). Two jobs here, and both matter downstream.
    //
    // VALIDATE, so a draw built from a range is in bounds by construction rather than by hope. The
    // cooked reader already range-checks its table, but a CpuMesh can also be hand-built, and this
    // is the one place that has both the table and the true index count. A bad range is DROPPED and
    // COUNTED rather than clamped: clamping would silently draw the wrong triangles, which is the
    // harder failure to notice.
    //
    // SYNTHESISE when the table is empty, so every uploaded mesh has at least one range. That is
    // what lets extraction loop over submeshes unconditionally instead of branching on "does this
    // mesh have a table" — the branch that, forgotten in one of the several draw paths, would draw
    // nothing at all.
    for (const SubmeshRange& sm : mesh.submeshes) {
        const std::uint64_t end =
            static_cast<std::uint64_t>(sm.first_index) + static_cast<std::uint64_t>(sm.index_count);
        if (sm.index_count == 0 || end > g.index_count) {
            ++rejected_submeshes_;
            RIME_ERROR("render: MeshRegistry::add('{}') dropping submesh [{}, {}) outside its {} "
                       "indices",
                       debug_name,
                       sm.first_index,
                       end,
                       g.index_count);
            continue;
        }
        g.submeshes.push_back(sm);
    }
    if (g.submeshes.empty()) {
        g.submeshes.push_back({0, g.index_count, 0});
    }

    meshes_.push_back(std::move(g));
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

// ── Cooked mesh → renderable mesh (m15.1) ────────────────────────────────────────────────────────

namespace {

[[nodiscard]] float read_f32(const std::byte* p) noexcept {
    float v = 0.0f;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// The byte offset of one attribute inside the interleaved vertex, given the mesh's attribute set:
// the sum of the sizes of the attributes that precede it in the cook's fixed order (which is what
// `expected_vertex_stride` also sums). Callers check `has_attrib` first for optional attributes.
[[nodiscard]] std::uint32_t attrib_offset(assets::VertexAttribs set,
                                          assets::VertexAttribs which) noexcept {
    using A = assets::VertexAttribs;
    std::uint32_t off = 0;
    for (const A bit : {A::Position, A::Normal, A::Uv, A::Tangent, A::Joints, A::Weights}) {
        if (bit == which) {
            return off;
        }
        if (assets::has_attrib(set, bit)) {
            off += assets::attrib_size(bit);
        }
    }
    return off;
}

} // namespace

CpuMesh mesh_from_cooked(const assets::MeshAsset& asset) {
    using A = assets::VertexAttribs;
    CpuMesh out;
    out.indices = asset.indices;
    // Carry the cook's submesh table through (m16.2). It has been in the cooked format since
    // `Mesh::from_primitives` existed and the reader has always validated it; this function simply
    // had nowhere to put it, so a multi-material glTF rendered in one material. `MeshRegistry::add`
    // re-validates against the real index count and synthesises a whole-mesh range if this is
    // empty, so a mesh cooked before submeshes existed still draws.
    out.submeshes.reserve(asset.submeshes.size());
    for (const assets::Submesh& sm : asset.submeshes) {
        out.submeshes.push_back({sm.first_index, sm.index_count, sm.material_slot});
    }
    out.vertices.resize(asset.vertex_count);

    const bool has_n = assets::has_attrib(asset.attribs, A::Normal);
    const bool has_uv = assets::has_attrib(asset.attribs, A::Uv);
    const bool has_t = assets::has_attrib(asset.attribs, A::Tangent);
    const std::uint32_t off_p = attrib_offset(asset.attribs, A::Position);
    const std::uint32_t off_n = attrib_offset(asset.attribs, A::Normal);
    const std::uint32_t off_uv = attrib_offset(asset.attribs, A::Uv);
    const std::uint32_t off_t = attrib_offset(asset.attribs, A::Tangent);

    for (std::uint32_t i = 0; i < asset.vertex_count; ++i) {
        const std::byte* base =
            asset.vertices.data() + static_cast<std::size_t>(i) * asset.vertex_stride;
        MeshVertex v;
        v.px = read_f32(base + off_p);
        v.py = read_f32(base + off_p + 4);
        v.pz = read_f32(base + off_p + 8);
        if (has_n) {
            v.nx = read_f32(base + off_n);
            v.ny = read_f32(base + off_n + 4);
            v.nz = read_f32(base + off_n + 8);
        }
        if (has_uv) {
            v.u = read_f32(base + off_uv);
            v.v = read_f32(base + off_uv + 4);
        }
        if (has_t) {
            v.tx = read_f32(base + off_t);
            v.ty = read_f32(base + off_t + 4);
            v.tz = read_f32(base + off_t + 8);
            v.tw = read_f32(base + off_t + 12);
        }
        out.vertices[i] = v;
    }
    // Derivable only with both — a mesh with no uvs has no tangent frame to speak of, and the
    // default (1,0,0,1) at least decodes to a finite basis.
    if (!has_t && has_uv && has_n) {
        compute_tangents(out);
    }
    return out;
}

} // namespace rime::render
