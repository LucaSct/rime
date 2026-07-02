// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "rime/core/math/vec.hpp"

// A minimal binary-STL loader for the ICEM viewer. ICEM writes its meshes as *binary* STL
// (icem::geo::write_stl_binary — an 80-byte header that deliberately does not start with "solid",
// a uint32 triangle count, then 50 bytes per triangle: a face normal, three vertices, a 2-byte
// attribute word). We parse that into a flat-shaded triangle soup: three vertices per triangle, each
// tagged with the triangle's own face normal. Flat shading is the honest choice for a *computed*
// engineering part — it shows the true facets of the mesh ICEM emitted rather than smoothing them
// away — and it needs no vertex welding. We recompute the normal from the geometry (n = (v1−v0) ×
// (v2−v0)) rather than trusting the file's, since some exporters write zero or unreliable normals.
//
// Header-only and pure rime::core, so the viewer app and the unit tests share one parser (the same
// "one source of truth" pattern the triangle/quad render helpers use). Derivation of the lighting
// that consumes these normals is in the fragment shader; the STL format itself is just bytes.
namespace rime::viewer {

// One vertex as the GPU consumes it: position then flat-shaded normal (both world-space; the viewer
// keeps the model transform at identity and orbits the camera, so file coordinates *are* world).
struct MeshVertex {
    float px, py, pz;
    float nx, ny, nz;
};

struct CpuMesh {
    std::vector<MeshVertex> vertices; // 3 per triangle (a soup; no shared/indexed vertices)
    core::Vec3 bb_min{std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max(),
                      std::numeric_limits<float>::max()};
    core::Vec3 bb_max{std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest(),
                      std::numeric_limits<float>::lowest()};

    [[nodiscard]] std::uint32_t triangle_count() const noexcept {
        return static_cast<std::uint32_t>(vertices.size() / 3);
    }
    // The bounding box's center and the radius of the sphere that encloses it — what the camera frames.
    [[nodiscard]] core::Vec3 center() const noexcept { return (bb_min + bb_max) * 0.5f; }
    [[nodiscard]] float radius() const noexcept { return 0.5f * core::length(bb_max - bb_min); }
};

namespace detail {

// Read a little-endian POD at byte offset `off` (STL is little-endian; on our x86/ARM targets that is
// the host order, so a memcpy is exact and avoids unaligned-read UB).
template <class T>
[[nodiscard]] inline T read_le(const std::byte* p, std::size_t off) noexcept {
    T v{};
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}

inline void expand_bounds(CpuMesh& m, core::Vec3 v) noexcept {
    m.bb_min.x = std::min(m.bb_min.x, v.x);
    m.bb_min.y = std::min(m.bb_min.y, v.y);
    m.bb_min.z = std::min(m.bb_min.z, v.z);
    m.bb_max.x = std::max(m.bb_max.x, v.x);
    m.bb_max.y = std::max(m.bb_max.y, v.y);
    m.bb_max.z = std::max(m.bb_max.z, v.z);
}

} // namespace detail

// Parse a binary STL from memory. Returns std::nullopt if the buffer is too small to be a valid binary
// STL or declares zero triangles.
[[nodiscard]] inline std::optional<CpuMesh> load_stl_binary(const std::byte* data, std::size_t len) {
    constexpr std::size_t kHeader = 80;
    constexpr std::size_t kTriBytes = 50; // 12 floats (normal + 3 verts) + 2-byte attribute
    if (data == nullptr || len < kHeader + 4) return std::nullopt;

    const std::uint32_t tri_count = detail::read_le<std::uint32_t>(data, kHeader);
    if (tri_count == 0) return std::nullopt;
    if (len < kHeader + 4 + static_cast<std::size_t>(tri_count) * kTriBytes) return std::nullopt;

    CpuMesh mesh;
    mesh.vertices.reserve(static_cast<std::size_t>(tri_count) * 3);
    for (std::uint32_t i = 0; i < tri_count; ++i) {
        const std::size_t base = kHeader + 4 + static_cast<std::size_t>(i) * kTriBytes;
        const auto read_vec = [&](std::size_t off) {
            return core::Vec3{detail::read_le<float>(data, off),
                              detail::read_le<float>(data, off + 4),
                              detail::read_le<float>(data, off + 8)};
        };
        const core::Vec3 v0 = read_vec(base + 12);
        const core::Vec3 v1 = read_vec(base + 24);
        const core::Vec3 v2 = read_vec(base + 36);

        // Geometric face normal, normalized *scale-independently*: divide the cross product by its own
        // length rather than going through core::normalize, whose zero-guard (kEpsilon = 1e-6) would
        // wrongly null out the legitimately tiny cross products of sub-millimeter triangles — exactly
        // what a finely-meshed metre-scale ICEM part produces (a ~7 cm part with ~10^6 triangles has
        // ~10^-8 cross-product magnitudes). A null normal becomes normalize(0) = NaN in the shader and
        // shades the surface black, which is the bug this avoids. Only a truly zero-area triangle (which
        // does not rasterize anyway) falls back to the file's stored normal.
        core::Vec3 n = core::cross(v1 - v0, v2 - v0);
        float nlen = core::length(n);
        if (nlen <= 0.0f) {
            n = read_vec(base); // degenerate triangle: trust the file's stored normal
            nlen = core::length(n);
        }
        n = (nlen > 0.0f) ? n / nlen : core::Vec3{0.0f, 0.0f, 1.0f};

        for (const core::Vec3& v : {v0, v1, v2}) {
            mesh.vertices.push_back({v.x, v.y, v.z, n.x, n.y, n.z});
            detail::expand_bounds(mesh, v);
        }
    }
    return mesh;
}

// Parse a binary STL file from disk.
[[nodiscard]] inline std::optional<CpuMesh> load_stl_binary_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return std::nullopt;
    const std::streamsize size = in.tellg();
    if (size <= 0) return std::nullopt;
    in.seekg(0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) return std::nullopt;
    return load_stl_binary(bytes.data(), bytes.size());
}

// A unit cube centered at the origin as a flat-shaded soup — the viewer's fallback when no STL is
// given, and a deterministic fixture for the render test. 12 triangles, outward normals.
[[nodiscard]] inline CpuMesh make_unit_cube() {
    const core::Vec3 c[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                             {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    // Six faces, each two triangles, wound counter-clockwise as seen from outside.
    const int faces[6][4] = {{0, 3, 2, 1},  // -z
                             {4, 5, 6, 7},  // +z
                             {0, 1, 5, 4},  // -y
                             {3, 7, 6, 2},  // +y
                             {0, 4, 7, 3},  // -x
                             {1, 2, 6, 5}}; // +x
    CpuMesh m;
    m.vertices.reserve(36);
    for (const auto& f : faces) {
        const core::Vec3 a = c[f[0]], b = c[f[1]], d = c[f[2]], e = c[f[3]];
        const core::Vec3 n = core::normalize(core::cross(b - a, d - a));
        const core::Vec3 quad[6] = {a, b, d, a, d, e}; // two triangles
        for (const core::Vec3& v : quad) {
            m.vertices.push_back({v.x, v.y, v.z, n.x, n.y, n.z});
            detail::expand_bounds(m, v);
        }
    }
    return m;
}

} // namespace rime::viewer
