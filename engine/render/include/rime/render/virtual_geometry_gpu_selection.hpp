// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/assets/virtual_geometry.hpp"
#include "rime/render/virtual_geometry_selection.hpp"
#include "rime/rhi/device.hpp"

// GPU-side implementation of M18's virtual-geometry replacement-cut selection. This is a flat
// parallel LOD cut: one compute invocation per group, each deciding independently by walking its
// own ancestor chain. No inter-thread synchronization is required because the cut decision for a
// group depends only on immutable asset data and frame-local constants, not on neighbouring
// invocations. The result is sorted ascending before returning, so callers compare against the CPU
// oracle by sorting both sides.
namespace rime::render {

// Maximum ancestry/dependency depth the GPU flat-parallel selector handles. This value is mirrored
// in engine/render/shaders/vg_select.comp as kMaxDepth; the two must be kept in lock-step.
inline constexpr std::uint32_t kVirtualGeometryGpuSelectionMaxDepth = 256u;

// The selection's per-group verdict, LEFT ON THE GPU (M18.3c). The dispatch always writes this
// buffer; by default it is destroyed before the call returns, because the only consumer was the
// readback that fills VirtualGeometrySelection::groups. Ask for it back and the draw-command
// builder (virtual_geometry_visibility_pass.hpp) can consume the verdict GPU-to-GPU, which is what
// ADR-0043 gate 4 means by "no GPU readback decides the same frame's draw list".
//
// Ownership transfers to the caller: destroy `selected_flags` through the same device. One u32 per
// group, 1 = on the cut. It stays host-readable so a test can still compare it against the oracle;
// the frame path does not read it.
struct VirtualGeometryGpuSelectionBuffers {
    rhi::BufferHandle selected_flags;
    std::uint32_t group_count = 0;
};

// Run the CPU oracle's selection algorithm on the GPU. The input and output structs are the same
// as the CPU path; invalid inputs are rejected on the CPU before any dispatch, setting
// rejected_invalid_input = 1 and leaving the other fields empty.
//
// `keep_flags`, when non-null, receives the per-group flag buffer instead of it being destroyed —
// including on the invalid-input path, where it is left empty (no dispatch ran, so there is no
// verdict to hand on; a caller must treat an invalid `selected_flags` as "select nothing").
[[nodiscard]] VirtualGeometrySelection
select_virtual_geometry_on_gpu(rhi::Device& device,
                               const assets::VirtualGeometryAsset& asset,
                               const VirtualGeometrySelectionInput& input,
                               VirtualGeometryGpuSelectionBuffers* keep_flags = nullptr);

} // namespace rime::render
