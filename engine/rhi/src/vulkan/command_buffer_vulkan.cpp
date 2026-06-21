// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Command recording for the Vulkan backend: each method translates one RHI call to a vkCmd*. The
// backend owns image-layout transitions (the caller never sees a barrier), inserting them with
// synchronization2 — the Vulkan 1.3 barrier model (VkImageMemoryBarrier2/VkDependencyInfo), which
// expresses src/dst stage+access in one place and is what ADR-0007 standardizes on.

#include "vulkan/vulkan_backend.hpp"

namespace rime::rhi {

// Image-layout transitions use the shared transition_image() in vulkan_common.hpp (synchronization2);
// the swapchain's present-layout transition reuses the same helper.

void VulkanCommandBuffer::begin_rendering(const RenderingInfo& info) {
    VulkanTexture* tex = device_.lookup(info.color.target);
    if (!tex) {
        RIME_ERROR("rhi: begin_rendering with an invalid color target");
        return;
    }

    // Move the target into a color-attachment layout. Coming from whatever it was (UNDEFINED on
    // first use), nothing earlier needs to complete first.
    transition_image(cmd_,
                     tex->image,
                     tex->layout,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                     0,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    tex->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = tex->view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = to_vk(info.color.load_op);
    att.storeOp = to_vk(info.color.store_op);
    att.clearValue.color.float32[0] = info.color.clear.r;
    att.clearValue.color.float32[1] = info.color.clear.g;
    att.clearValue.color.float32[2] = info.color.clear.b;
    att.clearValue.color.float32[3] = info.color.clear.a;

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.offset = {0, 0};
    ri.renderArea.extent = {tex->extent.width, tex->extent.height};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd_, &ri);
}

void VulkanCommandBuffer::end_rendering() {
    vkCmdEndRendering(cmd_);
}

void VulkanCommandBuffer::bind_pipeline(PipelineHandle pipeline) {
    VulkanPipeline* p = device_.lookup(pipeline);
    if (!p) {
        RIME_ERROR("rhi: bind_pipeline with an invalid handle");
        return;
    }
    current_pipeline_ = p; // remembered so bind_texture can find the pipeline's descriptor layout
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
}

void VulkanCommandBuffer::bind_vertex_buffer(BufferHandle buffer, std::uint64_t offset) {
    VulkanBuffer* b = device_.lookup(buffer);
    if (!b) {
        RIME_ERROR("rhi: bind_vertex_buffer with an invalid handle");
        return;
    }
    VkBuffer vb = b->buffer;
    VkDeviceSize off = offset;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &vb, &off);
}

void VulkanCommandBuffer::bind_index_buffer(BufferHandle buffer, IndexType type, std::uint64_t offset) {
    VulkanBuffer* b = device_.lookup(buffer);
    if (!b) {
        RIME_ERROR("rhi: bind_index_buffer with an invalid handle");
        return;
    }
    vkCmdBindIndexBuffer(cmd_, b->buffer, offset, to_vk(type));
}

void VulkanCommandBuffer::bind_texture(std::uint32_t binding,
                                       TextureHandle texture,
                                       SamplerHandle sampler) {
    if (current_pipeline_ == nullptr || current_pipeline_->set_layout == VK_NULL_HANDLE) {
        RIME_ERROR("rhi: bind_texture without a bound texture-sampling pipeline (sampled_texture?)");
        return;
    }
    VulkanTexture* tex = device_.lookup(texture);
    VulkanSampler* smp = device_.lookup(sampler);
    if (!tex || !smp) {
        RIME_ERROR("rhi: bind_texture with an invalid texture/sampler handle");
        return;
    }

    // Allocate the pipeline's descriptor set once (cached on the pipeline), then write it only when
    // the texture/sampler it points at changes — so a static material never re-writes a set that may
    // still be in flight on another frame. M3.5's single-material descriptor model (see ADR-0010).
    VulkanPipeline* p = current_pipeline_;
    if (p->set == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = device_.descriptor_pool();
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &p->set_layout;
        if (vkAllocateDescriptorSets(device_.vk_device(), &ai, &p->set) != VK_SUCCESS) {
            RIME_ERROR("rhi: failed to allocate a descriptor set");
            p->set = VK_NULL_HANDLE;
            return;
        }
        p->bound_texture = {};
        p->bound_sampler = {};
    }
    if (p->bound_texture != texture || p->bound_sampler != sampler) {
        VkDescriptorImageInfo img{};
        img.sampler = smp->sampler;
        img.imageView = tex->view;
        img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = p->set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &img;
        vkUpdateDescriptorSets(device_.vk_device(), 1, &w, 0, nullptr);
        p->bound_texture = texture;
        p->bound_sampler = sampler;
    }
    vkCmdBindDescriptorSets(
        cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &p->set, 0, nullptr);
}

void VulkanCommandBuffer::set_viewport(const Viewport& viewport) {
    VkViewport vp{};
    vp.x = viewport.x;
    vp.y = viewport.y;
    vp.width = viewport.width;
    vp.height = viewport.height;
    vp.minDepth = viewport.min_depth;
    vp.maxDepth = viewport.max_depth;
    vkCmdSetViewport(cmd_, 0, 1, &vp);
}

void VulkanCommandBuffer::set_scissor(const Rect2D& scissor) {
    VkRect2D sc{};
    sc.offset = {scissor.x, scissor.y};
    sc.extent = {scissor.width, scissor.height};
    vkCmdSetScissor(cmd_, 0, 1, &sc);
}

void VulkanCommandBuffer::draw(std::uint32_t vertex_count,
                               std::uint32_t instance_count,
                               std::uint32_t first_vertex,
                               std::uint32_t first_instance) {
    vkCmdDraw(cmd_, vertex_count, instance_count, first_vertex, first_instance);
}

void VulkanCommandBuffer::draw_indexed(std::uint32_t index_count,
                                       std::uint32_t instance_count,
                                       std::uint32_t first_index,
                                       std::int32_t vertex_offset,
                                       std::uint32_t first_instance) {
    vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VulkanCommandBuffer::copy_texture_to_buffer(TextureHandle src, BufferHandle dst) {
    VulkanTexture* tex = device_.lookup(src);
    VulkanBuffer* buf = device_.lookup(dst);
    if (!tex || !buf) {
        RIME_ERROR("rhi: copy_texture_to_buffer with an invalid handle");
        return;
    }

    // The render wrote the image as a color attachment; make those writes available, then move it
    // to a transfer-source layout for the copy.
    transition_image(cmd_,
                     tex->image,
                     tex->layout,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT,
                     VK_ACCESS_2_TRANSFER_READ_BIT);
    tex->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0; // 0 => tightly packed to the image width
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {tex->extent.width, tex->extent.height, 1};
    vkCmdCopyImageToBuffer(
        cmd_, tex->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf->buffer, 1, &region);

    // Make the copy's writes visible to host reads (after the submit fence). Paired with the
    // vmaInvalidateAllocation in read_buffer, this guarantees the CPU sees the rendered pixels.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &dep);
}

} // namespace rime::rhi
