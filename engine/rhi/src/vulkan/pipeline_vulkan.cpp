// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Graphics pipeline creation for the Vulkan backend. We bake almost all fixed-function state into
// the pipeline object (the explicit, modern model), leaving only viewport/scissor dynamic. Crucially
// the pipeline targets a *color format* via VkPipelineRenderingCreateInfo rather than a VkRenderPass
// — that is the Vulkan 1.3 "dynamic rendering" baseline (ADR-0007), and it is what keeps the RHI
// free of render-pass/framebuffer objects and friendly to the future render graph.

#include <vector>

#include "vulkan/vulkan_backend.hpp"

namespace rime::rhi {

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
    // null) so the pipeline is well-defined on every driver; when depth_test is off it simply disables
    // the test+write, which is exactly the old behavior. Stencil stays disabled until the cross-section
    // brick needs it.
    VkPipelineDepthStencilStateCreateInfo ds{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = desc.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (desc.depth_test && desc.depth_write) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = to_vk(desc.depth_compare);
    ds.depthBoundsTestEnable = VK_FALSE;
    ds.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_FALSE;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo cb{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;

    const VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    // Descriptor set layout (M3.5): a sampling pipeline declares set 0 with one combined image
    // sampler at binding 0, visible to the fragment shader (a GLSL `sampler2D`). A non-sampling
    // pipeline keeps an empty layout. The descriptor *set* is allocated lazily and cached by
    // bind_texture; here we only describe its shape. No push constants yet.
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    if (desc.sampled_texture) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dslci.bindingCount = 1;
        dslci.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device_, &dslci, nullptr, &set_layout) != VK_SUCCESS) {
            RIME_ERROR("rhi: vkCreateDescriptorSetLayout failed");
            return {};
        }
    }

    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    if (set_layout != VK_NULL_HANDLE) {
        lci.setLayoutCount = 1;
        lci.pSetLayouts = &set_layout;
    }
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &lci, nullptr, &layout) != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreatePipelineLayout failed");
        if (set_layout) vkDestroyDescriptorSetLayout(device_, set_layout, nullptr);
        return {};
    }

    // Dynamic rendering: declare the color attachment format(s) the pipeline will render into,
    // instead of referencing a VkRenderPass.
    VkFormat color_format = to_vk(desc.color_format);
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    // Declare the depth attachment's format too, so dynamic rendering can match this pipeline to a
    // pass that carries a depth buffer. UNDEFINED (the default when depth_test is off) means "no depth
    // attachment", matching a RenderingInfo with no depth_stencil.
    if (desc.depth_test) rendering.depthAttachmentFormat = to_vk(desc.depth_format);

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
    const VkResult r =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &p.pipeline);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateGraphicsPipelines('{}') failed: {}",
                   desc.debug_name,
                   result_string(r));
        vkDestroyPipelineLayout(device_, layout, nullptr);
        if (set_layout) vkDestroyDescriptorSetLayout(device_, set_layout, nullptr);
        return {};
    }
    return rebrand<Pipeline>(pipelines_.insert(p));
}

void VulkanDevice::destroy(PipelineHandle handle) {
    const auto h = rebrand<VulkanPipeline>(handle);
    if (auto* p = pipelines_.get(h)) {
        if (p->pipeline) vkDestroyPipeline(device_, p->pipeline, nullptr);
        if (p->layout) vkDestroyPipelineLayout(device_, p->layout, nullptr);
        if (p->set_layout) vkDestroyDescriptorSetLayout(device_, p->set_layout, nullptr);
        // p->set (if allocated) is freed when the descriptor pool is destroyed at device teardown —
        // M3.5 has one material, so we don't individually free pooled sets here.
        pipelines_.erase(h);
    }
}

} // namespace rime::rhi
