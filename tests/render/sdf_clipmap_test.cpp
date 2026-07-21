// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Structural proofs for the runtime SDF clipmap (m10.4b, ADR-0032 §2 + §10). No golden images —
// every claim is either a pure CPU/GPU-agreement check or a GPU-computed sphere-trace hit distance
// compared against an analytic formula, with a documented margin:
//
//  (1) Analytic agreement: compose one sphere and one box (hand-built analytically-exact fields —
//      this brick tests the CLIPMAP, not the m10.4a cooker, which already proves the cook itself
//      in tools/asset-pipeline/src/sdf.rs), sphere-trace straight-line rays at each, and check the
//      hit distance against the one-line analytic formula within (voxel size + band error).
//
//  (2) THE HEADLINE — the re-stamp proof: a ray hits a box. Remove the instance and invalidate()
//      its region (the C2 destruction hook). Recompose. The same ray now passes through — "break a
//      wall between the ray and whatever is behind it, and the field lets the ray through" as an
//      executable assertion.
//
//  (3) Dirty economy (ADR-0032 §11, "idle work is a bug"): a static scene of many small,
//      non-overlapping instances settles to ZERO stamps after warmup; destroying ONE of them
//      recomposes only its own small neighbourhood — a hard bound against the full recompose the
//      warmup needed, never a global rebuild.
//
//  (4) Snap stability: nudging the camera well under level 0's own voxel causes zero
//      recomposition (never any shimmer risk); a move past level 0's own voxel but under level 1's
//      recomposes ONLY level 0 (each level snaps to its OWN grid independently); a move past a
//      level's whole extent forces a full recompose.
//
// Shares nothing with pbr_pipeline/shadow_test's HDR-image harness — this brick's proof is
// entirely compute + buffer readback (a sphere-trace hit distance, not a pixel), so it drives the
// device directly rather than through render_test_support.hpp's pixel helpers (only its
// vulkan_required() is reused).

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/sdf_clipmap.hpp"
#include "rime/render/render_graph.hpp"
#include "sdf_probe_trace.comp.spv.h"

using namespace rime;
using namespace rime::render;
using rime::render::test::vulkan_required;

namespace {

// ── Hand-built, analytically-exact SDFs ──────────────────────────────────────────────────────
// This brick tests SdfClipmap's compose/sample math, not the m10.4a cooker (already proven
// against these same two formulas in tools/asset-pipeline/src/sdf.rs) — so the fields are
// evaluated directly at each voxel centre rather than cooked from a triangle mesh.

// Inigo Quilez's box SDF (docs/math/sdf.md's verification pins cite the same formula): the
// "outside" term is the length of the positive part of |p|-h, the "inside" term is the
// least-negative axis of |p|-h clamped to <= 0.
float analytic_box_distance(core::Vec3 p, core::Vec3 h) {
    const core::Vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
    const core::Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    const float outside_len =
        std::sqrt(outside.x * outside.x + outside.y * outside.y + outside.z * outside.z);
    const float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
    return outside_len + inside;
}

struct GridSpec {
    core::Vec3 origin{};
    float voxel_size = 0.0f;
    std::array<std::uint32_t, 3> resolution{};
};

// voxel_size fixed from the longest unpadded axis (mirroring the m10.4a cooker's own policy, see
// docs/math/sdf.md §4), padded by `padding_voxels` on every side so the field stays defined a
// little past the shape's own surface.
GridSpec
make_grid(core::Vec3 half_extents, std::uint32_t target_resolution, std::uint32_t padding_voxels) {
    const float longest = std::max({half_extents.x, half_extents.y, half_extents.z}) * 2.0f;
    const float voxel_size = longest / static_cast<float>(target_resolution);
    const float pad = static_cast<float>(padding_voxels) * voxel_size;
    const float half[3] = {half_extents.x, half_extents.y, half_extents.z};
    std::uint32_t res[3] = {0, 0, 0};
    float origin[3] = {0.0f, 0.0f, 0.0f};
    for (int a = 0; a < 3; ++a) {
        const float padded_extent = 2.0f * half[a] + 2.0f * pad;
        res[a] = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil(padded_extent / voxel_size)), 4u);
        origin[a] = -0.5f * static_cast<float>(res[a]) * voxel_size;
    }
    GridSpec g;
    g.voxel_size = voxel_size;
    g.origin = {origin[0], origin[1], origin[2]};
    g.resolution = {res[0], res[1], res[2]};
    return g;
}

assets::MeshSdfAsset build_box_sdf(core::Vec3 half_extents, std::uint32_t target_resolution = 24) {
    const GridSpec g = make_grid(half_extents, target_resolution, 2);
    assets::MeshSdfAsset sdf;
    sdf.grid_origin = g.origin;
    sdf.voxel_size = g.voxel_size;
    sdf.resolution = g.resolution;
    sdf.local_bounds =
        assets::Aabb{core::Vec3{-half_extents.x, -half_extents.y, -half_extents.z}, half_extents};
    sdf.distances.resize(sdf.voxel_count());
    float max_abs = 0.0f;
    for (std::uint32_t kz = 0; kz < g.resolution[2]; ++kz) {
        for (std::uint32_t jy = 0; jy < g.resolution[1]; ++jy) {
            for (std::uint32_t ix = 0; ix < g.resolution[0]; ++ix) {
                const core::Vec3 p{g.origin.x + (static_cast<float>(ix) + 0.5f) * g.voxel_size,
                                   g.origin.y + (static_cast<float>(jy) + 0.5f) * g.voxel_size,
                                   g.origin.z + (static_cast<float>(kz) + 0.5f) * g.voxel_size};
                const float d = analytic_box_distance(p, half_extents);
                sdf.distances[sdf.index(ix, jy, kz)] = d;
                max_abs = std::max(max_abs, std::fabs(d));
            }
        }
    }
    sdf.max_abs_distance = max_abs;
    return sdf;
}

assets::MeshSdfAsset build_sphere_sdf(float radius, std::uint32_t target_resolution = 24) {
    const core::Vec3 he{radius, radius, radius};
    const GridSpec g = make_grid(he, target_resolution, 2);
    assets::MeshSdfAsset sdf;
    sdf.grid_origin = g.origin;
    sdf.voxel_size = g.voxel_size;
    sdf.resolution = g.resolution;
    sdf.local_bounds = assets::Aabb{core::Vec3{-radius, -radius, -radius}, he};
    sdf.distances.resize(sdf.voxel_count());
    float max_abs = 0.0f;
    for (std::uint32_t kz = 0; kz < g.resolution[2]; ++kz) {
        for (std::uint32_t jy = 0; jy < g.resolution[1]; ++jy) {
            for (std::uint32_t ix = 0; ix < g.resolution[0]; ++ix) {
                const core::Vec3 p{g.origin.x + (static_cast<float>(ix) + 0.5f) * g.voxel_size,
                                   g.origin.y + (static_cast<float>(jy) + 0.5f) * g.voxel_size,
                                   g.origin.z + (static_cast<float>(kz) + 0.5f) * g.voxel_size};
                const float d = core::length(p) - radius;
                sdf.distances[sdf.index(ix, jy, kz)] = d;
                max_abs = std::max(max_abs, std::fabs(d));
            }
        }
    }
    sdf.max_abs_distance = max_abs;
    return sdf;
}

// ── The GPU probe tracer: sdf_probe_trace.comp, driven directly (no RenderGraph — nothing else
// in these tests reads the clipmap textures, so there is no producer/consumer ordering for a
// graph to own; a plain command buffer after the compose graph's own submit_blocking() has
// completed is simpler and exactly as correct). ─────────────────────────────────────────────────
struct GpuRay {
    float origin_maxdist[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz origin, w max trace distance
    float dir_pad[4] = {0.0f, 0.0f, 0.0f, 0.0f};        // xyz direction (need not be unit)
};

GpuRay ray_along_minus_z(float x0, float z0, float max_dist) {
    GpuRay r;
    r.origin_maxdist[0] = x0;
    r.origin_maxdist[2] = z0;
    r.origin_maxdist[3] = max_dist;
    r.dir_pad[2] = -1.0f;
    return r;
}

class ProbeTracer {
public:
    explicit ProbeTracer(rhi::Device& device) : device_(device) {
        rhi::ShaderDesc sd{};
        sd.stage = rhi::ShaderStage::Compute;
        sd.spirv = sdf_probe_trace_comp_spv;
        sd.spirv_size_bytes = sizeof(sdf_probe_trace_comp_spv);
        sd.debug_name = "sdf_probe_trace.comp";
        shader_ = device.create_shader(sd);

        const rhi::BindingDesc bindings[] = {
            {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
            {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
            {2, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Compute},
            {3, rhi::BindingType::UniformBuffer, rhi::StageMask::Compute},
            {4, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
            {5, rhi::BindingType::StorageBuffer, rhi::StageMask::Compute},
        };
        rhi::ComputePipelineDesc pd{};
        pd.shader = shader_;
        pd.bindings = bindings;
        pd.debug_name = "sdf-probe-trace";
        pipeline_ = device.create_compute_pipeline(pd);

        rhi::SamplerDesc smd{};
        smd.mag_filter = rhi::Filter::Linear;
        smd.min_filter = rhi::Filter::Linear;
        smd.address_mode = rhi::AddressMode::ClampToEdge;
        smd.debug_name = "sdf-probe-sampler";
        sampler_ = device.create_sampler(smd);
    }

    ~ProbeTracer() {
        device_.destroy(sampler_);
        device_.destroy(pipeline_);
        device_.destroy(shader_);
    }

    ProbeTracer(const ProbeTracer&) = delete;
    ProbeTracer& operator=(const ProbeTracer&) = delete;

    // `out_trace_ms`, when given, receives the GPU time of the dispatch ALONE — bracketed by
    // timestamps rather than wall-clocked around submit_blocking, so buffer creation, the upload
    // and the readback stay out of the number. That isolation is the whole point for the GI spike
    // below: what it needs to know is the cost of the sphere-marching itself.
    std::vector<float> trace(const SdfClipmap& clipmap,
                             const std::vector<GpuRay>& rays,
                             double* out_trace_ms = nullptr) {
        rhi::BufferDesc lb{};
        lb.size = sizeof(GpuSdfClipmapLevels);
        lb.usage = rhi::BufferUsage::Uniform;
        lb.memory = rhi::MemoryUsage::CpuToGpu;
        lb.debug_name = "sdf-probe-levels";
        const rhi::BufferHandle levels_ubo = device_.create_buffer(lb);
        const GpuSdfClipmapLevels levels = clipmap.gpu_levels();
        device_.write_buffer(levels_ubo, &levels, sizeof(levels));

        rhi::BufferDesc rb{};
        rb.size = rays.size() * sizeof(GpuRay);
        rb.usage = rhi::BufferUsage::Storage;
        rb.memory = rhi::MemoryUsage::CpuToGpu;
        rb.debug_name = "sdf-probe-rays";
        const rhi::BufferHandle rays_buf = device_.create_buffer(rb);
        device_.write_buffer(rays_buf, rays.data(), rb.size);

        rhi::BufferDesc hb{};
        hb.size = rays.size() * sizeof(float);
        hb.usage = rhi::BufferUsage::Storage;
        hb.memory = rhi::MemoryUsage::GpuToCpu;
        hb.debug_name = "sdf-probe-hits";
        const rhi::BufferHandle hits_buf = device_.create_buffer(hb);

        auto cmd = device_.begin_commands();
        cmd->bind_compute_pipeline(pipeline_);
        cmd->bind_texture(0, clipmap.level(0).texture, sampler_);
        cmd->bind_texture(1, clipmap.level(1).texture, sampler_);
        cmd->bind_texture(2, clipmap.level(2).texture, sampler_);
        cmd->bind_uniform_buffer(3, levels_ubo);
        cmd->bind_storage_buffer(4, rays_buf);
        cmd->bind_storage_buffer(5, hits_buf);
        cmd->write_timestamp(0);
        cmd->dispatch(static_cast<std::uint32_t>((rays.size() + 31) / 32), 1, 1);
        cmd->write_timestamp(1);
        device_.submit_blocking(*cmd);

        if (out_trace_ms != nullptr) {
            std::array<std::uint64_t, 2> stamps{};
            *out_trace_ms = cmd->read_timestamps(stamps)
                                ? static_cast<double>(stamps[1] - stamps[0]) / 1e6
                                : 0.0;
        }

        std::vector<float> hits(rays.size());
        device_.read_buffer(hits_buf, hits.data(), hits.size() * sizeof(float), 0);

        device_.destroy(hits_buf);
        device_.destroy(rays_buf);
        device_.destroy(levels_ubo);
        return hits;
    }

private:
    rhi::Device& device_;
    rhi::ShaderHandle shader_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
};

// Declare-execute-submit one add() call and hand back the stats it produced.
SdfClipmapStats step(rhi::Device& device, SdfClipmap& clipmap, core::Vec3 camera_pos) {
    RenderGraph graph(device);
    graph.reset();
    clipmap.add(graph, camera_pos);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return clipmap.stats();
}

} // namespace

TEST_CASE("sdf clipmap: a composed sphere and box sphere-trace to the analytic distance (m10.4b)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the SDF clipmap analytic proof");
        return;
    }

    SdfClipmap clipmap(*device);
    ProbeTracer tracer(*device);

    const float radius = 1.0f;
    const core::Vec3 box_half{0.5f, 0.75f, 0.4f};
    const core::Vec3 box_pos{3.0f, 0.0f, 0.0f};

    clipmap.update_instance(1, build_sphere_sdf(radius), core::identity());
    clipmap.update_instance(2, build_box_sdf(box_half), core::mat4_translation(box_pos));

    const SdfClipmapStats warm = step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});
    REQUIRE(warm.stamps >= 2); // both instances reached at least one level

    // Level 0's own voxel size bounds the discretization/trilinear error; the marching stop
    // threshold (0.001, sdf_probe_trace.comp) is far smaller. 2 voxels is the same margin the Rust
    // cooker's own analytic tests use (tools/asset-pipeline/src/sdf.rs); a small slice of the
    // level's own band covers any residual roughness right at a saturation boundary — the brief's
    // "(voxel size + band error)" headroom.
    const SdfClipmap::LevelInfo& l0 = clipmap.level(0);
    const float tol = 2.0f * l0.voxel_size + 0.05f * l0.band;

    // Two straight-in rays along -z: their analytic hit distance is a one-line formula, so this
    // test needs no general ray/shape intersector of its own.
    std::vector<GpuRay> rays = {ray_along_minus_z(0.0f, 5.0f, 10.0f),
                                ray_along_minus_z(box_pos.x, 5.0f, 10.0f)};
    const std::vector<float> hits = tracer.trace(clipmap, rays);
    REQUIRE(hits.size() == 2);

    const float analytic_sphere_hit = 5.0f - radius;
    const float analytic_box_hit = 5.0f - box_half.z; // the box's nearest (+z) face
    MESSAGE("sphere hit " << hits[0] << " (analytic " << analytic_sphere_hit << "), box hit "
                          << hits[1] << " (analytic " << analytic_box_hit << "), tol " << tol);
    CHECK(hits[0] > 0.0f);
    CHECK(std::fabs(hits[0] - analytic_sphere_hit) <= tol);
    CHECK(hits[1] > 0.0f);
    CHECK(std::fabs(hits[1] - analytic_box_hit) <= tol);
}

TEST_CASE("sdf clipmap: remove + invalidate recomposes, and the ray that hit now passes through "
          "(m10.4b, the headline)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the re-stamp proof");
        return;
    }

    SdfClipmap clipmap(*device);
    ProbeTracer tracer(*device);

    const core::Vec3 box_half{0.5f, 0.5f, 0.5f};
    constexpr std::uint64_t kId = 42;
    clipmap.update_instance(kId, build_box_sdf(box_half), core::identity());
    (void)step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});

    const std::vector<GpuRay> rays = {ray_along_minus_z(0.0f, 3.0f, 10.0f)};
    const std::vector<float> before = tracer.trace(clipmap, rays);
    REQUIRE(before.size() == 1);
    MESSAGE("before removal: hit at " << before[0]);
    CHECK(before[0] > 0.0f); // the box blocks the ray
    CHECK(before[0] < 10.0f);

    // Remove the instance AND explicitly invalidate its region — remove_instance() already does
    // this internally (see its header comment), but calling invalidate() too exercises the public
    // C2 seam directly, exactly as a destruction event's own world-space AABB would drive it
    // (independent of whether the caller also happens to deregister the SDF instance itself).
    clipmap.remove_instance(kId);
    clipmap.invalidate(WorldAabb{{-2.0f, -2.0f, -2.0f}, {2.0f, 2.0f, 2.0f}});
    const SdfClipmapStats after_remove = step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});
    CHECK(after_remove.clears > 0);  // the region genuinely recomposed
    CHECK(after_remove.stamps == 0); // nothing left to stamp there

    const std::vector<float> after = tracer.trace(clipmap, rays);
    REQUIRE(after.size() == 1);
    MESSAGE("after removal: hit at " << after[0]);
    CHECK(after[0] < 0.0f); // -1: the ray now passes all the way through
}

TEST_CASE("sdf clipmap: a static scene settles to zero stamps, and a small removal recomposes a "
          "small fraction (m10.4b, ADR-0032 §11)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the dirty-economy proof");
        return;
    }

    SdfClipmap clipmap(*device);

    // 16 small boxes on a 4x4 grid, spaced 1.5 m apart — comfortably farther than any two
    // instances' own (padded) footprints (well under half a metre each), so no instance's world
    // bounds overlaps another's, and all 16 sit inside every level's volume around the camera.
    constexpr int kCount = 16;
    const core::Vec3 half{0.15f, 0.15f, 0.15f};
    const assets::MeshSdfAsset box_sdf = build_box_sdf(half, 12);
    for (int i = 0; i < kCount; ++i) {
        const float x = -3.0f + 1.5f * static_cast<float>(i % 4);
        const float y = -3.0f + 1.5f * static_cast<float>(i / 4);
        clipmap.update_instance(
            static_cast<std::uint64_t>(i + 1), box_sdf, core::mat4_translation({x, y, 0.0f}));
    }

    // The first-ever add() is a full recompose of all 3 levels: every instance reaches every one
    // of them (all comfortably inside even level 0's 8 m volume), so 16 instances x 3 levels.
    const SdfClipmapStats warm = step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});
    MESSAGE("full recompose: " << warm.stamps << " stamps, " << warm.clears << " clears");
    CHECK(warm.levels_recomposed == 3);
    CHECK(warm.clears == 3);
    CHECK(warm.stamps == kCount * 3);

    // Same camera, nothing changed: ADR-0032 §11's "idle work is a bug", made structural.
    const SdfClipmapStats idle = step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});
    CHECK(idle.levels_recomposed == 0);
    CHECK(idle.clears == 0);
    CHECK(idle.stamps == 0);
    CHECK(idle.dirty_regions == 0);

    // Destroy ONE small object. Its own world bounds do not touch any of its 15 neighbours, so
    // only a clear happens where it used to be — a hard bound against the 48-stamp full recompose
    // above (well under the brief's "e.g. <10%": zero is the strictest possible bound).
    clipmap.remove_instance(1);
    const SdfClipmapStats after_small = step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});
    MESSAGE("small removal: " << after_small.stamps << " stamps, " << after_small.clears
                              << " clears (of " << warm.stamps << " stamps / " << warm.clears
                              << " clears during the full recompose)");
    CHECK(after_small.levels_recomposed == 0); // camera never moved
    CHECK(after_small.clears > 0); // SOMETHING recomposed — invalidation was not a no-op
    CHECK(after_small.clears <= kSdfClipmapLevels);
    CHECK(after_small.stamps * 10 < warm.stamps); // << the full-recompose stamp count
}

TEST_CASE("sdf clipmap: texel-snapping — sub-voxel motion recomposes nothing, each level snaps to "
          "its own grid, and a big jump forces a full recompose (m10.4b)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the snap-stability proof");
        return;
    }

    SdfClipmap clipmap(*device);

    const SdfClipmapStats warm = step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});
    CHECK(warm.levels_recomposed == 3); // the first-ever call always fully recomposes every level

    const SdfClipmapStats settled = step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});
    CHECK(settled.levels_recomposed == 0);
    CHECK(settled.clears == 0);

    const float origin0_x = clipmap.level(0).origin.x;
    const float origin1_x = clipmap.level(1).origin.x;
    const float origin2_x = clipmap.level(2).origin.x;

    // (a) Nudge WELL under level 0's own voxel (0.125 m): zero recomposition anywhere.
    const SdfClipmapStats nudged = step(*device, clipmap, core::Vec3{0.01f, 0.0f, 0.0f});
    CHECK(nudged.levels_recomposed == 0);
    CHECK(nudged.clears == 0);
    CHECK(clipmap.level(0).origin.x == origin0_x); // bit-identical — the anti-shimmer property

    // Back to the exact same position the origins above were measured against.
    (void)step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});

    // (b) Move 0.2 m — past level 0's 0.125 m voxel, but (from this exact starting point) not past
    // level 1's 0.5 m or level 2's 2.0 m voxel: each level snaps to ITS OWN grid independently, so
    // only level 0 recomposes. Hand-verified and exact in binary floating point (every voxel size
    // and half-extent here is a power of two): level 0's ideal min goes from -4.0 to -3.8 m,
    // floor(-4.0/0.125) = -32 -> floor(-3.8/0.125) = -31 (different, so it recomposes); level 1's
    // goes from -16.0 to -15.8, floor(-16.0/0.5) = floor(-15.8/0.5) = -32 (unchanged); level 2's
    // from -64.0 to -63.8, floor(-64.0/2.0) = floor(-63.8/2.0) = -32 (unchanged).
    const SdfClipmapStats one_level = step(*device, clipmap, core::Vec3{0.2f, 0.0f, 0.0f});
    CHECK(one_level.levels_recomposed == 1);
    CHECK(clipmap.level(0).origin.x != origin0_x);
    CHECK(clipmap.level(1).origin.x == origin1_x);
    CHECK(clipmap.level(2).origin.x == origin2_x);

    // (c) A big jump — past level 0's WHOLE 8 m extent — forces it to recompose again.
    const float before_jump0_x = clipmap.level(0).origin.x;
    const SdfClipmapStats jumped = step(*device, clipmap, core::Vec3{20.0f, 0.0f, 0.0f});
    CHECK(jumped.levels_recomposed >= 1);
    CHECK(clipmap.level(0).origin.x != before_jump0_x);
}

// ── The GI feasibility spike (ADR-0032 §2) ───────────────────────────────────────────────────────
// The ADR names one critical unknown for the GI half of M10 and refuses to guess at it: "rays per
// second at probe-grid scale on lavapipe. It cannot be answered by reading code; it must be
// measured." This is that measurement. It runs here rather than in a throwaway harness so the
// numbers are reproducible by anyone running the suite, and so they get re-measured for free on
// real hardware at m12.0 — where the same sweep becomes the actual budget rather than a shape.
//
// What it must inform is m10.5's SCOPE, not its architecture: how many probes, how many rays each,
// and whether a full-grid update per frame is affordable or the probes must be cycled over several
// frames (the standard DDGI amortization). Per ADR-0032 §11 the conclusion is expressed as a
// RELATIVE fence — cost against an already-measured pass — because lavapipe runs compute on the CPU
// and its absolute milliseconds mean nothing for a shipping frame.
//
// The interesting variable is not ray count. It is how far a ray MARCHES: a sphere trace that hits
// something close converges in a handful of steps, while one that escapes into open space burns
// every one of its 128 iterations. So the sweep measures both populations, and the honest cost
// model for m10.5 is driven by the miss case.
namespace {

// A small room — floor, four walls, and three pillars — sized to sit inside clipmap level 0's own
// 8 m extent, so every probe and every ray stays in the finest level. That is deliberately the
// EXPENSIVE case to sample (level 0 is where the field is most informative and the marching
// converges tightest); a coarser level would fetch identically but step further per iteration.
void compose_spike_room(SdfClipmap& clipmap) {
    std::uint64_t id = 1;
    const auto place = [&](core::Vec3 half, core::Vec3 at) {
        clipmap.update_instance(id++, build_box_sdf(half, 20), core::mat4_translation(at));
    };
    place({3.5f, 0.2f, 3.5f}, {0.0f, -2.0f, 0.0f}); // floor
    place({3.5f, 2.0f, 0.2f}, {0.0f, 0.0f, -3.5f}); // walls
    place({3.5f, 2.0f, 0.2f}, {0.0f, 0.0f, 3.5f});
    place({0.2f, 2.0f, 3.5f}, {-3.5f, 0.0f, 0.0f});
    place({0.2f, 2.0f, 3.5f}, {3.5f, 0.0f, 0.0f});
    place({0.35f, 1.8f, 0.35f}, {-1.6f, 0.0f, -1.2f}); // pillars
    place({0.35f, 1.8f, 0.35f}, {1.7f, 0.0f, 0.9f});
    place({0.35f, 1.8f, 0.35f}, {0.2f, 0.0f, 2.2f});
}

// Spherical Fibonacci: N directions spread near-uniformly over the sphere with no clustering at the
// poles, from a closed form rather than a rejection loop. This is the standard DDGI ray
// distribution (a real implementation also rotates the set per frame so successive frames sample
// different directions — irrelevant to cost, which is what this measures).
core::Vec3 fibonacci_direction(std::uint32_t i, std::uint32_t n) {
    const float golden = 3.14159265358979f * (3.0f - std::sqrt(5.0f));
    const float y = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
    const float r = std::sqrt(std::max(0.0f, 1.0f - y * y));
    const float theta = golden * static_cast<float>(i);
    return {std::cos(theta) * r, y, std::sin(theta) * r};
}

// One DDGI update's worth of rays: `side`³ probes on a lattice through the room, each casting
// `rays_per_probe` Fibonacci-distributed rays out to `max_dist`.
std::vector<GpuRay>
probe_grid_rays(std::uint32_t side, std::uint32_t rays_per_probe, float max_dist) {
    std::vector<GpuRay> rays;
    rays.reserve(static_cast<std::size_t>(side) * side * side * rays_per_probe);
    const float step_xz = 6.0f / static_cast<float>(side + 1);
    const float step_y = 3.0f / static_cast<float>(side + 1);
    for (std::uint32_t z = 0; z < side; ++z) {
        for (std::uint32_t y = 0; y < side; ++y) {
            for (std::uint32_t x = 0; x < side; ++x) {
                const core::Vec3 p{-3.0f + step_xz * static_cast<float>(x + 1),
                                   -1.5f + step_y * static_cast<float>(y + 1),
                                   -3.0f + step_xz * static_cast<float>(z + 1)};
                for (std::uint32_t r = 0; r < rays_per_probe; ++r) {
                    const core::Vec3 d = fibonacci_direction(r, rays_per_probe);
                    GpuRay ray;
                    ray.origin_maxdist[0] = p.x;
                    ray.origin_maxdist[1] = p.y;
                    ray.origin_maxdist[2] = p.z;
                    ray.origin_maxdist[3] = max_dist;
                    ray.dir_pad[0] = d.x;
                    ray.dir_pad[1] = d.y;
                    ray.dir_pad[2] = d.z;
                    rays.push_back(ray);
                }
            }
        }
    }
    return rays;
}

} // namespace

TEST_CASE("gi spike: sphere-trace cost at DDGI probe-grid scale (ADR-0032 §2, shape only)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the GI feasibility spike");
        return;
    }

    constexpr std::uint32_t kRaysPerProbe = 64; // the DDGI norm
    constexpr float kMaxDist = 8.0f;            // level 0's full extent

    SdfClipmap clipmap(*device);
    ProbeTracer tracer(*device);
    compose_spike_room(clipmap);
    (void)step(*device, clipmap, core::Vec3{0.0f, 0.0f, 0.0f});

    // Warm up before measuring anything. The FIRST dispatch through a freshly created compute
    // pipeline pays lavapipe's shader JIT, which showed up as the smallest grid appearing 40x
    // dearer per ray than the largest — a measurement artefact, not a cost curve.
    (void)tracer.trace(clipmap, probe_grid_rays(2, 8, kMaxDist));

    // (a) A furnished room: the realistic population, where most rays terminate on a wall.
    //
    // Two clocks, on purpose. `trace_ms` is the GPU timestamp pair around the dispatch alone;
    // `wall_ms` is CPU time across the whole submit. A spike that trusts a single clock is how you
    // publish a number that is off by an order of magnitude — the two should bracket each other
    // (wall > GPU, by the buffer create/upload/readback this measurement deliberately excludes).
    for (const std::uint32_t side : {4u, 6u, 8u}) {
        const std::uint32_t probes = side * side * side;
        const std::vector<GpuRay> rays = probe_grid_rays(side, kRaysPerProbe, kMaxDist);
        double trace_ms = 0.0;
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<float> hits = tracer.trace(clipmap, rays, &trace_ms);
        const double wall_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        const auto misses = static_cast<std::size_t>(
            std::count_if(hits.begin(), hits.end(), [](float t) { return t < 0.0f; }));
        const double ns_per_ray = trace_ms * 1e6 / static_cast<double>(rays.size());
        MESSAGE(probes << " probes x " << kRaysPerProbe << " rays = " << rays.size()
                       << " rays: " << trace_ms << " ms gpu / " << wall_ms << " ms wall ("
                       << ns_per_ray << " ns/ray), " << misses << " escaped ("
                       << 100.0 * static_cast<double>(misses) / static_cast<double>(hits.size())
                       << "%)");
        CHECK(misses < hits.size()); // an enclosed room: rays must mostly terminate
    }

    // (b) The same grid against an EMPTY field — every ray exhausts its step budget. This is the
    //     worst case a scene can present, and therefore the number m10.5's budget must be built on:
    //     an open sky, a probe grid outdoors, or the instant after a wall is destroyed all look
    //     like this.
    SdfClipmap empty(*device);
    (void)step(*device, empty, core::Vec3{0.0f, 0.0f, 0.0f});
    const std::vector<GpuRay> rays = probe_grid_rays(8, kRaysPerProbe, kMaxDist);
    double empty_ms = 0.0;
    const std::vector<float> empty_hits = tracer.trace(empty, rays, &empty_ms);
    CHECK(std::all_of(empty_hits.begin(), empty_hits.end(), [](float t) { return t < 0.0f; }));
    MESSAGE("empty field, " << rays.size() << " rays: " << empty_ms << " ms ("
                            << empty_ms * 1e6 / static_cast<double>(rays.size())
                            << " ns/ray, every ray marches to max_dist)");

    // (c) GRAZING rays — the actual worst case, and not the one you would guess. An empty field is
    //     cheap because the narrow band puts a FLOOR under the step size: every sample out in open
    //     space reads the saturated value and advances a full `band` (4 voxels = 0.5 m at level 0),
    //     so an 8 m trace through nothing costs ~16 steps, not 128. What actually burns the step
    //     budget is a ray running parallel to a surface and just above it: the field stays small,
    //     so every step is small, and the march runs to its iteration cap without ever converging.
    //     This is the number m10.5's budget has to be built on — it is the ceiling, and unlike the
    //     other two it is a property of the trace, not of how full the scene happens to be.
    std::vector<GpuRay> grazing;
    grazing.reserve(rays.size());
    for (std::size_t i = 0; i < rays.size(); ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(rays.size());
        GpuRay g;
        g.origin_maxdist[0] = -3.0f;
        g.origin_maxdist[1] = -1.75f + 0.02f * std::sin(u * 40.0f); // a hair above the floor slab
        g.origin_maxdist[2] = -3.0f + 6.0f * u;
        g.origin_maxdist[3] = kMaxDist;
        g.dir_pad[0] = 1.0f;
        g.dir_pad[1] = 0.004f; // very slightly off-parallel: skims without ever quite hitting
        grazing.push_back(g);
    }
    double grazing_ms = 0.0;
    const std::vector<float> grazing_hits = tracer.trace(clipmap, grazing, &grazing_ms);
    const auto grazing_misses = static_cast<std::size_t>(
        std::count_if(grazing_hits.begin(), grazing_hits.end(), [](float t) { return t < 0.0f; }));
    MESSAGE("grazing rays, " << grazing.size() << " rays: " << grazing_ms << " ms ("
                             << grazing_ms * 1e6 / static_cast<double>(grazing.size())
                             << " ns/ray), " << grazing_misses << " ran out of steps");
    // The population really is pathological — otherwise this measures nothing interesting and the
    // "3.5x" conclusion in docs/design/gi-spike.md is measuring the wrong thing.
    CHECK(grazing_misses > 0);
}
