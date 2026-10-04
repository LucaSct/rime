// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/terrain_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "rime/assets/heightfield_asset.hpp"
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

TerrainPush
terrain_push(const TerrainTile& tile, const core::Mat4& view_proj, const TerrainLight& light) {
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
    // lost, and the 16 bytes it saves are what let the light share the 128-byte push floor.
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
    return p;
}

TerrainPass::TerrainPass(rhi::Device& device) : device_(device) {
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
}

TerrainPass::~TerrainPass() {
    for (const TerrainTile& t : tiles_) {
        device_.destroy(t.indices);
        device_.destroy(t.heights);
    }
    device_.destroy(sampler_);
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
}

TerrainTileId TerrainPass::upload(const assets::HeightfieldAsset& asset) {
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
    device_.write_buffer(tile.indices, indices.data(), bd.size, 0);

    tiles_.push_back(tile);
    return static_cast<TerrainTileId>(tiles_.size() - 1);
}

void TerrainPass::add(RenderGraph& graph,
                      RGTexture hdr,
                      RGTexture depth,
                      TerrainTileId id,
                      const core::Mat4& view_proj,
                      const TerrainLight& light) {
    // The structural gate: an unknown tile declares NO pass, so the frame is byte-identical to one
    // from a build without this file. A pass that ran and drew zero indices would be *almost*
    // that, and almost is not a regression bridge (ADR-0032 §11).
    if (!contains(id)) {
        return;
    }
    const TerrainTile& tile = tiles_[id];

    const TerrainPush push = terrain_push(tile, view_proj, light);

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
    const RGTexture sampled[] = {
        graph.import_texture(tile.heights, rhi::ResourceState::ShaderRead)};
    desc.sampled = sampled;

    ++drawn_;
    graph.add_raster_pass("terrain",
                          desc,
                          [pipeline = pipeline_,
                           sampler = sampler_,
                           heights = tile.heights,
                           indices = tile.indices,
                           index_count = tile.index_count,
                           push](rhi::CommandBuffer& cmd) {
                              cmd.bind_pipeline(pipeline);
                              cmd.bind_texture(0, heights, sampler);
                              cmd.bind_index_buffer(indices, rhi::IndexType::Uint32);
                              cmd.push_constants(&push, sizeof(push));
                              // No vertex buffer is bound because there is nothing to bind: the
                              // vertex stage derives (i, j) from the index it is handed and pulls
                              // the height out of the texture.
                              cmd.draw_indexed(index_count);
                          });
}

} // namespace rime::render
