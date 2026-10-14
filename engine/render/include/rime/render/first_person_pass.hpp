// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/render/render_graph.hpp"

namespace rime::render {

// Finishes the first-person overlay by selecting model depth wherever a model pixel exists.
// The world depth remains untouched for SSR; TAA receives this combined depth instead.
class FirstPersonPass {
public:
    explicit FirstPersonPass(rhi::Device& device);
    ~FirstPersonPass();

    FirstPersonPass(const FirstPersonPass&) = delete;
    FirstPersonPass& operator=(const FirstPersonPass&) = delete;

    [[nodiscard]] RGTexture combine_depth(RenderGraph& graph,
                                          RGTexture world_depth,
                                          RGTexture model_depth,
                                          rhi::Extent2D extent) const;

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
};

} // namespace rime::render
