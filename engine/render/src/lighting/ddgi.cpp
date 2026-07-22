// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// DDGI probes, CPU half (m10.5a, ADR-0032 §2). See the header for the technique and the honest
// list of what this brick does not do; docs/math/ddgi.md for the full derivation. This file is:
// the octahedral-mapping / Fibonacci / lattice-indexing math (mirrored by the GLSL in
// ddgi_trace.comp / ddgi_blend_irradiance.comp / ddgi_blend_visibility.comp), the lattice
// snap-and-round-robin bookkeeping (the SdfClipmap::add() pattern, applied to probes instead of
// clipmap levels), and the three compute-pass declarations.

#include "rime/render/lighting/ddgi.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ddgi_blend_irradiance.comp.spv.h"
#include "ddgi_blend_visibility.comp.spv.h"
#include "ddgi_trace.comp.spv.h"
#include "rime/core/math/quat.hpp"

namespace rime::render {

namespace {

constexpr float kPi = 3.14159265358979f;

// Must equal ddgi_trace.comp's local_size_x (the GLSL-reserved-word trap bit m10.3's
// cluster_cull.comp — spelled out here rather than left as a bare number at the call site).
constexpr std::uint32_t kTraceGroupSize = 64;

// ── Destruction-reactive hysteresis (m10.5b, docs/math/ddgi.md §8/§11) ──────────────────────────
// A probe invalidate() has touched rides this MUCH lower hysteresis for its next few updates
// instead of the steady-state settings.ddgi_hysteresis (0.97 by default). Halving the stored
// value's remaining influence every update (rather than keeping ~97% of it) is what turns a
// ~30-frame time constant into a handful of frames: after kFastTrackUpdates updates, the
// pre-invalidation history's weight has decayed to kFastTrackHysteresis^kFastTrackUpdates ≈
// 0.5^5 ≈ 3% of its original value — "materially converged," not merely "started to move." Both
// numbers are deliberately conservative (a bigger drop, or more updates, converges faster still)
// rather than tuned to a razor's edge; docs/math/ddgi.md §8 derives the decay this predicts and
// tests/render/ddgi_test.cpp checks it against that prediction the same way m10.5a's own
// convergence test does.
constexpr float kFastTrackHysteresis = 0.5f;
constexpr std::uint32_t kFastTrackUpdates = 5;

float sign_not_zero(float v) noexcept {
    return v >= 0.0f ? 1.0f : -1.0f;
}

} // namespace

// ── Octahedral mapping (Cigolle/Meyer et al.; docs/math/ddgi.md §3) ────────────────────────────
core::Vec2 ddgi_oct_encode(core::Vec3 dir) noexcept {
    const float denom = std::fabs(dir.x) + std::fabs(dir.y) + std::fabs(dir.z);
    const float inv = denom > 1.0e-8f ? 1.0f / denom : 0.0f;
    core::Vec2 p{dir.x * inv, dir.y * inv};
    if (dir.z <= 0.0f) {
        p = core::Vec2{(1.0f - std::fabs(p.y)) * sign_not_zero(p.x),
                       (1.0f - std::fabs(p.x)) * sign_not_zero(p.y)};
    }
    return p;
}

core::Vec3 ddgi_oct_decode(core::Vec2 uv) noexcept {
    core::Vec3 v{uv.x, uv.y, 1.0f - std::fabs(uv.x) - std::fabs(uv.y)};
    if (v.z < 0.0f) {
        const float x = (1.0f - std::fabs(v.y)) * sign_not_zero(v.x);
        const float y = (1.0f - std::fabs(v.x)) * sign_not_zero(v.y);
        v.x = x;
        v.y = y;
    }
    return core::normalize(v);
}

core::Vec3 ddgi_fibonacci_direction(std::uint32_t i, std::uint32_t n) noexcept {
    const float golden = kPi * (3.0f - std::sqrt(5.0f));
    const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const float theta = golden * static_cast<float>(i);
    return core::Vec3{std::cos(theta) * r, y, std::sin(theta) * r};
}

DdgiProbeCoord ddgi_probe_coord(std::uint32_t global_index,
                                std::uint32_t count_x,
                                std::uint32_t count_y) noexcept {
    DdgiProbeCoord c;
    c.x = global_index % count_x;
    c.y = (global_index / count_x) % count_y;
    c.z = global_index / (count_x * count_y);
    return c;
}

core::Vec3 ddgi_probe_position(std::uint32_t global_index,
                               core::Vec3 origin,
                               float spacing,
                               std::uint32_t count_x,
                               std::uint32_t count_y) noexcept {
    const DdgiProbeCoord c = ddgi_probe_coord(global_index, count_x, count_y);
    return core::Vec3{origin.x + static_cast<float>(c.x) * spacing,
                      origin.y + static_cast<float>(c.y) * spacing,
                      origin.z + static_cast<float>(c.z) * spacing};
}

DdgiAtlasTile ddgi_atlas_tile(std::uint32_t global_index, std::uint32_t probes_per_row) noexcept {
    DdgiAtlasTile t;
    t.col = global_index % probes_per_row;
    t.row = global_index / probes_per_row;
    return t;
}

// ── DdgiProbes ───────────────────────────────────────────────────────────────────────────────────

DdgiProbes::DdgiProbes(rhi::Device& device) : device_(device) {
    // Trace: sphere-trace + shade a batch of (probe, ray) pairs, write one GpuDdgiRay each.
    {
        rhi::ShaderDesc sd{};
        sd.stage = rhi::ShaderStage::Compute;
        sd.spirv = ddgi_trace_comp_spv;
        sd.spirv_size_bytes = sizeof(ddgi_trace_comp_spv);
        sd.debug_name = "ddgi_trace.comp";
        trace_shader_ = device.create_shader(sd);

        const rhi::BindingDesc bindings[] = {
            {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute}, // clipmap level 0
            {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute}, // clipmap level 1
            {2, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute}, // clipmap level 2
            {3, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},        // clipmap Levels
            {4, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},        // DdgiParams
            {5, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},        // out Rays
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = trace_shader_;
        pd.bindings = bindings;
        pd.debug_name = "ddgi-trace";
        trace_pipeline_ = device.create_compute_pipeline(pd);
    }

    // Blend-irradiance: gather this update's rays into the octahedral irradiance atlas.
    {
        rhi::ShaderDesc sd{};
        sd.stage = rhi::ShaderStage::Compute;
        sd.spirv = ddgi_blend_irradiance_comp_spv;
        sd.spirv_size_bytes = sizeof(ddgi_blend_irradiance_comp_spv);
        sd.debug_name = "ddgi_blend_irradiance.comp";
        blend_irradiance_shader_ = device.create_shader(sd);

        const rhi::BindingDesc bindings[] = {
            {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // Rays (read)
            {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute}, // DdgiBlendParams
            {2, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute}, // Hysteresis (read)
            {3, rhi::BindingType::StorageImage, rhi::StageMask::Compute},  // irradiance atlas (r+w)
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = blend_irradiance_shader_;
        pd.bindings = bindings;
        pd.debug_name = "ddgi-blend-irradiance";
        blend_irradiance_pipeline_ = device.create_compute_pipeline(pd);
    }

    // Blend-visibility: the identical shape, gathering (distance, distance^2) instead of radiance.
    {
        rhi::ShaderDesc sd{};
        sd.stage = rhi::ShaderStage::Compute;
        sd.spirv = ddgi_blend_visibility_comp_spv;
        sd.spirv_size_bytes = sizeof(ddgi_blend_visibility_comp_spv);
        sd.debug_name = "ddgi_blend_visibility.comp";
        blend_visibility_shader_ = device.create_shader(sd);

        const rhi::BindingDesc bindings[] = {
            {0, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
            {1, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},
            {2, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
            {3, rhi::BindingType::StorageImage, rhi::StageMask::Compute}, // visibility atlas (r+w)
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = blend_visibility_shader_;
        pd.bindings = bindings;
        pd.debug_name = "ddgi-blend-visibility";
        blend_visibility_pipeline_ = device.create_compute_pipeline(pd);
    }

    rhi::SamplerDesc smd{};
    smd.mag_filter = rhi::Filter::Linear;
    smd.min_filter = rhi::Filter::Linear;
    smd.address_mode = rhi::AddressMode::ClampToEdge;
    smd.debug_name = "ddgi-clipmap-sampler";
    clipmap_sampler_ = device.create_sampler(smd);

    // m10.5b: the forward pass's own sampler for BOTH atlases — Linear is what makes the border
    // ring (ddgi.hpp's kDdgiTileBorder) actually fix the octahedral seam for a sampled fragment,
    // exactly as it does for the trace pass's clipmap_sampler_ above. Confirmed on lavapipe that
    // RG32Float (the visibility atlas's format) reports FILTER_LINEAR support identically to
    // RGBA16Float — not a given for a 32-bit-per-channel format on real hardware, but true here.
    rhi::SamplerDesc asd{};
    asd.mag_filter = rhi::Filter::Linear;
    asd.min_filter = rhi::Filter::Linear;
    asd.address_mode = rhi::AddressMode::ClampToEdge;
    asd.debug_name = "ddgi-atlas-sampler";
    atlas_sampler_ = device.create_sampler(asd);

    // The DDGI-off placeholder pair: 1x1, format-matched to the real atlases so the shadowed
    // pipeline's fixed sampler2D bindings are always satisfied regardless of whether DDGI ever
    // ran. Zero-initialized and left in ShaderRead by write_texture's own post-condition (the same
    // reasoning ensure_atlases uses below for the real atlases' first clear) — never sampled for
    // real content (the shader's `enabled` flag gates that), so their exact value never matters.
    {
        rhi::TextureDesc did{};
        did.extent = {1, 1};
        did.format = rhi::Format::RGBA16Float;
        did.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        did.debug_name = "ddgi-dummy-irradiance";
        dummy_irradiance_ = device.create_texture(did);
        const std::uint16_t zero_half4[4] = {0, 0, 0, 0};
        device.write_texture(dummy_irradiance_, zero_half4, sizeof(zero_half4));

        rhi::TextureDesc dvd{};
        dvd.extent = {1, 1};
        dvd.format = rhi::Format::RG32Float;
        dvd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        dvd.debug_name = "ddgi-dummy-visibility";
        dummy_visibility_ = device.create_texture(dvd);
        const float zero_float2[2] = {0.0f, 0.0f};
        device.write_texture(dummy_visibility_, zero_float2, sizeof(zero_float2));
    }

    rhi::BufferDesc lb{};
    lb.size = sizeof(GpuSdfClipmapLevels);
    lb.usage = rhi::BufferUsage::Uniform;
    lb.memory = rhi::MemoryUsage::CpuToGpu;
    lb.debug_name = "ddgi-clipmap-levels";
    clipmap_levels_ubo_ = device.create_buffer(lb);

    rhi::BufferDesc tpd{};
    tpd.size = sizeof(GpuDdgiTraceParams);
    tpd.usage = rhi::BufferUsage::Uniform;
    tpd.memory = rhi::MemoryUsage::CpuToGpu;
    tpd.debug_name = "ddgi-trace-params";
    trace_params_ubo_ = device.create_buffer(tpd);

    rhi::BufferDesc bpd{};
    bpd.size = sizeof(GpuDdgiBlendParams);
    bpd.usage = rhi::BufferUsage::Uniform;
    bpd.memory = rhi::MemoryUsage::CpuToGpu;
    bpd.debug_name = "ddgi-blend-params";
    blend_params_ubo_ = device.create_buffer(bpd);

    // Fixed-size and created once: `probes_this_update` is bounded by kMaxDdgiProbesPerUpdate
    // regardless of how large the configured lattice grows (that bound is the whole point of the
    // round-robin), so unlike the ray buffer this one never needs to grow.
    rhi::BufferDesc hb{};
    hb.size = static_cast<std::uint64_t>(kMaxDdgiProbesPerUpdate) * sizeof(float);
    hb.usage = rhi::BufferUsage::Storage;
    hb.memory = rhi::MemoryUsage::CpuToGpu;
    hb.debug_name = "ddgi-hysteresis";
    hysteresis_buffer_ = device.create_buffer(hb);

    // m10.5b: the forward pass's sample-time uniform block (GpuDdgiSampleParams) — written by
    // BOTH add() (the real lattice + enabled=1) and empty_binding() (enabled=0), so it needs to
    // exist before either can run.
    rhi::BufferDesc spd{};
    spd.size = sizeof(GpuDdgiSampleParams);
    spd.usage = rhi::BufferUsage::Uniform;
    spd.memory = rhi::MemoryUsage::CpuToGpu;
    spd.debug_name = "ddgi-sample-params";
    sample_params_ubo_ = device.create_buffer(spd);
}

DdgiProbes::~DdgiProbes() {
    device_.destroy(sample_params_ubo_);
    if (ray_buffer_.is_valid())
        device_.destroy(ray_buffer_);
    device_.destroy(hysteresis_buffer_);
    device_.destroy(blend_params_ubo_);
    device_.destroy(trace_params_ubo_);
    device_.destroy(clipmap_levels_ubo_);
    device_.destroy(dummy_visibility_);
    device_.destroy(dummy_irradiance_);
    if (visibility_atlas_.is_valid())
        device_.destroy(visibility_atlas_);
    if (irradiance_atlas_.is_valid())
        device_.destroy(irradiance_atlas_);
    device_.destroy(atlas_sampler_);
    device_.destroy(clipmap_sampler_);
    device_.destroy(blend_visibility_pipeline_);
    device_.destroy(blend_visibility_shader_);
    device_.destroy(blend_irradiance_pipeline_);
    device_.destroy(blend_irradiance_shader_);
    device_.destroy(trace_pipeline_);
    device_.destroy(trace_shader_);
}

void DdgiProbes::ensure_atlases(std::uint32_t total_probes, std::uint32_t probes_per_row) {
    if (irradiance_atlas_.is_valid() && total_probes == atlas_total_probes_ &&
        probes_per_row == atlas_probes_per_row_) {
        return;
    }
    if (irradiance_atlas_.is_valid())
        device_.destroy(irradiance_atlas_);
    if (visibility_atlas_.is_valid())
        device_.destroy(visibility_atlas_);

    const std::uint32_t rows = (total_probes + probes_per_row - 1) / probes_per_row;

    rhi::TextureDesc itd{};
    itd.extent = {probes_per_row * kDdgiIrradianceTileSize, rows * kDdgiIrradianceTileSize};
    itd.format = rhi::Format::RGBA16Float;
    // TransferDst: the zero-clear upload just below needs it (write_texture copies through a
    // staging buffer); TransferSrc: tests read the atlas back.
    itd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled |
                rhi::TextureUsage::TransferSrc | rhi::TextureUsage::TransferDst;
    itd.debug_name = "ddgi-irradiance-atlas";
    irradiance_atlas_ = device_.create_texture(itd);
    // Zero-clear: a fresh probe's first update ignores the old value via hysteresis=0 (see add()),
    // so this is not what correctness rests on — but GLSL's mix(new, old, 0.0) still evaluates
    // `1*new + 0*old`, and IEEE 754 says 0 * NaN = NaN, not 0. Uninitialized device memory is not
    // guaranteed non-NaN, so a defined zero is one small upload cheaper than a real, if unlikely,
    // NaN-poisoning bug.
    {
        const std::vector<std::uint8_t> zeros(
            static_cast<std::size_t>(itd.extent.width) * itd.extent.height * 8, 0);
        device_.write_texture(irradiance_atlas_, zeros.data(), zeros.size());
    }
    irradiance_state_ = rhi::ResourceState::ShaderRead; // write_texture's own post-condition

    rhi::TextureDesc vtd{};
    vtd.extent = {probes_per_row * kDdgiVisibilityTileSize, rows * kDdgiVisibilityTileSize};
    vtd.format = rhi::Format::RG32Float;
    vtd.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled |
                rhi::TextureUsage::TransferSrc | rhi::TextureUsage::TransferDst;
    vtd.debug_name = "ddgi-visibility-atlas";
    visibility_atlas_ = device_.create_texture(vtd);
    {
        const std::vector<std::uint8_t> zeros(
            static_cast<std::size_t>(vtd.extent.width) * vtd.extent.height * 8, 0);
        device_.write_texture(visibility_atlas_, zeros.data(), zeros.size());
    }
    visibility_state_ = rhi::ResourceState::ShaderRead;

    atlas_total_probes_ = total_probes;
    atlas_probes_per_row_ = probes_per_row;
}

void DdgiProbes::ensure_ray_buffer_capacity(std::uint32_t ray_count) {
    if (ray_count <= ray_capacity_)
        return;
    std::uint32_t capacity = std::max<std::uint32_t>(ray_capacity_, 64u);
    while (capacity < ray_count)
        capacity *= 2;
    if (ray_buffer_.is_valid())
        device_.destroy(ray_buffer_);
    rhi::BufferDesc bd{};
    bd.size = static_cast<std::uint64_t>(capacity) * sizeof(GpuDdgiRay);
    bd.usage = rhi::BufferUsage::Storage;
    bd.memory = rhi::MemoryUsage::GpuOnly; // trace writes it, both blends read it, same frame
    bd.debug_name = "ddgi-rays";
    ray_buffer_ = device_.create_buffer(bd);
    ray_buffer_state_ = rhi::ResourceState::Undefined; // fresh allocation, nothing to preserve
    ray_capacity_ = capacity;
}

core::Mat4 DdgiProbes::next_ray_rotation() noexcept {
    // splitmix64 (Vigna): a tiny, fast, well-distributed PRNG — not cryptographic (fine for a
    // visual ray-rotation seed) and DETERMINISTIC (same starting state -> same sequence), which is
    // what keeps this brick's convergence test reproducible without mocking a "real" RNG.
    const auto next_u64 = [this]() -> std::uint64_t {
        rng_state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = rng_state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    };
    const auto next01 = [&next_u64]() {
        // Top 24 bits -> a float uniform in [0,1). 24 bits is exactly a float's mantissa width, so
        // every output is exactly representable — no rounding bias folded into the distribution.
        return static_cast<float>(next_u64() >> 40) / static_cast<float>(1u << 24);
    };

    // A uniformly random rotation = a uniformly random AXIS (standard sphere point-picking: z
    // uniform in [-1,1], azimuth uniform in [0,2pi)) turned through a uniformly random ANGLE in
    // [0,2pi) — Majercik et al.'s own per-frame ray-rotation scheme. Reuses
    // core::quat_from_axis_angle + core::to_mat4 rather than re-deriving a rotation matrix.
    const float u1 = next01();
    const float u2 = next01();
    const float u3 = next01();
    const float z = 1.0f - 2.0f * u1;
    const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
    const float phi = 2.0f * kPi * u2;
    const core::Vec3 axis{r * std::cos(phi), r * std::sin(phi), z};
    const float angle = 2.0f * kPi * u3;
    return core::to_mat4(core::quat_from_axis_angle(axis, angle));
}

DdgiBinding DdgiProbes::add(RenderGraph& graph,
                            SdfClipmap& clipmap,
                            core::Vec3 camera_pos,
                            const DdgiLightingInputs& lighting,
                            const LightingSettings& settings) {
    stats_ = DdgiStats{};

    const std::uint32_t count_x = std::max<std::uint32_t>(settings.ddgi_probe_count_x, 1u);
    const std::uint32_t count_y = std::max<std::uint32_t>(settings.ddgi_probe_count_y, 1u);
    const std::uint32_t count_z = std::max<std::uint32_t>(settings.ddgi_probe_count_z, 1u);
    const std::uint32_t total_probes = count_x * count_y * count_z;
    const float spacing = std::max(settings.ddgi_probe_spacing, 1.0e-4f);

    // ── Recentre the lattice, texel-snapped to its own spacing — SdfClipmap::add()'s level-snap
    // trick (lighting/sdf_clipmap.cpp), applied to probes instead of voxels: floor to a whole
    // multiple of `spacing` so sub-spacing camera motion leaves the snapped origin BIT-IDENTICAL
    // (no probe reshuffled), while crossing a whole spacing step moves every probe by exactly one
    // lattice step. The lattice spans (count-1)*spacing per axis, centred on the camera.
    const core::Vec3 half_extent{static_cast<float>(count_x - 1) * spacing * 0.5f,
                                 static_cast<float>(count_y - 1) * spacing * 0.5f,
                                 static_cast<float>(count_z - 1) * spacing * 0.5f};
    const core::Vec3 ideal_min = camera_pos - half_extent;
    const core::Vec3 snapped{std::floor(ideal_min.x / spacing) * spacing,
                             std::floor(ideal_min.y / spacing) * spacing,
                             std::floor(ideal_min.z / spacing) * spacing};

    const bool dims_changed =
        count_x != last_count_x_ || count_y != last_count_y_ || count_z != last_count_z_;
    const bool shifted = !origin_initialized_ || dims_changed || snapped.x != origin_.x ||
                         snapped.y != origin_.y || snapped.z != origin_.z;
    origin_ = snapped;
    origin_initialized_ = true;
    last_count_x_ = count_x;
    last_count_y_ = count_y;
    last_count_z_ = count_z;

    if (shifted) {
        // A whole-lattice move invalidates every probe's accumulated history — the same "changed
        // at all => start over" simplification SdfClipmap makes for its compose state (there, a
        // full recompose; here, forgetting who was primed). A toroidal remap that PRESERVES
        // history for probes that land at the same world position across the shift is the natural
        // follow-up optimization, deferred until a profile asks for it (docs/math/ddgi.md §6).
        primed_.assign(total_probes, false);
        // m10.5b: a shifted lattice's indices no longer name the same world-space probes, so any
        // pending fast-track (indexed by the OLD lattice) would fast-track the wrong points in
        // space — drop it along with the primed history it is paired with.
        fast_track_remaining_.assign(total_probes, 0);
        round_robin_cursor_ = 0;
        stats_.grid_shifted = true;
    } else if (primed_.size() != total_probes) {
        primed_.assign(total_probes, false); // defensive: dims changed without origin moving
        fast_track_remaining_.assign(total_probes, 0);
    }

    // ── Destruction-reactive hysteresis (m10.5b, C2) — drain invalidate()'s queue ────────────
    // Every pending region marks any probe within one lattice SPACING of it (an "err broad"
    // expansion, the same conservatism LocalShadowMap's frustum-AABB test uses for its own
    // invalidate()): a probe sits AT a lattice point, not a cell centre, so a destruction event's
    // AABB landing anywhere inside the cell between two probes must still catch BOTH of that
    // cell's corners, not just whichever corner the event's own (typically tight) box happens to
    // overlap. This is deferred to here (not done inside invalidate() itself) because add() is the
    // first place the CURRENT lattice shape (origin_, spacing, counts) is actually known —
    // invalidate() may be called before the lattice has ever been placed at all.
    if (!pending_regions_.empty()) {
        const core::Vec3 pad{spacing, spacing, spacing};
        for (const WorldAabb& region : pending_regions_) {
            const core::Vec3 lo = region.min - pad;
            const core::Vec3 hi = region.max + pad;
            for (std::uint32_t i = 0; i < total_probes; ++i) {
                const core::Vec3 p = ddgi_probe_position(i, origin_, spacing, count_x, count_y);
                if (p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z &&
                    p.z <= hi.z) {
                    fast_track_remaining_[i] = kFastTrackUpdates;
                }
            }
        }
        pending_regions_.clear();
    }

    const std::uint32_t probes_per_row = count_x * count_y;
    ensure_atlases(total_probes, probes_per_row);

    const std::uint32_t probes_this_update = std::min(total_probes, kMaxDdgiProbesPerUpdate);
    const std::uint32_t round_robin_base = round_robin_cursor_ % total_probes;
    round_robin_cursor_ = (round_robin_cursor_ + probes_this_update) % total_probes;

    const std::uint32_t rays_per_probe = std::max<std::uint32_t>(settings.ddgi_rays_per_probe, 1u);
    const std::uint32_t ray_count = probes_this_update * rays_per_probe;
    ensure_ray_buffer_capacity(ray_count);

    // Per-probe effective hysteresis this update: 0 snaps straight to this frame's estimate (a
    // probe's first-ever update, never blended with the atlas's initial zero — see the header's
    // "newly_primed" note); a probe invalidate() touched uses kFastTrackHysteresis for its next
    // kFastTrackUpdates updates (m10.5b); otherwise the configured steady-state value.
    std::vector<float> hysteresis(probes_this_update);
    std::uint32_t fast_tracked_this_update = 0;
    for (std::uint32_t i = 0; i < probes_this_update; ++i) {
        const std::uint32_t global = (round_robin_base + i) % total_probes;
        const bool was_primed = primed_[global];
        if (!was_primed) {
            hysteresis[i] = 0.0f;
            ++stats_.newly_primed;
            primed_[global] = true;
            // A first-ever update already snaps straight to this update's own estimate — there is
            // no stale history left for a fast-track to shake off, so any pending budget for a
            // probe that got primed and invalidated in the same stretch is spent, not saved.
            fast_track_remaining_[global] = 0;
        } else if (fast_track_remaining_[global] > 0) {
            hysteresis[i] = kFastTrackHysteresis;
            --fast_track_remaining_[global];
            ++fast_tracked_this_update;
        } else {
            hysteresis[i] = settings.ddgi_hysteresis;
        }
    }
    stats_.fast_tracked = fast_tracked_this_update;
    device_.write_buffer(hysteresis_buffer_, hysteresis.data(), hysteresis.size() * sizeof(float));

    const GpuSdfClipmapLevels levels = clipmap.gpu_levels();
    device_.write_buffer(clipmap_levels_ubo_, &levels, sizeof(levels));

    GpuDdgiTraceParams tp{};
    tp.ray_rotation = next_ray_rotation();
    tp.grid_origin_spacing[0] = origin_.x;
    tp.grid_origin_spacing[1] = origin_.y;
    tp.grid_origin_spacing[2] = origin_.z;
    tp.grid_origin_spacing[3] = spacing;
    tp.grid_dims_base[0] = count_x;
    tp.grid_dims_base[1] = count_y;
    tp.grid_dims_base[2] = count_z;
    tp.grid_dims_base[3] = round_robin_base;
    tp.probes_rays_total[0] = probes_this_update;
    tp.probes_rays_total[1] = rays_per_probe;
    tp.probes_rays_total[2] = total_probes;
    tp.sun_dir_maxdist[0] = lighting.sun_direction.x;
    tp.sun_dir_maxdist[1] = lighting.sun_direction.y;
    tp.sun_dir_maxdist[2] = lighting.sun_direction.z;
    tp.sun_dir_maxdist[3] = std::max(settings.ddgi_max_trace_distance, 0.1f);
    if (lighting.has_sun) {
        tp.sun_radiance_albedo[0] = lighting.sun_radiance[0];
        tp.sun_radiance_albedo[1] = lighting.sun_radiance[1];
        tp.sun_radiance_albedo[2] = lighting.sun_radiance[2];
    }
    tp.sun_radiance_albedo[3] = settings.ddgi_albedo;
    tp.sky_radiance_pad[0] = lighting.sky_radiance[0];
    tp.sky_radiance_pad[1] = lighting.sky_radiance[1];
    tp.sky_radiance_pad[2] = lighting.sky_radiance[2];
    device_.write_buffer(trace_params_ubo_, &tp, sizeof(tp));

    GpuDdgiBlendParams bp{};
    bp.base_total_perrow_rays[0] = round_robin_base;
    bp.base_total_perrow_rays[1] = total_probes;
    bp.base_total_perrow_rays[2] = probes_per_row;
    bp.base_total_perrow_rays[3] = rays_per_probe;
    device_.write_buffer(blend_params_ubo_, &bp, sizeof(bp));

    // ── Declare the graph passes ─────────────────────────────────────────────────────────────
    // Import the clipmap's 3 levels as SAMPLED inputs, using their ACTUAL current state (not a
    // claimed Undefined) — SdfClipmap::level_state() is authoritative because DDGI reports back
    // where it leaves each level via note_level_state() below, so the clipmap's OWN bookkeeping
    // never goes stale even though DDGI is a second, independent reader of its textures.
    std::array<RGTexture, kSdfClipmapLevels> level_rg{};
    for (std::uint32_t i = 0; i < kSdfClipmapLevels; ++i)
        level_rg[i] = graph.import_texture(clipmap.level(i).texture, clipmap.level_state(i));

    const RGBuffer rays_rg = graph.import_buffer(ray_buffer_, ray_buffer_state_);

    {
        const RGTexture sampled[] = {level_rg[0], level_rg[1], level_rg[2]};
        const RGBuffer writes[] = {rays_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.sampled = sampled;
        desc.buffer_writes = writes;
        graph.add_compute_pass("ddgi-trace",
                               desc,
                               [pipe = trace_pipeline_,
                                sampler = clipmap_sampler_,
                                l0 = clipmap.level(0).texture,
                                l1 = clipmap.level(1).texture,
                                l2 = clipmap.level(2).texture,
                                levels_ubo = clipmap_levels_ubo_,
                                params = trace_params_ubo_,
                                rays = ray_buffer_,
                                ray_count](rhi::CommandBuffer& cmd) {
                                   cmd.bind_compute_pipeline(pipe);
                                   cmd.bind_texture(0, l0, sampler);
                                   cmd.bind_texture(1, l1, sampler);
                                   cmd.bind_texture(2, l2, sampler);
                                   cmd.bind_uniform_buffer(3, levels_ubo);
                                   cmd.bind_uniform_buffer(4, params);
                                   cmd.bind_storage_buffer(5, rays);
                                   cmd.dispatch(
                                       (ray_count + kTraceGroupSize - 1) / kTraceGroupSize, 1, 1);
                               });
    }
    // The trace pass just declared above is a SAMPLED read of all 3 levels, so the graph will
    // transition (and leave) each in ShaderRead — tell the clipmap that is where they now sit
    // (SdfClipmap::note_level_state's own comment explains why this call has to exist at all).
    for (std::uint32_t i = 0; i < kSdfClipmapLevels; ++i)
        clipmap.note_level_state(i, rhi::ResourceState::ShaderRead);

    const RGTexture irradiance_rg = graph.import_texture(irradiance_atlas_, irradiance_state_);
    {
        const RGBuffer reads[] = {rays_rg};
        const RGTexture writes[] = {irradiance_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.buffer_reads = reads;
        desc.storage_write = writes;
        graph.add_compute_pass("ddgi-blend-irradiance",
                               desc,
                               [pipe = blend_irradiance_pipeline_,
                                rays = ray_buffer_,
                                params = blend_params_ubo_,
                                hyst = hysteresis_buffer_,
                                atlas = irradiance_atlas_,
                                probes_this_update](rhi::CommandBuffer& cmd) {
                                   cmd.bind_compute_pipeline(pipe);
                                   cmd.bind_storage_buffer(0, rays);
                                   cmd.bind_uniform_buffer(1, params);
                                   cmd.bind_storage_buffer(2, hyst);
                                   cmd.bind_storage_image(3, atlas);
                                   cmd.dispatch(1, 1, probes_this_update);
                               });
    }

    const RGTexture visibility_rg = graph.import_texture(visibility_atlas_, visibility_state_);
    {
        const RGBuffer reads[] = {rays_rg};
        const RGTexture writes[] = {visibility_rg};
        RenderGraph::ComputePassDesc desc{};
        desc.buffer_reads = reads;
        desc.storage_write = writes;
        graph.add_compute_pass("ddgi-blend-visibility",
                               desc,
                               [pipe = blend_visibility_pipeline_,
                                rays = ray_buffer_,
                                params = blend_params_ubo_,
                                hyst = hysteresis_buffer_,
                                atlas = visibility_atlas_,
                                probes_this_update](rhi::CommandBuffer& cmd) {
                                   cmd.bind_compute_pipeline(pipe);
                                   cmd.bind_storage_buffer(0, rays);
                                   cmd.bind_uniform_buffer(1, params);
                                   cmd.bind_storage_buffer(2, hyst);
                                   cmd.bind_storage_image(3, atlas);
                                   cmd.dispatch(1, 1, probes_this_update);
                               });
    }

    ray_buffer_state_ = rhi::ResourceState::ShaderRead; // read by both blend passes above
    irradiance_state_ = rhi::ResourceState::StorageReadWrite;
    visibility_state_ = rhi::ResourceState::StorageReadWrite;

    stats_.probes_updated = probes_this_update;
    stats_.rays_traced = ray_count;

    // ── m10.5b: the sample-time binding the forward pass consumes ────────────────────────────
    // `irradiance_rg`/`visibility_rg` are THIS frame's graph handles for the same two atlases the
    // blend passes above just wrote (imported once, above) — reusing them (rather than importing
    // a SECOND time) is what gives the forward pass's sampled-read a real dependency edge on those
    // writes, so the graph orders it after them and the fragment shader sees THIS update's data.
    GpuDdgiSampleParams sp{};
    sp.grid_origin_spacing[0] = origin_.x;
    sp.grid_origin_spacing[1] = origin_.y;
    sp.grid_origin_spacing[2] = origin_.z;
    sp.grid_origin_spacing[3] = spacing;
    sp.grid_dims_perrow[0] = count_x;
    sp.grid_dims_perrow[1] = count_y;
    sp.grid_dims_perrow[2] = count_z;
    sp.grid_dims_perrow[3] = probes_per_row;
    sp.enabled_pad[0] = 1u;
    device_.write_buffer(sample_params_ubo_, &sp, sizeof(sp));

    return DdgiBinding{irradiance_rg, visibility_rg, sample_params_ubo_, atlas_sampler_};
}

DdgiBinding DdgiProbes::empty_binding(RenderGraph& graph) {
    // enabled=0 is the ONLY thing that matters here (ADR-0032 §11): the shader's own `if (enabled)`
    // gate is what makes DDGI-off byte-identical to the M5.6 baseline, not whatever the dummy
    // atlases happen to contain. The rest of the block is left zeroed — a shader that (incorrectly)
    // read it anyway would at worst index atlas texel (0,0) of a 1x1 dummy, never out of bounds.
    GpuDdgiSampleParams sp{};
    sp.enabled_pad[0] = 0u;
    device_.write_buffer(sample_params_ubo_, &sp, sizeof(sp));
    // Both dummies sit permanently in ShaderRead (write_texture's own post-condition at
    // construction, never touched again) — import at that state, exactly the dummy_shadow_array_
    // pattern SceneRenderer's own shadow empty_binding()s already use.
    return DdgiBinding{graph.import_texture(dummy_irradiance_, rhi::ResourceState::ShaderRead),
                       graph.import_texture(dummy_visibility_, rhi::ResourceState::ShaderRead),
                       sample_params_ubo_,
                       atlas_sampler_};
}

void DdgiProbes::invalidate(const WorldAabb& region) {
    pending_regions_.push_back(region);
}

rhi::Extent2D DdgiProbes::irradiance_atlas_extent() const noexcept {
    const std::uint32_t rows =
        atlas_probes_per_row_ == 0
            ? 0
            : (atlas_total_probes_ + atlas_probes_per_row_ - 1) / atlas_probes_per_row_;
    return {atlas_probes_per_row_ * kDdgiIrradianceTileSize, rows * kDdgiIrradianceTileSize};
}

rhi::Extent2D DdgiProbes::visibility_atlas_extent() const noexcept {
    const std::uint32_t rows =
        atlas_probes_per_row_ == 0
            ? 0
            : (atlas_total_probes_ + atlas_probes_per_row_ - 1) / atlas_probes_per_row_;
    return {atlas_probes_per_row_ * kDdgiVisibilityTileSize, rows * kDdgiVisibilityTileSize};
}

} // namespace rime::render
