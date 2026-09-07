// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/ground/derive.hpp"

#include <algorithm>

namespace rime::ground {

core::Vec3 half_extents(const GroundSurface& s) noexcept {
    return {s.half_x, s.thickness * 0.5f, s.half_z};
}

render::CpuMesh derive_mesh(const GroundSurface& s) {
    // At least one cell in each direction: a surface authored with zero cells is still a surface,
    // and returning an empty mesh would make it invisible rather than obviously wrong.
    const std::uint32_t nx = std::max<std::uint32_t>(1, s.cells_x);
    const std::uint32_t nz = std::max<std::uint32_t>(1, s.cells_z);
    const float tile = s.tile_metres > 0.0f ? s.tile_metres : 1.0f;

    render::CpuMesh m;
    m.vertices.reserve(static_cast<std::size_t>(nx + 1) * (nz + 1));
    for (std::uint32_t iz = 0; iz <= nz; ++iz) {
        for (std::uint32_t ix = 0; ix <= nx; ++ix) {
            // Corner-indexed rather than accumulated, so the far edge lands exactly on +half and
            // the two halves of the surface are symmetric to the bit.
            const float fx = static_cast<float>(ix) / static_cast<float>(nx);
            const float fz = static_cast<float>(iz) / static_cast<float>(nz);
            const float x = -s.half_x + 2.0f * s.half_x * fx;
            const float z = -s.half_z + 2.0f * s.half_z * fz;
            render::MeshVertex v;
            v.px = x;
            v.py = 0.0f;
            v.pz = z;
            v.nx = 0.0f;
            v.ny = 1.0f;
            v.nz = 0.0f;
            v.u = (x + s.half_x) / tile;
            v.v = (z + s.half_z) / tile;
            v.tx = 1.0f;
            v.ty = 0.0f;
            v.tz = 0.0f;
            v.tw = 1.0f;
            m.vertices.push_back(v);
        }
    }

    // Counter-clockwise seen from +y, matching make_plane's winding so the existing cull state is
    // unchanged by the swap.
    m.indices.reserve(static_cast<std::size_t>(nx) * nz * 6);
    const std::uint32_t stride = nx + 1;
    for (std::uint32_t iz = 0; iz < nz; ++iz) {
        for (std::uint32_t ix = 0; ix < nx; ++ix) {
            const std::uint32_t a = iz * stride + ix;
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + stride;
            const std::uint32_t d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    return m;
}

GroundCollider derive_collider(const GroundSurface& s) noexcept {
    // A slab hanging below the drawn surface, so the surface IS the top face — the property the
    // raycast proof asserts, and the reason the drawn edge and the standable edge cannot drift.
    const float half_y = std::max(s.thickness, 0.0f) * 0.5f;
    GroundCollider g;
    g.shape.type = physics::ShapeType::Box;
    g.shape.half_extents = {s.half_x, half_y, s.half_z};
    g.offset = {0.0f, -half_y, 0.0f};
    return g;
}

} // namespace rime::ground
