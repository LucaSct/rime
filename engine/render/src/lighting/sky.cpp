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
#include "sky_multiple_scattering.comp.spv.h"
#include "sky_sh.comp.spv.h"
#include "sky_skyview.comp.spv.h"
#include "sky_transmittance.comp.spv.h"

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

// Unlike GpuSkyUniforms this block is deliberately free of camera and art-direction data.  The
// two view-independent tables are functions of the medium, not of the weather controls used by
// the analytic sky that currently draws the frame.  Every field is a vec4 so std140 has no hidden
// padding rule for the CPU and GLSL sides to disagree about.
struct GpuAtmosphereUniforms {
    float radii[4]{};             // planet radius, top height, Rayleigh height, Mie height (km)
    float rayleigh_scattering[4]; // 1/km
    float mie_scattering[4];      // 1/km
    float mie_absorption[4];      // 1/km
    float ground_albedo[4]{};
    float solar_irradiance[4]{}; // unit white: a physical LUT must not inherit an authored sun
};

// The sky-view LUT's size. 192x108 is Hillaire 2020's published figure, and it is generous here:
// the table holds a gradient, a broad scatter lobe and a cloud layer, all of which are smooth. The
// one thing it CANNOT hold is the sun's disc (~0.7 deg against a ~1.8 deg texel), which is why
// sky_skyview.comp bakes sky_lighting_radiance() -- disc excluded -- and not the whole sky.
constexpr std::uint32_t kSkyViewWidth = 192;
constexpr std::uint32_t kSkyViewHeight = 108;
constexpr std::uint32_t kSkyViewGroup = 8; // must match sky_skyview.comp's local_size
constexpr std::uint32_t kTransmittanceWidth = 256;
constexpr std::uint32_t kTransmittanceHeight = 64;
constexpr std::uint32_t kTransmittanceGroup = 8; // must match sky_transmittance.comp's local_size
constexpr std::uint32_t kMultipleScatteringSize = 32;
constexpr std::uint32_t kMultipleScatteringGroup =
    8; // must match sky_multiple_scattering.comp's local_size

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

[[nodiscard]] GpuAtmosphereUniforms fill_atmosphere_uniforms(const SkyParams& params) {
    const SkyParams::Atmosphere& a = params.atmosphere;
    GpuAtmosphereUniforms u{};
    u.radii[0] = a.planet_radius_km;
    u.radii[1] = a.atmosphere_height_km;
    u.radii[2] = a.rayleigh_scale_height_km;
    u.radii[3] = a.mie_scale_height_km;
    for (std::uint32_t i = 0; i < 3; ++i) {
        u.rayleigh_scattering[i] = a.rayleigh_scattering[i];
        u.mie_scattering[i] = a.mie_scattering[i];
        u.mie_absorption[i] = a.mie_absorption[i];
        u.ground_albedo[i] = a.ground_albedo[i];
        u.solar_irradiance[i] = 1.0f;
    }
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
        {3, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment}, // physical sky-view
    };
    rhi::GraphicsPipelineDesc graphics_pd{};
    graphics_pd.vertex_shader = vertex_shader_;
    graphics_pd.fragment_shader = fragment_shader_;
    graphics_pd.color_format = kHdrFormat;  // writes the second HDR target the next stage reads
    graphics_pd.cull = rhi::CullMode::None; // one oversized triangle; nothing to cull
    graphics_pd.bindings = bindings;
    graphics_pd.debug_name = "sky";
    pipeline_ = device.create_graphics_pipeline(graphics_pd);

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

    rhi::ShaderDesc transmittance_cs{};
    transmittance_cs.stage = rhi::ShaderStage::Compute;
    transmittance_cs.spirv = sky_transmittance_comp_spv;
    transmittance_cs.spirv_size_bytes = sizeof(sky_transmittance_comp_spv);
    transmittance_cs.debug_name = "sky_transmittance.comp";
    transmittance_shader_ = device.create_shader(transmittance_cs);

    rhi::ShaderDesc multiple_scattering_cs{};
    multiple_scattering_cs.stage = rhi::ShaderStage::Compute;
    multiple_scattering_cs.spirv = sky_multiple_scattering_comp_spv;
    multiple_scattering_cs.spirv_size_bytes = sizeof(sky_multiple_scattering_comp_spv);
    multiple_scattering_cs.debug_name = "sky_multiple_scattering.comp";
    multiple_scattering_shader_ = device.create_shader(multiple_scattering_cs);

    {
        const rhi::BindingDesc b[] = {
            {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute},  // the LUT being written
            {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute}, // SkyParams
            {3, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute}, // physical atmosphere
            {4, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
            {5, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
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
            {3, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute}, // physical atmosphere
            {4, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
            {5, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = sh_shader_;
        pd.bindings = b;
        pd.debug_name = "sky-sh";
        sh_pipeline_ = device.create_compute_pipeline(pd);
    }
    {
        const rhi::BindingDesc b[] = {
            {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
            {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = transmittance_shader_;
        pd.bindings = b;
        pd.debug_name = "sky-transmittance-lut";
        transmittance_pipeline_ = device.create_compute_pipeline(pd);
    }
    {
        const rhi::BindingDesc b[] = {
            {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute},
            {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
            {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = multiple_scattering_shader_;
        pd.bindings = b;
        pd.debug_name = "sky-multiple-scattering-lut";
        multiple_scattering_pipeline_ = device.create_compute_pipeline(pd);
    }

    // Linear, because a consumer looks up a direction that falls between texels and a nearest
    // lookup would quantise the sky into visible facets in a reflection.
    //
    // Sky-view wraps around azimuth but terminates at real elevation poles. Its first full-screen
    // reader makes that distinction visible, so use the RHI's axis overrides rather than accepting
    // a clamp seam at +/-180 degrees or blending zenith into the nadir.
    rhi::SamplerDesc ls{};
    ls.mag_filter = rhi::Filter::Linear;
    ls.min_filter = rhi::Filter::Linear;
    ls.address_mode = rhi::AddressMode::ClampToEdge;
    ls.address_mode_u = rhi::AddressMode::Repeat;
    ls.debug_name = "sky-view-lut-sampler";
    lut_sampler_ = device.create_sampler(ls);

    // The physical LUTs have no azimuth seam and are parameter tables, so clamp is correct on
    // both axes.  This is separate from the sky-view sampler because its azimuth really wraps.
    ls.debug_name = "sky-atmosphere-lut-sampler";
    atmosphere_sampler_ = device.create_sampler(ls);

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
    if (multiple_scattering_lut_.is_valid())
        device_.destroy(multiple_scattering_lut_);
    if (transmittance_lut_.is_valid())
        device_.destroy(transmittance_lut_);
    if (skyview_lut_.is_valid())
        device_.destroy(skyview_lut_);
    if (sh_buffer_.is_valid())
        device_.destroy(sh_buffer_);
    device_.destroy(dummy_sh_);
    device_.destroy(dummy_skyview_);
    device_.destroy(atmosphere_sampler_);
    device_.destroy(lut_sampler_);
    device_.destroy(multiple_scattering_pipeline_);
    device_.destroy(transmittance_pipeline_);
    device_.destroy(sh_pipeline_);
    device_.destroy(skyview_pipeline_);
    device_.destroy(multiple_scattering_shader_);
    device_.destroy(transmittance_shader_);
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
                  const SkyInputs& inputs,
                  const SkyLightBinding& lighting) {
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
    const RGTexture sampled[] = {scene_color, depth, lighting.skyview};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.sampled = sampled;
    graph.add_raster_pass("sky",
                          desc,
                          [pipe = pipeline_,
                           ubo = ubo_slice.buffer,
                           ubo_offset = ubo_slice.offset,
                           smp = sampler_,
                           sky_sampler = lighting.sampler,
                           scene_color,
                           depth,
                           skyview = lighting.skyview,
                           &graph](rhi::CommandBuffer& cmd) {
                              cmd.bind_pipeline(pipe);
                              cmd.bind_texture(0, graph.physical(scene_color), smp);
                              cmd.bind_texture(1, graph.physical(depth), smp);
                              cmd.bind_uniform_buffer(2, ubo, ubo_offset, sizeof(GpuSkyUniforms));
                              cmd.bind_texture(3, graph.physical(skyview), sky_sampler);
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
    // zenith, horizon and ground belong only to the retained legacy fallback. The m17.7d
    // sky-view/SH/background body no longer reads them, so letting any of them refill this cache
    // would waste a physical integration and conceal a dependency regression in the opposite
    // direction.
    return a.intensity == b.intensity && v3(a.sun_direction, b.sun_direction) &&
           v3(a.sun_radiance, b.sun_radiance) && a.clouds_enabled == b.clouds_enabled &&
           a.coverage == b.coverage && a.density == b.density && a.altitude == b.altitude &&
           a.scale == b.scale && a.sharpness == b.sharpness && a.wind[0] == b.wind[0] &&
           a.wind[1] == b.wind[1];
}

bool SkyPass::transmittance_inputs_equal(const SkyParams& a, const SkyParams& b) noexcept {
    const auto v3 = [](const float (&x)[3], const float (&y)[3]) {
        return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
    };
    const SkyParams::Atmosphere& x = a.atmosphere;
    const SkyParams::Atmosphere& y = b.atmosphere;
    // Ground albedo is absent: it cannot change the optical depth from a point to the top of the
    // atmosphere.  Keeping it out makes a ground-only edit visibly reuse this table.
    return x.planet_radius_km == y.planet_radius_km &&
           x.atmosphere_height_km == y.atmosphere_height_km &&
           x.rayleigh_scale_height_km == y.rayleigh_scale_height_km &&
           x.mie_scale_height_km == y.mie_scale_height_km &&
           v3(x.rayleigh_scattering, y.rayleigh_scattering) &&
           v3(x.mie_scattering, y.mie_scattering) && v3(x.mie_absorption, y.mie_absorption);
}

bool SkyPass::multiple_scattering_inputs_equal(const SkyParams& a, const SkyParams& b) noexcept {
    const auto v3 = [](const float (&x)[3], const float (&y)[3]) {
        return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
    };
    const SkyParams::Atmosphere& x = a.atmosphere;
    const SkyParams::Atmosphere& y = b.atmosphere;
    // This table samples transmittance, so every physical input that can change that source is a
    // dependency too.  Nothing authored by the analytic sky, its sun, or the camera belongs here.
    return x.planet_radius_km == y.planet_radius_km &&
           x.atmosphere_height_km == y.atmosphere_height_km &&
           x.rayleigh_scale_height_km == y.rayleigh_scale_height_km &&
           x.mie_scale_height_km == y.mie_scale_height_km &&
           v3(x.rayleigh_scattering, y.rayleigh_scattering) &&
           v3(x.mie_scattering, y.mie_scattering) && v3(x.mie_absorption, y.mie_absorption) &&
           v3(x.ground_albedo, y.ground_albedo);
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

    td.extent = {kTransmittanceWidth, kTransmittanceHeight};
    td.debug_name = "sky-transmittance-lut";
    transmittance_lut_ = device_.create_texture(td);
    transmittance_state_ = rhi::ResourceState::Undefined;

    td.extent = {kMultipleScatteringSize, kMultipleScatteringSize};
    td.debug_name = "sky-multiple-scattering-lut";
    multiple_scattering_lut_ = device_.create_texture(td);
    multiple_scattering_state_ = rhi::ResourceState::Undefined;

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
    const RGTexture transmittance_rg =
        graph.import_texture(transmittance_lut_, transmittance_state_);
    const RGTexture multiple_scattering_rg =
        graph.import_texture(multiple_scattering_lut_, multiple_scattering_state_);
    const RGBuffer sh_rg = graph.import_buffer(sh_buffer_, sh_state_);

    const bool transmittance_dirty =
        !has_transmittance_ || !transmittance_inputs_equal(transmittance_baked_, params);
    const bool multiple_scattering_dirty =
        !has_multiple_scattering_ ||
        !multiple_scattering_inputs_equal(multiple_scattering_baked_, params);
    // m17.7d makes this bake sample both tables. Invalidate it from their dirty flags already, so
    // that body replacement cannot turn a physical edit into a stale visible sky or SH buffer.
    // The physical clear-air body fixes its observer height and is camera-independent, but the
    // deliberately cheap cloud slab samples camera x/z.  Do not let a moving camera reuse clouds
    // baked for a different world position while the analytic background visibly scrolls.
    const bool cloud_camera_dirty =
        params.clouds_enabled && (!has_bake_ || inputs.camera_pos.x != baked_inputs_.camera_pos.x ||
                                  inputs.camera_pos.z != baked_inputs_.camera_pos.z);
    const bool lighting_dirty = !has_bake_ || !bake_inputs_equal(baked_, params) ||
                                transmittance_dirty || multiple_scattering_dirty ||
                                cloud_camera_dirty;

    // A physical table is persistent for exactly the same reason as the m17.7b sky-view table:
    // it represents an unchanging medium, and a frame with no relevant edit must not pay its
    // integration again.  The two keys are deliberately separate; a ground-albedo edit leaves
    // transmittance alone, while neither key has an analytic colour/cloud/camera/sun dependency.
    if (!transmittance_dirty) {
        ++atmosphere_stats_.transmittance_reused;
    } else {
        const GpuAtmosphereUniforms u = fill_atmosphere_uniforms(params);
        const RenderGraph::FrameSlice ubo = graph.push_frame_data(&u, sizeof(u));
        const RGTexture writes[] = {transmittance_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.storage_write = writes;
        graph.add_compute_pass(
            "sky-transmittance-lut",
            desc,
            [pipe = transmittance_pipeline_, transmittance_rg, ubo, &graph](
                rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_image(0, graph.physical(transmittance_rg));
                cmd.bind_uniform_buffer(1, ubo.buffer, ubo.offset, sizeof(GpuAtmosphereUniforms));
                cmd.dispatch((kTransmittanceWidth + kTransmittanceGroup - 1) / kTransmittanceGroup,
                             (kTransmittanceHeight + kTransmittanceGroup - 1) / kTransmittanceGroup,
                             1);
            });
        transmittance_baked_ = params;
        has_transmittance_ = true;
        ++atmosphere_stats_.transmittance_filled;
    }

    if (!multiple_scattering_dirty) {
        ++atmosphere_stats_.multiple_scattering_reused;
    } else {
        const GpuAtmosphereUniforms u = fill_atmosphere_uniforms(params);
        const RenderGraph::FrameSlice ubo = graph.push_frame_data(&u, sizeof(u));
        const RGTexture sampled[] = {transmittance_rg};
        const RGTexture writes[] = {multiple_scattering_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.sampled = sampled;
        desc.storage_write = writes;
        graph.add_compute_pass(
            "sky-multiple-scattering-lut",
            desc,
            [pipe = multiple_scattering_pipeline_,
             transmittance_rg,
             multiple_scattering_rg,
             ubo,
             smp = atmosphere_sampler_,
             &graph](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_image(0, graph.physical(multiple_scattering_rg));
                cmd.bind_texture(1, graph.physical(transmittance_rg), smp);
                cmd.bind_uniform_buffer(2, ubo.buffer, ubo.offset, sizeof(GpuAtmosphereUniforms));
                cmd.dispatch((kMultipleScatteringSize + kMultipleScatteringGroup - 1) /
                                 kMultipleScatteringGroup,
                             (kMultipleScatteringSize + kMultipleScatteringGroup - 1) /
                                 kMultipleScatteringGroup,
                             1);
            });
        multiple_scattering_baked_ = params;
        has_multiple_scattering_ = true;
        ++atmosphere_stats_.multiple_scattering_filled;
    }

    // Nothing the lighting bake depends on moved, so last frame's sky-view and SH are still
    // correct.  Physical LUT work above remains live because writes to imported persistent
    // resources are observable even without a sky-view/SH refill this frame.
    if (!lighting_dirty) {
        ++stats_.reused;
        // The multiple-scattering solve samples transmittance even on a frame where sky-view/SH
        // are reused.  Its read therefore owns transmittance's final state; only a lone direct
        // transmittance fill remains in general layout.
        if (multiple_scattering_dirty) {
            transmittance_state_ = rhi::ResourceState::ShaderRead;
            multiple_scattering_state_ = rhi::ResourceState::StorageReadWrite;
        } else if (transmittance_dirty) {
            transmittance_state_ = rhi::ResourceState::StorageReadWrite;
        }
        return SkyLightBinding{lut_rg, sh_rg, lut_sampler_};
    }

    const GpuSkyUniforms u = fill_uniforms(params, inputs);
    const GpuAtmosphereUniforms au = fill_atmosphere_uniforms(params);
    const RenderGraph::FrameSlice ubo = graph.push_frame_data(&u, sizeof(u));
    const RenderGraph::FrameSlice atmosphere_ubo = graph.push_frame_data(&au, sizeof(au));

    {
        const RGTexture sampled[] = {transmittance_rg, multiple_scattering_rg};
        const RGTexture writes[] = {lut_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.sampled = sampled;
        desc.storage_write = writes;
        graph.add_compute_pass(
            "sky-view-lut",
            desc,
            [pipe = skyview_pipeline_,
             lut_rg,
             transmittance_rg,
             multiple_scattering_rg,
             ubo,
             atmosphere_ubo,
             smp = atmosphere_sampler_,
             &graph](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_image(0, graph.physical(lut_rg));
                cmd.bind_uniform_buffer(2, ubo.buffer, ubo.offset, sizeof(GpuSkyUniforms));
                cmd.bind_uniform_buffer(
                    3, atmosphere_ubo.buffer, atmosphere_ubo.offset, sizeof(GpuAtmosphereUniforms));
                cmd.bind_texture(4, graph.physical(transmittance_rg), smp);
                cmd.bind_texture(5, graph.physical(multiple_scattering_rg), smp);
                cmd.dispatch((kSkyViewWidth + kSkyViewGroup - 1) / kSkyViewGroup,
                             (kSkyViewHeight + kSkyViewGroup - 1) / kSkyViewGroup,
                             1);
            });
    }
    {
        // sky_sh.comp integrates the physical body directly rather than inheriting sky-view's
        // angular resolution.  The Fibonacci integration therefore remains independent of LUT
        // texel density while both physical sampled dependencies keep graph ordering explicit.
        const RGTexture sampled[] = {transmittance_rg, multiple_scattering_rg};
        const RGBuffer writes[] = {sh_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.sampled = sampled;
        desc.buffer_writes = writes;
        graph.add_compute_pass(
            "sky-sh",
            desc,
            [pipe = sh_pipeline_,
             sh_rg,
             transmittance_rg,
             multiple_scattering_rg,
             ubo,
             atmosphere_ubo,
             smp = atmosphere_sampler_,
             &graph](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(pipe);
                cmd.bind_storage_buffer(0, graph.physical_buffer(sh_rg));
                cmd.bind_uniform_buffer(2, ubo.buffer, ubo.offset, sizeof(GpuSkyUniforms));
                cmd.bind_uniform_buffer(
                    3, atmosphere_ubo.buffer, atmosphere_ubo.offset, sizeof(GpuAtmosphereUniforms));
                cmd.bind_texture(4, graph.physical(transmittance_rg), smp);
                cmd.bind_texture(5, graph.physical(multiple_scattering_rg), smp);
                cmd.dispatch(1, 1, 1); // one workgroup reduces the whole set
            });
    }

    // What each resource is ACTUALLY left in, which is not the same answer for the two.
    //
    // The SH buffer is written here and READ by the forward pass in this same frame (it is in that
    // pass's buffer_reads), so the graph transitions it and it ends in ShaderRead.
    //
    // The LUT ends in whatever its compute WRITE left it in — unless a consumer samples it later
    // this frame, which moves it to ShaderRead. This class cannot see that, so the consumers
    // report back through note_skyview_state() and SceneRenderer makes the call once both are
    // declared. Guessing here instead is what produced `texture_barrier 'from' disagrees with the
    // tracked layout` twice, first when nothing sampled the LUT and again when SSR started to.
    skyview_state_ = rhi::ResourceState::StorageReadWrite;
    // Both LUTs were sampled by both existing compute passes above, so their final state is known
    // here rather than guessed by a downstream consumer as sky-view's state must be.
    transmittance_state_ = rhi::ResourceState::ShaderRead;
    multiple_scattering_state_ = rhi::ResourceState::ShaderRead;
    sh_state_ = rhi::ResourceState::ShaderRead;
    baked_ = params;
    baked_inputs_ = inputs;
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
