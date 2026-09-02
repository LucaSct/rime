// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "rime/assets/sdf_asset.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/culling.hpp"
#include "rime/render/lighting/clustered.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/lighting/local_shadows.hpp"
#include "rime/render/lighting/sdf_clipmap.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/lighting/shadows.hpp"
#include "rime/render/lighting/ssr.hpp"
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

struct ResolveDrawStats {
    std::size_t dropped = 0;  // draws whose mesh id the registry could not resolve
    std::size_t expanded = 0; // extra draws produced by splitting meshes into their submeshes
};

// Turn an entity-level draw list into a submesh-level one, dropping anything unresolvable.
//
// THE MANDATORY PRE-PASS. Every consumer of an `ExtractedScene` that touches the registry must call
// this exactly once, before anything else. It is one function rather than two because the two jobs
// have the same precondition and the same failure mode — a path that remembered one and forgot the
// other would either index out of bounds or silently draw a multi-material object in one material.
//
// It cannot live inside `extract_scene`, which is deliberately registry-free: extraction turns a
// World into a flat list and knows nothing about GPU state, so it can filter only the sentinel
// MeshId. Both jobs need the registry:
//
//   DROPPING — a `MeshRef` is a dense index into a runtime registry and can reach the World from a
//   `.rscene` on disk (untrusted input: the reader validates the field is a u32, never that it
//   names a real mesh) or from a scene saved in a session whose registry differed. Indexing with
//   one is an out-of-bounds read of a GpuMesh whose garbage buffer handles then reach the RHI.
//
//   EXPANDING — a mesh cooked from a multi-material glTF carries a submesh table, and each range
//   is a separate draw. `MeshRegistry::add` guarantees every mesh has at least one range, so a
//   single-material mesh expands to exactly one draw covering the whole index buffer and renders
//   byte-identically to the pre-m16.2 path.
//
// Both counts are returned rather than swallowed: a scene that quietly stops drawing, or quietly
// draws half of itself, is the hardest bug class in this engine to notice.
//
// The parallel `draw_entities` array is kept in step — an entity is repeated once per submesh — so
// the pick pass still maps a rasterised pixel back to the right entity.
[[nodiscard]] ResolveDrawStats resolve_draws(ExtractedScene& scene, const MeshRegistry& meshes);

// Pull the renderable view of a World: draws, the active camera, lights. Reads WorldTransform —
// run ecs::propagate_transforms first or camera/meshes/lights sit at stale poses. Conventions
// (asserted by the M5.6 tests): a camera looks down its entity's local −z; a directional light
// shines along its entity's local −z; a point light sits at its entity's world translation.
//
// An entity carrying an `ecs::RenderTransform` is extracted at THAT pose instead (m11.6b): it is
// the per-frame presentation override a producer of smoothed motion deposits — network
// interpolation is the first — while WorldTransform stays the simulated truth the tick computed.
// The component is absent on essentially every entity, so this costs a null check and changes
// nothing for a world that never produces one.
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
        // The thin SSR G-buffer (m10.7a), valid only when LightingSettings::ssr_enabled — export it
        // to read it back, or feed it to the SSR march (m10.7b). Invalid otherwise (no allocation,
        // no write): the pre-SSR frame is untouched.
        RGTexture gbuffer;
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

    // ── View-frustum culling (m13.2a) ─────────────────────────────────────────────────────────
    //
    // ON by default, and deliberately NOT hung off the "off is the byte-identical baseline" gate
    // every M10 lighting technique uses. That discipline is for techniques that CHANGE the picture;
    // culling must not change it at all — it removes draws that could not have contributed a pixel.
    // So the honest proof is not "off is identical to before" but **"on is identical to off"** for
    // a scene where everything is visible, which is what `culling_test.cpp` asserts.
    //
    // The toggle exists so that proof can be written, and so a suspected cull bug can be bisected
    // in one line rather than by rebuilding.
    void set_culling_enabled(bool enabled) noexcept { cull_enabled_ = enabled; }

    [[nodiscard]] bool culling_enabled() const noexcept { return cull_enabled_; }

    // The ledger entry ADR-0035 §2a names: draws submitted vs. draws the frustum removed,
    // accumulated across frames. **`culled == 0` with a large `submitted` is the failure mode this
    // exists to catch** — a cull that has stopped culling is invisible in every pixel and, on a
    // fast enough machine, in every frame time.
    [[nodiscard]] const CullStats& cull_stats() const noexcept { return cull_stats_; }

    void reset_cull_stats() noexcept { cull_stats_ = {}; }

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

    // How many draws render() has refused because their MeshRef named a mesh this registry does
    // not hold, cumulative. Nonzero means content is wrong — a scene saved against a different
    // registry, or a hand-edited `.rscene` — and the entities concerned are not being drawn.
    [[nodiscard]] std::size_t unresolvable_draws() const noexcept { return unresolvable_draws_; }

    // Tell the renderer how many frames the presentation path keeps in flight, so it can size its
    // uniform ring to `n + 1` (see the ring's note below for why that is the provably sufficient
    // number). Call it once, before the first render(), with `Swapchain::frames_in_flight()`.
    //
    // The DEFAULT IS THE SAFE MAXIMUM rather than the headless minimum, deliberately: a windowed
    // caller who forgets this call gets a slightly larger ring, while the opposite default would
    // silently restore a use-after-free that only appears under load. Headless callers using
    // submit_blocking pay two extra small buffers and need not call it at all.
    void set_frames_in_flight(std::uint32_t frames);

    // Slots in the uniform ring — `frames_in_flight + 1`. Exposed for the proof, which asserts the
    // ring is actually deeper than one and therefore that the growth path cannot free a live
    // buffer.
    [[nodiscard]] std::uint32_t ubo_slot_count() const noexcept {
        return static_cast<std::uint32_t>(frame_ubos_.size());
    }

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
    SsrPass ssr_;     // m10.7b: screen-space reflections resolve (only added when enabled)

    // The m10.4b/m10.5a extraction bridge: which SdfRef entities are currently registered with
    // sdf_clipmap_, and the change-detection watermark sync_sdf_instances reads from.
    std::vector<assets::MeshSdfAsset> sdf_sources_; // register_sdf_source's backing store
    std::unordered_set<std::uint64_t>
        tracked_sdf_entities_; // entity keys currently in sdf_clipmap_
    ecs::Version sdf_instances_since_ = 0;

    LightingSettings lighting_{}; // M10 feature gates; default off == the M5.6 baseline
    bool cull_enabled_ = true;
    CullStats cull_stats_{};

    // ── The uniform ring: one slot per frame that can be in flight ────────────────────────────
    //
    // These used to be single buffers, which was correct only in the v0 blocking model where the
    // GPU is idle between frames. `ensure_draw_capacity`'s own comment said so and named the seam
    // ("frames-in-flight will demand per-frame buffering here"); m13.3a then shipped
    // frames-in-flight without closing it. Two things went wrong at once, both measured:
    // `ensure_draw_capacity` DESTROYED a buffer that the previous frame's command buffer still had
    // baked into its descriptor sets, and `write_buffer` OVERWROTE contents the GPU was still
    // reading — a host write to mapped memory, ordered by nothing a pipeline barrier can express.
    //
    // The fix is the rule `rhi/swapchain.hpp` already states for command buffers: keep
    // `frames_in_flight() + 1` of the resource. There are only that many fence slots, so a slot
    // that old was submitted to a slot since re-acquired, and acquire waits on its fence. Each slot
    // therefore grows and is written only when its own frame comes round again.
    std::vector<rhi::BufferHandle> frame_ubos_;
    std::vector<rhi::BufferHandle> draw_ubos_;
    // Per slot, because slots grow independently — growing the one we are about to write is safe,
    // growing all of them would destroy buffers the other in-flight frames are still using.
    std::vector<std::uint32_t> draw_capacities_;
    std::uint32_t ubo_slot_ = 0;
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
    // Draws refused by resolve_draws, cumulative, plus a warn-once latch so a scene with
    // a bad ref does not print per frame. A content error, surfaced rather than silently absorbed.
    std::size_t unresolvable_draws_ = 0;
    bool warned_unresolvable_mesh_ = false;

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
