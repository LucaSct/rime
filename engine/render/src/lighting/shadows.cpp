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
#include <string_view>
#include <utility>

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
    //
    // The far end is the SHADOW range, not necessarily the camera's: a terrain view may draw 3 km
    // but the cascades would then spend 2048 texels on a ~1 km first slice. A valid cap only ever
    // shortens the range; anything unusable (unset, <= near, NaN, inf) leaves the camera's z_far.
    const float cap = settings.shadow_max_distance;
    const float far_d =
        (std::isfinite(cap) && cap > in.z_near) ? std::min(in.z_far, cap) : in.z_far;
    std::array<float, kMaxCascades + 1> splits{};
    splits[0] = in.z_near;
    for (std::uint32_t i = 1; i <= n; ++i) {
        const float p = static_cast<float>(i) / static_cast<float>(n);
        const float log_d = in.z_near * std::pow(far_d / in.z_near, p);
        const float uni_d = in.z_near + (far_d - in.z_near) * p;
        splits[i] = lambda * log_d + (1.0f - lambda) * uni_d;
    }

    // The rotation-only light view (eye at the origin). Its rotation is the ONE fixed light basis
    // every cascade and every frame reuses: only the translation column changes per cascade, and it
    // is written explicitly below. We never rebuild a look_at(eye, snapped) from translated floats,
    // because look_at re-normalises (eye->target) and so re-derives the basis from the very
    // coordinates we just snapped, injecting world-magnitude round-off into the texel phase.
    const core::Mat4 light_rot = core::look_at(core::Vec3{0.0f, 0.0f, 0.0f}, light_dir, up);
    const core::Mat4 inv_camera_view = core::inverse(in.camera_view);
    const float tan_half_y = std::tan(in.fov_y * 0.5f);
    const float res = static_cast<float>(settings.shadow_map_resolution);

    for (std::uint32_t c = 0; c < n; ++c) {
        // Local fit (camera space, analytic). The slice [d0, d1] is a frustum along -Z whose
        // geometry depends only on fov/aspect/splits — NOT on where the camera is or where it
        // looks. So we build its bounding sphere in camera space from those numbers alone: centre
        // on the axis at the mean depth, radius to a far corner. (The old fit unprojected 8 corners
        // through inverse(proj * view); at world coordinates ~100 the inverse's round-off made the
        // sphere radius, hence the texel size, wobble from frame to frame — a shimmer source that
        // snapping cannot remove, since the snap quantum itself kept moving.) Only the centre
        // is then carried into the world, through the camera's rigid inverse.
        const float d0 = splits[c];
        const float d1 = splits[c + 1];
        const float half_h = d1 * tan_half_y;
        const float half_w = half_h * in.aspect;
        const float half_d = 0.5f * (d1 - d0);
        float radius = std::sqrt(half_w * half_w + half_h * half_h + half_d * half_d);
        const core::Vec3 center =
            core::transform_point(inv_camera_view, core::Vec3{0.0f, 0.0f, -0.5f * (d0 + d1)});

        // Texel size is quantised by the radius, and a nearest-texel snap moves the centre by up to
        // half a texel, so grow the radius by N/(N-1): texel = 2r/N = 2 r0/(N-1), half a texel =
        // r0/(N-1), and r0 + r0/(N-1) = r0*N/(N-1) — the guarded sphere still holds the whole
        // slice after the snap. N <= 1 has no room for a guard (and no texel grid worth keeping),
        // so it simply skips snapping instead of dividing by zero.
        const bool snap = settings.shadow_map_resolution > 1;
        if (snap) {
            radius *= res / (res - 1.0f);
        }
        radius = std::max(radius, 1e-3f);

        // Texel-snap the centre in light space: as the camera translates, the snapped centre jumps
        // in whole-texel steps, so the shadow's texel grid stays world-aligned — the fix for
        // translation shimmer (docs/math/shadow-mapping.md §3).
        core::Vec3 center_ls = core::transform_point(light_rot, center);
        if (snap) {
            const float units_per_texel = (2.0f * radius) / res;
            center_ls.x = std::round(center_ls.x / units_per_texel) * units_per_texel;
            center_ls.y = std::round(center_ls.y / units_per_texel) * units_per_texel;
        }

        // The light "camera": pulled back so its near plane (0) sits behind the slice, capturing
        // occluders between the sun and the slice. far reaches the slice's far side. The eye is
        // centre - dir*(2r) and the basis maps dir to light-space -Z, so in light space the eye is
        // centre_ls + (0,0,2r): the view is the fixed rotation with this exact translation.
        const float z_extra = radius; // room for off-slice casters
        core::Mat4 light_view = light_rot;
        light_view.at(0, 3) = -center_ls.x;
        light_view.at(1, 3) = -center_ls.y;
        light_view.at(2, 3) = -(center_ls.z + radius + z_extra);
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
}

CascadedShadowMap::~CascadedShadowMap() {
    device_.destroy(compare_sampler_);
}

ShadowBinding CascadedShadowMap::add(RenderGraph& graph,
                                     const DepthPrepass& prepass,
                                     const SceneDrawData& scene_data,
                                     const CascadeInputs& inputs,
                                     const LightingSettings& settings) {
    caster_stats_ = std::make_shared<std::array<CullStats, kMaxCascades>>();
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
    // This frame's scratch, not a buffer this pass owns (m17.4). The whole strided array is one
    // push, so a cascade's own 256-byte slice is still just an offset within it — the slice's base
    // simply moves each frame instead of being overwritten under the GPU.
    const RenderGraph::FrameSlice vp_slice =
        graph.push_frame_data(vp_staging.data(), vp_staging.size());

    su.params[0] = static_cast<float>(fit.count);
    su.params[1] = settings.shadow_pcf_radius;
    su.params[2] = settings.shadow_depth_bias;
    su.params[3] = settings.shadow_normal_bias;
    su.texel[0] = 1.0f / static_cast<float>(settings.shadow_map_resolution);
    const RenderGraph::FrameSlice su_slice = graph.push_frame_data(&su, sizeof(su));

    // The cascade depth array — one layered transient (m10.1a): DepthStencil (rendered per cascade)
    // + Sampled (the forward pass reads it), both accumulated by the graph from the declarations.
    const rhi::Extent2D res{settings.shadow_map_resolution, settings.shadow_map_resolution};
    //
    // array_view = true: pbr_forward_shadowed.frag samples this as `sampler2DArrayShadow` for every
    // cascade count, so a one-cascade atlas (the low-end preset) must still get a 2-D-ARRAY view,
    // not the plain 2-D view a 1-layer image would otherwise be given (VUID-...-viewType-07752).
    const RGTexture cascades =
        graph.create_texture({res, kDepthFormat, "csm-cascades", fit.count, /*array_view=*/true});

    // One depth pass per cascade, reusing the pre-pass verbatim but aimed from the light: the same
    // draw list, binding 0 pointed at cascade c's view_proj slice, rendering into layer c.
    //
    // Each cascade names itself (m17.3). Before this, all four declared "depth-prepass" alongside
    // the camera's, so the perf report folded five different pieces of work into one distribution
    // and NO row in a committed report was named for shadows at all — in the milestone that has to
    // decide what shadows cost. A fixed table rather than a formatted string: kMaxCascades is 4 and
    // this runs every frame, so the names are literals and cost nothing.
    static constexpr std::array<std::string_view, kMaxCascades> kCascadeLabels{
        "csm-cascade-0", "csm-cascade-1", "csm-cascade-2", "csm-cascade-3"};
    static_assert(kCascadeLabels.back() != std::string_view{},
                  "raising kMaxCascades without naming the new cascade leaves it unnamed, and an "
                  "empty name collides with the next one");
    for (std::uint32_t c = 0; c < fit.count; ++c) {
        SceneDrawData cascade_data = scene_data;
        // This cascade's slice, not the whole block.
        cascade_data.bind_frame_ubo(
            vp_slice.buffer, vp_slice.offset + c * kCascadeStride, kCascadeStride);
        const std::optional<Frustum> caster_frustum =
            settings.shadow_caster_culling
                ? std::optional<Frustum>{frustum_from_view_proj(fit.view_proj[c])}
                : std::nullopt;
        // Aliasing ownership pins this frame's array through deferred graph execution.
        std::shared_ptr<CullStats> counter(caster_stats_, &(*caster_stats_)[c]);
        prepass.add(graph,
                    cascades,
                    cascade_data,
                    c,
                    kCascadeLabels[c],
                    caster_frustum,
                    std::move(counter));
    }

    return ShadowBinding{cascades, su_slice, compare_sampler_};
}

ShadowBinding CascadedShadowMap::empty_binding(RenderGraph& graph, rhi::TextureHandle dummy) {
    // No sun: a count-0 ShadowUniforms so sun_shadow() short-circuits to "lit", and the shared
    // dummy depth array so binding 7 still points at a real 2-D-array image. Import at Undefined —
    // never sampled, so the graph may discard whatever layout it was in.
    GpuShadowUniforms su{}; // params[0] (cascade count) defaults to 0
    const RenderGraph::FrameSlice su_slice = graph.push_frame_data(&su, sizeof(su));
    // The dummy sits permanently in ShaderRead (the SceneRenderer transitions it once) — import it
    // there so the graph's barrier bookkeeping matches the backend's tracked layout.
    const RGTexture map =
        graph.import_texture(dummy, rhi::ResourceState::ShaderRead, {1, 1}, kDepthFormat, 2);
    return ShadowBinding{map, su_slice, compare_sampler_};
}

} // namespace rime::render
