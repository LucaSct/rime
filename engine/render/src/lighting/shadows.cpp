// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Directional cascaded shadow maps (m10.1, ADR-0032 §3). compute_cascades fits the light frustums
// (GPU-free, unit-tested); CascadedShadowMap owns the GPU resources and declares the per-cascade
// depth passes each frame, reusing the depth pre-pass aimed from the light. The math — practical
// splits, the rotation-invariant bounding sphere, texel snapping — is derived in
// docs/math/shadow-mapping.md §2–3.

#include "rime/render/lighting/shadows.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace rime::render {

namespace {

// Per-cascade slice of the light-view_proj buffer the depth passes read at binding 0. 256 bytes is
// the universally-valid uniform-offset alignment (as in kDrawUniformStride); depth_only.vert reads
// only the leading mat4.
constexpr std::uint32_t kCascadeStride = 256;

} // namespace

CascadeFit compute_cascades(const CascadeInputs& in, const LightingSettings& settings) {
    CascadeFit fit;
    const std::uint32_t n = std::clamp<std::uint32_t>(settings.cascade_count, 1, kMaxCascades);
    fit.count = n;
    const float lambda = std::clamp(settings.cascade_split_lambda, 0.0f, 1.0f);
    const core::Vec3 light_dir = core::normalize(in.light_dir);
    // An up vector that is never (near-)parallel to the light — otherwise the light basis is
    // degenerate. Swap to +Z when the sun is (near) straight up or down.
    const core::Vec3 up = std::fabs(light_dir.y) > 0.99f ? core::Vec3{0.0f, 0.0f, 1.0f}
                                                         : core::Vec3{0.0f, 1.0f, 0.0f};

    // Practical-split distances: blend a logarithmic split (even in perspective, good near) with a
    // uniform one (even in world, good far). splits[0] = near .. splits[n] = far.
    std::array<float, kMaxCascades + 1> splits{};
    splits[0] = in.z_near;
    for (std::uint32_t i = 1; i <= n; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(n);
        const float log_d = in.z_near * std::pow(in.z_far / in.z_near, p);
        const float uni_d = in.z_near + (in.z_far - in.z_near) * p;
        splits[i] = lambda * log_d + (1.0f - lambda) * uni_d;
    }

    // The rotation-only light view (eye at the origin): its xy plane is the shadow map's plane,
    // used to snap the cascade centre to whole texels.
    const core::Mat4 light_rot = core::look_at(core::Vec3{0.0f, 0.0f, 0.0f}, light_dir, up);
    const core::Mat4 light_rot_inv = core::inverse(light_rot);

    for (std::uint32_t c = 0; c < n; ++c) {
        // World-space corners of this depth slice: unproject the 8 clip-cube corners of a frustum
        // built for [splits[c], splits[c+1]].
        const core::Mat4 proj = core::perspective(in.fov_y, in.aspect, splits[c], splits[c + 1]);
        const core::Mat4 inv_view_proj = core::inverse(proj * in.camera_view);
        std::array<core::Vec3, 8> corners{};
        int k = 0;
        for (int z = 0; z <= 1; ++z) { // Vulkan clip z ∈ [0,1]
            for (int y = -1; y <= 1; y += 2) {
                for (int x = -1; x <= 1; x += 2) {
                    const core::Vec4 v = inv_view_proj * core::Vec4{static_cast<float>(x),
                                                                    static_cast<float>(y),
                                                                    static_cast<float>(z),
                                                                    1.0f};
                    corners[k++] = core::Vec3{v.x / v.w, v.y / v.w, v.z / v.w};
                }
            }
        }

        // Bounding sphere: rotation-invariant, so the radius (and thus the texel size) is stable
        // frame to frame no matter how the camera turns — the fix for rotation shimmer.
        core::Vec3 center{0.0f, 0.0f, 0.0f};
        for (const core::Vec3& p : corners) {
            center += p;
        }
        center = center * (1.0f / 8.0f);
        float radius = 0.0f;
        for (const core::Vec3& p : corners) {
            radius = std::max(radius, core::length(p - center));
        }
        radius = std::max(radius, 1e-3f);

        // Texel-snap the centre in light space: as the camera translates, the snapped centre jumps
        // in whole-texel steps, so the shadow's texel grid stays world-aligned — the fix for
        // translation shimmer (docs/math/shadow-mapping.md §3).
        core::Vec3 center_ls = core::transform_point(light_rot, center);
        const float units_per_texel =
            (2.0f * radius) / static_cast<float>(settings.shadow_map_resolution);
        center_ls.x = std::floor(center_ls.x / units_per_texel) * units_per_texel;
        center_ls.y = std::floor(center_ls.y / units_per_texel) * units_per_texel;
        const core::Vec3 snapped = core::transform_point(light_rot_inv, center_ls);

        // The light "camera": pulled back so its near plane (0) sits behind the slice, capturing
        // occluders between the sun and the slice. far reaches the slice's far side.
        const float z_extra = radius; // room for off-slice casters
        const core::Vec3 eye = snapped - light_dir * (radius + z_extra);
        const core::Mat4 light_view = core::look_at(eye, snapped, up);
        const core::Mat4 light_proj =
            core::ortho(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + z_extra);
        fit.view_proj[c] = light_proj * light_view;
    }
    return fit;
}

CascadedShadowMap::CascadedShadowMap(rhi::Device& device) : device_(device) {
    // The depth-compare sampler (m10.1a): LessEqual with linear filtering, so a single texture()
    // fetch is already the bilinear-weighted 2×2 PCF; the shader's 3×3 loop widens it. ClampToEdge:
    // a receiver outside the map reads the border, never wraps a shadow across the scene.
    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Linear;
    sd.min_filter = rhi::Filter::Linear;
    sd.address_mode = rhi::AddressMode::ClampToEdge;
    sd.compare_enable = true;
    sd.compare_op = rhi::CompareOp::LessEqual;
    sd.debug_name = "csm-compare-sampler";
    compare_sampler_ = device.create_sampler(sd);

    rhi::BufferDesc vp{};
    vp.size = static_cast<std::uint64_t>(kMaxCascades) * kCascadeStride;
    vp.usage = rhi::BufferUsage::Uniform;
    vp.memory = rhi::MemoryUsage::CpuToGpu;
    vp.debug_name = "csm-cascade-viewproj";
    cascade_vp_ubo_ = device.create_buffer(vp);

    rhi::BufferDesc su{};
    su.size = sizeof(GpuShadowUniforms);
    su.usage = rhi::BufferUsage::Uniform;
    su.memory = rhi::MemoryUsage::CpuToGpu;
    su.debug_name = "csm-shadow-ubo";
    shadow_ubo_ = device.create_buffer(su);
}

CascadedShadowMap::~CascadedShadowMap() {
    device_.destroy(shadow_ubo_);
    device_.destroy(cascade_vp_ubo_);
    device_.destroy(compare_sampler_);
}

ShadowBinding CascadedShadowMap::add(RenderGraph& graph,
                                     const DepthPrepass& prepass,
                                     const SceneDrawData& scene_data,
                                     const CascadeInputs& inputs,
                                     const LightingSettings& settings) {
    resolution_ = settings.shadow_map_resolution;
    const CascadeFit fit = compute_cascades(inputs, settings);

    // Upload each cascade's light view_proj into its 256-byte slice (the depth passes read binding
    // 0 at frame_ubo_offset = c * stride), and fill the ShadowUniforms the forward pass samples
    // with.
    std::array<std::byte, static_cast<std::size_t>(kMaxCascades) * kCascadeStride> vp_staging{};
    GpuShadowUniforms su{};
    for (std::uint32_t c = 0; c < fit.count; ++c) {
        std::memcpy(&vp_staging[static_cast<std::size_t>(c) * kCascadeStride],
                    &fit.view_proj[c],
                    sizeof(core::Mat4));
        su.cascade_view_proj[c] = fit.view_proj[c];
    }
    device_.write_buffer(cascade_vp_ubo_, vp_staging.data(), vp_staging.size());

    su.params[0] = static_cast<float>(fit.count);
    su.params[1] = settings.shadow_pcf_radius;
    su.params[2] = settings.shadow_depth_bias;
    su.params[3] = settings.shadow_normal_bias;
    su.texel[0] = 1.0f / static_cast<float>(settings.shadow_map_resolution);
    device_.write_buffer(shadow_ubo_, &su, sizeof(su));

    // The cascade depth array — one layered transient (m10.1a): DepthStencil (rendered per cascade)
    // + Sampled (the forward pass reads it), both accumulated by the graph from the declarations.
    const rhi::Extent2D res{settings.shadow_map_resolution, settings.shadow_map_resolution};
    const RGTexture cascades = graph.create_texture({res, kDepthFormat, "csm-cascades", fit.count});

    // One depth pass per cascade, reusing the pre-pass verbatim but aimed from the light: the same
    // draw list, binding 0 pointed at cascade c's view_proj slice, rendering into layer c.
    for (std::uint32_t c = 0; c < fit.count; ++c) {
        SceneDrawData cascade_data = scene_data;
        cascade_data.frame_ubo = cascade_vp_ubo_;
        cascade_data.frame_ubo_offset = c * kCascadeStride;
        prepass.add(graph, cascades, cascade_data, c);
    }

    return ShadowBinding{cascades, shadow_ubo_, compare_sampler_};
}

ShadowBinding CascadedShadowMap::empty_binding(RenderGraph& graph, rhi::TextureHandle dummy) {
    // No sun: a count-0 ShadowUniforms so sun_shadow() short-circuits to "lit", and the shared
    // dummy depth array so binding 7 still points at a real 2-D-array image. Import at Undefined —
    // never sampled, so the graph may discard whatever layout it was in.
    GpuShadowUniforms su{}; // params[0] (cascade count) defaults to 0
    device_.write_buffer(shadow_ubo_, &su, sizeof(su));
    // The dummy sits permanently in ShaderRead (the SceneRenderer transitions it once) — import it
    // there so the graph's barrier bookkeeping matches the backend's tracked layout.
    const RGTexture map =
        graph.import_texture(dummy, rhi::ResourceState::ShaderRead, {1, 1}, kDepthFormat, 2);
    return ShadowBinding{map, shadow_ubo_, compare_sampler_};
}

} // namespace rime::render
