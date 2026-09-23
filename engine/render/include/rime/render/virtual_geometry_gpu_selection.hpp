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

// Run the CPU oracle's selection algorithm on the GPU. The input and output structs are the same
// as the CPU path; invalid inputs are rejected on the CPU before any dispatch, setting
// rejected_invalid_input = 1 and leaving the other fields empty.
[[nodiscard]] VirtualGeometrySelection
select_virtual_geometry_on_gpu(rhi::Device& device,
                               const assets::VirtualGeometryAsset& asset,
                               const VirtualGeometrySelectionInput& input);

} // namespace rime::render
