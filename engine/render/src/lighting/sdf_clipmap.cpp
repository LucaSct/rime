// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The runtime SDF clipmap (m10.4b, ADR-0032 §2 + §10). See the header for the shape (three
// nested 64³ R16Snorm levels) and docs/math/sdf.md for the full derivation (the narrow-band
// encoding + its error, the texel-snap, why min-blend composes, the per-instance-dispatch cost).
// This file is the CPU half: dirty-region bookkeeping (the point of the brick) and the per-frame
// job list the compose shader (sdf_compose.comp) executes.

#include "rime/render/lighting/sdf_clipmap.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "rime/core/diagnostics/log.hpp"
#include "sdf_compose.comp.spv.h"

namespace rime::render {

namespace {

// Per-dispatch slice of the compose job buffer — the same 256-byte-stride pattern
// CascadedShadowMap/LocalShadowMap use for their per-cascade/per-spot view_proj buffers (universal
// uniform-offset alignment, one write per frame, N passes each pointing at their own offset).
constexpr std::uint32_t kComposeStride = 256;

// Must equal sdf_compose.comp's local_size_x/y/z (the GLSL reserved-word trap bit m10.3's
// cluster_cull.comp — `active` — so this is spelled out here explicitly rather than left as a
// bare "4" at each call site).
constexpr std::uint32_t kComposeGroupSize = 4;

// Mirrors ComposeJob in sdf_compose.comp EXACTLY, field for field. Every member is a mat4 or a
// 16-byte (u/i)vec4-equivalent chunk, so C++'s natural layout and std140 agree by construction —
// the same discipline as GpuShadowUniforms/GpuClusterUniforms. Kept private to this translation
// unit (like kCascadeStride/kSpotStride): nothing outside the compose pass ever sees a job.
struct GpuComposeJob {
    core::Mat4 local_from_world;                            // world -> instance local (STAMP only)
    float inst_origin_voxel[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // xyz origin, w voxel_size
    std::uint32_t inst_resolution_mode[4] = {0, 0, 0, 0};   // xyz resolution, w 0=clear/1=stamp
    float level_origin_voxel[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz this level's origin, w voxel_size
    std::int32_t voxel_offset[4] = {0, 0, 0, 0};            // xyz base voxel of this dispatch
    std::int32_t region_size[4] = {0, 0, 0, 0};             // xyz this dispatch's voxel extent
    float params[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // x band, y inst scale, z inst max_abs_distance
};

static_assert(sizeof(GpuComposeJob) == 160 && offsetof(GpuComposeJob, inst_origin_voxel) == 64 &&
                  offsetof(GpuComposeJob, inst_resolution_mode) == 80 &&
                  offsetof(GpuComposeJob, level_origin_voxel) == 96 &&
                  offsetof(GpuComposeJob, voxel_offset) == 112 &&
                  offsetof(GpuComposeJob, region_size) == 128 &&
                  offsetof(GpuComposeJob, params) == 144,
              "GpuComposeJob no longer matches the std140 ComposeJob block in sdf_compose.comp");
static_assert(sizeof(GpuComposeJob) <= kComposeStride);

// Does `m` scale every axis alike? A signed distance is only a DISTANCE if every axis scales the
// same amount (docs/math/sdf.md): a non-uniform scale changes how far the surface really is
// without changing the stored number, which silently breaks the "step by the field's value is
// always safe" guarantee sphere-tracing depends on. `out_scale` is the (average) scale factor;
// meaningful only when this returns true.
bool detect_uniform_scale(const core::Mat4& m, float& out_scale) {
    const core::Vec3 col_x{m.at(0, 0), m.at(1, 0), m.at(2, 0)};
    const core::Vec3 col_y{m.at(0, 1), m.at(1, 1), m.at(2, 1)};
    const core::Vec3 col_z{m.at(0, 2), m.at(1, 2), m.at(2, 2)};
    const float sx = core::length(col_x);
    const float sy = core::length(col_y);
    const float sz = core::length(col_z);
    out_scale = (sx + sy + sz) / 3.0f;
    const float tol = 0.01f * std::max({sx, sy, sz, 1.0e-6f});
    return std::fabs(sx - sy) <= tol && std::fabs(sy - sz) <= tol && std::fabs(sx - sz) <= tol;
}

// The world-space AABB of the local-space box [lo, hi], carried through `world_from_local` corner
// by corner and re-boxed — the standard "transform an AABB" idiom (mirrors
// destruction/src/damage.cpp's part_world_aabb, specialized to a Mat4 rather than a
// core::Transform since a clipmap instance is placed by a plain matrix).
WorldAabb transform_aabb(core::Vec3 lo, core::Vec3 hi, const core::Mat4& world_from_local) {
    WorldAabb out;
    for (int corner = 0; corner < 8; ++corner) {
        const core::Vec3 local{(corner & 1) != 0 ? hi.x : lo.x,
                               (corner & 2) != 0 ? hi.y : lo.y,
                               (corner & 4) != 0 ? hi.z : lo.z};
        const core::Vec3 w = core::transform_point(world_from_local, local);
        if (corner == 0) {
            out.min = w;
            out.max = w;
        } else {
            out.min = core::Vec3{
                std::min(out.min.x, w.x), std::min(out.min.y, w.y), std::min(out.min.z, w.z)};
            out.max = core::Vec3{
                std::max(out.max.x, w.x), std::max(out.max.y, w.y), std::max(out.max.z, w.z)};
        }
    }
    return out;
}

WorldAabb merge(const WorldAabb& a, const WorldAabb& b) {
    return WorldAabb{
        core::Vec3{
            std::min(a.min.x, b.min.x), std::min(a.min.y, b.min.y), std::min(a.min.z, b.min.z)},
        core::Vec3{
            std::max(a.max.x, b.max.x), std::max(a.max.y, b.max.y), std::max(a.max.z, b.max.z)}};
}

// Intersection of two boxes; false (and `out` left partially written) if they do not overlap.
bool intersect(const WorldAabb& a, const WorldAabb& b, WorldAabb& out) {
    out.min = core::Vec3{
        std::max(a.min.x, b.min.x), std::max(a.min.y, b.min.y), std::max(a.min.z, b.min.z)};
    out.max = core::Vec3{
        std::min(a.max.x, b.max.x), std::min(a.max.y, b.max.y), std::min(a.max.z, b.max.z)};
    return out.min.x <= out.max.x && out.min.y <= out.max.y && out.min.z <= out.max.z;
}

// An inclusive-lo/exclusive-hi integer voxel box within one level's kSdfClipmapResolution³ grid.
struct VoxelRange {
    std::array<std::int32_t, 3> lo{0, 0, 0};
    std::array<std::int32_t, 3> size{0, 0, 0};

    [[nodiscard]] bool empty() const noexcept {
        return size[0] <= 0 || size[1] <= 0 || size[2] <= 0;
    }
};

// `region` is assumed already known to lie within `info`'s current world extent (every call site
// below only ever passes a region built by intersecting with that extent) — the clamps here are a
// defensive backstop against float rounding exactly AT the boundary, not the primary bound.
VoxelRange voxel_range_for(const SdfClipmap::LevelInfo& info, const WorldAabb& region) {
    const float origin[3] = {info.origin.x, info.origin.y, info.origin.z};
    const float lo3[3] = {region.min.x, region.min.y, region.min.z};
    const float hi3[3] = {region.max.x, region.max.y, region.max.z};
    VoxelRange vr;
    for (int a = 0; a < 3; ++a) {
        auto lo = static_cast<std::int32_t>(std::floor((lo3[a] - origin[a]) / info.voxel_size));
        auto hi = static_cast<std::int32_t>(std::ceil((hi3[a] - origin[a]) / info.voxel_size));
        lo = std::clamp(lo, 0, static_cast<std::int32_t>(kSdfClipmapResolution));
        hi = std::clamp(hi, 0, static_cast<std::int32_t>(kSdfClipmapResolution));
        vr.lo[a] = lo;
        vr.size[a] = hi - lo;
    }
    return vr;
}

std::array<std::uint32_t, 3> group_counts(const std::array<std::int32_t, 3>& size) {
    const auto groups = [](std::int32_t n) {
        return (static_cast<std::uint32_t>(n) + kComposeGroupSize - 1) / kComposeGroupSize;
    };
    return {groups(size[0]), groups(size[1]), groups(size[2])};
}

} // namespace

SdfClipmap::SdfClipmap(rhi::Device& device) : device_(device) {
    rhi::ShaderDesc sd{};
    sd.stage = rhi::ShaderStage::Compute;
    sd.spirv = sdf_compose_comp_spv;
    sd.spirv_size_bytes = sizeof(sdf_compose_comp_spv);
    sd.debug_name = "sdf_compose.comp";
    compose_shader_ = device.create_shader(sd);

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::StorageImage, rhi::StageMask::Compute},         // the clipmap level
        {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute}, // the instance field
        {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},        // this job
    };
    rhi::ComputePipelineDesc pd{};
    pd.shader = compose_shader_;
    pd.bindings = bindings;
    pd.debug_name = "sdf-clipmap-compose";
    compose_pipeline_ = device.create_compute_pipeline(pd);

    // The placeholder every "clear" dispatch binds at binding 1 (the pipeline's descriptor layout
    // requires every declared binding attached before a dispatch, whether or not the shader's
    // runtime branch reads it — the same invariant ClusteredLights' empty_lists_ and
    // LocalShadowMap's dummy depth array keep). A depth of 2 (not 1) is deliberate: the RHI's
    // create_texture treats `depth > 1` as "this is a 3-D image" (resources_vulkan.cpp), so a
    // literal 1x1x1 volume is not representable — 1x1x2 is the smallest true 3-D image, and a
    // sampler3D binding needs a real 3-D image view (a depth-1 texture would instead create an
    // ordinary 2-D view, which is not compatible with `sampler3D` in the shader).
    rhi::TextureDesc ptd{};
    ptd.extent = {1, 1};
    ptd.depth = 2;
    ptd.format = rhi::Format::R16Snorm;
    ptd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    ptd.debug_name = "sdf-clipmap-placeholder-instance";
    placeholder_instance_sdf_ = device.create_texture(ptd);
    const std::int16_t zeros[2] = {0, 0};
    device.write_texture(placeholder_instance_sdf_, zeros, sizeof(zeros));

    rhi::SamplerDesc smd{};
    smd.mag_filter = rhi::Filter::Linear;
    smd.min_filter = rhi::Filter::Linear;
    smd.address_mode = rhi::AddressMode::ClampToEdge;
    smd.debug_name = "sdf-instance-sampler";
    instance_sampler_ = device.create_sampler(smd);
}

SdfClipmap::~SdfClipmap() {
    for (auto& [id, inst] : instances_) {
        device_.destroy(inst.gpu_sdf);
    }
    for (Level& lvl : levels_) {
        if (lvl.info.texture.is_valid())
            device_.destroy(lvl.info.texture);
    }
    device_.destroy(instance_sampler_);
    device_.destroy(placeholder_instance_sdf_);
    if (compose_ubo_.is_valid())
        device_.destroy(compose_ubo_);
    device_.destroy(compose_pipeline_);
    device_.destroy(compose_shader_);
}

void SdfClipmap::ensure_level_texture(std::uint32_t index) {
    Level& lvl = levels_[index];
    if (lvl.info.texture.is_valid())
        return;

    rhi::TextureDesc td{};
    td.extent = {kSdfClipmapResolution, kSdfClipmapResolution};
    td.depth = kSdfClipmapResolution;
    td.format = rhi::Format::R16Snorm;
    td.usage = rhi::TextureUsage::Storage | rhi::TextureUsage::Sampled; // Sampled: m10.5 traces it
    td.debug_name = "sdf-clipmap-level";
    lvl.info.texture = device_.create_texture(td);

    // Level i is kSdfClipmapLevelScale× coarser than level i-1 (repeated multiplication rather
    // than std::pow: exact for these power-of-two-friendly numbers, no rounding surprises).
    float voxel_size = kSdfClipmapBaseVoxelSize;
    for (std::uint32_t i = 0; i < index; ++i)
        voxel_size *= static_cast<float>(kSdfClipmapLevelScale);
    lvl.info.voxel_size = voxel_size;
    lvl.info.band = kSdfClipmapBandVoxels * voxel_size;
    lvl.texture_state = rhi::ResourceState::Undefined;
    lvl.origin_initialized = false;
}

void SdfClipmap::ensure_job_capacity(std::uint32_t count) {
    if (count <= job_capacity_)
        return;
    std::uint32_t capacity = std::max<std::uint32_t>(job_capacity_, 8);
    while (capacity < count)
        capacity *= 2;
    if (compose_ubo_.is_valid())
        device_.destroy(compose_ubo_);
    rhi::BufferDesc bd{};
    bd.size = static_cast<std::uint64_t>(capacity) * kComposeStride;
    bd.usage = rhi::BufferUsage::Uniform;
    bd.memory = rhi::MemoryUsage::CpuToGpu;
    bd.debug_name = "sdf-clipmap-compose-jobs";
    compose_ubo_ = device_.create_buffer(bd);
    job_capacity_ = capacity;
}

rhi::TextureHandle SdfClipmap::upload_instance_sdf(const assets::MeshSdfAsset& sdf) const {
    rhi::TextureDesc td{};
    td.extent = {sdf.resolution[0], sdf.resolution[1]};
    td.depth = sdf.resolution[2];
    td.format = rhi::Format::R16Snorm;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "sdf-instance";
    const rhi::TextureHandle tex = device_.create_texture(td);

    // Quantize into R16Snorm using the COOK's own max_abs_distance as the encode scale — a
    // different number from any clipmap level's band, decoded back out in the compose shader via
    // GpuComposeJob::params.z. A degenerate (all-zero) field floors the scale to 0 rather than
    // dividing by it.
    const float inv_scale = sdf.max_abs_distance > 1.0e-8f ? 1.0f / sdf.max_abs_distance : 0.0f;
    std::vector<std::int16_t> quantized(sdf.distances.size());
    for (std::size_t i = 0; i < sdf.distances.size(); ++i) {
        const float n = std::clamp(sdf.distances[i] * inv_scale, -1.0f, 1.0f);
        quantized[i] = static_cast<std::int16_t>(std::lround(n * 32767.0f));
    }
    device_.write_texture(tex, quantized.data(), quantized.size() * sizeof(std::int16_t));
    return tex;
}

void SdfClipmap::update_instance(std::uint64_t id,
                                 const assets::MeshSdfAsset& sdf,
                                 const core::Mat4& world_from_local) {
    float scale = 1.0f;
    if (!detect_uniform_scale(world_from_local, scale)) {
        RIME_WARN("SdfClipmap: instance {} has a non-uniform-scale transform — a signed distance "
                  "is only meaningful under uniform scale (docs/math/sdf.md); excluding it from "
                  "the field rather than composing it wrong. Re-register it once its transform is "
                  "uniform again.",
                  id);
        remove_instance(id); // drop any previously-valid registration too — never leave stale data
        return;
    }

    const auto existing = instances_.find(id);
    if (existing != instances_.end()) {
        pending_regions_.push_back(existing->second.world_bounds); // the region it is LEAVING
        device_.destroy(existing->second.gpu_sdf);
    }

    TrackedInstance inst{};
    inst.resolution = sdf.resolution;
    inst.grid_origin = sdf.grid_origin;
    inst.voxel_size = sdf.voxel_size;
    inst.max_abs_distance = sdf.max_abs_distance;
    inst.local_from_world = core::inverse(world_from_local);
    inst.uniform_scale = scale;
    const core::Vec3 grid_hi{
        sdf.grid_origin.x + static_cast<float>(sdf.resolution[0]) * sdf.voxel_size,
        sdf.grid_origin.y + static_cast<float>(sdf.resolution[1]) * sdf.voxel_size,
        sdf.grid_origin.z + static_cast<float>(sdf.resolution[2]) * sdf.voxel_size};
    inst.world_bounds = transform_aabb(sdf.grid_origin, grid_hi, world_from_local);
    inst.gpu_sdf = upload_instance_sdf(sdf);

    pending_regions_.push_back(inst.world_bounds); // the region it is ARRIVING in (C1)
    instances_[id] = inst;
}

void SdfClipmap::remove_instance(std::uint64_t id) {
    const auto it = instances_.find(id);
    if (it == instances_.end())
        return;
    pending_regions_.push_back(it->second.world_bounds);
    device_.destroy(it->second.gpu_sdf);
    instances_.erase(it);
}

void SdfClipmap::invalidate(const WorldAabb& region) {
    pending_regions_.push_back(region);
}

GpuSdfClipmapLevels SdfClipmap::gpu_levels() const noexcept {
    GpuSdfClipmapLevels out{};
    for (std::uint32_t i = 0; i < kSdfClipmapLevels; ++i) {
        const LevelInfo& info = levels_[i].info;
        const float extent = info.voxel_size * static_cast<float>(kSdfClipmapResolution);
        out.levels[i].origin_extent[0] = info.origin.x;
        out.levels[i].origin_extent[1] = info.origin.y;
        out.levels[i].origin_extent[2] = info.origin.z;
        out.levels[i].origin_extent[3] = extent;
        out.levels[i].band_voxel[0] = info.band;
        out.levels[i].band_voxel[1] = info.voxel_size;
    }
    return out;
}

void SdfClipmap::add(RenderGraph& graph, core::Vec3 camera_pos) {
    stats_ = SdfClipmapStats{};

    // ── Pass 1: for every level, decide WHAT (if anything) must recompose this frame ──────────
    struct LevelWork {
        std::uint32_t level = 0;
        WorldAabb region{};
        std::vector<const TrackedInstance*> stampers;
    };

    std::vector<LevelWork> work;
    work.reserve(kSdfClipmapLevels);

    for (std::uint32_t i = 0; i < kSdfClipmapLevels; ++i) {
        ensure_level_texture(i);
        Level& lvl = levels_[i];
        const float voxel_size = lvl.info.voxel_size;
        const float extent = voxel_size * static_cast<float>(kSdfClipmapResolution);

        // Texel-snap this level's origin to ITS OWN voxel grid, centred on the camera — exactly
        // compute_cascades' (lighting/shadows.cpp) anti-shimmer trick: floor to a whole-voxel
        // multiple so sub-voxel camera motion leaves the snapped corner BIT-IDENTICAL (zero
        // recomposition), while crossing a voxel boundary moves it by a whole voxel and forces
        // this level's ENTIRE volume to recompose (a scrolling/toroidal partial update — only
        // recomputing the newly-exposed band — is the natural follow-up optimization; v1 keeps
        // the simpler, always-correct "changed at all ⇒ recompose everything" rule and measures
        // whether a profile ever asks for the scroll — see docs/math/sdf.md).
        const core::Vec3 ideal_min{camera_pos.x - extent * 0.5f,
                                   camera_pos.y - extent * 0.5f,
                                   camera_pos.z - extent * 0.5f};
        const core::Vec3 snapped{std::floor(ideal_min.x / voxel_size) * voxel_size,
                                 std::floor(ideal_min.y / voxel_size) * voxel_size,
                                 std::floor(ideal_min.z / voxel_size) * voxel_size};

        const bool full_recompose = !lvl.origin_initialized || snapped.x != lvl.info.origin.x ||
                                    snapped.y != lvl.info.origin.y ||
                                    snapped.z != lvl.info.origin.z;
        lvl.info.origin = snapped;
        lvl.origin_initialized = true;
        if (full_recompose)
            ++stats_.levels_recomposed;

        const WorldAabb level_extent{snapped, snapped + core::Vec3{extent, extent, extent}};

        WorldAabb region{};
        bool have_region = false;
        if (full_recompose) {
            region = level_extent;
            have_region = true;
        } else {
            // The union of every pending region overlapping this level at all. Taking the BOUNDING
            // BOX of several simultaneous changes (rather than clearing+stamping each individually)
            // is a deliberate, conservative simplification: it guarantees this level's one clear
            // dispatch can never land between two of this frame's own stamps and erase one of them
            // (see sdf_compose.comp's header for why ordering the clear first matters). The cost is
            // occasionally recomposing a few voxels that, in isolation, did not need it — paid only
            // when multiple UNRELATED regions happen to go dirty in the very same frame.
            for (const WorldAabb& r : pending_regions_) {
                WorldAabb clipped{};
                if (!intersect(r, level_extent, clipped))
                    continue;
                region = have_region ? merge(region, clipped) : clipped;
                have_region = true;
            }
        }
        if (!have_region)
            continue; // nothing to do for this level — ADR-0032 §11's "idle work is a bug", made
                      // structural: no dirty region means no dispatch is even declared below.

        LevelWork w;
        w.level = i;
        w.region = region;
        for (const auto& [id, inst] : instances_) {
            if (inst.world_bounds.overlaps(region))
                w.stampers.push_back(&inst);
        }
        work.push_back(std::move(w));
    }
    pending_regions_.clear();

    if (work.empty())
        return; // a fully static, unmoved frame: zero passes declared, zero GPU work

    // ── Pass 2: build every job this frame needs into ONE staging buffer + ONE upload ─────────
    // (the cascade/spot-shadow pattern: fill every slice, one write_buffer, then N passes each
    // pointing at their own 256-byte offset).
    struct Dispatch {
        std::uint32_t level = 0;
        std::uint32_t slot = 0;
        rhi::TextureHandle instance_texture{}; // the placeholder for a clear, the real one to stamp
        std::array<std::uint32_t, 3> groups{1, 1, 1};
        bool is_stamp = false;
    };

    std::vector<Dispatch> dispatches;

    std::uint32_t job_count = 0;
    for (const LevelWork& w : work)
        job_count += 1 + static_cast<std::uint32_t>(w.stampers.size());
    ensure_job_capacity(job_count);

    std::vector<std::byte> staging(static_cast<std::size_t>(job_count) * kComposeStride,
                                   std::byte{0});
    std::uint32_t slot = 0;
    const auto write_job = [&](const GpuComposeJob& job) {
        std::memcpy(&staging[static_cast<std::size_t>(slot) * kComposeStride], &job, sizeof(job));
    };

    for (const LevelWork& w : work) {
        const Level& lvl = levels_[w.level];

        const VoxelRange clear_range = voxel_range_for(lvl.info, w.region);
        if (!clear_range.empty()) {
            GpuComposeJob job{};
            job.level_origin_voxel[0] = lvl.info.origin.x;
            job.level_origin_voxel[1] = lvl.info.origin.y;
            job.level_origin_voxel[2] = lvl.info.origin.z;
            job.level_origin_voxel[3] = lvl.info.voxel_size;
            job.voxel_offset[0] = clear_range.lo[0];
            job.voxel_offset[1] = clear_range.lo[1];
            job.voxel_offset[2] = clear_range.lo[2];
            job.region_size[0] = clear_range.size[0];
            job.region_size[1] = clear_range.size[1];
            job.region_size[2] = clear_range.size[2];
            job.inst_resolution_mode[3] = 0; // clear
            job.params[0] = lvl.info.band;
            write_job(job);
            dispatches.push_back(
                {w.level, slot, placeholder_instance_sdf_, group_counts(clear_range.size), false});
            ++slot;
            ++stats_.clears;
        }

        for (const TrackedInstance* inst : w.stampers) {
            WorldAabb stamp_region{};
            if (!intersect(w.region, inst->world_bounds, stamp_region))
                continue; // the coarse overlap test above can pass while the exact box misses
            const VoxelRange range = voxel_range_for(lvl.info, stamp_region);
            if (range.empty())
                continue;

            GpuComposeJob job{};
            job.local_from_world = inst->local_from_world;
            job.inst_origin_voxel[0] = inst->grid_origin.x;
            job.inst_origin_voxel[1] = inst->grid_origin.y;
            job.inst_origin_voxel[2] = inst->grid_origin.z;
            job.inst_origin_voxel[3] = inst->voxel_size;
            job.inst_resolution_mode[0] = inst->resolution[0];
            job.inst_resolution_mode[1] = inst->resolution[1];
            job.inst_resolution_mode[2] = inst->resolution[2];
            job.inst_resolution_mode[3] = 1; // stamp
            job.level_origin_voxel[0] = lvl.info.origin.x;
            job.level_origin_voxel[1] = lvl.info.origin.y;
            job.level_origin_voxel[2] = lvl.info.origin.z;
            job.level_origin_voxel[3] = lvl.info.voxel_size;
            job.voxel_offset[0] = range.lo[0];
            job.voxel_offset[1] = range.lo[1];
            job.voxel_offset[2] = range.lo[2];
            job.region_size[0] = range.size[0];
            job.region_size[1] = range.size[1];
            job.region_size[2] = range.size[2];
            job.params[0] = lvl.info.band;
            job.params[1] = inst->uniform_scale;
            job.params[2] = inst->max_abs_distance;
            write_job(job);
            dispatches.push_back({w.level, slot, inst->gpu_sdf, group_counts(range.size), true});
            ++slot;
            ++stats_.stamps;
        }
    }

    if (slot == 0)
        return; // every region clipped away to nothing (float roundoff at a shared boundary) —
                // nothing was written to `staging`, nothing to declare

    device_.write_buffer(
        compose_ubo_, staging.data(), static_cast<std::size_t>(slot) * kComposeStride);

    // ── Pass 3: declare the graph passes ───────────────────────────────────────────────────────
    // Import each touched level EXACTLY ONCE this frame; every dispatch against it below reuses
    // that SAME RGTexture, so the graph's write-after-write edges (declared order breaks ties,
    // RenderGraph::compile()) serialize them in declaration order. Building `dispatches` level by
    // level, clear-before-stamps within each level, is what makes "every stamp observes the clear,
    // never the reverse" a property of DECLARATION ORDER rather than something the shader has to
    // defend against.
    std::array<RGTexture, kSdfClipmapLevels> level_rg{};
    for (const LevelWork& w : work) {
        Level& lvl = levels_[w.level];
        level_rg[w.level] = graph.import_texture(lvl.info.texture, lvl.texture_state);
    }

    for (const Dispatch& d : dispatches) {
        const RGTexture rg = level_rg[d.level];
        const rhi::TextureHandle level_texture = levels_[d.level].info.texture;
        const rhi::TextureHandle instance_texture = d.instance_texture;
        const std::uint32_t offset = d.slot * kComposeStride;
        const std::array<std::uint32_t, 3> groups = d.groups;
        const RGTexture writes[] = {rg};
        RenderGraph::ComputePassDesc desc{};
        desc.storage_write = writes;
        graph.add_compute_pass(
            d.is_stamp ? "sdf-clipmap-stamp" : "sdf-clipmap-clear",
            desc,
            [this, level_texture, instance_texture, offset, groups](rhi::CommandBuffer& cmd) {
                cmd.bind_compute_pipeline(compose_pipeline_);
                cmd.bind_storage_image(0, level_texture);
                cmd.bind_texture(1, instance_texture, instance_sampler_);
                cmd.bind_uniform_buffer(2, compose_ubo_, offset, sizeof(GpuComposeJob));
                cmd.dispatch(groups[0], groups[1], groups[2]);
            });
    }

    for (const LevelWork& w : work)
        levels_[w.level].texture_state = rhi::ResourceState::StorageReadWrite;
    stats_.dirty_regions = static_cast<std::uint32_t>(work.size());
}

} // namespace rime::render
