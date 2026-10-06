// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/terrain_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "rime/assets/heightfield_asset.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/render/passes.hpp" // kHdrFormat / kDepthFormat — one place decides the formats
#include "terrain.frag.spv.h"
#include "terrain.vert.spv.h"

namespace rime::render {
namespace {

[[nodiscard]] rhi::ShaderHandle make_shader(rhi::Device& device,
                                            rhi::ShaderStage stage,
                                            const std::uint32_t* words,
                                            std::size_t bytes,
                                            std::string_view name) {
    rhi::ShaderDesc sd{};
    sd.stage = stage;
    sd.spirv = words;
    sd.spirv_size_bytes = bytes;
    sd.debug_name = name;
    return device.create_shader(sd);
}

[[nodiscard]] bool unit_interval(float v) noexcept {
    return std::isfinite(v) && v >= 0.0f && v <= 1.0f;
}

[[nodiscard]] bool finite_positive(float v) noexcept {
    return std::isfinite(v) && v > 0.0f;
}

// ADR-0060 §1's triangulation, written out once. Cell (ci, cj) splits along the v00–v11 diagonal:
//
//     v01 ---- v11          triangle A = (v00, v10, v11)   — the u >= v half
//      |  B  /  |           triangle B = (v00, v11, v01)   — the u <  v half
//      |  /  A  |
//     v00 ---- v10
//
// This is the SAME order `heightfield_detail::triangle_vertices` builds on the CPU
// (engine/physics/src/heightfield.hpp) and the same order the diagonal's physics test pins. The
// two must agree: a cell whose four corners are not coplanar has a different surface under each
// diagonal, so a transposed index here would move the drawn ground off the collided ground by the
// cell's non-planarity — every sample still matching, and the ball still floating.
void fill_grid_indices(std::uint32_t columns, std::uint32_t rows, std::vector<std::uint32_t>& out) {
    const std::uint32_t cells_x = columns - 1;
    const std::uint32_t cells_z = rows - 1;
    out.clear();
    out.reserve(std::size_t{cells_x} * cells_z * 6);
    for (std::uint32_t cj = 0; cj < cells_z; ++cj) {
        for (std::uint32_t ci = 0; ci < cells_x; ++ci) {
            const std::uint32_t i00 = ci + columns * cj;
            const std::uint32_t i10 = i00 + 1;
            const std::uint32_t i01 = i00 + columns;
            const std::uint32_t i11 = i01 + 1;
            out.push_back(i00); // A
            out.push_back(i10);
            out.push_back(i11);
            out.push_back(i00); // B
            out.push_back(i11);
            out.push_back(i01);
        }
    }
}

} // namespace

TerrainPush terrain_push(const TerrainTile& tile,
                         const core::Mat4& view_proj,
                         const core::Vec3& eye,
                         const TerrainLight& light,
                         const TerrainLodDraw& lod) {
    TerrainPush p{};
    p.view_proj = view_proj;
    p.placement[0] = tile.origin.x;
    p.placement[1] = tile.origin.y;
    p.placement[2] = tile.origin.z;
    p.placement[3] = tile.height_offset;
    p.grid[0] = tile.cell_size_x;
    p.grid[1] = tile.cell_size_z;
    p.grid[2] = tile.height_scale;
    // `columns` as a float: exact below 2^24 and ADR-0060 caps an axis at 32768, so nothing is
    // lost, and it saves a vec4 of the push block.
    p.grid[3] = static_cast<float>(tile.columns);

    // Normalise here rather than in the shader: the fragment stage would otherwise renormalise
    // per pixel, and a caller handing over a zero vector would get a NaN normal rather than an
    // obviously-wrong direction. Straight down is the fallback — a sun nobody aimed.
    const float len = std::sqrt(light.sun_direction.x * light.sun_direction.x +
                                light.sun_direction.y * light.sun_direction.y +
                                light.sun_direction.z * light.sun_direction.z);
    const core::Vec3 dir = (std::isfinite(len) && len > 1e-6f)
                               ? core::Vec3{light.sun_direction.x / len,
                                            light.sun_direction.y / len,
                                            light.sun_direction.z / len}
                               : core::Vec3{0.0f, -1.0f, 0.0f};
    p.sun[0] = dir.x;
    p.sun[1] = dir.y;
    p.sun[2] = dir.z;
    p.sun[3] = std::max(light.sun_irradiance, 0.0f);
    p.surface[0] = light.albedo.x;
    p.surface[1] = light.albedo.y;
    p.surface[2] = light.albedo.z;
    p.surface[3] = std::max(light.ambient, 0.0f);
    p.eye[0] = eye.x;
    p.eye[1] = eye.y;
    p.eye[2] = eye.z;
    // Sanitised here because add() has no per-light refusal path, and a NaN reaching the shader
    // would blank the tile silently (every BRDF term goes NaN). Non-finite falls back to the
    // defaults (dielectric, fully rough), like the zero sun direction above; finite values clamp.
    p.material[0] = std::isfinite(light.metallic) ? std::clamp(light.metallic, 0.0f, 1.0f) : 0.0f;
    p.material[1] = std::isfinite(light.roughness) ? std::clamp(light.roughness, 0.0f, 1.0f) : 1.0f;
    if (lod.enabled) {
        p.lod_origin[0] = lod.grid_origin.x;
        p.lod_origin[1] = lod.grid_origin.y;
        p.lod_origin[2] = lod.grid_origin.z;
        p.lod_camera[0] = lod.camera.x;
        p.lod_camera[1] = lod.camera.y;
        p.lod_camera[2] = lod.camera.z;
        // A level that never morphs (the top, or a start at +inf) gets the largest finite start
        // and a zero slope, so the shader's clamp((d − start) · slope) is exactly 0 with no inf·0
        // NaN anywhere. A degenerate region (end <= start) morphs as a step at `start`.
        const bool morphs = std::isfinite(lod.morph_start) && std::isfinite(lod.morph_end);
        const float span = lod.morph_end - lod.morph_start;
        p.lod_origin[3] = morphs ? lod.morph_start : std::numeric_limits<float>::max();
        p.lod_camera[3] = !morphs       ? 0.0f
                          : span > 0.0f ? 1.0f / span
                                        : std::numeric_limits<float>::max();
        p.lod_tile[0] = lod.base_x;
        p.lod_tile[1] = lod.base_z;
        p.lod_tile[2] =
            static_cast<std::int32_t>(lod.coarser_edges & 0xFu) | kTerrainPushLodEnabled;
        p.lod_tile[3] = static_cast<std::int32_t>(lod.level);
    }
    return p;
}

TerrainPass::TerrainPass(rhi::Device& device) : device_(device) {
    // The push block is 208 bytes (m19.8d2), above Vulkan's guaranteed 128 (see TerrainPush). The
    // limit is read once; a device below it makes every upload() refuse (counted, warned once)
    // instead of drawing with a block the pipeline layout cannot hold.
    push_fits_ = device.adapter().max_push_constant_bytes >= sizeof(TerrainPush);
    vertex_shader_ = make_shader(device,
                                 rhi::ShaderStage::Vertex,
                                 terrain_vert_spv,
                                 sizeof(terrain_vert_spv),
                                 "terrain.vert");
    fragment_shader_ = make_shader(device,
                                   rhi::ShaderStage::Fragment,
                                   terrain_frag_spv,
                                   sizeof(terrain_frag_spv),
                                   "terrain.frag");

    // The heightfield is read in the VERTEX stage — unusual enough to be worth saying out loud,
    // and the whole reason this pass needs no vertex buffer.
    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Vertex},
        // m19.4: the splat weights and the per-tile constants, fragment-only.
        {1, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {2, rhi::BindingType::UniformBuffer, rhi::StageMask::Fragment},
        // m19.6: the sky — the sky-view LUT and the SH buffer (terrain.frag's SKY_SH_BINDING).
        // Always bound (a placeholder pair when there is no sky): the layout is fixed.
        {3, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {4, rhi::BindingType::StorageBuffer, rhi::StageMask::Fragment},
        // m19.7b: one albedo+height texture per layer. FOUR SEPARATE 2-D bindings, not one
        // texture array (ADR-0066 addendum): an array forces every layer to one size and one mip
        // count, and the textures are the builder's cooked assets, shared between tiles and
        // authored independently — a gravel at 512² and a grass at 2048² is the normal case.
        {5, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {6, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {7, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
        {8, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    };

    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    // No vertex layout: terrain.vert pulls from `gl_VertexIndex` and the heightfield texture.
    pd.color_format = kHdrFormat;
    // Culling OFF. The winding of the two triangles is ADR-0060's, chosen to match the physics
    // triangulation rather than to present a consistent facing to a camera — so "which side faces
    // you" is not a property this geometry guarantees, and culling by it would drop triangles for
    // a reason unrelated to visibility. A terrain tile is seen from above; there is no back side
    // to save work on.
    pd.cull = rhi::CullMode::None;
    pd.blend = rhi::BlendMode::None;
    pd.depth_test = true;
    pd.depth_write = true; // opaque geometry: terrain occludes, and is occluded
    pd.depth_compare = rhi::CompareOp::Less;
    pd.depth_format = kDepthFormat;
    pd.bindings = bindings;
    // On a device that cannot hold the block no tile is ever resident, so no draw is submitted;
    // the pipeline is still created with the real size so the failure stays in upload()'s counter
    // rather than becoming a backend validation error at construction.
    pd.push_constant_size = sizeof(TerrainPush);
    pd.debug_name = "terrain";
    pipeline_ = device.create_graphics_pipeline(pd);

    // NEAREST, CLAMP, no mips, no anisotropy — and none of it matters, because terrain.vert reads
    // the texture with `texelFetch`, which bypasses filtering and addressing entirely. The sampler
    // exists only because Vulkan's combined image-sampler binding demands one. Picking the inert
    // configuration is deliberate: if anyone later switches the fetch to a filtered `texture()`
    // call, a Nearest sampler keeps the heights exact instead of silently blending samples into a
    // surface physics never agreed to.
    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Nearest;
    sd.min_filter = rhi::Filter::Nearest;
    sd.mip_filter = rhi::Filter::Nearest;
    sd.address_mode = rhi::AddressMode::ClampToEdge;
    sd.debug_name = "terrain-heights";
    sampler_ = device.create_sampler(sd);

    // The weight map is the one place filtering is wanted: a painted splat map is meant to fade
    // between texels. CLAMP so the edge texels hold to the tile's border instead of wrapping.
    rhi::SamplerDesc wd{};
    wd.mag_filter = rhi::Filter::Linear;
    wd.min_filter = rhi::Filter::Linear;
    wd.mip_filter = rhi::Filter::Nearest;
    wd.address_mode = rhi::AddressMode::ClampToEdge;
    wd.debug_name = "terrain-weights";
    weight_sampler_ = device.create_sampler(wd);

    // m19.7b: the layer textures. TRILINEAR (linear within and across mips — a ground texture is
    // seen minified at every distance from the camera, so it needs its cooked mip chain), and
    // REPEAT, because the coordinate is world position over a period: a layer is meant to tile
    // the ground forever. No anisotropy yet, so grazing views blur toward the coarser mip; that is
    // a quality knob for later, and leaving it off keeps every proof independent of a per-device
    // anisotropic footprint the Vulkan spec does not pin down.
    rhi::SamplerDesc ld{};
    ld.mag_filter = rhi::Filter::Linear;
    ld.min_filter = rhi::Filter::Linear;
    ld.mip_filter = rhi::Filter::Linear;
    ld.address_mode = rhi::AddressMode::Repeat;
    ld.debug_name = "terrain-layers";
    layer_sampler_ = device.create_sampler(ld);

    // THE WHITE FALLBACK — what a layer with no texture samples. RGB 255 in an sRGB format decodes
    // to EXACTLY 1.0 (the sRGB curve maps 1 to 1, and Vulkan requires the 8-bit sRGB conversion to
    // be exact at the end points), and any filter over a texture whose every texel is 1.0 returns
    // 1.0. So `base_color * texture(...)` is `base_color` bit for bit, which is why a palette with
    // no textures renders exactly as m19.6 did, rather than to within a tolerance. A = 0: the
    // height of a flat, featureless layer (brick 3's blend treats equal heights as "no
    // redistribution", so the value matters only in that it is the same for every untextured
    // layer). Same format as a cooked layer texture, so the fallback cannot hide a format bug.
    rhi::TextureDesc wtd{};
    wtd.extent = {1, 1};
    wtd.format = rhi::Format::RGBA8Srgb;
    wtd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    wtd.debug_name = "terrain-white-layer";
    white_layer_ = device.create_texture(wtd);
    const std::uint8_t white_texel[4] = {255, 255, 255, 0};
    device.write_texture(white_layer_, white_texel, sizeof(white_texel));

    // m19.6: the no-sky placeholders — SkyPass::empty_binding's pair, owned here so a caller with
    // no SkyPass at all can still draw. The all-zero SH buffer's flag (its tenth vec4) is what
    // keeps terrain.frag on the flat-ambient branch; the 1x1 LUT is never actually sampled, it
    // exists because the descriptor layout is fixed. 10 x vec4 matches sky_sh_eval.glsl's block.
    rhi::TextureDesc dd{};
    dd.extent = {1, 1};
    dd.format = rhi::Format::RGBA16Float;
    dd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    dd.debug_name = "terrain-dummy-skyview";
    dummy_skyview_ = device.create_texture(dd);
    const std::uint16_t zero_half4[4] = {0, 0, 0, 0};
    device.write_texture(dummy_skyview_, zero_half4, sizeof(zero_half4));

    const float zero_sh[10 * 4] = {};
    rhi::BufferDesc sbd{};
    sbd.size = sizeof(zero_sh);
    sbd.usage = rhi::BufferUsage::Storage;
    sbd.memory = rhi::MemoryUsage::CpuToGpu;
    sbd.debug_name = "terrain-dummy-sky-sh";
    dummy_sh_ = device.create_buffer(sbd);
    device.write_buffer(dummy_sh_, zero_sh, sizeof(zero_sh), 0);
}

TerrainPass::~TerrainPass() {
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        if (!live_[i]) {
            continue; // released: its handles were destroyed then
        }
        const TerrainTile& t = tiles_[i];
        device_.destroy(t.splat_ubo);
        device_.destroy(t.weights);
        device_.destroy(t.indices);
        device_.destroy(t.heights);
    }
    device_.destroy(dummy_sh_);
    device_.destroy(dummy_skyview_);
    device_.destroy(white_layer_);
    device_.destroy(layer_sampler_);
    device_.destroy(weight_sampler_);
    device_.destroy(sampler_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

namespace {
// std140 mirror of terrain.frag's `Splat` block: 10 x vec4 = 160 bytes. m19.7b APPENDED the two
// uv-scale vec4s after m19.5's 112 bytes rather than interleaving them, so the m19.6 proof's
// frozen m19.5 shader, which declares only the first 112, still reads this buffer correctly.
// m19.7c appended the contrast vec4 the same way, for the same reason.
struct SplatUniform {
    float info[4] = {0.0f, 0.0f, 0.0f, 0.0f};      // x = splat flag, yz = tile extent (m)
    float dims[4] = {1.0f, 1.0f, 0.0f, 0.0f};      // xy = weight map size in texels
    float color[4][4] = {};                        // rgb = base colour, w = metallic, per layer
    float roughness[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // x..w = layer 0..3
    // Metres per repeat, world X and Z: [0] = (layer 0 x, z, layer 1 x, z), [1] = layers 2, 3.
    float uv_scale[2][4] = {{1.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};
    float height_contrast[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // x..w = layer 0..3; 0 = plain blend
};

static_assert(sizeof(SplatUniform) == 160, "SplatUniform must match terrain.frag's Splat block");
} // namespace

TerrainTileId TerrainPass::upload(const assets::HeightfieldAsset& asset) {
    return upload_impl(asset, nullptr);
}

TerrainTileId TerrainPass::upload(const assets::HeightfieldAsset& asset,
                                  const TerrainPalette& palette) {
    return upload_impl(asset, &palette);
}

TerrainTileId TerrainPass::upload_impl(const assets::HeightfieldAsset& asset,
                                       const TerrainPalette* palette) {
    if (!push_fits_) {
        ++refused_;
        if (!push_warned_) {
            push_warned_ = true;
            RIME_WARN("terrain: refusing every tile — the push block is {} bytes but the device's"
                      " maxPushConstantsSize is {}",
                      sizeof(TerrainPush),
                      device_.adapter().max_push_constant_bytes);
        }
        return kInvalidTerrainTile;
    }

    // Validate, never repair (ADR-0060 §2's registration posture). Every rejection is one of the
    // asset reader's own invariants restated at the GPU boundary, because an asset can also be
    // built in memory by a caller that never went through the reader.
    const bool grid_ok =
        asset.columns >= 2 && asset.rows >= 2 && asset.columns <= kMaxTileSamplesPerAxis &&
        asset.rows <= kMaxTileSamplesPerAxis && asset.samples.size() == asset.sample_count();
    const bool scale_ok = finite_positive(asset.cell_size_x) &&
                          finite_positive(asset.cell_size_z) &&
                          finite_positive(asset.height_scale) && std::isfinite(asset.height_offset);
    const bool triangulation_ok =
        asset.triangulation == assets::HeightfieldTriangulation::DiagonalMinToMax;
    if (!grid_ok || !scale_ok || !triangulation_ok) {
        ++refused_;
        return kInvalidTerrainTile;
    }

    // Splat-specific validation (m19.4). Every refusal here moves BOTH counters, so a material
    // failure is distinguishable from a heights failure.
    const bool splat = asset.has_splat();
    if (splat) {
        bool splat_ok = palette != nullptr && // no materials: refuse, never guess a colour
                        asset.weight_columns <= kMaxSplatTexelsPerAxis &&
                        asset.weight_rows <= kMaxSplatTexelsPerAxis &&
                        asset.weights.size() == asset.weight_texel_count() * 4;
        if (splat_ok) {
            for (const TerrainLayer& l : *palette) {
                // m19.7b: a zero, negative or non-finite period has no meaningful texture
                // coordinate (a divide by zero, a mirrored pattern, or NaN), so it is refused,
                // not clamped to something that would draw.
                splat_ok = splat_ok && std::isfinite(l.base_color.x) &&
                           std::isfinite(l.base_color.y) && std::isfinite(l.base_color.z) &&
                           unit_interval(l.metallic) && unit_interval(l.roughness) &&
                           finite_positive(l.uv_scale[0]) && finite_positive(l.uv_scale[1]) &&
                           // m19.7c: a negative contrast would INVERT the blend (the lower layer
                           // wins) and a NaN or infinity would reach exp2 — refused like the rest.
                           std::isfinite(l.height_contrast) && l.height_contrast >= 0.0f;
            }
        }
        if (!splat_ok) {
            ++refused_;
            ++splat_refused_;
            return kInvalidTerrainTile;
        }
    }

    TerrainTile tile{};
    tile.columns = asset.columns;
    tile.rows = asset.rows;
    tile.cell_size_x = asset.cell_size_x;
    tile.cell_size_z = asset.cell_size_z;
    tile.height_scale = asset.height_scale;
    tile.height_offset = asset.height_offset;
    tile.origin = asset.origin;
    tile.vertex_count = asset.columns * asset.rows;

    rhi::TextureDesc td{};
    td.extent = {asset.columns, asset.rows};
    td.format = rhi::Format::R16Unorm;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "terrain-heights";
    tile.heights = device_.create_texture(td);
    if (!tile.heights.is_valid()) {
        // The device logged why. Counting it here too is what keeps `tiles_refused()` the one
        // number that answers "why is there no terrain?" — an out-of-memory tile and a malformed
        // one both end with nothing drawn, and a caller should not have to read the log to tell
        // them apart from a tile nobody ever handed over.
        ++refused_;
        return kInvalidTerrainTile;
    }
    // THE MEMCPY THAT IS THE POINT OF THE BRICK. `samples` is row-major with x fastest, which is
    // exactly a 2-D texture upload's walk, and R16_UNORM's storage is the u16 itself — so the
    // bytes physics holds and the bytes the GPU holds are the same bytes. No conversion exists to
    // get wrong, which is a stronger guarantee than any amount of careful conversion code.
    device_.write_texture(
        tile.heights, asset.samples.data(), asset.samples.size() * sizeof(std::uint16_t));

    std::vector<std::uint32_t> indices;
    fill_grid_indices(asset.columns, asset.rows, indices);
    tile.index_count = static_cast<std::uint32_t>(indices.size());

    rhi::BufferDesc bd{};
    bd.size = indices.size() * sizeof(std::uint32_t);
    bd.usage = rhi::BufferUsage::Index;
    // CpuToGpu, written once: the index pattern never changes for a given grid, so this is the
    // one-shot upload path, not a per-frame one. A device-local staging copy would be the right
    // answer once terrain streams (ADR-0062's deferred list).
    bd.memory = rhi::MemoryUsage::CpuToGpu;
    bd.debug_name = "terrain-indices";
    tile.indices = device_.create_buffer(bd);
    if (!tile.indices.is_valid()) {
        device_.destroy(tile.heights); // no half-resident tile enters the store
        ++refused_;
        return kInvalidTerrainTile;
    }
    device_.write_buffer(tile.indices, indices.data(), bd.size, 0);

    // ── m19.4: the weight texture and the per-tile constants ─────────────────────────────────
    // A v1 tile gets a 1x1 dummy so every tile binds the same layout. The weights are uploaded
    // VERBATIM (4 bytes per texel, row-major, x fastest — exactly an RGBA8 2-D upload's walk).
    const std::uint8_t dummy_texel[4] = {255, 0, 0, 0};
    tile.has_splat = splat;
    rhi::TextureDesc wtd{};
    wtd.extent =
        splat ? rhi::Extent2D{asset.weight_columns, asset.weight_rows} : rhi::Extent2D{1, 1};
    wtd.format = rhi::Format::RGBA8Unorm;
    wtd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    wtd.debug_name = "terrain-weights";
    tile.weights = device_.create_texture(wtd);

    SplatUniform u{};
    tile.layer_textures.fill(white_layer_);
    if (splat) {
        u.info[0] = 1.0f;
        u.info[1] = asset.cell_size_x * static_cast<float>(asset.columns - 1);
        u.info[2] = asset.cell_size_z * static_cast<float>(asset.rows - 1);
        u.dims[0] = static_cast<float>(asset.weight_columns);
        u.dims[1] = static_cast<float>(asset.weight_rows);
        // Unused slots (zero AssetId) repeat layer 0, so a stray filter tail pulls in a ZERO
        // difference rather than garbage; see terrain.frag.
        // The same repeat applies to metallic and roughness: a zero difference in every blended
        // quantity, so an unused slot can never leak a material into the picture.
        for (std::size_t k = 0; k < 4; ++k) {
            const TerrainLayer& l =
                (k == 0 || asset.layers[k].is_valid()) ? (*palette)[k] : (*palette)[0];
            u.color[k][0] = l.base_color.x;
            u.color[k][1] = l.base_color.y;
            u.color[k][2] = l.base_color.z;
            u.color[k][3] = l.metallic;
            u.roughness[k] = l.roughness;
            // m19.7b: the texture and its period follow the SAME repeat, so an unused slot's
            // colour `base_color * texture` is layer 0's to the bit and its difference stays 0.
            u.uv_scale[k / 2][(k % 2) * 2 + 0] = l.uv_scale[0];
            u.uv_scale[k / 2][(k % 2) * 2 + 1] = l.uv_scale[1];
            if (l.albedo_height.is_valid()) {
                tile.layer_textures[k] = l.albedo_height;
            }
            // m19.7c: and the contrast. An unused slot's weight is 0, so the shader never counts
            // it among the painted layers; repeating layer 0 just keeps the rule uniform.
            u.height_contrast[k] = l.height_contrast;
        }
    }
    rhi::BufferDesc ubd{};
    ubd.size = sizeof(SplatUniform);
    ubd.usage = rhi::BufferUsage::Uniform;
    ubd.memory = rhi::MemoryUsage::CpuToGpu;
    ubd.debug_name = "terrain-splat-ubo";
    tile.splat_ubo = device_.create_buffer(ubd);
    if (!tile.weights.is_valid() || !tile.splat_ubo.is_valid()) {
        device_.destroy(tile.splat_ubo);
        device_.destroy(tile.weights);
        device_.destroy(tile.indices);
        device_.destroy(tile.heights);
        ++refused_;
        if (splat) {
            ++splat_refused_;
        }
        return kInvalidTerrainTile;
    }
    tile.weight_bytes = splat ? asset.weights.size() : sizeof(dummy_texel);
    if (splat) {
        device_.write_texture(tile.weights, asset.weights.data(), asset.weights.size());
    } else {
        device_.write_texture(tile.weights, dummy_texel, sizeof(dummy_texel));
    }
    device_.write_buffer(tile.splat_ubo, &u, sizeof(u), 0);

    // m19.8a: a released id is recycled before the store grows, so a streaming world that keeps a
    // bounded number of tiles resident also keeps this vector bounded.
    if (!free_.empty()) {
        const TerrainTileId id = free_.back();
        free_.pop_back();
        tiles_[id] = tile;
        live_[id] = true;
        return id;
    }
    tiles_.push_back(tile);
    live_.push_back(true);
    return static_cast<TerrainTileId>(tiles_.size() - 1);
}

bool TerrainPass::release(TerrainTileId id) {
    if (!contains(id)) {
        return false;
    }
    TerrainTile& t = tiles_[id];
    device_.destroy(t.splat_ubo);
    device_.destroy(t.weights);
    device_.destroy(t.indices);
    device_.destroy(t.heights);
    t = TerrainTile{}; // no stale handle survives in the slot
    live_[id] = false;
    free_.push_back(id);
    return true;
}

std::uint64_t TerrainPass::tile_bytes(TerrainTileId id) const noexcept {
    if (!contains(id)) {
        return 0;
    }
    const TerrainTile& t = tiles_[id];
    // What upload() allocated, by construction: u16 heights, u32 indices, RGBA8 weights (a 1x1
    // dummy on a v1 tile) and the 160-byte splat block. Allocator padding is not counted.
    return std::uint64_t{t.vertex_count} * sizeof(std::uint16_t) +
           std::uint64_t{t.index_count} * sizeof(std::uint32_t) + t.weight_bytes +
           sizeof(SplatUniform);
}

void TerrainPass::add(RenderGraph& graph,
                      RGTexture hdr,
                      RGTexture depth,
                      TerrainTileId id,
                      const core::Mat4& view_proj,
                      const core::Vec3& eye,
                      const TerrainLight& light,
                      const SkyLightBinding& sky,
                      const TerrainLodDraw& lod) {
    // The structural gate: an unknown tile declares NO pass, so the frame is byte-identical to one
    // from a build without this file. A pass that ran and drew zero indices would be *almost*
    // that, and almost is not a regression bridge (ADR-0032 §11).
    if (!contains(id)) {
        return;
    }
    const TerrainTile& tile = tiles_[id];

    const TerrainPush push = terrain_push(tile, view_proj, eye, light, lod);

    // LOAD the HDR target: terrain is one contributor to a frame, not its owner. Depth is written,
    // because terrain is opaque — a tile must occlude what is behind it, and be occluded by what
    // is in front.
    const RGColorAttachment colors[] = {{hdr, rhi::LoadOp::Load, rhi::StoreOp::Store, {}}};
    RGDepthAttachment depth_att{};
    depth_att.texture = depth;
    depth_att.load = rhi::LoadOp::Load;
    depth_att.store = rhi::StoreOp::Store;
    depth_att.read_only = false;

    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    desc.depth = &depth_att;
    // The heightfield is a SAMPLED input of this pass. Declaring it is what lets the graph put the
    // texture into a shader-read state before the draw; a pass that read it without saying so
    // would work by accident today and break the first time a compute pass wrote one.
    //
    // m19.6: the sky. A caller's binding is used only when it is COMPLETE; anything less is "no
    // sky" as a whole, so a half-bound sky can never pair a live LUT with a placeholder SH (or the
    // reverse). Both the LUT and the SH buffer are declared reads either way: the sky bakes them
    // with compute earlier in the frame, and declaring the read is what orders this pass after
    // that dispatch and emits the write -> shader-read barrier. An undeclared read would work
    // right up until the first frame that actually re-baked.
    const bool lut_ok = sky.skyview.is_valid();
    const bool sh_ok = sky.sh.is_valid();
    const bool sampler_ok = sky.sampler.is_valid();
    const bool caller_sky = lut_ok && sh_ok && sampler_ok;
    // A binding with SOME members set is a caller's mistake, not "no sky": it still draws on the
    // placeholders, but counted and warned once (guardrail 5), so a sky that never arrives cannot
    // pass for a frame that simply had none. All three invalid is the default, legitimate no-sky.
    if (!caller_sky && (lut_ok || sh_ok || sampler_ok)) {
        ++sky_partial_;
        if (!sky_partial_warned_) {
            sky_partial_warned_ = true;
            RIME_WARN("terrain: partial sky binding ignored, drawing with flat ambient — invalid:"
                      "{}{}{}",
                      lut_ok ? "" : " skyview",
                      sh_ok ? "" : " sh",
                      sampler_ok ? "" : " sampler");
        }
    }
    const RGTexture sky_lut =
        caller_sky ? sky.skyview
                   : graph.import_texture(dummy_skyview_, rhi::ResourceState::ShaderRead);
    const RGBuffer sky_sh =
        caller_sky ? sky.sh : graph.import_buffer(dummy_sh_, rhi::ResourceState::ShaderRead);
    const rhi::SamplerHandle sky_sampler = caller_sky ? sky.sampler : weight_sampler_;
    // m19.7b: the four layer textures are sampled inputs too, declared for the same reason as the
    // heightfield. Each DISTINCT handle is imported once — four untextured layers share the white
    // fallback, and two layers may share one cooked texture — so the graph never tracks one image
    // as two resources with two independent states.
    RGTexture sampled[3 + 4] = {graph.import_texture(tile.heights, rhi::ResourceState::ShaderRead),
                                graph.import_texture(tile.weights, rhi::ResourceState::ShaderRead),
                                sky_lut};
    std::size_t sampled_count = 3;
    for (std::size_t k = 0; k < tile.layer_textures.size(); ++k) {
        const rhi::TextureHandle h = tile.layer_textures[k];
        bool seen = false;
        for (std::size_t j = 0; j < k; ++j) {
            seen = seen || tile.layer_textures[j] == h;
        }
        if (!seen) {
            sampled[sampled_count++] = graph.import_texture(h, rhi::ResourceState::ShaderRead);
        }
    }
    desc.sampled = std::span<const RGTexture>(sampled, sampled_count);
    const RGBuffer buffers[] = {sky_sh};
    desc.buffer_reads = buffers;

    ++drawn_;
    if (caller_sky) {
        ++sky_bound_;
    }
    graph.add_raster_pass("terrain",
                          desc,
                          [pipeline = pipeline_,
                           sampler = sampler_,
                           weight_sampler = weight_sampler_,
                           layer_sampler = layer_sampler_,
                           layers = tile.layer_textures,
                           heights = tile.heights,
                           weights = tile.weights,
                           splat_ubo = tile.splat_ubo,
                           indices = tile.indices,
                           index_count = tile.index_count,
                           sky_lut,
                           sky_sh,
                           sky_sampler,
                           &graph,
                           push](rhi::CommandBuffer& cmd) {
                              cmd.bind_pipeline(pipeline);
                              cmd.bind_texture(0, heights, sampler);
                              cmd.bind_texture(1, weights, weight_sampler);
                              cmd.bind_uniform_buffer(2, splat_ubo);
                              // The sky's handles resolve now, after assign_physicals — the
                              // late-resolve add_shadowed uses for the same graph resources.
                              cmd.bind_texture(3, graph.physical(sky_lut), sky_sampler);
                              cmd.bind_storage_buffer(4, graph.physical_buffer(sky_sh));
                              for (std::uint32_t k = 0; k < 4; ++k) {
                                  cmd.bind_texture(5 + k, layers[k], layer_sampler);
                              }
                              cmd.bind_index_buffer(indices, rhi::IndexType::Uint32);
                              cmd.push_constants(&push, sizeof(push));
                              // No vertex buffer is bound because there is nothing to bind: the
                              // vertex stage derives (i, j) from the index it is handed and pulls
                              // the height out of the texture.
                              cmd.draw_indexed(index_count);
                          });
}

} // namespace rime::render
