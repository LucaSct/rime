// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// The Vulkan backend's private toolbox: the third-party includes (confined to this directory by
// the seam, ADR-0002), a Vk_CHECK macro, the RHI-enum -> Vulkan-enum translation table, and the
// "rebrand" trick that lets a public handle index a backend SlotMap. Nothing here is visible
// outside engine/rhi/src/vulkan.

#include <volk.h> // volk defines VK_NO_PROTOTYPES and brings in the Vulkan headers

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h> // declarations only; the implementation lives in vma_impl.cpp

#include <cstdint>

#include "rime/core/containers/handle.hpp"
#include "rime/core/diagnostics/assert.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/rhi/types.hpp"

namespace rime::rhi {

// Map a VkResult to its enumerator name, for log messages. Only the codes we actually expect are
// spelled out; anything else prints its number.
[[nodiscard]] inline const char* result_string(VkResult r) noexcept {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        default: return "VK_ERROR_<other>";
    }
}

// Check a Vulkan call: log on failure and trip an assertion in checked builds. Vulkan creation
// paths additionally test their results and return gracefully; this macro is the loud backstop for
// "should never fail" calls (and surfaces the exact call site in the message).
#define VK_CHECK(expr)                                                                             \
    do {                                                                                           \
        const VkResult _vk_r = (expr);                                                             \
        if (_vk_r != VK_SUCCESS) {                                                                 \
            RIME_ERROR("Vulkan: {} -> {}", #expr, ::rime::rhi::result_string(_vk_r));              \
            RIME_ASSERT_MSG(_vk_r == VK_SUCCESS, "VK_CHECK failed");                               \
        }                                                                                          \
    } while (false)

// A public RHI handle (e.g. Handle<Buffer>) and the backend's SlotMap handle (Handle<VulkanBuffer>)
// have identical layout — both are {index, generation}. They differ only in their phantom tag, which
// keeps the *public* API type-safe. At the public/backend boundary we re-tag a handle to the
// backend's value type so it can index the backend's SlotMap. This is the one sanctioned place that
// conversion happens, and it is a pure field copy.
template <class Dst, class Src>
[[nodiscard]] inline core::Handle<Dst> rebrand(core::Handle<Src> h) noexcept {
    return core::Handle<Dst>{h.index, h.generation};
}

// ── RHI enum -> Vulkan enum translation ─────────────────────────────────────────────────────
[[nodiscard]] inline VkFormat to_vk(Format f) noexcept {
    switch (f) {
        case Format::Undefined: return VK_FORMAT_UNDEFINED;
        case Format::R8Unorm: return VK_FORMAT_R8_UNORM;
        case Format::RGBA8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case Format::RGBA8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case Format::BGRA8Unorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case Format::BGRA8Srgb: return VK_FORMAT_B8G8R8A8_SRGB;
        case Format::RG32Float: return VK_FORMAT_R32G32_SFLOAT;
        case Format::RGB32Float: return VK_FORMAT_R32G32B32_SFLOAT;
        case Format::RGBA32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case Format::D32Float: return VK_FORMAT_D32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

// Reverse of to_vk for the handful of formats a swapchain surface reports back to us (M3.4): we pick
// a VkFormat from the surface's supported list and need to hand the caller the matching rhi::Format
// so it can build a pipeline whose color attachment matches. Only swapchain-relevant formats are
// mapped; anything else is Undefined (and logged by the caller).
[[nodiscard]] inline Format from_vk(VkFormat f) noexcept {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8Unorm;
        case VK_FORMAT_R8G8B8A8_SRGB: return Format::RGBA8Srgb;
        case VK_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8Unorm;
        case VK_FORMAT_B8G8R8A8_SRGB: return Format::BGRA8Srgb;
        default: return Format::Undefined;
    }
}

[[nodiscard]] inline VkBufferUsageFlags to_vk(BufferUsage u) noexcept {
    VkBufferUsageFlags out = 0;
    if (has(u, BufferUsage::Vertex)) out |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (has(u, BufferUsage::Index)) out |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (has(u, BufferUsage::Uniform)) out |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (has(u, BufferUsage::Storage)) out |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (has(u, BufferUsage::TransferSrc)) out |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (has(u, BufferUsage::TransferDst)) out |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    return out;
}

[[nodiscard]] inline VkImageUsageFlags to_vk(TextureUsage u) noexcept {
    VkImageUsageFlags out = 0;
    if (has(u, TextureUsage::ColorAttachment)) out |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (has(u, TextureUsage::DepthStencil)) out |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (has(u, TextureUsage::Sampled)) out |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (has(u, TextureUsage::TransferSrc)) out |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (has(u, TextureUsage::TransferDst)) out |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return out;
}

[[nodiscard]] inline VkAttachmentLoadOp to_vk(LoadOp op) noexcept {
    switch (op) {
        case LoadOp::Load: return VK_ATTACHMENT_LOAD_OP_LOAD;
        case LoadOp::Clear: return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case LoadOp::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

[[nodiscard]] inline VkAttachmentStoreOp to_vk(StoreOp op) noexcept {
    switch (op) {
        case StoreOp::Store: return VK_ATTACHMENT_STORE_OP_STORE;
        case StoreOp::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    return VK_ATTACHMENT_STORE_OP_DONT_CARE;
}

[[nodiscard]] inline VkPrimitiveTopology to_vk(PrimitiveTopology t) noexcept {
    switch (t) {
        case PrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case PrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case PrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case PrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

[[nodiscard]] inline VkCullModeFlags to_vk(CullMode c) noexcept {
    switch (c) {
        case CullMode::None: return VK_CULL_MODE_NONE;
        case CullMode::Front: return VK_CULL_MODE_FRONT_BIT;
        case CullMode::Back: return VK_CULL_MODE_BACK_BIT;
    }
    return VK_CULL_MODE_NONE;
}

[[nodiscard]] inline VkCompareOp to_vk(CompareOp op) noexcept {
    switch (op) {
        case CompareOp::Never: return VK_COMPARE_OP_NEVER;
        case CompareOp::Less: return VK_COMPARE_OP_LESS;
        case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
        case CompareOp::LessEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
        case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
        case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
        case CompareOp::GreaterEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_LESS;
}

// Is this a depth (or depth-stencil) format? Used to pick the image-view/barrier aspect: depth images
// are referenced through their depth aspect, color images through the color aspect. Kept in one place
// so every site that builds a subresource range agrees.
[[nodiscard]] inline bool is_depth_format(VkFormat f) noexcept {
    switch (f) {
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D16_UNORM:
            return true;
        default:
            return false;
    }
}

// The image aspect a view/barrier should target for a given format. Depth-only for the depth formats
// we use today; the stencil aspect is added alongside the cross-section (stencil) brick.
[[nodiscard]] inline VkImageAspectFlags aspect_for(VkFormat f) noexcept {
    return is_depth_format(f) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
}

[[nodiscard]] inline VkIndexType to_vk(IndexType t) noexcept {
    return t == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
}

[[nodiscard]] inline VkFilter to_vk(Filter f) noexcept {
    return f == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

[[nodiscard]] inline VkSamplerAddressMode to_vk(AddressMode m) noexcept {
    return m == AddressMode::ClampToEdge ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                         : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

[[nodiscard]] inline DeviceType to_rhi(VkPhysicalDeviceType t) noexcept {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return DeviceType::IntegratedGpu;
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return DeviceType::DiscreteGpu;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return DeviceType::VirtualGpu;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return DeviceType::Cpu;
        default: return DeviceType::Other;
    }
}

// One image-layout transition via synchronization2 (the Vulkan 1.3 barrier model). The stage/access
// masks name "the work that must finish before the transition" (src) and "the work that waits for
// it" (dst), so each call site states its exact dependency instead of over-synchronizing. Shared by
// the command encoder (render-target / transfer transitions) and the swapchain (the present-layout
// transition) — one definition, one place the barrier idiom is explained.
inline void transition_image(VkCommandBuffer cmd,
                             VkImage image,
                             VkImageLayout old_layout,
                             VkImageLayout new_layout,
                             VkPipelineStageFlags2 src_stage,
                             VkAccessFlags2 src_access,
                             VkPipelineStageFlags2 dst_stage,
                             VkAccessFlags2 dst_access,
                             VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) noexcept {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = src_stage;
    barrier.srcAccessMask = src_access;
    barrier.dstStageMask = dst_stage;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dep);
}

} // namespace rime::rhi
