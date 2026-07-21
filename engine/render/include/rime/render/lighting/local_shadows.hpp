// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"

// Local-light (spot) shadow maps with a DESTRUCTIBILITY-AWARE CACHE (m10.2, ADR-0032 §3 + C1/C2).
// Each shadowing spot light renders the scene depth from its own position through a perspective
// shadow map — one layer of a shared depth ARRAY texture (the m10.1a array RHI) — and the forward
// pass samples it with the same sampler2DArrayShadow / hardware-PCF path the cascades use. A spot
// is just a perspective light matrix into a per-light layer, so the whole cascade machinery is
// reused.
//
// The distinguishing feature is the CACHE. A local light's shadow is expensive and usually static —
// a street lamp over a courtyard re-renders the same depth every frame for nothing. So the map
// holds its depth across frames (a PERSISTENT texture, imported into the graph — not a transient)
// and a slot is re-rendered only when it is INVALIDATED:
//
//   * first sight (the slot has never been drawn), or
//   * the light itself moved / retargeted (its cached parameters changed), or
//   * a destruction event's world-space AABB (ADR-0032 C2) overlaps the slot's frustum — the wall
//     between the lamp and the floor came down, so the shadow it cast is now stale.
//
// That last case is this brick's thesis: `invalidate(region)` is the seam the C2 destruction
// channel (destruction/events.hpp, `world_bounds`) drives, so *breaking geometry moves the shadows
// it cast*. The math (perspective shadow, cone, the cache) is derived in
// docs/math/shadow-mapping.md §6–7.
namespace rime::render {

// A world-space axis-aligned box. Render-local on purpose: the render module must not depend on
// physics (module-boundary rule), and the C2 event carries a `physics::Aabb` — an app/sample-side
// bridge converts one to the other when it forwards destruction events into `invalidate()`. Two
// corners; `overlaps` is the standard separating-axis test on the three axes.
struct WorldAabb {
    core::Vec3 min{0.0f, 0.0f, 0.0f};
    core::Vec3 max{0.0f, 0.0f, 0.0f};

    [[nodiscard]] bool overlaps(const WorldAabb& o) const noexcept {
        return min.x <= o.max.x && max.x >= o.min.x && min.y <= o.max.y && max.y >= o.min.y &&
               min.z <= o.max.z && max.z >= o.min.z;
    }
};

// One spot light the renderer extracted (CPU side, GPU-free so the fit + cache logic is unit-
// testable). Position is world; `direction` is the unit travel direction (world); the cone's inner
// and outer HALF-angles are pre-cosined for the shader's smooth edge; radiance is colour ×
// intensity.
struct SpotLightData {
    core::Vec3 position{0.0f, 0.0f, 0.0f};
    core::Vec3 direction{0.0f, -1.0f, 0.0f};
    float range = 20.0f;
    float outer_angle = 0.6108652f; // half-angle; the shadow map's FOV is 2× this
    float cos_inner = 0.9063078f;   // cos(inner_angle)
    float cos_outer = 0.8191520f;   // cos(outer_angle)
    float radiance[3] = {0.0f, 0.0f, 0.0f};
};

// The GPU mirror (std140) of one spot in the LocalShadowUniforms block of
// pbr_forward_shadowed.frag. Every member is a mat4 or a 16-byte vec4, so C++'s layout and std140
// agree (the vec3 trap dodged by construction, as with GpuFrameUniforms / GpuShadowUniforms).
struct GpuSpotShadow {
    core::Mat4 view_proj;                                   // light clip-from-world (perspective)
    float pos_range[4] = {0.0f, 0.0f, 0.0f, 0.0f};          // xyz world pos, w = range
    float dir_cos_inner[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // xyz unit dir, w = cos(inner)
    float radiance_cos_outer[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // rgb radiance, w = cos(outer)
};

struct GpuLocalShadows {
    GpuSpotShadow spots[kMaxLocalShadows];
    // x = spot count, y = PCF radius (texels), z = depth bias, w = normal-offset bias.
    float params[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float texel[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // x = 1 / local_shadow_resolution
};

static_assert(sizeof(GpuSpotShadow) == 112, "GpuSpotShadow must be std140-tight (mat4 + 3 vec4)");
static_assert(
    sizeof(GpuLocalShadows) == kMaxLocalShadows * 112 + 32 &&
        offsetof(GpuLocalShadows, params) == kMaxLocalShadows * 112,
    "GpuLocalShadows no longer matches the std140 LocalShadowUniforms block in the shader");

// How many slots this frame were re-rendered vs served from cache — the hit-rate the money test and
// a static-scene proof assert on (≈100% reuse after warmup).
struct LocalShadowStats {
    std::uint32_t rendered = 0; // slots whose depth was re-drawn this frame
    std::uint32_t reused = 0;   // slots served from the cached depth (the win)
};

// Owns the local-shadow GPU resources — the persistent depth ARRAY (one layer per shadowing spot,
// held ACROSS frames so the cache can skip re-rendering), the per-spot light-view_proj buffer the
// depth passes read, the LocalShadowUniforms buffer the forward pass reads, and the depth-compare
// sampler — plus the per-slot cache state. One per SceneRenderer.
class LocalShadowMap {
public:
    explicit LocalShadowMap(rhi::Device& device);
    ~LocalShadowMap();

    LocalShadowMap(const LocalShadowMap&) = delete;
    LocalShadowMap& operator=(const LocalShadowMap&) = delete;

    // Declare this frame's spot-shadow depth passes into `graph` (reusing `prepass` aimed from each
    // spot), refresh the uniforms the forward pass reads, and return what the shadowed pass binds.
    // Only INVALIDATED slots get a depth pass — the rest keep last frame's depth (the cache). The
    // persistent array is created lazily at the configured resolution on the first call. `spots`
    // beyond kMaxLocalShadows are rendered unshadowed (their data is still uploaded, count is
    // capped).
    [[nodiscard]] LocalShadowBinding add(RenderGraph& graph,
                                         const DepthPrepass& prepass,
                                         const SceneDrawData& scene_data,
                                         std::span<const SpotLightData> spots,
                                         const LightingSettings& settings);

    // The valid-but-empty binding for a shadowed frame that has NO spot shadows (local shadows off,
    // or no spots, or the sun-only path) — the shadowed pipeline's descriptors must always point at
    // a real 2-D-array depth image + a count-0 uniform buffer even when nothing is sampled. `dummy`
    // is the SceneRenderer's shared 1×1 depth-array placeholder.
    [[nodiscard]] LocalShadowBinding empty_binding(RenderGraph& graph, rhi::TextureHandle dummy);

    // Mark every in-use slot whose shadow frustum overlaps `region` as needing a re-render — the C2
    // destruction hook. Persists until the slot is next drawn, so it survives frames where the
    // light is off-screen. Idempotent; safe to call many times per frame as events drain.
    void invalidate(const WorldAabb& region);

    [[nodiscard]] const LocalShadowStats& stats() const noexcept { return stats_; }

private:
    // Cross-frame state for one array layer: the cached light parameters (to detect a moved light)
    // and the world-space frustum bounds (to test destruction overlap), plus the dirty flag.
    struct Slot {
        bool rendered = false; // has valid depth from an earlier frame
        bool dirty = true;     // forced re-render (invalidate() / first sight)
        SpotLightData light{}; // cached parameters; a mismatch means the light moved
        WorldAabb bounds{};    // the shadow frustum's world AABB, for invalidate() overlap
    };

    void ensure_array(std::uint32_t resolution);

    rhi::Device& device_;
    rhi::SamplerHandle compare_sampler_; // sampler2DArrayShadow: LessEqual hardware PCF
    rhi::BufferHandle spot_vp_ubo_;      // kMaxLocalShadows × 256-byte light view_proj slices
    rhi::BufferHandle local_ubo_;        // GpuLocalShadows for the forward pass
    rhi::TextureHandle depth_array_{};   // the persistent per-spot depth array (lazy)
    rhi::ResourceState array_state_ = rhi::ResourceState::Undefined; // carried across frames
    std::uint32_t resolution_ = 0; // the array's per-layer size once created
    std::vector<Slot> slots_;      // kMaxLocalShadows entries
    LocalShadowStats stats_{};
};

} // namespace rime::render
