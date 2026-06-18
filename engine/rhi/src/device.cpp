// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The backend-agnostic factory. create_device() picks a backend at build time. Today there is one
// (Vulkan, ADR-0002); a D3D12/Metal backend would slot in here behind the same RHI_* option without
// any caller changing. This file deliberately includes NO Vulkan header — it only forward-declares
// the backend factory, so the agnostic glue stays on the clean side of the seam.

#include "rime/rhi/device.hpp"

#include "rime/core/diagnostics/log.hpp"

namespace rime::rhi {

#if defined(RIME_RHI_VULKAN)
// Defined in src/vulkan/device_vulkan.cpp. Forward-declared here so we don't pull in Vulkan headers.
std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc);
#endif

std::unique_ptr<Device> create_device(const DeviceDesc& desc) {
#if defined(RIME_RHI_VULKAN)
    return create_vulkan_device(desc);
#else
    (void)desc;
    RIME_ERROR("rhi::create_device: no RHI backend was compiled in (RIME_RHI_VULKAN is off)");
    return nullptr;
#endif
}

} // namespace rime::rhi
