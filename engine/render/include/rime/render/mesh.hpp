// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "rime/rhi/rhi.hpp"

// Meshes for the scene layer (M5.5): the CPU-side vertex soup, procedural primitives (until the
// M6 asset pipeline imports real ones), and the registry that owns their GPU buffers behind small
// dense ids — because ECS components must stay trivially-copyable PODs (ADR-0018), an entity
// carries a MeshId, never buffer handles or pointers.
namespace rime::render {

// One vertex, 32 bytes, interleaved: position + normal (shading) + uv (texturing). Deliberately
// the PBR-ready minimum — tangents (for normal mapping) arrive with the M6 importer, where real
// meshes bring real tangent spaces.
struct MeshVertex {
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 0.0f, nz = 1.0f;
    float u = 0.0f, v = 0.0f;
};

// The CPU-side mesh: an indexed triangle list. Indices are 32-bit throughout — 16-bit halves the
// bandwidth for small meshes, but one index format keeps every path simple until a measurement
// says otherwise (measure before optimize).
struct CpuMesh {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
};

// ── Procedural primitives ─────────────────────────────────────────────────────────────────
// The M5 stand-ins for imported assets: analytically exact normals/uvs, so shading tests can
// assert against geometry instead of golden images.

// A y = 0 plane spanning ±half_extent on x/z, facing +y. `uv_tiles` repeats the texture that many
// times across the span (a mipmapped checker floor wants > 1).
[[nodiscard]] CpuMesh make_plane(float half_extent, float uv_tiles = 1.0f);

// An axis-aligned cube of the given half extent, centered at the origin: 24 vertices (4 per face,
// so each face has its own flat normal — a shared-corner cube cannot have hard edges), 36 indices,
// counter-clockwise winding seen from outside (the front-face convention the rasterizer defaults
// to).
[[nodiscard]] CpuMesh make_cube(float half_extent);

// A UV sphere: `rings` latitude bands from pole to pole, `segments` longitude slices. The
// parameterization (θ from +y pole, φ around y):
//     p = r·(sinθ·cosφ, cosθ, sinθ·sinφ),   n = p / r,   uv = (φ/2π, θ/π)
// For a unit sphere the normal IS the normalized position — which is what makes spheres the
// canonical shading test body (any lighting error shows as a wrong gradient). Poles are vertex
// rings collapsed to points; the seam duplicates one column so uv wraps cleanly.
[[nodiscard]] CpuMesh
make_uv_sphere(float radius, std::uint32_t rings = 16, std::uint32_t segments = 32);

// ── The GPU-side registry ─────────────────────────────────────────────────────────────────

// A dense mesh id (index into the registry). POD so an ECS component can carry it (ADR-0018).
using MeshId = std::uint32_t;
inline constexpr MeshId kInvalidMeshId = 0xFFFFFFFFu;

// The GPU residency of one mesh.
struct GpuMesh {
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    std::uint32_t index_count = 0;
};

// Owns every mesh's GPU buffers; hands out dense MeshIds. Host-visible uploads for M5 (the same
// simple path the viewer uses); staged device-local uploads arrive with the M6 asset pipeline
// where they belong. Not thread-safe: meshes are added during setup, read during rendering.
class MeshRegistry {
public:
    explicit MeshRegistry(rhi::Device& device) noexcept : device_(device) {}

    ~MeshRegistry();

    MeshRegistry(const MeshRegistry&) = delete;
    MeshRegistry& operator=(const MeshRegistry&) = delete;

    // Upload a mesh; returns its id (kInvalidMeshId on failure, after logging).
    [[nodiscard]] MeshId add(const CpuMesh& mesh, std::string_view debug_name = {});

    [[nodiscard]] const GpuMesh& get(MeshId id) const { return meshes_[id]; }

    [[nodiscard]] std::size_t size() const noexcept { return meshes_.size(); }

    // The vertex layout every registry mesh shares — what pipelines drawing these meshes declare.
    [[nodiscard]] static std::span<const rhi::VertexAttribute> vertex_attributes() noexcept;

    [[nodiscard]] static constexpr std::uint32_t vertex_stride() noexcept {
        return sizeof(MeshVertex);
    }

private:
    rhi::Device& device_;
    std::vector<GpuMesh> meshes_;
};

} // namespace rime::render
