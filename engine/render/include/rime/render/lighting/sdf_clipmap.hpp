// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "rime/assets/sdf_asset.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/local_shadows.hpp" // WorldAabb
#include "rime/render/render_graph.hpp"

// The runtime SDF clipmap (m10.4b, ADR-0032 §2 + §10): the GLOBAL signed-distance field the m10.5
// DDGI probes will sphere-trace through, composed each frame from the cooked PER-MESH/PER-PART
// fields m10.4a produces (engine/assets/include/rime/assets/sdf_asset.hpp). "Composed" means
// nested volumes centred on the camera (a CLIPMAP — see docs/math/sdf.md), each a 3-D R16Snorm
// storage image, filled by a compute pass that stamps each tracked instance's own cooked field
// into it by MIN-BLENDING (§ below). This header builds the field; it does not trace it — m10.5
// owns the probes, and this brick does not touch pbr_forward_shadowed.frag at all.
//
// The point of the whole brick is the DIRTY TRACKING, not the compose math (which is a few lines
// of trilinear sampling): a static scene must cost nothing after warmup, and a single destroyed
// object must recompose only the region it could possibly have affected — never the whole volume.
// `invalidate()` mirrors LocalShadowMap's C2 hook exactly (lighting/local_shadows.hpp); moving or
// removing a tracked instance drives the analogous C1 seam through `update_instance`/
// `remove_instance`. See docs/math/sdf.md for the levels, the narrow-band encoding, the snapping
// scheme, and why min-blend composes safely across many incremental updates.
namespace rime::render {

// ── The clipmap's shape (pre-decided, ADR-0032 §10 — "3 nested levels, 64³ each, R16Snorm") ─────

// Every level is a cube of this many voxels on a side. Compile-time (not a LightingSettings
// field): it sizes the storage image, the compose shader's bounds math, and the GPU-facing level
// struct below — a runtime-tunable resolution would need all three to agree from one source,
// which is worth doing only if a profile ever asks for it (the same reasoning kClusterGridX etc.
// use in lighting/settings.hpp).
inline constexpr std::uint32_t kSdfClipmapResolution = 64;

// Three levels: near/fine, mid, far/coarse.
inline constexpr std::uint32_t kSdfClipmapLevels = 3;

// Level 0's voxel size; level i is kSdfClipmapLevelScale× coarser than level i-1. 0.125 m / 0.5 m /
// 2.0 m ⇒ 8 m / 32 m / 128 m of world coverage per level (voxel_size × kSdfClipmapResolution) — see
// docs/math/sdf.md for the derivation (matched against a destructible part's own coarse-preset
// voxel size, m10.4a) and the honest statement of what a scene bigger than 128 m across means for
// level 2's traceable range.
inline constexpr float kSdfClipmapBaseVoxelSize = 0.125f;
inline constexpr std::uint32_t kSdfClipmapLevelScale = 4;

// The narrow band half-width for a level, as a multiple of ITS OWN voxel size (docs/math/sdf.md's
// "why 4"): R16Snorm only ever stores clamp(distance / band, -1, 1), so anything farther than
// `band` away reads as the same saturated value — the compose shader treats that value as "at
// least band away, direction unknown" and a sphere-trace against it takes a conservative,
// band-sized step rather than the (unknowable) true distance.
inline constexpr float kSdfClipmapBandVoxels = 4.0f;

// ── The GPU-facing shape of one level (m10.5/m10.7's sampling contract) ──────────────────────────
// What a shader needs to turn a WORLD-space point into a lookup in one level's 3-D texture and
// decode the fetched texel back into a real distance: `origin` is voxel (0,0,0)'s world-space
// corner (texel-snapped every frame — see add()), `extent` is the level's full world-space size
// (kSdfClipmapResolution × voxel_size) so `uvw = (world_pos - origin) / extent` is the [0,1]³
// texture lookup with no division by a second field, and `band` is the decode scale
// (`true_distance = texture(level, uvw).r * band`). Every member is a vec4-sized chunk so C++'s
// natural layout agrees with std140/std430 by construction, the same discipline as
// GpuClusterUniforms/GpuShadowUniforms.
struct GpuSdfLevel {
    float origin_extent[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz = world origin, w = world extent
    float band_voxel[4] = {0.0f, 0.0f, 0.0f, 0.0f};    // x = band, y = voxel size, z/w unused
};

// Every level, in near-to-far order — the whole block a probe-tracing shader needs to walk levels
// from finest to coarsest (lighting/sdf_clipmap.hpp's SdfClipmap::gpu_levels() packs this).
struct GpuSdfClipmapLevels {
    GpuSdfLevel levels[kSdfClipmapLevels];
};

static_assert(sizeof(GpuSdfLevel) == 32, "GpuSdfLevel must be two tightly-packed vec4s");
static_assert(sizeof(GpuSdfClipmapLevels) == kSdfClipmapLevels * 32,
              "GpuSdfClipmapLevels must be a flat array of GpuSdfLevel with no padding");

// How much work the last add() did — the tests assert on this directly (ADR-0032 §11: "idle work
// is a bug"). `stamps`/`clears` are compute dispatches issued this frame; `dirty_regions` is how
// many (level, recompose-region) pairs had ANY work; `levels_recomposed` counts levels whose
// texel-snapped origin changed (forcing that level's ENTIRE volume to recompose — see add()).
struct SdfClipmapStats {
    std::uint32_t stamps = 0;
    std::uint32_t clears = 0;
    std::uint32_t dirty_regions = 0;
    std::uint32_t levels_recomposed = 0;
};

// Owns the clipmap's GPU resources (three persistent 3-D R16Snorm storage images, the compose
// pipeline, the per-dispatch uniform slab, a placeholder instance texture for the off/clear path)
// and the CPU-side bookkeeping that turns "an instance moved/appeared/disappeared" or "the C2
// destruction channel fired" into the minimal set of compose dispatches a frame actually needs.
// One per SceneRenderer, gated by LightingSettings::sdf_clipmap_enabled (default false — off is the
// byte-identical M5.6/ADR-0022 baseline, ADR-0032 §11).
class SdfClipmap {
public:
    explicit SdfClipmap(rhi::Device& device);
    ~SdfClipmap();

    SdfClipmap(const SdfClipmap&) = delete;
    SdfClipmap& operator=(const SdfClipmap&) = delete;

    // Register or refresh a composed instance: `id` is any stable key the caller controls (an
    // entity id works well), `sdf` is its cooked field (m10.4a; COPIED into a GPU texture here —
    // the caller need not keep `sdf` alive past this call), and `world_from_local` places it.
    //
    // Marks BOTH the region the instance is LEAVING (its previous world bounds, if this id was
    // already tracked) and the region it is ARRIVING in as dirty — the C1 seam: a caller that
    // walks `for_each_changed<WorldTransform>`/`for_each_changed<MeshRef>` and calls this per
    // change gets exactly the "what moved" invalidation the ADR's C1 contract promises, with no
    // further bookkeeping of its own.
    //
    // UNIFORM SCALE ONLY: a signed distance is only a distance if every axis scales alike (a
    // non-uniform scale warps the field's isosurfaces without warping the stored NUMBER, so
    // sphere-tracing through it can step too far or too little — silently). A non-uniform
    // `world_from_local` is DETECTED and WARNED about (once), and the instance is excluded from
    // the field entirely (as if `remove_instance` had been called) rather than composed wrong —
    // ADR-0032 v1's explicit "detect it and warn; do not silently produce a wrong field" rule.
    void update_instance(std::uint64_t id,
                         const assets::MeshSdfAsset& sdf,
                         const core::Mat4& world_from_local);

    // Stop composing a tracked instance (a despawn, or a destructible part that died): marks its
    // last known world bounds dirty so the field forgets it, then forgets the instance itself.
    // A no-op (not an error) if `id` was never tracked, or was excluded for non-uniform scale.
    void remove_instance(std::uint64_t id);

    // The C2 destruction hook — identical contract to LocalShadowMap::invalidate(): mark every
    // level's overlap with `region` dirty, regardless of which (if any) tracked instance caused
    // it. This is what makes "break a wall whose SDF this clipmap never even knew about" still
    // correctly recompose the region — e.g. a destructible whose PARTS are not (yet) individually
    // registered as clipmap instances, but whose destruction event's world-space AABB still says
    // "something here changed." Idempotent; safe to call many times per frame as events drain.
    void invalidate(const WorldAabb& region);

    // Recentre each level on `camera_pos` (texel-snapping the move so sub-voxel camera motion
    // causes zero recomposition — docs/math/sdf.md), then declare this frame's compose dispatches
    // into `graph`: a "clear to +band" pass followed by one stamp pass per overlapping tracked
    // instance, for every level that actually has something to recompose. A level with no dirty
    // region and an unchanged snapped origin declares NOTHING — the ADR-0032 §11 "idle work is a
    // bug" rule, made structural rather than promised.
    void add(RenderGraph& graph, core::Vec3 camera_pos);

    [[nodiscard]] const SdfClipmapStats& stats() const noexcept { return stats_; }

    // Per-level GPU state (m10.5's probes trace these; the tests read them back directly). `level`
    // must be < kSdfClipmapLevels.
    struct LevelInfo {
        rhi::TextureHandle texture{};        // the persistent 3-D R16Snorm storage image
        core::Vec3 origin{0.0f, 0.0f, 0.0f}; // world-space corner of voxel (0,0,0), snapped
        float voxel_size = 0.0f;
        float band = 0.0f; // decode scale: distance = snorm * band
    };

    [[nodiscard]] const LevelInfo& level(std::uint32_t index) const noexcept {
        return levels_[index].info;
    }

    // The level's CURRENT resource state — what m10.5's DdgiProbes (the first sampler of this
    // clipmap) must import() the level texture as before binding it. Exposed rather than assumed
    // because a consumer outside this class cannot otherwise know whether a level has ever been
    // touched (Undefined) or holds a live compose result (StorageReadWrite).
    [[nodiscard]] rhi::ResourceState level_state(std::uint32_t index) const noexcept {
        return levels_[index].texture_state;
    }

    // Tell the clipmap what state a CONSUMER (m10.5's DdgiProbes) left a level's texture in after
    // its own pass touched it — the write half of the level_state()/note_level_state() pair, and
    // the reason both exist: this class is the texture's OWNER, so it is the one place a "current
    // state" query can be answered authoritatively, but the moment a second system (DDGI) also
    // reads the SAME texture, this class's own bookkeeping goes stale the instant that read
    // finishes unless the reader reports back. Without this, SdfClipmap's OWN next add() call
    // would import a level it recomposes using a STALE claimed state (whatever ITS last compose
    // pass left it in), which no longer matches the REAL layout DDGI's sampled read moved it to —
    // not a correctness bug (the RHI backend's own tracked-layout fallback still emits a correct
    // barrier — see command_buffer_vulkan.cpp's texture_barrier), but a validation-noise smell
    // that means two systems independently guessed at shared state instead of one owning it.
    void note_level_state(std::uint32_t index, rhi::ResourceState state) noexcept {
        levels_[index].texture_state = state;
    }

    // The GPU-ready packed form of every level's origin/extent/band — what a probe-tracing shader
    // uploads once per frame to walk the clipmap finest-to-coarsest (see GpuSdfClipmapLevels).
    [[nodiscard]] GpuSdfClipmapLevels gpu_levels() const noexcept;

    [[nodiscard]] std::size_t tracked_instance_count() const noexcept { return instances_.size(); }

private:
    // One instance currently composed into the field. `world_bounds` is the transform of the
    // cooked field's OWN grid extent (assets::MeshSdfAsset::grid_origin/resolution/voxel_size),
    // not the tighter source-mesh AABB — that grid extent is exactly the domain the compose
    // shader ever samples from (it skips any voxel whose local-space position falls outside it,
    // see sdf_compose.comp), so this bound is EXACT (not merely conservative): an instance cannot
    // have influenced the field anywhere outside it, which is what lets invalidate-on-move/remove
    // recompose only this box with no extra safety padding.
    struct TrackedInstance {
        rhi::TextureHandle gpu_sdf{}; // this instance's own cooked field, uploaded as R16Snorm
        core::Vec3 grid_origin{0.0f, 0.0f, 0.0f};
        float voxel_size = 0.0f;
        std::array<std::uint32_t, 3> resolution{0, 0, 0};
        float max_abs_distance = 0.0f; // the cook's own decode scale for gpu_sdf
        core::Mat4 local_from_world{}; // world -> this instance's local space
        float uniform_scale = 1.0f;    // world_from_local's (uniform) scale factor
        WorldAabb world_bounds{};      // world-space transform of the cooked grid extent
    };

    struct Level {
        LevelInfo info;
        bool origin_initialized = false;
        rhi::ResourceState texture_state = rhi::ResourceState::Undefined; // carried across frames
    };

    void ensure_level_texture(std::uint32_t index);
    void ensure_job_capacity(std::uint32_t count);
    [[nodiscard]] rhi::TextureHandle upload_instance_sdf(const assets::MeshSdfAsset& sdf) const;

    rhi::Device& device_;

    rhi::ShaderHandle compose_shader_;
    rhi::PipelineHandle compose_pipeline_;
    rhi::BufferHandle compose_ubo_; // grow-on-demand array of per-dispatch job slices
    std::uint32_t job_capacity_ = 0;
    rhi::TextureHandle placeholder_instance_sdf_; // 1x1x1 R16Snorm=0; bound for a "clear" dispatch
    rhi::SamplerHandle instance_sampler_;         // linear + ClampToEdge, shared by every instance

    std::array<Level, kSdfClipmapLevels> levels_{};
    std::unordered_map<std::uint64_t, TrackedInstance> instances_;
    // World-space regions to recompose, accumulated by update_instance/remove_instance/invalidate
    // since the last add() and drained (cleared) there.
    std::vector<WorldAabb> pending_regions_;

    SdfClipmapStats stats_{};
};

} // namespace rime::render
