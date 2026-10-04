// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// The M18.5 streaming fixture, shared by the CPU policy proofs and the lavapipe teleport proof: a
// three-level replacement DAG, one group per cluster per page, every cluster a clip-space quad at
// z = 0.5 (the MVP is the identity):
//
//   group 0  coarse, PERMANENT   [-0.62, 0.62]^2             error 4.0   page 0
//   group 1  left half            x [-0.56, 0],  y +-0.56    error 1.0   page 1   children 3, 4
//   group 2  right half           x [0, 0.56],   y +-0.56    error 1.0   page 2   children 5, 6
//   groups 3..6  quadrants of     [-0.5, 0.5]^2              error 0.1   pages 3..6 (leaves)
//
// Each level is a conservative superset of the one below — 0.06 NDC, about two pixels on a 64^2
// target — which is how a real simplifier's coarse cut behaves at its silhouette, and which gives
// the "coarse covers every pixel the fine cut covers" proof a margin no rasterization rule can
// eat. Siblings share their interior edges exactly, so within a level coverage is watertight.
// Refinement: pixels_per_metre 2 wants the leaves (4.0*2 > 1, 1.0*2 > 1, 0.1*2 <= 1); 0.1 wants
// only the coarse group.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "rime/assets/virtual_geometry.hpp"

namespace rime::test::vg_streaming {

inline constexpr std::uint32_t kPageBytes = 4 * 32 + 12 * 4; // 4 v1 vertices + 12 indices = 176
inline constexpr float kNear = 2.0f;                         // pixels_per_metre that wants leaves
inline constexpr float kFar = 0.1f;                          // … that wants only the coarse cut

struct Rect {
    float x0, y0, x1, y1;
};

// Both windings, so the proof does not depend on the pipeline's front-face convention.
inline std::vector<std::byte> quad_page(Rect r) {
    constexpr std::uint32_t stride = 32;
    const float corners[4][2] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
    const std::uint32_t indices[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};
    std::vector<std::byte> bytes(4 * stride + sizeof(indices));
    for (std::uint32_t v = 0; v < 4; ++v) {
        const float position[3] = {corners[v][0], corners[v][1], 0.5f};
        std::memcpy(bytes.data() + v * stride, position, sizeof(position));
    }
    std::memcpy(bytes.data() + 4 * stride, indices, sizeof(indices)); // test host is little-endian
    return bytes;
}

inline assets::VirtualGeometryAsset tree() {
    const Rect rects[7] = {{-0.62f, -0.62f, 0.62f, 0.62f},
                           {-0.56f, -0.56f, 0.0f, 0.56f},
                           {0.0f, -0.56f, 0.56f, 0.56f},
                           {-0.5f, -0.5f, 0.0f, 0.0f},
                           {-0.5f, 0.0f, 0.0f, 0.5f},
                           {0.0f, -0.5f, 0.5f, 0.0f},
                           {0.0f, 0.0f, 0.5f, 0.5f}};
    const float errors[7] = {4.0f, 1.0f, 1.0f, 0.1f, 0.1f, 0.1f, 0.1f};

    assets::VirtualGeometryAsset asset{};
    asset.source_mesh = assets::AssetId{1};
    asset.attribs = assets::kMeshV1Attribs;
    asset.vertex_stride = assets::expected_vertex_stride(asset.attribs);
    for (std::uint32_t i = 0; i < 7; ++i) {
        const std::vector<std::byte> page = quad_page(rects[i]);
        assets::VirtualGeometryPage record{};
        record.byte_offset = asset.page_bytes.size();
        record.byte_size = static_cast<std::uint32_t>(page.size());
        record.first_cluster = i;
        record.cluster_count = 1;
        record.permanently_resident = i == 0;
        asset.pages.push_back(record);
        asset.page_bytes.insert(asset.page_bytes.end(), page.begin(), page.end());

        assets::VirtualGeometryCluster cluster{};
        cluster.bounds.min = {rects[i].x0, rects[i].y0, 0.5f};
        cluster.bounds.max = {rects[i].x1, rects[i].y1, 0.5f};
        cluster.lod_error_m = errors[i];
        cluster.page = i;
        cluster.vertex_count = 4;
        cluster.index_count = 12;
        cluster.replacement_group = i;
        asset.clusters.push_back(cluster);
    }
    asset.child_groups = {1, 2, 3, 4, 5, 6};
    asset.groups = {{0, 1, 0, 2, 4.0f, true},
                    {1, 1, 2, 2, 1.0f, false},
                    {2, 1, 4, 2, 1.0f, false},
                    {3, 1, 0, 0, 0.1f, false},
                    {4, 1, 0, 0, 0.1f, false},
                    {5, 1, 0, 0, 0.1f, false},
                    {6, 1, 0, 0, 0.1f, false}};
    asset.coarse_group = 0;
    return asset;
}

} // namespace rime::test::vg_streaming
