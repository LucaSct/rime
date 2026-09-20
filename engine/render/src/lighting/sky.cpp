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
#include "sky_sh.comp.spv.h"
#include "sky_skyview.comp.spv.h"

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

// The sky-view LUT's size. 192x108 is Hillaire 2020's published figure, and it is generous here:
// the table holds a gradient, a broad scatter lobe and a cloud layer, all of which are smooth. The
// one thing it CANNOT hold is the sun's disc (~0.7 deg against a ~1.8 deg texel), which is why
// sky_skyview.comp bakes sky_lighting_radiance() -- disc excluded -- and not the whole sky.
constexpr std::uint32_t kSkyViewWidth = 192;
constexpr std::uint32_t kSkyViewHeight = 108;
constexpr std::uint32_t kSkyViewGroup = 8; // must match sky_skyview.comp's local_size

// Ten vec4: nine SH coefficients, then the live flag. Must match sky_sh.comp and sky_sh_eval.glsl.
constexpr std::uint64_t kSkyShBytes = 10 * 4 * sizeof(float);

// Fill the block sky.frag and both compute shaders read. Shared so the sky that is DRAWN and the
// sky that is BAKED are filled by one piece of code -- if they could drift, the scene would be lit
// by a sky subtly different from the one on screen, which is a bug with no symptom you could point
// at.
[[nodiscard]] GpuSkyUniforms fill_uniforms(const SkyParams& params, const SkyInputs& inputs) {
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

    return u;
}

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

    // Point + clamp: the pass reads both inputs with texelFetch at the fragment's own pixel, so
    // filtering would be meaningless and a blended DEPTH in particular is a fictional surface.
    rhi::SamplerDesc ss{};
    ss.mag_filter = rhi::Filter::Nearest;
    ss.min_filter = rhi::Filter::Nearest;
    ss.address_mode = rhi::AddressMode::ClampToEdge;
    ss.debug_name = "sky-sampler";
    sampler_ = device.create_sampler(ss);

    // ── The lighting half (m17.7b) ────────────────────────────────────────────────────────────
    rhi::ShaderDesc lut_cs{};
    lut_cs.stage = rhi::ShaderStage::Compute;
    lut_cs.spirv = sky_skyview_comp_spv;
    lut_cs.spirv_size_bytes = sizeof(sky_skyview_comp_spv);
    lut_cs.debug_name = "sky_skyview.comp";
    skyview_shader_ = device.create_shader(lut_cs);

    rhi::ShaderDesc sh_cs{};
    sh_cs.stage = rhi::ShaderStage::Compute;
    sh_cs.spirv = sky_sh_comp_spv;
    sh_cs.spirv_size_bytes = sizeof(sky_sh_comp_spv);
    sh_cs.debug_name = "sky_sh.comp";
    sh_shader_ = device.create_shader(sh_cs);

    {
        const rhi::BindingDesc b[] = {
            {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute},  // the LUT being written
            {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute}, // SkyParams
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = skyview_shader_;
        pd.bindings = b;
        pd.debug_name = "sky-view-lut";
        skyview_pipeline_ = device.create_compute_pipeline(pd);
    }
    {
        const rhi::BindingDesc b[] = {
            {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // the nine coefficients
            {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute}, // SkyParams
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = sh_shader_;
        pd.bindings = b;
        pd.debug_name = "sky-sh";
        sh_pipeline_ = device.create_compute_pipeline(pd);
    }

    // Linear, because a consumer looks up a direction that falls between texels and a nearest
    // lookup would quantise the sky into visible facets in a reflection.
    //
    // KNOWN LIMITATION, recorded rather than discovered later: the LUT's u axis is azimuth and
    // genuinely WRAPS, while v is elevation and must not. rhi::SamplerDesc carries ONE address
    // mode for both axes, so the two cannot be served at once. ClampToEdge is the lesser evil:
    // clamping v is correct, and the cost is a one-texel seam at the +/-180 degree azimuth where
    // filtering cannot blend across the join. Repeat would fix that seam and break the POLES,
    // blending the zenith into the nadir -- and straight-up is exactly where a floor's reflection
    // rays point, so it is the worse trade. The real fix is per-axis address modes in the RHI, or
    // a duplicated border column; neither is worth an RHI change inside this brick.
    rhi::SamplerDesc ls{};
    ls.mag_filter = rhi::Filter::Linear;
    ls.min_filter = rhi::Filter::Linear;
    ls.address_mode = rhi::AddressMode::ClampToEdge;
    ls.debug_name = "sky-view-lut-sampler";
    lut_sampler_ = device.create_sampler(ls);

    // The no-sky placeholders. Both are created once and never written again: the shaders gate on
    // the SH buffer's flag (which is zero here), so their contents are never consulted for real
    // content -- they exist only so the consuming pipelines' fixed descriptor layouts are always
    // satisfied. Exactly DdgiProbes' dummy_irradiance_ reasoning.
    {
        rhi::TextureDesc dd{};
        dd.extent = {1, 1};
        dd.format = rhi::Format::RGBA16Float;
        dd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        dd.debug_name = "sky-dummy-skyview";
        dummy_skyview_ = device.create_texture(dd);
        const std::uint16_t zero_half4[4] = {0, 0, 0, 0};
        device.write_texture(dummy_skyview_, zero_half4, sizeof(zero_half4));

        rhi::BufferDesc bd{};
        bd.size = kSkyShBytes;
        bd.usage = rhi::BufferUsage::Storage;
        bd.memory = rhi::MemoryUsage::CpuToGpu;
        bd.debug_name = "sky-dummy-sh";
        dummy_sh_ = device.create_buffer(bd);
        const float zeros[10 * 4] = {};
        device.write_buffer(dummy_sh_, zeros, sizeof(zeros));
    }
}

SkyPass::~SkyPass() {
    if (skyview_lut_.is_valid())
        device_.destroy(skyview_lut_);
    if (sh_buffer_.is_valid())
        device_.destroy(sh_buffer_);
    device_.destroy(dummy_sh_);
    device_.destroy(dummy_skyview_);
    device_.destroy(lut_sampler_);
    device_.destroy(sh_pipeline_);
    device_.destroy(skyview_pipeline_);
    device_.destroy(sh_shader_);
    device_.destroy(skyview_shader_);
    device_.destroy(sampler_);
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
    const GpuSkyUniforms u = fill_uniforms(params, inputs);

    // This frame's slice of the graph's scratch ring, not a buffer this pass owns (m17.4). A
    // pass-owned host-visible buffer is written by the CPU every frame with nothing ordering that
    // write against the GPU still reading last frame's — invisible under `submit_blocking`, a
    // corrupt frame the moment the loop pipelines.
    const RenderGraph::FrameSlice ubo_slice = graph.push_frame_data(&u, sizeof(u));

    // DontCare load: the fullscreen triangle writes every pixel. Declaring the sampled reads is
    // what orders this after the forward pass and transitions depth from DepthAttachment to
    // ShaderRead.
    const RGColorAttachment colors[] = {{out_hdr, rhi::LoadOp::DontCare, rhi::StoreOp::Store, {}}};
    const RGTexture sampled[] = {scene_color, depth};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.sampled = sampled;
    graph.add_raster_pass("sky",
                          desc,
                          [pipe = pipeline_,
                           ubo = ubo_slice.buffer,
                           ubo_offset = ubo_slice.offset,
                           smp = sampler_,
                           scene_color,
                           depth,
                           &graph](rhi::CommandBuffer& cmd) {
                              cmd.bind_pipeline(pipe);
                              cmd.bind_texture(0, graph.physical(scene_color), smp);
                              cmd.bind_texture(1, graph.physical(depth), smp);
                              cmd.bind_uniform_buffer(2, ubo, ubo_offset, sizeof(GpuSkyUniforms));
                              cmd.draw(3);
                          });
}

// ── The lighting half (m17.7b) ───────────────────────────────────────────────────────────────────

bool SkyPass::bake_inputs_equal(const SkyParams& a, const SkyParams& b) noexcept {
    // Compared field by field rather than with memcmp: SkyParams has padding between its bools and
    // the floats that follow, and padding bytes are indeterminate, so a memcmp can report a
    // difference that does not exist -- which here would mean re-baking every frame forever while
    // `reused` sat at zero and nothing looked wrong.
    //
    // `enabled` is deliberately absent: whether the sky is ON is decided by the caller, which calls
    // empty_binding() instead. What this answers is the narrower question "would the bake come out
    // the same", and every field below is one sky_lighting_radiance() actually reads. The sun's
    // angular radius is absent for the same reason -- it only scales the disc, which the bake
    // excludes.
    const auto v3 = [](const float (&x)[3], const float (&y)[3]) {
        return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
    };
    return v3(a.zenith, b.zenith) && v3(a.horizon, b.horizon) && a.intensity == b.intensity &&
           a.ground == b.ground && v3(a.sun_direction, b.sun_direction) &&
           v3(a.sun_radiance, b.sun_radiance) && a.clouds_enabled == b.clouds_enabled &&
           a.coverage == b.coverage && a.density == b.density && a.altitude == b.altitude &&
           a.scale == b.scale && a.sharpness == b.sharpness && a.wind[0] == b.wind[0] &&
           a.wind[1] == b.wind[1];
}

void SkyPass::ensure_lighting_resources() {
    if (skyview_lut_.is_valid())
        return;

    rhi::TextureDesc td{};
    td.extent = {kSkyViewWidth, kSkyViewHeight};
    td.format = rhi::Format::RGBA16Float;
    // Storage: the bake writes it with imageStore. Sampled: SSR and DDGI read it. TransferSrc: a
    // test reads it back to check the bake against the sky the background pass paints.
    td.usage =
        rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSrc;
    td.debug_name = "sky-view-lut";
    skyview_lut_ = device_.create_texture(td);
    skyview_state_ = rhi::ResourceState::Undefined; // fresh allocation; the bake writes it whole

    rhi::BufferDesc bd{};
    bd.size = kSkyShBytes;
    bd.usage = rhi::BufferUsage::Storage;
    bd.memory = rhi::MemoryUsage::GpuOnly; // written by sky-sh, read by the forward pass
    bd.debug_name = "sky-sh";
    sh_buffer_ = device_.create_buffer(bd);
    sh_state_ = rhi::ResourceState::Undefined;
}

SkyLightBinding
SkyPass::add_lighting(RenderGraph& graph, const SkyParams& params, const SkyInputs& inputs) {
    ensure_lighting_resources();

    const RGTexture lut_rg = graph.import_texture(skyview_lut_, skyview_state_);
    const RGBuffer sh_rg = graph.import_buffer(sh_buffer_, sh_state_);

    // Nothing the bake depends on moved, so last frame's bake is still correct. Declare no passes
    // at all -- the imported resources carry forward and the consumers read exactly what they read
    // last frame. This is the whole reason the LUT and the SH buffer are persistent rather than
    // graph transients.
    if (has_bake_ && bake_inputs_equal(baked_, params)) {
        ++stats_.reused;
        return SkyLightBinding{lut_rg, sh_rg, lut_sampler_};
    }

    const GpuSkyUniforms u = fill_uniforms(params, inputs);
    const RenderGraph::FrameSlice ubo = graph.push_frame_data(&u, sizeof(u));

    {
        const RGTexture writes[] = {lut_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.storage_write = writes;
        graph.add_compute_pass(
            "sky-view-lut",
            desc,
            [pipe = skyview_pipeline_, lut_rg, ubo, &graph](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_image(0, graph.physical(lut_rg));
                cmd.bind_uniform_buffer(2, ubo.buffer, ubo.offset, sizeof(GpuSkyUniforms));
                cmd.dispatch((kSkyViewWidth + kSkyViewGroup - 1) / kSkyViewGroup,
                             (kSkyViewHeight + kSkyViewGroup - 1) / kSkyViewGroup,
                             1);
            });
    }
    {
        // Note what this pass does NOT declare: it does not read the LUT. sky_sh.comp integrates
        // sky_lighting_radiance() directly over a spherical Fibonacci set, so its accuracy does not
        // inherit the table's angular resolution and -- more to the point -- there is no per-texel
        // solid angle to get subtly wrong. docs/math/sky-lighting.md §3 has the argument.
        const RGBuffer writes[] = {sh_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.buffer_writes = writes;
        graph.add_compute_pass(
            "sky-sh", desc, [pipe = sh_pipeline_, sh_rg, ubo, &graph](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_buffer(0, graph.physical_buffer(sh_rg));
                cmd.bind_uniform_buffer(2, ubo.buffer, ubo.offset, sizeof(GpuSkyUniforms));
                cmd.dispatch(1, 1, 1); // one workgroup reduces the whole set
            });
    }

    // What each resource is ACTUALLY left in, which is not the same answer for the two.
    //
    // The SH buffer is written here and READ by the forward pass in this same frame (it is in that
    // pass's buffer_reads), so the graph transitions it and it ends in ShaderRead.
    //
    // The LUT is written here and, for now, read by nobody — the passes that will sample it are
    // the second half of m17.7b — so it ends in the general layout the compute write left it in.
    // Claiming ShaderRead here instead produced a real, visible symptom: `texture_barrier 'from'
    // disagrees with the tracked layout` on every frame after the first, because next frame's
    // import declared a state the texture was not in. DdgiProbes records StorageReadWrite after
    // its own blend pass for exactly this reason (ddgi.cpp:606). When SSR and DDGI start sampling
    // the LUT, this becomes ShaderRead and the warning is how you will know you forgot.
    skyview_state_ = rhi::ResourceState::StorageReadWrite;
    sh_state_ = rhi::ResourceState::ShaderRead;
    baked_ = params;
    has_bake_ = true;
    ++stats_.filled;
    return SkyLightBinding{lut_rg, sh_rg, lut_sampler_};
}

SkyLightBinding SkyPass::empty_binding(RenderGraph& graph) {
    // The all-zero SH buffer is what makes sky_sh_enabled() false in every consumer, and that
    // shader branch -- not the absence of a resource -- is what keeps the sky-off frame
    // byte-identical (ADR-0032 §11). Both dummies sit permanently in ShaderRead from their
    // one-time write at construction.
    return SkyLightBinding{graph.import_texture(dummy_skyview_, rhi::ResourceState::ShaderRead),
                           graph.import_buffer(dummy_sh_, rhi::ResourceState::ShaderRead),
                           lut_sampler_};
}

} // namespace rime::render
