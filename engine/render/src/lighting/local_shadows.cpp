// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Local-light (spot) shadow maps + the destructibility-aware cache (m10.2, ADR-0032 §3 + C1/C2). A
// spot's shadow is a perspective depth render from the light (one layer of a persistent depth
// array), reusing the depth pre-pass exactly as the cascades do. The cache holds each layer's depth
// ACROSS frames and re-renders a slot only when it is invalidated (moved light, or a destruction
// event whose world AABB overlaps the slot's frustum). Derivation: docs/math/shadow-mapping.md
// §6–7.

#include "rime/render/lighting/local_shadows.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace rime::render {

namespace {

// Per-spot slice of the light-view_proj buffer the depth passes read at binding 0 — 256 bytes is
// the universally-valid uniform-offset alignment, as in the cascades' kCascadeStride.
constexpr std::uint32_t kSpotStride = 256;

// The perspective light matrix for a spot: a camera at the light looking down its cone, FOV = the
// full outer cone angle (clamped away from a degenerate ~180°), far = the light's range.
core::Mat4 spot_view_proj(const SpotLightData& s) {
    const core::Vec3 dir = core::normalize(s.direction);
    const core::Vec3 up =
        std::fabs(dir.y) > 0.99f ? core::Vec3{0.0f, 0.0f, 1.0f} : core::Vec3{0.0f, 1.0f, 0.0f};
    const float fov = std::min(2.0f * s.outer_angle, 2.6f); // ≤ ~150°, never a flat frustum
    const float far = std::max(s.range, 0.05f);
    const float near = std::max(far * 0.01f, 0.02f);
    const core::Mat4 view = core::look_at(s.position, s.position + dir, up);
    const core::Mat4 proj = core::perspective(fov, 1.0f, near, far);
    return proj * view;
}

// The world-space AABB of a spot's shadow frustum: the bounds of the 8 unprojected clip-cube
// corners. This is what a destruction region is tested against — a break outside the frustum casts
// no shadow into it, so it cannot invalidate the slot (docs/math/shadow-mapping.md §7).
WorldAabb frustum_bounds(const core::Mat4& view_proj) {
    const core::Mat4 inv = core::inverse(view_proj);
    WorldAabb b;
    b.min = core::Vec3{1e30f, 1e30f, 1e30f};
    b.max = core::Vec3{-1e30f, -1e30f, -1e30f};
    for (int z = 0; z <= 1; ++z) { // Vulkan clip z ∈ [0,1]
        for (int y = -1; y <= 1; y += 2) {
            for (int x = -1; x <= 1; x += 2) {
                const core::Vec4 v = inv * core::Vec4{static_cast<float>(x),
                                                      static_cast<float>(y),
                                                      static_cast<float>(z),
                                                      1.0f};
                const core::Vec3 p{v.x / v.w, v.y / v.w, v.z / v.w};
                b.min = core::Vec3{
                    std::min(b.min.x, p.x), std::min(b.min.y, p.y), std::min(b.min.z, p.z)};
                b.max = core::Vec3{
                    std::max(b.max.x, p.x), std::max(b.max.y, p.y), std::max(b.max.z, p.z)};
            }
        }
    }
    return b;
}

// Has the light in this slot moved or retargeted since we cached it? A small epsilon on position,
// direction and the cone/range — any real edit trips it and forces a re-render.
bool light_changed(const SpotLightData& a, const SpotLightData& b) {
    const float eps = 1e-4f;
    return core::length(a.position - b.position) > eps ||
           core::length(a.direction - b.direction) > eps || std::fabs(a.range - b.range) > eps ||
           std::fabs(a.cos_inner - b.cos_inner) > eps || std::fabs(a.cos_outer - b.cos_outer) > eps;
}

} // namespace

LocalShadowMap::LocalShadowMap(rhi::Device& device) : device_(device) {
    // The same depth-compare sampler the cascades use: LessEqual, linear (so one fetch is already
    // 2×2 PCF), ClampToEdge (a receiver off the map reads the border, never wraps).
    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Linear;
    sd.min_filter = rhi::Filter::Linear;
    sd.address_mode = rhi::AddressMode::ClampToEdge;
    sd.compare_enable = true;
    sd.compare_op = rhi::CompareOp::LessEqual;
    sd.debug_name = "local-shadow-compare-sampler";
    compare_sampler_ = device.create_sampler(sd);

    rhi::BufferDesc vp{};
    vp.size = static_cast<std::uint64_t>(kMaxLocalShadows) * kSpotStride;
    vp.usage = rhi::BufferUsage::Uniform;
    vp.memory = rhi::MemoryUsage::CpuToGpu;
    vp.debug_name = "local-shadow-viewproj";
    spot_vp_ubo_ = device.create_buffer(vp);

    rhi::BufferDesc lu{};
    lu.size = sizeof(GpuLocalShadows);
    lu.usage = rhi::BufferUsage::Uniform;
    lu.memory = rhi::MemoryUsage::CpuToGpu;
    lu.debug_name = "local-shadow-ubo";
    local_ubo_ = device.create_buffer(lu);

    slots_.resize(kMaxLocalShadows);
}

LocalShadowMap::~LocalShadowMap() {
    if (depth_array_.is_valid())
        device_.destroy(depth_array_);
    device_.destroy(local_ubo_);
    device_.destroy(spot_vp_ubo_);
    device_.destroy(compare_sampler_);
}

void LocalShadowMap::ensure_array(std::uint32_t resolution) {
    if (depth_array_.is_valid() && resolution_ == resolution)
        return;
    // A resolution change (or first use) means the cached depth is worthless — drop it and force
    // every slot to re-render on the next add().
    if (depth_array_.is_valid())
        device_.destroy(depth_array_);
    rhi::TextureDesc td{};
    td.extent = {resolution, resolution};
    td.array_layers = kMaxLocalShadows;
    td.format = kDepthFormat;
    td.usage = rhi::TextureUsage::DepthStencil | rhi::TextureUsage::Sampled;
    td.debug_name = "local-shadow-depth-array";
    depth_array_ = device_.create_texture(td);
    resolution_ = resolution;
    array_state_ =
        rhi::ResourceState::Undefined; // fresh image: import discards its (absent) contents
    for (Slot& s : slots_)
        s.rendered = false;
}

LocalShadowBinding LocalShadowMap::add(RenderGraph& graph,
                                       const DepthPrepass& prepass,
                                       const SceneDrawData& scene_data,
                                       std::span<const SpotLightData> spots,
                                       const LightingSettings& settings) {
    ensure_array(settings.local_shadow_resolution);
    const auto count =
        static_cast<std::uint32_t>(std::min<std::size_t>(spots.size(), kMaxLocalShadows));

    stats_ = LocalShadowStats{};

    // Decide per slot: build the light matrix + frustum, detect whether it must be re-rendered, and
    // pack the GPU spot record (uploaded every frame — the forward pass needs all spots' data even
    // when their depth is cached).
    std::array<std::byte, static_cast<std::size_t>(kMaxLocalShadows) * kSpotStride> vp_staging{};
    GpuLocalShadows lu{};
    std::array<bool, kMaxLocalShadows> render_slot{};
    for (std::uint32_t i = 0; i < count; ++i) {
        const SpotLightData& s = spots[i];
        const core::Mat4 vp = spot_view_proj(s);
        const bool changed =
            !slots_[i].rendered || slots_[i].dirty || light_changed(slots_[i].light, s);
        render_slot[i] = changed;
        if (changed) {
            slots_[i].light = s;
            slots_[i].bounds = frustum_bounds(vp);
            slots_[i].rendered = true;
            slots_[i].dirty = false;
            ++stats_.rendered;
        } else {
            ++stats_.reused;
        }

        std::memcpy(&vp_staging[static_cast<std::size_t>(i) * kSpotStride], &vp, sizeof(vp));
        GpuSpotShadow& g = lu.spots[i];
        g.view_proj = vp;
        g.pos_range[0] = s.position.x;
        g.pos_range[1] = s.position.y;
        g.pos_range[2] = s.position.z;
        g.pos_range[3] = s.range;
        const core::Vec3 d = core::normalize(s.direction);
        g.dir_cos_inner[0] = d.x;
        g.dir_cos_inner[1] = d.y;
        g.dir_cos_inner[2] = d.z;
        g.dir_cos_inner[3] = s.cos_inner;
        g.radiance_cos_outer[0] = s.radiance[0];
        g.radiance_cos_outer[1] = s.radiance[1];
        g.radiance_cos_outer[2] = s.radiance[2];
        g.radiance_cos_outer[3] = s.cos_outer;
    }
    // Slots past the live spot count fall out of use — free them so a future spot at that index
    // re-renders instead of trusting stale cached depth.
    for (std::uint32_t i = count; i < kMaxLocalShadows; ++i)
        slots_[i].rendered = false;

    device_.write_buffer(spot_vp_ubo_, vp_staging.data(), vp_staging.size());
    lu.params[0] = static_cast<float>(count);
    lu.params[1] = settings.shadow_pcf_radius;
    lu.params[2] = settings.shadow_depth_bias;
    lu.params[3] = settings.shadow_normal_bias;
    lu.texel[0] = 1.0f / static_cast<float>(settings.local_shadow_resolution);
    device_.write_buffer(local_ubo_, &lu, sizeof(lu));

    // Import the PERSISTENT array (not a transient): its depth from earlier frames is the cache.
    // Only the invalidated slots get a depth pass this frame; the rest keep what they already hold.
    // The extent/layers let the graph aim each depth pass's viewport at the right layer.
    const RGTexture map = graph.import_texture(
        depth_array_, array_state_, {resolution_, resolution_}, kDepthFormat, kMaxLocalShadows);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!render_slot[i])
            continue;
        SceneDrawData spot_data = scene_data;
        spot_data.frame_ubo = spot_vp_ubo_;
        spot_data.frame_ubo_offset = i * kSpotStride;
        prepass.add(graph, map, spot_data, i);
    }
    // After this frame the forward pass samples the array, leaving it in ShaderRead — the state the
    // next frame imports it at, so the cached depth survives with no redundant transition.
    array_state_ = rhi::ResourceState::ShaderRead;

    return LocalShadowBinding{map, local_ubo_, compare_sampler_};
}

LocalShadowBinding LocalShadowMap::empty_binding(RenderGraph& graph, rhi::TextureHandle dummy) {
    // No spots: a count-0 uniform block so the shader's spot loop never runs, and the shared dummy
    // depth array so binding 9 still points at a real 2-D-array image. Import at Undefined — its
    // contents are never sampled, so the graph may discard whatever layout it was in.
    GpuLocalShadows lu{}; // params[0] (count) defaults to 0
    device_.write_buffer(local_ubo_, &lu, sizeof(lu));
    // The dummy sits permanently in ShaderRead (the SceneRenderer transitions it once), so
    // importing it there — not at Undefined — keeps the graph's barrier bookkeeping in step with
    // the backend.
    const RGTexture map =
        graph.import_texture(dummy, rhi::ResourceState::ShaderRead, {1, 1}, kDepthFormat, 2);
    return LocalShadowBinding{map, local_ubo_, compare_sampler_};
}

void LocalShadowMap::invalidate(const WorldAabb& region) {
    // The C2 destruction hook: any in-use slot whose shadow frustum the break touches is now stale.
    // Conservative by design (the frustum AABB over-covers the cone) — the ADR's "err broad" rule.
    for (Slot& s : slots_) {
        if (s.rendered && s.bounds.overlaps(region))
            s.dirty = true;
    }
}

} // namespace rime::render
