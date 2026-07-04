// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Graphics pipeline creation for the Vulkan backend. We bake almost all fixed-function state into
// the pipeline object (the explicit, modern model), leaving only viewport/scissor dynamic.
// Crucially the pipeline targets a *color format* via VkPipelineRenderingCreateInfo rather than a
// VkRenderPass — that is the Vulkan 1.3 "dynamic rendering" baseline (ADR-0007), and it is what
// keeps the RHI free of render-pass/framebuffer objects and friendly to the future render graph.

#include <array>
#include <vector>

#include "vulkan/vulkan_backend.hpp"

namespace rime::rhi {

namespace {

// Build the set-0 VkDescriptorSetLayout for a declared binding list (ADR-0020). Shared by the
// graphics and compute pipeline paths — the layout model is identical on both sides, which is
// what lets one dispatch and one draw attach the same resources.
[[nodiscard]] VkDescriptorSetLayout make_set_layout(VkDevice device,
                                                    const std::vector<BindingDesc>& bindings) {
    if (bindings.empty())
        return VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayoutBinding> vk_bindings;
    vk_bindings.reserve(bindings.size());
    for (const BindingDesc& b : bindings) {
        VkDescriptorSetLayoutBinding vb{};
        vb.binding = b.binding;
        vb.descriptorType = to_vk(b.type);
        vb.descriptorCount = 1;
        vb.stageFlags = to_vk(b.stages);
        vk_bindings.push_back(vb);
    }
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = static_cast<std::uint32_t>(vk_bindings.size());
    dslci.pBindings = vk_bindings.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    if (vkCreateDescriptorSetLayout(device, &dslci, nullptr, &layout) != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateDescriptorSetLayout failed");
        return VK_NULL_HANDLE;
    }
    return layout;
}

} // namespace

PipelineHandle VulkanDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc) {
    VulkanShader* vs = shaders_.get(rebrand<VulkanShader>(desc.vertex_shader));
    VulkanShader* fs = shaders_.get(rebrand<VulkanShader>(desc.fragment_shader));
    if (!vs || !fs) {
        RIME_ERROR("rhi: create_graphics_pipeline with an invalid shader handle");
        return {};
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs->module;
    stages[0].pName = vs->entry_point.c_str();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs->module;
    stages[1].pName = fs->entry_point.c_str();

    // Vertex input: one interleaved binding plus its attributes. A pipeline with no vertex layout
    // (stride 0) draws from constants/gl_VertexIndex, which is a fine path too.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = desc.vertex_layout.stride;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs;
    attrs.reserve(desc.vertex_layout.attributes.size());
    for (const auto& a : desc.vertex_layout.attributes) {
        VkVertexInputAttributeDescription vad{};
        vad.location = a.location;
        vad.binding = 0;
        vad.format = to_vk(a.format);
        vad.offset = a.offset;
        attrs.push_back(vad);
    }

    VkPipelineVertexInputStateCreateInfo vin{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (desc.vertex_layout.stride > 0 && !attrs.empty()) {
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &binding;
        vin.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attrs.size());
        vin.pVertexAttributeDescriptions = attrs.data();
    }

    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = to_vk(desc.topology);

    // Viewport/scissor are dynamic (set during recording), so counts are 1 and the pointers null.
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = to_vk(desc.cull);
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth/stencil state. We always provide a valid struct (rather than leaving pDepthStencilState
    // null) so the pipeline is well-defined on every driver; when depth_test is off it simply
    // disables the test+write, which is exactly the old behavior. Stencil (ADR-0014) is two-sided:
    // front- and back-facing triangles get separate ops, so the cross-section cap can count
    // surfaces in one draw.
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (desc.depth_test && desc.depth_write) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = to_vk(desc.depth_compare);
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = desc.stencil_test ? VK_TRUE : VK_FALSE;
    if (desc.stencil_test) {
        const auto face = [&](const StencilFace& f) {
            VkStencilOpState s{};
            s.failOp = to_vk(f.fail);
            s.passOp = to_vk(f.pass);
            s.depthFailOp = to_vk(f.depth_fail);
            s.compareOp = to_vk(f.compare);
            s.compareMask = desc.stencil_read_mask;
            s.writeMask = desc.stencil_write_mask;
            s.reference = desc.stencil_reference;
            return s;
        };
        ds.front = face(desc.stencil_front);
        ds.back = face(desc.stencil_back);
    }

    // Color attachment formats: the MRT list when declared, else the single-target sugar. One
    // blend state is replicated across all attachments (BlendMode applies pipeline-wide, M5.1b).
    std::array<VkFormat, kMaxColorAttachments> color_formats{};
    std::uint32_t color_count = 1;
    if (!desc.color_formats.empty()) {
        if (desc.color_formats.size() > kMaxColorAttachments) {
            RIME_ERROR("rhi: create_graphics_pipeline('{}') declares {} color targets (max {})",
                       desc.debug_name,
                       desc.color_formats.size(),
                       kMaxColorAttachments);
            return {};
        }
        color_count = static_cast<std::uint32_t>(desc.color_formats.size());
        for (std::uint32_t i = 0; i < color_count; ++i)
            color_formats[i] = to_vk(desc.color_formats[i]);
    } else {
        color_formats[0] = to_vk(desc.color_format);
    }

    // Blending (M5.1b): fixed-function "combine with what's already there". The presets bake the
    // two classic factor pairs — Alpha (the "over" operator: src.a·src + (1−src.a)·dst, with the
    // alpha channel accumulating coverage as a·1 + (1−a)·dst.a) and Additive (src + dst,
    // saturating). None keeps the pre-M5.1b overwrite.
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = desc.blend != BlendMode::None ? VK_TRUE : VK_FALSE;
    if (desc.blend == BlendMode::Alpha) {
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (desc.blend == BlendMode::Additive) {
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    blend.colorWriteMask = desc.color_write ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
                                            : 0;
    std::array<VkPipelineColorBlendAttachmentState, kMaxColorAttachments> blends{};
    blends.fill(blend);

    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = color_count;
    cb.pAttachments = blends.data();

    const VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    // Descriptor set-0 layout (ADR-0020): built from the pipeline's declared binding list — or
    // from the M3.5 `sampled_texture` sugar, which expands to the one-texture layout every pre-M5
    // pipeline used (binding 0, combined image-sampler, vertex+fragment — vertex too because the
    // viewer's warp samples its field volume in the vertex shader). Here we only describe the
    // set's *shape*; the actual descriptor sets are transient, baked per draw by the encoder's
    // flush_bindings() from whatever bind_texture/bind_uniform_buffer attached.
    std::vector<BindingDesc> bindings(desc.bindings.begin(), desc.bindings.end());
    if (bindings.empty() && desc.sampled_texture) {
        bindings.push_back(
            {0, BindingType::CombinedImageSampler, StageMask::Vertex | StageMask::Fragment});
    }
    VkDescriptorSetLayout set_layout = make_set_layout(device_, bindings);
    if (!bindings.empty() && set_layout == VK_NULL_HANDLE)
        return {};

    // One push-constant range (if requested), visible to both stages, covering [0, size). Storing
    // the stage mask on the pipeline lets push_constants() pass the matching stageFlags to
    // vkCmdPushConstants.
    const VkShaderStageFlags pc_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkPushConstantRange pc_range{};
    pc_range.stageFlags = pc_stages;
    pc_range.offset = 0;
    pc_range.size = desc.push_constant_size;

    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (set_layout != VK_NULL_HANDLE) {
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &set_layout;
    }
    if (desc.push_constant_size > 0) {
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pc_range;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &lci, nullptr, &layout) != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreatePipelineLayout failed");
        if (set_layout)
            vkDestroyDescriptorSetLayout(device_, set_layout, nullptr);
        return {};
    }

    // Dynamic rendering: declare the color attachment format(s) the pipeline will render into,
    // instead of referencing a VkRenderPass. `color_formats`/`color_count` were computed with the
    // blend state above (MRT list, or the single-target sugar).
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = color_count;
    rendering.pColorAttachmentFormats = color_formats.data();
    // Declare the depth/stencil attachment formats so dynamic rendering can match this pipeline to
    // a pass that carries them. We key off the attachment *format*, not just which test is enabled:
    // a stencil-only pass (the cap's marking draw) still binds the depth-stencil image, so every
    // pipeline used there must declare both formats to agree with the pass. UNDEFINED (the default
    // when neither test is on) means "no depth/stencil attachment", matching a RenderingInfo with
    // no depth_stencil.
    if (desc.depth_test || desc.stencil_test) {
        const VkFormat df = to_vk(desc.depth_format);
        rendering.depthAttachmentFormat = df;
        if (has_stencil(df))
            rendering.stencilAttachmentFormat = df;
    }

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = layout;
    pci.renderPass = VK_NULL_HANDLE; // <- dynamic rendering, no render pass object

    VulkanPipeline p;
    p.layout = layout;
    p.set_layout = set_layout;
    p.bindings = std::move(bindings);
    p.push_constant_stages = desc.push_constant_size > 0 ? pc_stages : 0;
    const VkResult r =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &p.pipeline);
    if (r != VK_SUCCESS) {
        RIME_ERROR(
            "rhi: vkCreateGraphicsPipelines('{}') failed: {}", desc.debug_name, result_string(r));
        vkDestroyPipelineLayout(device_, layout, nullptr);
        if (set_layout)
            vkDestroyDescriptorSetLayout(device_, set_layout, nullptr);
        return {};
    }
    set_debug_name(
        VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(p.pipeline), desc.debug_name);
    return rebrand<Pipeline>(pipelines_.insert(p));
}

PipelineHandle VulkanDevice::create_compute_pipeline(const ComputePipelineDesc& desc) {
    VulkanShader* cs = shaders_.get(rebrand<VulkanShader>(desc.shader));
    if (!cs) {
        RIME_ERROR("rhi: create_compute_pipeline with an invalid shader handle");
        return {};
    }
    if (cs->stage != ShaderStage::Compute) {
        RIME_ERROR("rhi: create_compute_pipeline('{}') with a non-compute shader", desc.debug_name);
        return {};
    }

    // Compute pipelines have no fixed-function state at all: one shader stage, the shared
    // set-layout model (ADR-0020), and an optional push-constant range — that is the whole
    // recipe. Everything a kernel touches beyond push constants arrives through bindings.
    std::vector<BindingDesc> bindings(desc.bindings.begin(), desc.bindings.end());
    VkDescriptorSetLayout set_layout = make_set_layout(device_, bindings);
    if (!bindings.empty() && set_layout == VK_NULL_HANDLE)
        return {};

    VkPushConstantRange pc_range{};
    pc_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc_range.offset = 0;
    pc_range.size = desc.push_constant_size;

    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (set_layout != VK_NULL_HANDLE) {
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &set_layout;
    }
    if (desc.push_constant_size > 0) {
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pc_range;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &lci, nullptr, &layout) != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreatePipelineLayout failed");
        if (set_layout)
            vkDestroyDescriptorSetLayout(device_, set_layout, nullptr);
        return {};
    }

    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pci.stage.module = cs->module;
    pci.stage.pName = cs->entry_point.c_str();
    pci.layout = layout;

    VulkanPipeline p;
    p.layout = layout;
    p.bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
    p.set_layout = set_layout;
    p.bindings = std::move(bindings);
    const VkShaderStageFlags cs_stage = VK_SHADER_STAGE_COMPUTE_BIT;
    p.push_constant_stages = desc.push_constant_size > 0 ? cs_stage : 0u;
    const VkResult r =
        vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &p.pipeline);
    if (r != VK_SUCCESS) {
        RIME_ERROR(
            "rhi: vkCreateComputePipelines('{}') failed: {}", desc.debug_name, result_string(r));
        vkDestroyPipelineLayout(device_, layout, nullptr);
        if (set_layout)
            vkDestroyDescriptorSetLayout(device_, set_layout, nullptr);
        return {};
    }
    set_debug_name(
        VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<std::uint64_t>(p.pipeline), desc.debug_name);
    return rebrand<Pipeline>(pipelines_.insert(p));
}

void VulkanDevice::destroy(PipelineHandle handle) {
    const auto h = rebrand<VulkanPipeline>(handle);
    if (auto* p = pipelines_.get(h)) {
        if (p->pipeline)
            vkDestroyPipeline(device_, p->pipeline, nullptr);
        if (p->layout)
            vkDestroyPipelineLayout(device_, p->layout, nullptr);
        if (p->set_layout)
            vkDestroyDescriptorSetLayout(device_, p->set_layout, nullptr);
        // Descriptor sets referencing this layout are transient (ADR-0020): they die with their
        // pool's whole-pool reset, so there is nothing per-set to free here.
        pipelines_.erase(h);
    }
}

} // namespace rime::rhi
