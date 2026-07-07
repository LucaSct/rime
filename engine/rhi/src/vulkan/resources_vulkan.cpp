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

    // Mip chain length (M5.3): what the caller asked for, clamped to what the extent supports
    // (each level halves until 1×1 — floor(log2(max_dim)) + 1 levels). Volumes stay single-level
    // (no consumer yet), loudly rather than silently.
    std::uint32_t mips = desc.mip_levels == 0 ? 1u : desc.mip_levels;
    if (is_3d && mips > 1) {
        RIME_ERROR("rhi: create_texture('{}'): mip-mapped 3-D textures are unsupported (using 1)",
                   desc.debug_name);
        mips = 1;
    }
    std::uint32_t max_dim =
        desc.extent.width > desc.extent.height ? desc.extent.width : desc.extent.height;
    std::uint32_t full_chain = 1;
    while (max_dim > 1) {
        max_dim >>= 1;
        ++full_chain;
    }
    if (mips > full_chain)
        mips = full_chain;

    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ici.format = to_vk(desc.format);
    ici.extent = {desc.extent.width, desc.extent.height, desc.depth};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = to_vk(desc.usage);
    // Generating the chain (write_texture) blits level i-1 → i, which needs the image to be both
    // a blit source and destination — added implicitly so callers only say "mip_levels = N".
    if (mips > 1)
        ici.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO; // device-local color target

    VulkanTexture t;
    t.extent = desc.extent;
    t.depth = desc.depth;
    t.mip_levels = mips;
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
    vci.subresourceRange.levelCount = mips; // the sampled view spans the whole chain
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = 1;

    r = vkCreateImageView(device_, &vci, nullptr, &t.view);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateImageView('{}') failed: {}", desc.debug_name, result_string(r));
        vmaDestroyImage(allocator_, t.image, t.allocation);
        return {};
    }
    set_debug_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(t.image), desc.debug_name);
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
    sci.mipmapMode = to_vk_mipmap(desc.mip_filter); // across-level blend (trilinear when Linear)
    sci.addressModeU = to_vk(desc.address_mode);
    sci.addressModeV = to_vk(desc.address_mode);
    sci.addressModeW = to_vk(desc.address_mode);
    sci.maxLod = VK_LOD_CLAMP_NONE;
    // Anisotropy (M5.3): on when the caller asks for >1 and the device has the feature; clamped
    // to the hardware limit. Absent feature = plain trilinear (documented degrade, not an error).
    if (desc.max_anisotropy > 1.0f && anisotropy_supported_) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = desc.max_anisotropy < max_anisotropy_limit_ ? desc.max_anisotropy
                                                                        : max_anisotropy_limit_;
    }

    VulkanSampler s;
    const VkResult r = vkCreateSampler(device_, &sci, nullptr, &s.sampler);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateSampler('{}') failed: {}", desc.debug_name, result_string(r));
        return {};
    }
    set_debug_name(
        VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<std::uint64_t>(s.sampler), desc.debug_name);
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

    // Mip generation (M5.3): the caller supplied level 0; each further level is a GPU blit of the
    // previous one at half size (linear filter = a 2×2 box average). The ping-pong is all in the
    // barriers: level i-1 flips DST→SRC once its content is final, level i is blitted, and so on
    // down to 1×1. The whole chain then moves to SHADER_READ_ONLY together.
    for (std::uint32_t level = 1; level < t->mip_levels; ++level) {
        transition_image(vk,
                         t->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_BLIT_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         level - 1,
                         1);
        const auto dim = [](std::uint32_t base, std::uint32_t lvl) {
            const std::uint32_t d = base >> lvl;
            return static_cast<std::int32_t>(d == 0 ? 1u : d);
        };
        VkImageBlit blit{};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = level - 1;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1] = {dim(t->extent.width, level - 1), dim(t->extent.height, level - 1), 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = level;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1] = {dim(t->extent.width, level), dim(t->extent.height, level), 1};
        vkCmdBlitImage(vk,
                       t->image,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       t->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1,
                       &blit,
                       VK_FILTER_LINEAR);
    }
    if (t->mip_levels > 1) {
        // Levels 0..N-2 sit in TRANSFER_SRC (each was a blit source), the last in TRANSFER_DST.
        transition_image(vk,
                         t->image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_BLIT_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         0,
                         t->mip_levels - 1);
        transition_image(vk,
                         t->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_BLIT_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT,
                         t->mip_levels - 1,
                         1);
    } else {
        transition_image(vk,
                         t->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    submit_blocking(
        *cmd); // blocks until the upload completes, then the staging buffer is safe to free
    vmaDestroyBuffer(allocator_, staging, staging_alloc);
}

void VulkanDevice::write_texture_mips(TextureHandle handle, std::span<const MipData> levels) {
    auto* t = textures_.get(rebrand<VulkanTexture>(handle));
    if (!t) {
        RIME_ERROR("rhi: write_texture_mips on an invalid handle");
        return;
    }
    // The caller must supply exactly one buffer per allocated level: a cooked chain is uploaded
    // whole, never partially (a mismatch means the cook and the texture disagree — reject it).
    if (levels.size() != t->mip_levels) {
        RIME_ERROR("rhi: write_texture_mips: {} levels supplied for a {}-level texture",
                   levels.size(),
                   t->mip_levels);
        return;
    }

    // One staging buffer holds the whole chain, the levels concatenated; each level is then copied
    // to its own mip. No blits: the cooked chain is authoritative (gamma-correct offline mips), so
    // we upload it verbatim rather than regenerate it on the GPU the way write_texture does.
    std::size_t total = 0;
    for (const MipData& level : levels) {
        total += level.pixels.size();
    }
    if (total == 0) {
        RIME_ERROR("rhi: write_texture_mips: no pixel data");
        return;
    }

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = total;
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
        RIME_ERROR("rhi: write_texture_mips staging allocation failed");
        return;
    }

    // memcpy each level into the staging buffer and record its buffer→image copy (mip level +
    // extent).
    std::vector<VkBufferImageCopy> regions;
    regions.reserve(levels.size());
    std::size_t offset = 0;
    auto* mapped = static_cast<std::byte*>(staging_info.pMappedData);
    for (std::uint32_t level = 0; level < levels.size(); ++level) {
        const std::span<const std::byte> px = levels[level].pixels;
        std::memcpy(mapped + offset, px.data(), px.size());
        const auto dim = [](std::uint32_t base, std::uint32_t lvl) {
            const std::uint32_t d = base >> lvl;
            return d == 0 ? 1u : d;
        };
        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = level;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {dim(t->extent.width, level), dim(t->extent.height, level), 1};
        regions.push_back(region);
        offset += px.size();
    }
    vmaFlushAllocation(allocator_, staging_alloc, 0, total);

    auto cmd = begin_commands();
    VkCommandBuffer vk = static_cast<VulkanCommandBuffer&>(*cmd).handle();

    // Whole chain UNDEFINED -> TRANSFER_DST, copy every level, whole chain -> SHADER_READ (the
    // default transition_image range covers all levels).
    transition_image(vk,
                     t->image,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                     0,
                     VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(vk,
                           staging,
                           t->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<std::uint32_t>(regions.size()),
                           regions.data());
    transition_image(vk,
                     t->image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                     VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    submit_blocking(*cmd);
    vmaDestroyBuffer(allocator_, staging, staging_alloc);
}

} // namespace rime::rhi
