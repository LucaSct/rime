// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/render/lighting/sky.hpp" // SkyLightBinding — the interface only, never SkyPass state
#include "rime/render/render_graph.hpp"

// The terrain heightfield DRAW pass (m19.3, ADR-0062-m19.3-terrain-render).
//
// M19.1 cooked the heightfield (ADR-0060: a regular grid of u16 samples plus a scale and an
// offset) and gave `rime::physics` a shape that collides with it. Nothing drew it. This pass is
// the pixels, and it is built around one hard requirement:
//
//   ── THE DRAWN SURFACE AND THE COLLIDED SURFACE COME FROM IDENTICAL INTEGERS ──
//
// Not "from the same asset", not "from the same numbers to within a tolerance" — from the same
// u16 samples, dequantised by the same `offset + scale * q`. The failure this prevents is the one
// every engine with a terrain has shipped at least once: a ball that visibly floats above, or
// sinks into, ground that is drawn a few centimetres off the ground that is simulated. ADR-0060 §1
// already named the mechanism; m19.3 is where it is actually built and measured.
//
// The three things that make it true, each verifiable by reading the code:
//
//  1. **The samples are uploaded VERBATIM, as R16_UNORM** (`rhi::Format::R16Unorm`, added for this
//     brick). `upload()` memcpys `HeightfieldAsset::samples` into the texture. No float ever
//     touches a height on the way to the GPU, so there is no conversion to get wrong.
//  2. **The vertex stage reconstructs the height from that texture** (terrain.vert — a vertex
//     puller; the draw binds no vertex buffer at all), applying `height_offset + height_scale * q`
//     — character for character the expression `HeightfieldShape::h()` evaluates on the CPU.
//  3. **The triangulation is ADR-0060's, taken from the asset's own header field.** Each cell is
//     split along its (i,j)→(i+1,j+1) diagonal, triangle A = (v00, v10, v11) then B = (v00, v11,
//     v01). A non-planar cell has two different surfaces depending on the split, so this is not a
//     stylistic choice: get it wrong and the drawn and collided surfaces differ by the cell's
//     non-planarity even though every sample agrees. `upload()` REFUSES a triangulation value it
//     does not know rather than guessing.
//
// The proof is `tests/terrain_render/terrain_height_test.cpp`: at 36 positions spread over a
// non-planar tile, the height this pass's vertex stage reconstructs on the GPU is compared with
// the height a `rime::physics` raycast lands on, against a margin derived from the R16_UNORM round
// trip. Structural, no golden images (the M5.6/M6.4 pattern).
//
// ── OWNERSHIP AND MODULARITY ─────────────────────────────────────────────────────────────────
//
// Owned exactly like `FxParticlePass`: a standalone pass object a caller composes into its frame,
// not something `SceneRenderer` reaches into. Nothing else in `rime_render` includes this header,
// so deleting these four files and their two CMake lines leaves the engine building — the
// modularity guardrail, satisfied structurally rather than by promise.
//
// ── WHAT THIS PASS IS NOT, YET (all deferred in ADR-0062, none of it silent) ──────────────────
//
//   * no LOD, no clipmap, no tessellation: a tile is drawn at full sample density, every frame;
//   * splat blending (m19.4/m19.5, ADR-0063/0064) blends base colour, metallic and roughness per
//     texel — see TerrainLayer; shading is the shared GGX BRDF (brdf.glsl). m19.7b (ADR-0066
//     addendum) multiplies each layer's colour by its own albedo texture at world-XZ UVs, and
//     m19.7c redistributes the painted weights by the height those textures carry (see
//     TerrainLayer::height_contrast). A layer has no normal map, and the textures are projected
//     along world Y only — no triplanar mapping, so they stretch on steep slopes;
//   * the environment is the SKY only (m19.6, ADR-0065), and only when the caller binds one: SH
//     diffuse plus the sky-view LUT along the mirror direction, through Karis' analytic env-BRDF.
//     No prefiltered radiance (roughness fades from the LUT toward the SH instead) and NO SKY
//     OCCLUSION — a valley reflects sky its own walls hide. With no sky the flat-ambient stand-in
//     of ADR-0064 renders as m19.5 did, to one f16 ULP (ADR-0065 §4);
//   * no streaming: `upload()` is a one-shot, and a tile stays resident until the pass dies;
//   * no holes, no decals, no per-cell best-fit diagonals (the format cannot express them either);
//   * a tile is placed by TRANSLATION only, because `HeightfieldAsset` carries an `origin` and no
//     rotation — physics can yaw a registered tile, the cooked asset cannot say so.
namespace rime::assets {
struct HeightfieldAsset;
}

namespace rime::render {

// A tile handle, dense and 0-based — the MeshRegistry convention (mesh.hpp), for the same reason:
// a caller holds an id, never GPU handles.
using TerrainTileId = std::uint32_t;
inline constexpr TerrainTileId kInvalidTerrainTile = 0xFFFFFFFFu;

// The largest tile this pass will upload, per axis. ADR-0060 caps a heightfield at 32768 samples
// per axis, which is far beyond what one DRAW should be: the index buffer for a grid is
// 6·(columns−1)·(rows−1) u32s, so a 32768² tile would ask for 25 GB of indices. 1024² asks for
// 25 MB, which is already generous for a tile — ADR-0060 §2's scaling answer is MANY TILES, not
// one enormous one, so a tile past this bound is a cook mistake and is refused and counted.
inline constexpr std::uint32_t kMaxTileSamplesPerAxis = 1024;

// One terrain tile's GPU residency. Public because the m19.3 proof builds a second pipeline around
// this pass's own vertex stage and needs the very resources the pass draws with — handing it these
// is what keeps the proof measuring the ENGINE's path instead of a copy of it.
struct TerrainTile {
    rhi::TextureHandle heights{}; // R16_UNORM, columns x rows, the cooked samples verbatim
    rhi::BufferHandle indices{};  // 6 per cell, ADR-0060's diagonal, u32
    std::uint32_t index_count = 0;
    std::uint32_t vertex_count = 0; // columns * rows — the grid the vertex stage indexes
    std::uint32_t columns = 0;
    std::uint32_t rows = 0;
    float cell_size_x = 0.0f;
    float cell_size_z = 0.0f;
    float height_scale = 0.0f;
    float height_offset = 0.0f;
    core::Vec3 origin{0.0f, 0.0f, 0.0f};
    // m19.4. EVERY tile owns these, so the descriptor layout (and the pipeline) is the same for a
    // v1 tile and a splat tile: a v1 tile holds a 1x1 dummy weight texture and a uniform block
    // whose flag is 0.
    rhi::TextureHandle weights{};  // RGBA8_UNORM, weight_columns x weight_rows, bytes verbatim
    rhi::BufferHandle splat_ubo{}; // flag, extent, dims, colours, roughness, uv scales, contrasts
    bool has_splat = false;
    // m19.7b: the four per-layer albedo+height textures, bound at bindings 5..8. NOT owned by the
    // tile — they are the builder's (see TerrainLayer::albedo_height) — except that a slot with no
    // texture holds the PASS's 1x1 white fallback, so every tile binds four valid handles. A v1
    // tile holds the fallback in all four (it never samples them: its flag is 0).
    std::array<rhi::TextureHandle, 4> layer_textures{};
};

// One palette entry, resolved BY THE CALLER. The pass must not reach into the asset system, so the
// caller looks each `HeightfieldAsset::layers[k]` up and hands over the colour.
//
// m19.4 carried `base_color` only, because terrain.frag was Lambert and a blended metallic or
// roughness would have been data nothing read. m19.5 (ADR-0064) moved terrain to the shared GGX
// BRDF, so metallic and roughness join the blend. Both must be finite and in [0,1]; upload()
// refuses (and counts) anything else rather than clamping it into a plausible-looking lie. The
// defaults — dielectric, fully rough — are the closest GGX gets to the old Lambert look.
//
// m19.7b (ADR-0066 addendum): a layer may carry a TEXTURE — the cooked `TerrainLayer` asset's
// packed albedo+height (RGB = albedo, sRGB; A = height, linear). The colour the blend sees becomes
// `base_color * texture.rgb`; metallic and roughness stay scalar. The pass never uploads it: the
// BUILDER (this upload()'s caller) resolves the layer's `albedo_height` AssetId to a GPU texture
// the way every material texture is resolved — `GpuAssetBridge::texture_or_placeholder`, which
// creates a cooked `Rgba8Srgb` texture as `rhi::Format::RGBA8Srgb` with its whole cooked mip chain.
// Vulkan's sRGB formats decode R, G and B only, so the height in A arrives linear (proven in the
// m19.7b tests, not assumed). The handle is BORROWED: the builder keeps it alive for as long as
// any tile uploaded with it is resident.
//
// An invalid handle (the default) means "no texture": the pass binds its own 1x1 WHITE texel
// instead, which decodes to exactly 1.0, so `base_color * 1.0` is `base_color` bit for bit — every
// pre-texture render is reproduced exactly, not approximately.
//
// `uv_scale` is metres per texture repeat along WORLD X and Z — the texture coordinate is
// world.xz / uv_scale, so a pattern runs continuously across tile seams. Finite and > 0, or the
// tile is refused and counted.
//
// m19.7c (ADR-0066 §5 and its m19.7c addendum): `height_contrast` is the asset's third field — how
// strongly this layer's HEIGHT (the texture's A) bends the painted weights. Where layers overlap,
// each painted layer k is scaled by 2^(contrast_k * (h_k - h_max)), h_max being the highest
// PAINTED layer at that pixel, and the weights are renormalised: the locally higher layer shows
// through first, the way gravel shows in the low cracks of grass. 0 (the default) is the plain
// m19.4 cross-fade, exactly: with every painted layer at contrast 0, or at one common height (all
// untextured layers are — the fallback's height is 0), the shader returns the sampled weights
// untouched, so the result is bit-identical to m19.7b. A layer painted at weight 0 never gains a
// share, whatever its height. Finite and >= 0, or the tile is refused and counted. KNOWN LIMIT:
// give layers that overlap the SAME contrast — with unequal contrasts, a tall layer painted in at
// even 1/255 re-divides the layers beneath it, a visible step (ADR-0066, m19.7c addendum). The pass still
// takes RESOLVED layers: filling this from the cooked `TerrainLayerAsset` is the builder's job.
struct TerrainLayer {
    core::Vec3 base_color{0.5f, 0.5f, 0.5f};
    float metallic = 0.0f;
    float roughness = 1.0f;
    rhi::TextureHandle albedo_height{}; // invalid = no texture (the white fallback)
    float uv_scale[2] = {1.0f, 1.0f};   // metres per repeat along world X, world Z
    float height_contrast = 0.0f;       // halvings per unit height below the highest painted layer
};

// Slot k is `HeightfieldAsset::layers[k]`. Entries for unused slots (zero AssetId) are ignored:
// their weights are 0 everywhere, and the pass writes a zero colour difference for them.
using TerrainPalette = std::array<TerrainLayer, 4>;

// The largest weight map, per axis. Past this a splat map is a cook mistake, refused and counted.
inline constexpr std::uint32_t kMaxSplatTexelsPerAxis = 4096;

// How the tile is lit. A struct rather than four arguments so a caller cannot transpose them, and
// deliberately NOT read out of the ECS: this pass is composed by its caller, which already knows
// the frame's sun (and `SceneRenderer`'s extraction is the right source once terrain joins a
// scene — ADR-0062's deferred list).
struct TerrainLight {
    core::Vec3 sun_direction{0.0f, -1.0f, 0.0f}; // the direction light TRAVELS; normalised by add()
    float sun_irradiance = 3.0f;                 // W/m² on a surface facing the sun
    core::Vec3 albedo{0.35f, 0.33f, 0.28f};      // a plausible dirt/grass grey-green
    float ambient = 0.05f;                       // flat irradiance, stands in for sky + bounce
    // The flat (v1, no splat map) surface's material, m19.5. A splat tile takes these per layer
    // from its palette instead, exactly as it takes base colour from the palette rather than
    // `albedo`.
    float metallic = 0.0f;
    float roughness = 1.0f;
};

// The push block both terrain shaders read, byte for byte. 160 bytes: m19.5 appended the camera
// position and the flat material to m19.3's 128. Vulkan only GUARANTEES 128 (maxPushConstantsSize),
// so 160 is above the floor and the pass checks `adapter().max_push_constant_bytes` (desktop
// drivers report 256 or more, MoltenVK 4096) — a device below it refuses every upload, counted
// and warned once, rather than drawing with a truncated block. Build it with `terrain_push()`.
struct TerrainPush {
    core::Mat4 view_proj;
    float placement[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // xyz = tile origin, w = height_offset
    float grid[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // cell_x, cell_z, height_scale, columns
    float sun[4] = {0.0f, -1.0f, 0.0f, 0.0f};      // xyz = travel direction, w = irradiance
    float surface[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // rgb = albedo, w = ambient
    float eye[4] = {0.0f, 0.0f, 0.0f, 0.0f};       // xyz = camera world position, w = 0
    float material[4] = {0.0f, 1.0f, 0.0f, 0.0f};  // x = metallic, y = roughness, z = w = 0
};

static_assert(sizeof(TerrainPush) == 160,
              "TerrainPush must match terrain.vert / terrain.frag's push_constant block");

// Fold a tile + a view + a light into the shader's constant block. Free and public on purpose:
// the pass uses it, and so does the proof's own pipeline, so the two cannot drift. A zero-length
// `sun_direction` falls back to straight down rather than producing a NaN normal. `eye` is the
// camera's world position — the BRDF's view vector needs it, and an orthographic projection
// cannot supply one, so it is passed explicitly rather than inverted out of `view_proj`.
[[nodiscard]] TerrainPush terrain_push(const TerrainTile& tile,
                                       const core::Mat4& view_proj,
                                       const core::Vec3& eye,
                                       const TerrainLight& light);

class TerrainPass {
public:
    explicit TerrainPass(rhi::Device& device);
    ~TerrainPass();

    TerrainPass(const TerrainPass&) = delete;
    TerrainPass& operator=(const TerrainPass&) = delete;

    // Make a cooked heightfield resident: the samples as an R16_UNORM texture, plus the grid's
    // index buffer. Returns `kInvalidTerrainTile` (and bumps `tiles_refused()`) for an asset this
    // pass will not draw — (when the device's push-constant limit is below sizeof(TerrainPush),
    // EVERY upload, counted and warned once) a grid smaller than one cell, a sample span that does
    // not match the grid, a non-finite or non-positive spacing/scale, an axis past
    // `kMaxTileSamplesPerAxis`, or a `triangulation` value this build does not know. It REFUSES
    // rather than repairing, the registration posture ADR-0060 §2 set for the physics side.
    //
    // An asset that carries a splat map is REFUSED here (counted in `tiles_refused()` and
    // `splat_refused()`): without materials it cannot be shaded correctly, and guessing a colour
    // would be a silent wrong picture.
    [[nodiscard]] TerrainTileId upload(const assets::HeightfieldAsset& asset);

    // As above, plus the caller-resolved palette for a splat asset (m19.4). On an asset without a
    // splat map the palette is ignored. Additional refusals, all also counted in `splat_refused()`:
    // a weight map past `kMaxSplatTexelsPerAxis`, a weight span that does not match its size, a
    // non-finite palette colour, a layer metallic/roughness that is non-finite or outside [0,1], a
    // layer `uv_scale` component that is non-finite or <= 0 (m19.7b), a layer `height_contrast`
    // that is non-finite or negative (m19.7c), or a failed weight-texture / uniform-buffer
    // allocation. A layer texture is only borrowed (see TerrainLayer).
    [[nodiscard]] TerrainTileId upload(const assets::HeightfieldAsset& asset,
                                       const TerrainPalette& palette);

    // UNCHECKED, the MeshRegistry::get contract: `id` must have come from a successful upload().
    [[nodiscard]] const TerrainTile& tile(TerrainTileId id) const { return tiles_[id]; }

    [[nodiscard]] bool contains(TerrainTileId id) const noexcept {
        return id != kInvalidTerrainTile && id < tiles_.size();
    }

    [[nodiscard]] std::size_t tile_count() const noexcept { return tiles_.size(); }

    // Declare the terrain draw into `hdr` (loaded, not cleared — terrain joins a frame other
    // passes have already contributed to) with `depth` written, like any other opaque geometry.
    //
    // A NO-OP when `id` is not a tile this pass holds: no pipeline bind, no barrier, no attachment
    // load, so a frame without terrain is byte-identical to a build without this file in it — the
    // structural gate discipline ADR-0032 §11 set and `FxParticlePass` follows.
    //
    // `sky` (m19.6, ADR-0065) is the sky's lighting half, as `SkyPass::add_lighting` (or
    // `empty_binding`) returns it — taken as a parameter, the convention
    // `ForwardPbrPass::add_shadowed` set. It is OPTIONAL: the default-constructed binding (all
    // handles invalid) means "no sky", and the pass binds its OWN 1x1 dummy LUT and all-zero SH
    // buffer, whose zero flag keeps terrain.frag on the m19.5 flat-ambient path — within one f16
    // ULP of m19.5 (proven against a frozen copy of that shader; ADR-0065 §4 says why not bits). A
    // binding with ANY member invalid is treated as no sky as a whole, never mixed. When it is lit
    // by the sky, `light.ambient` is ignored (the SH replaces it) and the sun is still `light`'s,
    // not the sky's.
    //
    // THE LUT STATE CONTRACT. `SkyPass` owns the sky-view LUT and imports it every frame in the
    // state the last consumer left it in, which it cannot see. Sampling it here leaves it in
    // ShaderRead, so the caller that owns the SkyPass must report
    // `sky.note_skyview_state(rhi::ResourceState::ShaderRead)` after declaring this pass — exactly
    // what SceneRenderer already does for the background composite and SSR.
    void add(RenderGraph& graph,
             RGTexture hdr,
             RGTexture depth,
             TerrainTileId id,
             const core::Mat4& view_proj,
             const core::Vec3& eye,
             const TerrainLight& light,
             const SkyLightBinding& sky = {});

    // Assets `upload()` would not draw. Guardrail 5: a refused tile and a tile that was never
    // handed over produce the same empty frame, so the difference has to be countable.
    [[nodiscard]] std::uint64_t tiles_refused() const noexcept { return refused_; }

    // The subset of `tiles_refused()` that is splat-specific (m19.4): every such refusal moves both
    // counters, so "why is there no terrain?" separates heights from materials.
    [[nodiscard]] std::uint64_t splat_refused() const noexcept { return splat_refused_; }

    // Tiles actually submitted, cumulative — the vacuity witness. "The pass drew nothing" and
    // "the pass drew a tile you cannot see" look identical on screen.
    [[nodiscard]] std::uint64_t tiles_drawn() const noexcept { return drawn_; }

    // The subset of `tiles_drawn()` that bound a CALLER's sky binding (m19.6) rather than the
    // pass's own placeholders — the witness that tells "lit by the sky" from "the caller's binding
    // was incomplete, so the pass fell back to flat ambient", which look alike on a dim frame.
    // (A caller's `empty_binding()` counts: whether a sky is LIVE is the SH flag's business.)
    [[nodiscard]] std::uint64_t sky_bound_draws() const noexcept { return sky_bound_; }

    // Draws whose caller handed over a PARTIAL sky binding — at least one of skyview/sh/sampler
    // valid, but not all three — and so fell back to the placeholders (warned once, naming the
    // invalid members). Guardrail 5: that fallback is a silent skip of the sky unless it is
    // counted. A fully default binding is the legitimate "no sky" and is NOT counted here.
    [[nodiscard]] std::uint64_t sky_partial_bindings() const noexcept { return sky_partial_; }

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
    rhi::SamplerHandle weight_sampler_; // LINEAR, clamp-to-edge: the weight map is meant to filter
    // m19.7b: the layer textures' sampler (trilinear, REPEAT — a layer tiles the ground) and the
    // 1x1 white texel a layer without a texture binds. Written once, permanently in ShaderRead.
    rhi::SamplerHandle layer_sampler_;
    rhi::TextureHandle white_layer_;
    // m19.6: the no-sky placeholders, SkyPass::empty_binding's pair owned here so a caller without
    // a SkyPass can still draw. Written once at construction, permanently in ShaderRead.
    rhi::TextureHandle dummy_skyview_;
    rhi::BufferHandle dummy_sh_;
    std::vector<TerrainTile> tiles_;
    TerrainTileId upload_impl(const assets::HeightfieldAsset& asset, const TerrainPalette* palette);

    bool push_fits_ = true; // adapter().max_push_constant_bytes >= sizeof(TerrainPush)
    bool push_warned_ = false;
    std::uint64_t refused_ = 0;
    std::uint64_t splat_refused_ = 0;
    std::uint64_t drawn_ = 0;
    std::uint64_t sky_bound_ = 0;
    std::uint64_t sky_partial_ = 0;
    bool sky_partial_warned_ = false;
};

} // namespace rime::render
