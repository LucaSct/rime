// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky pass's engine side (m17.0). Owns the fullscreen graphics pipeline, the uniform block
// sky.frag reads, and the point+clamp sampler it reads scene colour and depth through; declares one
// raster pass per frame that composites sky over the background and passes shaded pixels through
// untouched. See sky.hpp for what this sky is and is not, and ADR-0040 for where it is going.

#include "rime/render/lighting/sky.hpp"

#include <cmath>

#include "fullscreen.vert.spv.h"
#include "rime/core/math/mat.hpp"
#include "rime/render/passes.hpp" // kHdrFormat — the composited target's format
#include "sky.frag.spv.h"

namespace rime::render {
namespace {

// std140. Every member is a vec4 or a mat4, so the layout is the same on both sides without a
// single padding rule having to be remembered -- the discipline the other passes here follow.
struct GpuSkyUniforms {
    core::Mat4 inv_view_proj{};
    float camera_pos[4]{};
    float sun_dir[4]{};
    float sun_radiance[4]{};
    float zenith[4]{};
    float horizon[4]{};
    float cloud[4]{};
    float wind[4]{};
};

} // namespace

SkyPass::SkyPass(rhi::Device& device) : device_(device) {
    rhi::ShaderDesc vs{};
    vs.stage = rhi::ShaderStage::Vertex;
    vs.spirv = fullscreen_vert_spv;
    vs.spirv_size_bytes = sizeof(fullscreen_vert_spv);
    vs.debug_name = "fullscreen.vert";
    vertex_shader_ = device.create_shader(vs);

    rhi::ShaderDesc fs{};
    fs.stage = rhi::ShaderStage::Fragment;
    fs.spirv = sky_frag_spv;
    fs.spirv_size_bytes = sizeof(sky_frag_spv);
    fs.debug_name = "sky.frag";
    fragment_shader_ = device.create_shader(fs);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // scene colour (HDR)
        {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // scene depth
        {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment},        // SkyParams
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.color_format = kHdrFormat;  // writes the second HDR target the next stage reads
    pd.cull = rhi::CullMode::None; // one oversized triangle; nothing to cull
    pd.bindings = bindings;
    pd.debug_name = "sky";
    pipeline_ = device.create_graphics_pipeline(pd);

    rhi::BufferDesc ub{};
    ub.size = sizeof(GpuSkyUniforms);
    ub.usage = rhi::BufferUsage::Uniform;
    ub.memory = rhi::MemoryUsage::CpuToGpu;
    ub.debug_name = "sky-uniforms";
    uniforms_ = device.create_buffer(ub);

    // Point + clamp: the pass reads both inputs with texelFetch at the fragment's own pixel, so
    // filtering would be meaningless and a blended DEPTH in particular is a fictional surface.
    rhi::SamplerDesc ss{};
    ss.mag_filter = rhi::Filter::Nearest;
    ss.min_filter = rhi::Filter::Nearest;
    ss.address_mode = rhi::AddressMode::ClampToEdge;
    ss.debug_name = "sky-sampler";
    sampler_ = device.create_sampler(ss);
}

SkyPass::~SkyPass() {
    device_.destroy(sampler_);
    device_.destroy(uniforms_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

void SkyPass::add(RenderGraph& graph,
                  RGTexture scene_color,
                  RGTexture depth,
                  RGTexture out_hdr,
                  const SkyParams& params,
                  const SkyInputs& inputs) {
    GpuSkyUniforms u{};
    // One inverse on the CPU rather than per-pixel on the GPU. This is what turns a fragment's NDC
    // back into a world-space ray, and taking it from the actual view*proj means the sky is correct
    // for whatever projection the camera has -- including the editor viewport's own.
    u.inv_view_proj = core::inverse(inputs.proj * inputs.view);
    u.camera_pos[0] = inputs.camera_pos.x;
    u.camera_pos[1] = inputs.camera_pos.y;
    u.camera_pos[2] = inputs.camera_pos.z;

    // Normalize here rather than trusting the caller: an unnormalized sun makes the disc an ellipse
    // and the glow lobe the wrong width, which is a confusing thing to debug from the picture.
    float sx = params.sun_direction[0];
    float sy = params.sun_direction[1];
    float sz = params.sun_direction[2];
    const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (len > 1e-6f) {
        sx /= len;
        sy /= len;
        sz /= len;
    } else {
        sx = 0.0f;
        sy = 1.0f;
        sz = 0.0f;
    }
    u.sun_dir[0] = sx;
    u.sun_dir[1] = sy;
    u.sun_dir[2] = sz;

    u.sun_radiance[0] = params.sun_radiance[0];
    u.sun_radiance[1] = params.sun_radiance[1];
    u.sun_radiance[2] = params.sun_radiance[2];
    u.sun_radiance[3] = params.angular_radius;

    u.zenith[0] = params.zenith[0];
    u.zenith[1] = params.zenith[1];
    u.zenith[2] = params.zenith[2];
    u.zenith[3] = params.intensity;

    u.horizon[0] = params.horizon[0];
    u.horizon[1] = params.horizon[1];
    u.horizon[2] = params.horizon[2];
    u.horizon[3] = params.ground;

    u.cloud[0] = params.coverage;
    u.cloud[1] = params.density;
    u.cloud[2] = params.altitude;
    u.cloud[3] = params.scale;

    u.wind[0] = params.wind[0];
    u.wind[1] = params.wind[1];
    u.wind[2] = params.sharpness;
    u.wind[3] = params.clouds_enabled ? 1.0f : 0.0f;

    device_.write_buffer(uniforms_, &u, sizeof(u));

    // DontCare load: the fullscreen triangle writes every pixel. Declaring the sampled reads is
    // what orders this after the forward pass and transitions depth from DepthAttachment to
    // ShaderRead.
    const RGColorAttachment colors[] = {{out_hdr, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}}};
    const RGTexture sampled[] = {scene_color, depth};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.sampled = sampled;
    graph.add_raster_pass(
        "sky",
        desc,
        [pipe = pipeline_, ubo = uniforms_, smp = sampler_, scene_color, depth, &graph](
            rhi::CommandBuffer& cmd) {
            cmd.bind_pipeline(pipe);
            cmd.bind_texture(0, graph.physical(scene_color), smp);
            cmd.bind_texture(1, graph.physical(depth), smp);
            cmd.bind_uniform_buffer(2, ubo);
            cmd.draw(3);
        });
}

} // namespace rime::render
