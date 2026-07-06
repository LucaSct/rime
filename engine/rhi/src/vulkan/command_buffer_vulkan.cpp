// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Command recording for the Vulkan backend: each method translates one RHI call to a vkCmd*. The
// backend owns image-layout transitions (the caller never sees a barrier), inserting them with
// synchronization2 — the Vulkan 1.3 barrier model (VkImageMemoryBarrier2/VkDependencyInfo), which
// expresses src/dst stage+access in one place and is what ADR-0007 standardizes on.

#include "vulkan/vulkan_backend.hpp"

namespace rime::rhi {

// Image-layout transitions use the shared transition_image() in vulkan_common.hpp
// (synchronization2); the swapchain's present-layout transition reuses the same helper.

VulkanCommandBuffer::~VulkanCommandBuffer() {
    // Pools still owned here mean this encoder was never submitted (the submission paths strip
    // them via release_descriptor_pools), so no GPU work references their sets — recycling is
    // safe.
    for (VkDescriptorPool pool : pools_)
        device_.recycle_descriptor_pool(pool);
    // The timestamp query pool is encoder-owned so read_timestamps works after submission while
    // the caller still holds the encoder; destroyed with it (the usual encoders-die-before-the-
    // device contract).
    if (query_pool_ != VK_NULL_HANDLE)
        vkDestroyQueryPool(device_.vk_device(), query_pool_, nullptr);
}

void VulkanCommandBuffer::write_timestamp(std::uint32_t slot) {
    if (!device_.timestamps_supported())
        return; // documented degrade: read_timestamps will return false
    if (slot >= kMaxTimestamps) {
        RIME_ERROR(
            "rhi: write_timestamp slot {} exceeds kMaxTimestamps ({})", slot, kMaxTimestamps);
        return;
    }
    if (query_pool_ == VK_NULL_HANDLE) {
        if (in_rendering_) {
            RIME_ERROR("rhi: the first write_timestamp must be outside begin/end_rendering (the "
                       "pool reset is illegal inside)");
            return;
        }
        VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = kMaxTimestamps;
        if (vkCreateQueryPool(device_.vk_device(), &ci, nullptr, &query_pool_) != VK_SUCCESS) {
            RIME_ERROR("rhi: vkCreateQueryPool failed");
            query_pool_ = VK_NULL_HANDLE;
            return;
        }
        // Queries must be reset before first use; do the whole pool once, on this recording.
        vkCmdResetQueryPool(cmd_, query_pool_, 0, kMaxTimestamps);
    }
    // ALL_COMMANDS = stamp when every prior command has fully completed — the unambiguous (if
    // conservative) semantic; per-stage stamps are a graph-era refinement if profiles want them.
    vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, query_pool_, slot);
    if (slot + 1 > timestamp_high_water_)
        timestamp_high_water_ = slot + 1;
}

bool VulkanCommandBuffer::read_timestamps(std::span<std::uint64_t> out_ns) {
    if (query_pool_ == VK_NULL_HANDLE || timestamp_high_water_ == 0 || out_ns.empty())
        return false;
    const std::uint32_t count = out_ns.size() < timestamp_high_water_
                                    ? static_cast<std::uint32_t>(out_ns.size())
                                    : timestamp_high_water_;
    // The caller reads after submit_blocking returned, so results are ready; WAIT is a no-op
    // backstop rather than a real stall.
    const VkResult r = vkGetQueryPoolResults(device_.vk_device(),
                                             query_pool_,
                                             0,
                                             count,
                                             count * sizeof(std::uint64_t),
                                             out_ns.data(),
                                             sizeof(std::uint64_t),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkGetQueryPoolResults failed: {}", result_string(r));
        return false;
    }
    // Ticks → nanoseconds (timestampPeriod is ns-per-tick). Double math keeps 48-bit tick values
    // exact well past any frame duration we will ever time.
    for (std::uint32_t i = 0; i < count; ++i) {
        out_ns[i] = static_cast<std::uint64_t>(static_cast<double>(out_ns[i]) *
                                               static_cast<double>(device_.timestamp_period_ns()));
    }
    return true;
}

void VulkanCommandBuffer::begin_debug_label(std::string_view name) {
    if (!device_.debug_utils_enabled() || vkCmdBeginDebugUtilsLabelEXT == nullptr)
        return;
    const std::string owned(name); // NUL-terminated for the C API
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = owned.c_str();
    vkCmdBeginDebugUtilsLabelEXT(cmd_, &label);
}

void VulkanCommandBuffer::texture_barrier(TextureHandle texture,
                                          ResourceState from,
                                          ResourceState to) {
    if (in_rendering_) {
        RIME_ERROR("rhi: texture_barrier inside begin/end_rendering — transition between passes");
        return;
    }
    VulkanTexture* tex = device_.lookup(texture);
    if (!tex) {
        RIME_ERROR("rhi: texture_barrier with an invalid texture handle");
        return;
    }
    const StateInfo src = to_vk(from);
    const StateInfo dst = to_vk(to);
    // Trust the caller's `from` for the stage/access half (the graph knows what last touched the
    // resource) but the *tracked* layout for oldLayout: if the two disagree the tracked one is
    // what the image is really in, and validation will name the caller's bookkeeping bug.
    if (src.layout != tex->layout) {
        RIME_WARN("rhi: texture_barrier 'from' disagrees with the tracked layout (caller "
                  "bookkeeping vs backend tracking) — using the tracked layout");
    }
    transition_image(cmd_,
                     tex->image,
                     tex->layout,
                     dst.layout,
                     src.stages,
                     src.access,
                     dst.stages,
                     dst.access,
                     aspect_for(tex->format));
    tex->layout = dst.layout;
}

void VulkanCommandBuffer::end_debug_label() {
    if (!device_.debug_utils_enabled() || vkCmdEndDebugUtilsLabelEXT == nullptr)
        return;
    vkCmdEndDebugUtilsLabelEXT(cmd_);
}

void VulkanCommandBuffer::begin_rendering(const RenderingInfo& info) {
    // The attachment list: the MRT span when given, else the single `color` member (M5.1b) — or
    // none at all (M5.6): an empty span next to a default (never-assigned) `color` target means a
    // DEPTH-ONLY pass, the depth pre-pass's shape, rendered with zero color attachments and the
    // render area taken from the depth target instead.
    const bool depth_only = info.colors.empty() && !info.color.target.is_valid();
    if (depth_only && !info.depth_stencil) {
        RIME_ERROR("rhi: begin_rendering with neither a color nor a depth attachment");
        return;
    }
    const std::span<const ColorAttachment> colors =
        !info.colors.empty() ? info.colors
        : depth_only         ? std::span<const ColorAttachment>{}
                             : std::span<const ColorAttachment>{&info.color, 1};
    if (colors.size() > kMaxColorAttachments) {
        RIME_ERROR("rhi: begin_rendering with {} color targets (max {})",
                   colors.size(),
                   kMaxColorAttachments);
        return;
    }

    std::array<VkRenderingAttachmentInfo, kMaxColorAttachments> atts{};
    VulkanTexture* tex0 = nullptr; // attachment 0 — supplies the render area
    for (std::size_t i = 0; i < colors.size(); ++i) {
        VulkanTexture* tex = device_.lookup(colors[i].target);
        if (!tex) {
            RIME_ERROR("rhi: begin_rendering with an invalid color target (attachment {})", i);
            return;
        }
        if (i == 0)
            tex0 = tex;

        // Move the target into a color-attachment layout. On FIRST use (UNDEFINED) nothing can
        // have touched it, so the transition waits for nothing. On REUSE — a second pass loading
        // or overwriting the same target — this encoder cannot know what wrote it last (only a
        // render graph has frame-global knowledge), so it orders against ALL prior writes: the
        // conservative source is what makes back-to-back passes into one target well-defined
        // (this is the write-after-write barrier the render graph's attachment handling counts
        // on). Over-synchronized for a serial v0 queue that never overlaps anyway; the graph's
        // precise per-producer masks can take over when parallel recording lands (ADR-0019). The
        // destination includes READ because LoadOp::Load reads the attachment.
        const bool fresh = tex->layout == VK_IMAGE_LAYOUT_UNDEFINED;
        transition_image(
            cmd_,
            tex->image,
            tex->layout,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            fresh ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            fresh ? VkAccessFlags2{0} : VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        tex->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkRenderingAttachmentInfo& att = atts[i];
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = tex->view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = to_vk(colors[i].load_op);
        att.storeOp = to_vk(colors[i].store_op);
        att.clearValue.color.float32[0] = colors[i].clear.r;
        att.clearValue.color.float32[1] = colors[i].clear.g;
        att.clearValue.color.float32[2] = colors[i].clear.b;
        att.clearValue.color.float32[3] = colors[i].clear.a;
    }

    // Optional depth attachment — this is what turns a flat-2D pass into a depth-tested 3-D one.
    // Transition the depth image into a depth-attachment layout (through its *depth* aspect), then
    // describe it like the color attachment but with a depth/stencil clear value. The fragment
    // tests run at the early/late-fragment-test stages, so that is where the write becomes
    // available. `depth_att` must outlive the vkCmdBeginRendering call below, hence the
    // function-scope declaration. A combined depth-stencil target (D32FloatS8, ADR-0014) is
    // transitioned through its depth+stencil aspect and into the depth-stencil-attachment layout,
    // and is bound as *both* the depth and the stencil attachment (one view serves both). A plain
    // depth target keeps the depth-only path.
    VkRenderingAttachmentInfo depth_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil_att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool stencil_bound = false;
    VulkanTexture* dtex = nullptr; // hoisted: supplies the render area for a depth-only pass
    if (info.depth_stencil) {
        dtex = device_.lookup(info.depth_stencil->target);
        if (!dtex) {
            RIME_ERROR("rhi: begin_rendering with an invalid depth target");
        } else {
            const bool stencil = has_stencil(dtex->format);
            // One depth layout for every depth(-stencil) attachment, stencil aspect or not:
            // DEPTH_STENCIL_ATTACHMENT_OPTIMAL is valid for depth-only images (the stencil
            // aspect's rules apply only where the aspect exists), and standardizing on it keeps
            // the tracked layout in agreement with the ResourceState map (texture_barrier's
            // DepthTarget), so a graph-issued barrier and this implicit path never disagree
            // about what layout a depth image is really in. Same first-use/reuse split as the
            // color transition above: reuse must order against prior writes — that is exactly
            // the pre-pass -> forward-pass handoff, where the second pass LOADS the depth the
            // first one stored (hence READ in the destination access too).
            const VkImageLayout layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            const bool fresh = dtex->layout == VK_IMAGE_LAYOUT_UNDEFINED;
            transition_image(cmd_,
                             dtex->image,
                             dtex->layout,
                             layout,
                             fresh ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                   : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             fresh ? VkAccessFlags2{0} : VK_ACCESS_2_MEMORY_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                             aspect_for(dtex->format));
            dtex->layout = layout;

            depth_att.imageView = dtex->view;
            depth_att.imageLayout = layout;
            depth_att.loadOp = to_vk(info.depth_stencil->load_op);
            depth_att.storeOp = to_vk(info.depth_stencil->store_op);
            depth_att.clearValue.depthStencil.depth = info.depth_stencil->clear_depth;
            depth_att.clearValue.depthStencil.stencil = info.depth_stencil->clear_stencil;
            if (stencil) {
                stencil_att = depth_att; // same view + layout; the clear carries the stencil value
                stencil_bound = true;
            }
        }
    }

    // The render area: attachment 0's extent, or the depth target's for a depth-only pass. If
    // every lookup above failed there is nothing to render into — bail rather than dereference.
    VulkanTexture* area_tex = tex0 ? tex0 : dtex;
    if (!area_tex) {
        RIME_ERROR("rhi: begin_rendering with no usable attachment");
        return;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.offset = {0, 0};
    ri.renderArea.extent = {area_tex->extent.width, area_tex->extent.height};
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<std::uint32_t>(colors.size());
    ri.pColorAttachments = colors.empty() ? nullptr : atts.data();
    // imageView stays null if there was no depth attachment (or its lookup failed) → color-only
    // pass.
    if (depth_att.imageView != VK_NULL_HANDLE)
        ri.pDepthAttachment = &depth_att;
    if (stencil_bound)
        ri.pStencilAttachment = &stencil_att;
    vkCmdBeginRendering(cmd_, &ri);
    in_rendering_ = true;
}

void VulkanCommandBuffer::end_rendering() {
    vkCmdEndRendering(cmd_);
    in_rendering_ = false;
}

void VulkanCommandBuffer::bind_pipeline(PipelineHandle pipeline) {
    VulkanPipeline* p = device_.lookup(pipeline);
    if (!p) {
        RIME_ERROR("rhi: bind_pipeline with an invalid handle");
        return;
    }
    if (p->bind_point != VK_PIPELINE_BIND_POINT_GRAPHICS) {
        RIME_ERROR("rhi: bind_pipeline with a compute pipeline — use bind_compute_pipeline");
        return;
    }
    current_pipeline_ = p; // remembered so flush_bindings can find the layout + binding list
    // A new pipeline may have a different set layout, and a bound descriptor set is only valid
    // for compatible layouts — so the next draw must bake+bind a set for *this* pipeline even if
    // the attached resources didn't change. The pending attachments themselves survive pipeline
    // switches on purpose (attach a texture once, draw it through several pipelines).
    bindings_dirty_ = !p->bindings.empty();
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

void VulkanCommandBuffer::bind_index_buffer(BufferHandle buffer,
                                            IndexType type,
                                            std::uint64_t offset) {
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
    if (binding >= kMaxBindings) {
        RIME_ERROR("rhi: bind_texture binding {} exceeds kMaxBindings ({})", binding, kMaxBindings);
        return;
    }
    VulkanTexture* tex = device_.lookup(texture);
    VulkanSampler* smp = device_.lookup(sampler);
    if (!tex || !smp) {
        RIME_ERROR("rhi: bind_texture with an invalid texture/sampler handle");
        return;
    }
    // Record the attachment; it is baked into a transient descriptor set at the next draw
    // (flush_bindings, ADR-0020). We stash the raw Vk objects now so flush never re-resolves
    // handles — the price is the usual RHI rule that destroying a resource still referenced by
    // an un-submitted encoder is invalid.
    PendingBinding& pb = pending_[binding];
    pb.used = true;
    pb.type = BindingType::CombinedImageSampler;
    pb.view = tex->view;
    pb.sampler = smp->sampler;
    // Sampling normally promises SHADER_READ_ONLY_OPTIMAL; an image living in GENERAL (a storage
    // image a dispatch just wrote) is sampled in GENERAL instead — valid, and it saves the
    // caller a transition the render graph will later own (ADR-0019).
    pb.layout = tex->layout == VK_IMAGE_LAYOUT_GENERAL ? VK_IMAGE_LAYOUT_GENERAL
                                                       : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bindings_dirty_ = true;
}

void VulkanCommandBuffer::bind_uniform_buffer(std::uint32_t binding,
                                              BufferHandle buffer,
                                              std::uint64_t offset,
                                              std::uint64_t size) {
    if (binding >= kMaxBindings) {
        RIME_ERROR(
            "rhi: bind_uniform_buffer binding {} exceeds kMaxBindings ({})", binding, kMaxBindings);
        return;
    }
    VulkanBuffer* b = device_.lookup(buffer);
    if (!b) {
        RIME_ERROR("rhi: bind_uniform_buffer with an invalid buffer handle");
        return;
    }
    if (!has(b->usage, BufferUsage::Uniform)) {
        RIME_ERROR("rhi: bind_uniform_buffer on a buffer created without BufferUsage::Uniform");
        return;
    }
    PendingBinding& pb = pending_[binding];
    pb.used = true;
    pb.type = BindingType::UniformBuffer;
    pb.buffer = b->buffer;
    pb.offset = offset;
    pb.range = size == 0 ? VK_WHOLE_SIZE : size; // 0 = "through the end of the buffer"
    bindings_dirty_ = true;
}

void VulkanCommandBuffer::bind_storage_buffer(std::uint32_t binding,
                                              BufferHandle buffer,
                                              std::uint64_t offset,
                                              std::uint64_t size) {
    if (binding >= kMaxBindings) {
        RIME_ERROR(
            "rhi: bind_storage_buffer binding {} exceeds kMaxBindings ({})", binding, kMaxBindings);
        return;
    }
    VulkanBuffer* b = device_.lookup(buffer);
    if (!b) {
        RIME_ERROR("rhi: bind_storage_buffer with an invalid buffer handle");
        return;
    }
    if (!has(b->usage, BufferUsage::Storage)) {
        RIME_ERROR("rhi: bind_storage_buffer on a buffer created without BufferUsage::Storage");
        return;
    }
    PendingBinding& pb = pending_[binding];
    pb.used = true;
    pb.type = BindingType::StorageBuffer;
    pb.buffer = b->buffer;
    pb.offset = offset;
    pb.range = size == 0 ? VK_WHOLE_SIZE : size;
    bindings_dirty_ = true;
}

void VulkanCommandBuffer::bind_storage_image(std::uint32_t binding, TextureHandle texture) {
    if (binding >= kMaxBindings) {
        RIME_ERROR(
            "rhi: bind_storage_image binding {} exceeds kMaxBindings ({})", binding, kMaxBindings);
        return;
    }
    VulkanTexture* tex = device_.lookup(texture);
    if (!tex) {
        RIME_ERROR("rhi: bind_storage_image with an invalid texture handle");
        return;
    }
    if (!has(tex->usage, TextureUsage::Storage)) {
        RIME_ERROR("rhi: bind_storage_image on a texture created without TextureUsage::Storage");
        return;
    }
    // Storage access requires the GENERAL layout. Transition here, at bind time — dispatches
    // happen outside rendering scopes, and inside one a barrier is illegal anyway (hence the
    // guard). The M3 philosophy holds until the graph owns barriers (ADR-0019): the backend puts
    // the image where the declared use needs it.
    if (tex->layout != VK_IMAGE_LAYOUT_GENERAL) {
        if (in_rendering_) {
            RIME_ERROR("rhi: bind_storage_image inside begin/end_rendering needs the image "
                       "already in the general layout (bind it before the pass)");
            return;
        }
        transition_image(cmd_,
                         tex->image,
                         tex->layout,
                         VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                         0,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         aspect_for(tex->format));
        tex->layout = VK_IMAGE_LAYOUT_GENERAL;
    }
    PendingBinding& pb = pending_[binding];
    pb.used = true;
    pb.type = BindingType::StorageImage;
    pb.view = tex->view;
    pb.sampler = VK_NULL_HANDLE; // storage images are raw texel access — no sampler
    pb.layout = VK_IMAGE_LAYOUT_GENERAL;
    bindings_dirty_ = true;
}

void VulkanCommandBuffer::bind_compute_pipeline(PipelineHandle pipeline) {
    VulkanPipeline* p = device_.lookup(pipeline);
    if (!p) {
        RIME_ERROR("rhi: bind_compute_pipeline with an invalid handle");
        return;
    }
    if (p->bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) {
        RIME_ERROR("rhi: bind_compute_pipeline with a graphics pipeline — use bind_pipeline");
        return;
    }
    current_pipeline_ = p;
    bindings_dirty_ = !p->bindings.empty(); // same layout-compatibility rule as bind_pipeline
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
}

void VulkanCommandBuffer::dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) {
    if (current_pipeline_ == nullptr ||
        current_pipeline_->bind_point != VK_PIPELINE_BIND_POINT_COMPUTE) {
        RIME_ERROR("rhi: dispatch without a bound compute pipeline");
        return;
    }
    if (in_rendering_) {
        RIME_ERROR("rhi: dispatch inside begin/end_rendering — compute is not part of a raster "
                   "pass");
        return;
    }
    flush_bindings();
    vkCmdDispatch(cmd_, gx, gy, gz);

    // v0's deliberately blunt correctness net: make this dispatch's writes available to
    // EVERYTHING that follows (another dispatch, a draw that samples the result, a readback
    // copy). One full memory barrier per dispatch is over-synchronization the render graph
    // replaces with precise, declared-access-derived barriers at M5.4 (ADR-0019) — correctness
    // first, precision when the owner with frame-global knowledge exists.
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &dep);
}

VkDescriptorSet VulkanCommandBuffer::allocate_transient_set(VkDescriptorSetLayout layout) {
    // Try the newest pool first; treat exhaustion as growth, not failure. FRAGMENTED_POOL is the
    // pre-1.1 spelling of the same condition, so both fall through to chaining a fresh pool.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!pools_.empty()) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pools_.back();
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &layout;
            VkDescriptorSet set = VK_NULL_HANDLE;
            const VkResult r = vkAllocateDescriptorSets(device_.vk_device(), &ai, &set);
            if (r == VK_SUCCESS)
                return set;
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
                RIME_ERROR("rhi: vkAllocateDescriptorSets failed: {}", result_string(r));
                return VK_NULL_HANDLE;
            }
        }
        const VkDescriptorPool fresh = device_.acquire_descriptor_pool();
        if (fresh == VK_NULL_HANDLE)
            return VK_NULL_HANDLE; // acquire logged the cause
        pools_.push_back(fresh);
    }
    RIME_ERROR("rhi: could not allocate a descriptor set from a fresh pool");
    return VK_NULL_HANDLE;
}

void VulkanCommandBuffer::flush_bindings() {
    if (!bindings_dirty_ || current_pipeline_ == nullptr)
        return;
    bindings_dirty_ = false;
    VulkanPipeline* p = current_pipeline_;
    if (p->set_layout == VK_NULL_HANDLE)
        return; // pipeline declares no bindings; nothing to bake

    // One transient set per (draw × pipeline-layout) — never reused, never individually freed;
    // the whole pool resets when this encoder's submission finishes. This trades a tiny
    // allocation per draw (cheap: pool bump allocation) for the freedom to change any binding
    // between draws, which is exactly what per-draw UBO slices need. If profiling ever shows set
    // churn dominating, caching identical sets is a contained optimization behind this seam.
    const VkDescriptorSet set = allocate_transient_set(p->set_layout);
    if (set == VK_NULL_HANDLE)
        return;

    // Fixed-size scratch keyed by the pipeline's declared bindings: infos must outlive the
    // vkUpdateDescriptorSets call, and a fixed array keeps flush allocation-free.
    std::array<VkDescriptorBufferInfo, kMaxBindings> buffer_infos{};
    std::array<VkDescriptorImageInfo, kMaxBindings> image_infos{};
    std::array<VkWriteDescriptorSet, kMaxBindings> writes{};
    std::uint32_t write_count = 0;

    for (const BindingDesc& b : p->bindings) {
        if (b.binding >= kMaxBindings) {
            RIME_ERROR("rhi: pipeline declares binding {} beyond kMaxBindings ({})",
                       b.binding,
                       kMaxBindings);
            continue;
        }
        const PendingBinding& pb = pending_[b.binding];
        if (!pb.used || pb.type != b.type) {
            RIME_ERROR("rhi: draw with declared binding {} not attached (or wrong kind) — call "
                       "bind_texture / bind_uniform_buffer first",
                       b.binding);
            continue;
        }
        VkWriteDescriptorSet& w = writes[write_count];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = b.binding;
        w.descriptorCount = 1;
        w.descriptorType = to_vk(b.type);
        switch (b.type) {
            case BindingType::UniformBuffer: {
                VkDescriptorBufferInfo& bi = buffer_infos[write_count];
                bi.buffer = pb.buffer;
                bi.offset = pb.offset;
                bi.range = pb.range;
                w.pBufferInfo = &bi;
                break;
            }
            case BindingType::CombinedImageSampler: {
                VkDescriptorImageInfo& ii = image_infos[write_count];
                ii.sampler = pb.sampler;
                ii.imageView = pb.view;
                ii.imageLayout = pb.layout; // SHADER_READ_ONLY, or GENERAL for storage-written
                w.pImageInfo = &ii;
                break;
            }
            case BindingType::StorageBuffer: {
                VkDescriptorBufferInfo& bi = buffer_infos[write_count];
                bi.buffer = pb.buffer;
                bi.offset = pb.offset;
                bi.range = pb.range;
                w.pBufferInfo = &bi;
                break;
            }
            case BindingType::StorageImage: {
                VkDescriptorImageInfo& ii = image_infos[write_count];
                ii.sampler = VK_NULL_HANDLE;
                ii.imageView = pb.view;
                ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL; // storage access requires GENERAL
                w.pImageInfo = &ii;
                break;
            }
        }
        ++write_count;
    }
    if (write_count > 0)
        vkUpdateDescriptorSets(device_.vk_device(), write_count, writes.data(), 0, nullptr);
    vkCmdBindDescriptorSets(cmd_, p->bind_point, p->layout, 0, 1, &set, 0, nullptr);
}

void VulkanCommandBuffer::push_constants(const void* data,
                                         std::uint32_t size,
                                         std::uint32_t offset) {
    if (current_pipeline_ == nullptr || current_pipeline_->push_constant_stages == 0) {
        RIME_ERROR("rhi: push_constants without a bound pipeline that declares push_constant_size");
        return;
    }
    vkCmdPushConstants(cmd_,
                       current_pipeline_->layout,
                       current_pipeline_->push_constant_stages,
                       offset,
                       size,
                       data);
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
    flush_bindings(); // bake pending resource attachments into this draw's descriptor set
    vkCmdDraw(cmd_, vertex_count, instance_count, first_vertex, first_instance);
}

void VulkanCommandBuffer::draw_indexed(std::uint32_t index_count,
                                       std::uint32_t instance_count,
                                       std::uint32_t first_index,
                                       std::int32_t vertex_offset,
                                       std::uint32_t first_instance) {
    flush_bindings();
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
