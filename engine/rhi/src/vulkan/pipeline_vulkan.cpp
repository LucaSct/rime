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

    // No descriptor sets or push constants in M3 — an empty layout. Descriptors (for the textured
    // quad's sampler) arrive in M3.5.
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (vkCreatePipelineLayout(device_, &lci, nullptr, &layout) != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreatePipelineLayout failed");
        return {};
    }

    // Dynamic rendering: declare the color attachment format(s) the pipeline will render into,
    // instead of referencing a VkRenderPass.
    VkFormat color_format = to_vk(desc.color_format);
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vin;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = layout;
    pci.renderPass = VK_NULL_HANDLE; // <- dynamic rendering, no render pass object

    VulkanPipeline p;
    p.layout = layout;
    const VkResult r =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &p.pipeline);
    if (r != VK_SUCCESS) {
        RIME_ERROR("rhi: vkCreateGraphicsPipelines('{}') failed: {}",
                   desc.debug_name,
                   result_string(r));
        vkDestroyPipelineLayout(device_, layout, nullptr);
        return {};
    }
    return rebrand<Pipeline>(pipelines_.insert(p));
}

void VulkanDevice::destroy(PipelineHandle handle) {
    const auto h = rebrand<VulkanPipeline>(handle);
    if (auto* p = pipelines_.get(h)) {
        if (p->pipeline) vkDestroyPipeline(device_, p->pipeline, nullptr);
        if (p->layout) vkDestroyPipelineLayout(device_, p->layout, nullptr);
        pipelines_.erase(h);
    }
}

} // namespace rime::rhi
