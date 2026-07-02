// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// GPU resource creation/destruction and host<->device transfer for the Vulkan backend: buffers and
// images go through VMA (we never call vkAllocateMemory directly), shader modules wrap SPIR-V. The
// public API hands back opaque handles; here is where they are minted into the per-kind SlotMaps.

#include <cstring>
#include <string>
#include <utility>

#include "vulkan/vulkan_backend.hpp"

namespace rime::rhi {

BufferHandle VulkanDevice::create_buffer(const BufferDesc& desc) {
    VkBufferUsageFlags usage = to_vk(desc.usage);
    // Device-local buffers are almost always uploaded into, so always allow them as a transfer
    // destination (the staging copy that fills them lands in a later transfer brick).
    if (desc.memory == MemoryUsage::GpuOnly)
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = desc.size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO; // VMA picks the heap from the access pattern below
    switch (desc.memory) {
        case MemoryUsage::GpuOnly:
            break;
        case MemoryUsage::CpuToGpu:
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
        case MemoryUsage::GpuToCpu:
            aci.flags =
                VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            break;
    }

    VulkanBuffer b;
    b.size = desc.size;
    b.usage = desc.usage;
    b.memory = desc.memory;
    const VkResult r = vmaCreateBuffer(allocator_, &bci, &aci, &b.buffer, &b.allocation, &b.info);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vmaCreateBuffer('{}') failed: {}", desc.debug_name, result_string(r));
        return {};
    }

    // One-shot upload at creation, for host-visible memory (info.pMappedData is the persistent map
    // we requested with VMA_ALLOCATION_CREATE_MAPPED_BIT).
    if (desc.initial_data) {
        if (b.info.pMappedData) {
            std::memcpy(b.info.pMappedData, desc.initial_data, desc.size);
            vmaFlushAllocation(allocator_, b.allocation, 0, desc.size);
        } else {
            RIME_WARN("rhi: initial_data ignored for device-local buffer '{}' (needs staging)",
                      desc.debug_name);
        }
    }
    return rebrand<Buffer>(buffers_.insert(b));
}

TextureHandle VulkanDevice::create_texture(const TextureDesc& desc) {
    // depth > 1 selects a 3-D (volume) image, sampled with a sampler3D; depth == 1 is the ordinary
    // 2-D case. The same VkFormat/usage/aspect logic serves both — only the image/view type and the
    // extent's depth differ (ADR-0013).
    const bool is_3d = desc.depth > 1;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = to_vk(desc.format);
    ici.extent = {desc.extent.width, desc.extent.height, desc.depth};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = to_vk(desc.usage);
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO; // device-local color target

    VulkanTexture t;
    t.extent = desc.extent;
    t.depth = desc.depth;
    t.format = ici.format;
    t.usage = desc.usage;
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkResult r = vmaCreateImage(allocator_, &ici, &aci, &t.image, &t.allocation, nullptr);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vmaCreateImage('{}') failed: {}", desc.debug_name, result_string(r));
        return {};
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image;
    vci.viewType = is_3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ici.format;
    // A depth image is viewed through its depth aspect, a color image through its color aspect.
    vci.subresourceRange.aspectMask = aspect_for(ici.format);
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = 1;

    r = vkCreateImageView(device_, &vci, nullptr, &t.view);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateImageView('{}') failed: {}", desc.debug_name, result_string(r));
        vmaDestroyImage(allocator_, t.image, t.allocation);
        return {};
    }
    return rebrand<Texture>(textures_.insert(t));
}

ShaderHandle VulkanDevice::create_shader(const ShaderDesc& desc) {
    VkShaderModuleCreateInfo sci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sci.codeSize = desc.spirv_size_bytes; // bytes, not words
    sci.pCode = desc.spirv;

    VulkanShader s;
    s.stage = desc.stage;
    s.entry_point = std::string(desc.entry_point);
    const VkResult r = vkCreateShaderModule(device_, &sci, nullptr, &s.module);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateShaderModule('{}') failed: {}", desc.debug_name, result_string(r));
        return {};
    }
    return rebrand<Shader>(shaders_.insert(std::move(s)));
}

SamplerHandle VulkanDevice::create_sampler(const SamplerDesc& desc) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = to_vk(desc.mag_filter);
    sci.minFilter = to_vk(desc.min_filter);
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; // no mipmaps in M3.5
    sci.addressModeU = to_vk(desc.address_mode);
    sci.addressModeV = to_vk(desc.address_mode);
    sci.addressModeW = to_vk(desc.address_mode);
    sci.maxLod = VK_LOD_CLAMP_NONE;

    VulkanSampler s;
    const VkResult r = vkCreateSampler(device_, &sci, nullptr, &s.sampler);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateSampler('{}') failed: {}", desc.debug_name, result_string(r));
        return {};
    }
    return rebrand<Sampler>(samplers_.insert(s));
}

void VulkanDevice::destroy(BufferHandle handle) {
    const auto h = rebrand<VulkanBuffer>(handle);
    if (auto* b = buffers_.get(h)) {
        vmaDestroyBuffer(allocator_, b->buffer, b->allocation);
        buffers_.erase(h);
    }
}

void VulkanDevice::destroy(TextureHandle handle) {
    const auto h = rebrand<VulkanTexture>(handle);
    if (auto* t = textures_.get(h)) {
        if (t->view)
            vkDestroyImageView(device_, t->view, nullptr);
        vmaDestroyImage(allocator_, t->image, t->allocation);
        textures_.erase(h);
    }
}

void VulkanDevice::destroy(ShaderHandle handle) {
    const auto h = rebrand<VulkanShader>(handle);
    if (auto* s = shaders_.get(h)) {
        if (s->module)
            vkDestroyShaderModule(device_, s->module, nullptr);
        shaders_.erase(h);
    }
}

void VulkanDevice::destroy(SamplerHandle handle) {
    const auto h = rebrand<VulkanSampler>(handle);
    if (auto* s = samplers_.get(h)) {
        if (s->sampler)
            vkDestroySampler(device_, s->sampler, nullptr);
        samplers_.erase(h);
    }
}

void VulkanDevice::write_buffer(BufferHandle handle,
                                const void* data,
                                std::size_t size,
                                std::size_t offset) {
    auto* b = buffers_.get(rebrand<VulkanBuffer>(handle));
    if (!b) {
        RIME_ERROR("rhi: write_buffer on an invalid handle");
        return;
    }
    if (!b->info.pMappedData) {
        RIME_ERROR("rhi: write_buffer on a non-host-visible buffer (use CpuToGpu/GpuToCpu memory)");
        return;
    }
    std::memcpy(static_cast<char*>(b->info.pMappedData) + offset, data, size);
    vmaFlushAllocation(allocator_, b->allocation, offset, size); // no-op on coherent memory
}

void VulkanDevice::read_buffer(BufferHandle handle,
                               void* dst,
                               std::size_t size,
                               std::size_t offset) {
    auto* b = buffers_.get(rebrand<VulkanBuffer>(handle));
    if (!b) {
        RIME_ERROR("rhi: read_buffer on an invalid handle");
        return;
    }
    if (!b->info.pMappedData) {
        RIME_ERROR("rhi: read_buffer on a non-host-visible buffer (use GpuToCpu memory)");
        return;
    }
    vmaInvalidateAllocation(allocator_, b->allocation, offset, size); // make device writes visible
    std::memcpy(dst, static_cast<const char*>(b->info.pMappedData) + offset, size);
}

void VulkanDevice::write_texture(TextureHandle handle, const void* data, std::size_t size) {
    auto* t = textures_.get(rebrand<VulkanTexture>(handle));
    if (!t) {
        RIME_ERROR("rhi: write_texture on an invalid handle");
        return;
    }

    // Staging buffer: a host-visible, mapped buffer we memcpy the pixels into, then copy on the GPU
    // into the device-local image. The CPU never writes device-local memory directly — this
    // transfer is how data crosses to GpuOnly storage (the path the renderer/asset pipeline will
    // later batch).
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    aci.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_alloc = nullptr;
    VmaAllocationInfo staging_info{};
    if (vmaCreateBuffer(allocator_, &bci, &aci, &staging, &staging_alloc, &staging_info) !=
        VK_SUCCESS) {
        RIME_ERROR("rhi: write_texture staging allocation failed");
        return;
    }
    std::memcpy(staging_info.pMappedData, data, size);
    vmaFlushAllocation(allocator_, staging_alloc, 0, size);

    // One-shot, blocking copy: UNDEFINED -> TRANSFER_DST, copy buffer -> image, then -> SHADER_READ
    // so the image is immediately samplable. (We overwrite the whole image, so the old layout is
    // moot.)
    auto cmd = begin_commands();
    VkCommandBuffer vk = static_cast<VulkanCommandBuffer&>(*cmd).handle();

    transition_image(vk,
                     t->image,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                     0,
                     VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {t->extent.width, t->extent.height, t->depth};
    vkCmdCopyBufferToImage(vk, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    transition_image(vk,
                     t->image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    submit_blocking(
        *cmd); // blocks until the upload completes, then the staging buffer is safe to free
    vmaDestroyBuffer(allocator_, staging, staging_alloc);
}

} // namespace rime::rhi
