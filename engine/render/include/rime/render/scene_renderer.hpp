// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "rime/assets/sdf_asset.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/clustered.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/lighting/local_shadows.hpp"
#include "rime/render/lighting/sdf_clipmap.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/lighting/shadows.hpp"
#include "rime/render/passes.hpp"

// The scene renderer (M5.6, ADR-0022): the bridge from "a World full of entities" to "passes
// declared on a render graph". Each frame it
//
//   1. EXTRACTS — queries the World for everything renderable and copies it into flat arrays
//      (extract_scene below). After this step the World is untouched; simulation could already
//      be mutating it while the GPU work is recorded — the seam a parallel/pipelined frame
//      needs, kept even though v0 runs serially.
//   2. UPLOADS — packs the extraction into the frame/draw uniform buffers (std140 mirrors in
//      passes.hpp).
//   3. DECLARES — depth pre-pass (optional) → forward PBR → tonemap, using the pass library.
//
// It owns the GPU plumbing those steps need (uniform buffers, the 1x1 white / flat-normal fallback
// textures, the material sampler) and the three pass objects. It does NOT own the registries —
// meshes and materials belong to the scene/application; the renderer only reads them.
namespace rime::render {

// ── Extraction (GPU-free, unit-testable without a device) ────────────────────────────────────

struct ExtractedCamera {
    bool found = false;
    core::Mat4 view;                        // world → view (inverse of the camera's world matrix)
    float position[3] = {0.0f, 0.0f, 0.0f}; // eye, world space
    float fov_y = 0.87266f;                 // lens, copied from the Camera component
    float z_near = 0.1f;
    float z_far = 1000.0f;
};

struct ExtractedScene {
    std::vector<DrawItem> draws; // every {WorldTransform, MeshRef, MaterialRef}
    // Which entity each draw came from — parallel to `draws` (draw_entities[i] produced draws[i]).
    // A parallel array rather than a field on DrawItem, deliberately: DrawItem is the pass
    // library's flat, ECS-free vocabulary (passes.hpp must not depend on ecs), while "who was
    // that?" is exactly what the editor's ID-buffer pick pass (m9.6) needs to answer.
    std::vector<ecs::Entity> draw_entities;
    ExtractedCamera camera;                      // the FIRST active camera found
    std::vector<GpuDirectionalLight> dir_lights; // already GPU-shaped (uncapped; see render())
    std::vector<GpuPointLight> point_lights;
    std::vector<SpotLightData> spot_lights; // m10.2: CPU-shaped (the shadow fit needs pos/dir/cone)
};

// Pull the renderable view of a World: draws, the active camera, lights. Reads WorldTransform —
// run ecs::propagate_transforms first or camera/meshes/lights sit at stale poses. Conventions
// (asserted by the M5.6 tests): a camera looks down its entity's local −z; a directional light
// shines along its entity's local −z; a point light sits at its entity's world translation.
[[nodiscard]] ExtractedScene extract_scene(ecs::World& world);

// ── The renderer ──────────────────────────────────────────────────────────────────────────────

class SceneRenderer {
public:
    SceneRenderer(rhi::Device& device,
                  const MeshRegistry& meshes,
                  const MaterialRegistry& materials);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // The frame's graph-side products. `ldr` is the displayable output (already exported);
    // `hdr` is the pre-tonemap radiance — export it yourself if you want to read it back
    // (tests do) or feed it to more passes before the tonemap you then declare manually.
    struct Output {
        RGTexture hdr;
        RGTexture ldr;
    };

    // Extract → upload → declare into `graph` (which the caller later executes). Returns invalid
    // handles (and declares nothing) when the World has no active camera. The depth pre-pass is
    // optional per frame — same pixels either way (the M5.6 proof asserts it), different cost
    // profile; measure per workload.
    Output render(RenderGraph& graph,
                  ecs::World& world,
                  rhi::Extent2D extent,
                  bool use_depth_prepass = true);

    // Constant ambient radiance (linear RGB) — the crude GI stand-in until M10. Default is a dim
    // 0.02: unlit sides stay visible, tests can still tell lit from unlit by an order of
    // magnitude.
    void set_ambient(float r, float g, float b) {
        ambient_[0] = r;
        ambient_[1] = g;
        ambient_[2] = b;
    }

    // Lighting features (M10, ADR-0032). Default is everything off ⇒ the byte-identical M5.6
    // baseline. `shadows_enabled` makes the primary directional light cast a cascaded shadow map
    // (m10.1); `local_shadows_enabled` makes spot lights cast cached local shadows (m10.2);
    // `clustered_enabled` culls point lights into froxel lists so the 16-light cap lifts (m10.3).
    // The editor host and the M10 samples set these; the M5.6/M6.4 proofs leave them default.
    void set_lighting(const LightingSettings& lighting) { lighting_ = lighting; }

    [[nodiscard]] const LightingSettings& lighting() const noexcept { return lighting_; }

    // The C2 destruction hook (m10.2): tell the local-shadow cache that geometry in `region` (a
    // destruction event's world_bounds) changed, so any spot whose shadow frustum it touches
    // re-renders next frame. An app/sample bridges the destruction event stream to this call. Cheap
    // and idempotent — call it once per event as the stream drains.
    void invalidate_shadow_region(const WorldAabb& region) { local_shadows_.invalidate(region); }

    // Local-shadow cache stats from the most recent render() — how many spot maps were re-rendered
    // vs served from cache (the ≈100%-reuse property a static scene holds after warmup).
    [[nodiscard]] const LocalShadowStats& local_shadow_stats() const noexcept {
        return local_shadows_.stats();
    }

    // The C2 destruction hook for the SDF clipmap (m10.4b) — the WorldAabb twin of
    // invalidate_shadow_region above. An app/sample bridges the same destruction event stream to
    // both calls (a broken wall's shadow AND its contribution to the traceable field both need to
    // know). A no-op while sdf_clipmap_enabled is off (nothing ever reads the accumulated regions).
    void invalidate_sdf_region(const WorldAabb& region) { sdf_clipmap_.invalidate(region); }

    // The C2 destruction hook for DDGI's temporal hysteresis (m10.5b) — the third twin, alongside
    // invalidate_shadow_region/invalidate_sdf_region: an app/sample bridging one destruction event
    // into all three calls is what makes the FULL thesis true (the shadow moves, the field
    // recomposes, AND the bounced light catches up quickly rather than riding out ~30 frames of
    // default hysteresis — docs/math/ddgi.md §8/§11). A no-op while ddgi_enabled is off.
    void invalidate_ddgi_region(const WorldAabb& region) { ddgi_.invalidate(region); }

    // Direct access to the clipmap for reading its stats/level textures, or for registering a
    // composed instance BY HAND (SdfClipmap::update_instance/remove_instance) outside the ECS
    // entirely (a decal, a procedural volume — anything that is not an entity). Ordinary entities
    // register through `SdfRef` + register_sdf_source below instead (m10.5a closed the
    // "extraction doesn't exist yet" gap this comment used to describe); calling update_instance
    // directly here for an entity ALSO carrying SdfRef would race sync_sdf_instances's own C1
    // bookkeeping, so don't mix the two for the same id.
    [[nodiscard]] SdfClipmap& sdf_clipmap() noexcept { return sdf_clipmap_; }

    [[nodiscard]] const SdfClipmap& sdf_clipmap() const noexcept { return sdf_clipmap_; }

    // Register a decoded SDF source (the m10.4b gap, closed by m10.5a): entities carrying an
    // `SdfRef{source}` component are fed into the clipmap automatically each render() while
    // sdf_clipmap_enabled is on. Mirrors MeshRegistry/MaterialRegistry's own "add returns a dense
    // id, entities name it" shape rather than resolving a content-addressed asset id from disk —
    // there is no runtime id -> file-path resolver yet (MeshAsset's own doc comment admits the
    // identical gap for meshes); the caller decodes the cooked .rsdf however it already does
    // (read_mesh_sdf, tools/asset-pipeline, a test's hand-built field — the SAME data
    // SdfClipmap::update_instance always took directly) and hands the result over once.
    [[nodiscard]] SdfSourceId register_sdf_source(assets::MeshSdfAsset sdf) {
        sdf_sources_.push_back(std::move(sdf));
        return static_cast<SdfSourceId>(sdf_sources_.size() - 1);
    }

    // Direct access to the DDGI probes (m10.5a) — stats and the atlas textures/extents a consumer
    // (m10.5b, a debug view, a test driving SceneRenderer end-to-end) reads back.
    [[nodiscard]] DdgiProbes& ddgi() noexcept { return ddgi_; }

    [[nodiscard]] const DdgiProbes& ddgi() const noexcept { return ddgi_; }

    [[nodiscard]] const DdgiStats& ddgi_stats() const noexcept { return ddgi_.stats(); }

private:
    void ensure_draw_capacity(std::uint32_t draw_count);

    // Feed every (WorldTransform, SdfRef) entity into sdf_clipmap_ — the m10.4b extraction gap.
    // Two passes for two different costs: a full (but cheap — this component is sparse, worn only
    // by GI-relevant geometry) walk maintains `tracked_sdf_entities_` so a despawned/un-SdfRef'd
    // entity's instance is removed, while the actual GPU-texture-recreating
    // SdfClipmap::update_instance call is change-detection-gated (ADR-0032 C1) so a settled entity
    // costs nothing after its first frame. Called from render() only while sdf_clipmap_enabled.
    void sync_sdf_instances(ecs::World& world);

    rhi::Device& device_;
    const MeshRegistry& meshes_;
    const MaterialRegistry& materials_;

    DepthPrepass depth_prepass_;
    ForwardPbrPass forward_;
    TonemapPass tonemap_;
    CascadedShadowMap csm_; // m10.1: directional shadow cascades (only declared when enabled)
    LocalShadowMap local_shadows_; // m10.2: cached spot-light shadows (only declared when enabled)
    ClusteredLights clustered_;    // m10.3: froxel light culling (only declared when enabled)
    SdfClipmap sdf_clipmap_;       // m10.4b: the traceable field (only stepped when enabled)
    DdgiProbes ddgi_; // m10.5a: irradiance/visibility probes (only stepped when enabled)

    // The m10.4b/m10.5a extraction bridge: which SdfRef entities are currently registered with
    // sdf_clipmap_, and the change-detection watermark sync_sdf_instances reads from.
    std::vector<assets::MeshSdfAsset> sdf_sources_; // register_sdf_source's backing store
    std::unordered_set<std::uint64_t>
        tracked_sdf_entities_; // entity keys currently in sdf_clipmap_
    ecs::Version sdf_instances_since_ = 0;

    LightingSettings lighting_{}; // M10 feature gates; default off == the M5.6 baseline

    rhi::BufferHandle frame_ubo_;
    rhi::BufferHandle draw_ubo_;
    std::uint32_t draw_capacity_ = 0;
    rhi::TextureHandle white_;       // 1x1 white: base-color / MR / occlusion / emissive fallback
    rhi::TextureHandle flat_normal_; // 1x1 (128,128,255): the normal-map fallback = +Z (no bump)
    rhi::SamplerHandle material_sampler_; // trilinear + a little anisotropy, Repeat
    // A 1×1, 2-layer depth array that stands in at the shadowed pipeline's binding 7/9 when a
    // shadow type is absent (no sun, or no spots) — the descriptors must be valid even when
    // unsampled (m10.2). 2 layers so it gets a 2-D-ARRAY view compatible with sampler2DArrayShadow.
    rhi::TextureHandle dummy_shadow_array_;

    float ambient_[3] = {0.02f, 0.02f, 0.02f};
    bool warned_lights_ = false;
    bool warned_no_camera_ = false;

    // Per-frame arrays the pass lambdas' SceneDrawData spans point into — members (not locals)
    // because they must outlive render() and still be alive at graph.execute().
    std::vector<DrawItem> frame_draws_;
    // Five parallel per-draw texture arrays (one slot each), fallbacks already resolved — the spans
    // in SceneDrawData point at these, so they must outlive render() to graph.execute().
    std::vector<rhi::TextureHandle> frame_base_color_;
    std::vector<rhi::TextureHandle> frame_metallic_roughness_;
    std::vector<rhi::TextureHandle> frame_normal_;
    std::vector<rhi::TextureHandle> frame_occlusion_;
    std::vector<rhi::TextureHandle> frame_emissive_;
    std::vector<std::uint8_t> draw_staging_;
};

} // namespace rime::render
