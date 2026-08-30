// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/render/text/hud.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "hud.frag.spv.h"
#include "hud.vert.spv.h"
#include "rime/core/diagnostics/log.hpp"
#include "rime/rhi/device.hpp"

namespace rime::render::text {
namespace {

// pos.xy, (uv.xy, mode), rgba — 9 floats. One interleaved stream, because a HUD's geometry is
// rebuilt every frame and splitting it into parallel arrays would buy nothing but more bindings.
//
// The `mode` flag rides in the UV attribute's THIRD component rather than as its own scalar
// attribute, because the RHI has no R32Float vertex format — and inventing one for a single
// per-vertex bool would grow a backend-facing enum to save a float this vertex already had room
// for. Nine floats either way.
constexpr std::uint32_t kFloatsPerVertex = 9;
constexpr std::uint32_t kStride = kFloatsPerVertex * sizeof(float);

constexpr rhi::VertexAttribute kAttrs[] = {
    {0, rhi::Format::RG32Float, 0},                   // clip-space position
    {1, rhi::Format::RGB32Float, 2 * sizeof(float)},  // uv.xy + mode
    {2, rhi::Format::RGBA32Float, 5 * sizeof(float)}, // linear RGBA
};

// Enough for a dense HUD without ever reallocating mid-frame: ~2700 glyphs.
constexpr std::size_t kMaxQuads = 2048;

rhi::ShaderHandle make_shader(rhi::Device& device,
                              rhi::ShaderStage stage,
                              const std::uint32_t* spv,
                              std::size_t bytes,
                              const char* name) {
    rhi::ShaderDesc sd{};
    sd.stage = stage;
    sd.spirv = spv;
    sd.spirv_size_bytes = bytes;
    sd.debug_name = name;
    return device.create_shader(sd);
}

} // namespace

HudRenderer::HudRenderer(rhi::Device& device, rhi::Format target_format, const FontAtlasDesc& font)
    : device_(device), atlas_(build_font_atlas(font)) {
    if (!atlas_.valid()) {
        RIME_ERROR("hud: font atlas generation produced nothing");
        return;
    }

    // R8: the field is one channel, and saying so rather than padding to RGBA is a 4x saving on the
    // one texture the HUD binds every frame.
    rhi::TextureDesc td{};
    td.extent = {atlas_.width, atlas_.height};
    td.format = rhi::Format::R8Unorm;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "hud-font-atlas";
    atlas_texture_ = device.create_texture(td);
    device.write_texture(atlas_texture_, atlas_.pixels.data(), atlas_.pixels.size());

    // LINEAR filtering, and it is not a cosmetic choice: a distance field is meaningful UNDER
    // interpolation — the midpoint of two distances is very nearly the distance at the midpoint —
    // which is exactly why an SDF survives magnification where a bitmap does not. Nearest here
    // would throw that away and reintroduce the stair-stepping the field exists to remove.
    rhi::SamplerDesc sd{};
    sd.mag_filter = rhi::Filter::Linear;
    sd.min_filter = rhi::Filter::Linear;
    sd.address_mode = rhi::AddressMode::ClampToEdge;
    sd.debug_name = "hud-font";
    sampler_ = device.create_sampler(sd);

    vertex_shader_ = make_shader(
        device, rhi::ShaderStage::Vertex, hud_vert_spv, sizeof(hud_vert_spv), "hud.vert");
    fragment_shader_ = make_shader(
        device, rhi::ShaderStage::Fragment, hud_frag_spv, sizeof(hud_frag_spv), "hud.frag");

    const rhi::BindingDesc bindings[] = {
        {0, rhi::BindingType::CombinedImageSampler, rhi::StageMask::Fragment},
    };
    rhi::GraphicsPipelineDesc pd{};
    pd.vertex_shader = vertex_shader_;
    pd.fragment_shader = fragment_shader_;
    pd.color_format = target_format;
    pd.cull = rhi::CullMode::None;
    pd.blend = rhi::BlendMode::Alpha; // the HUD sits ON the frame, it does not replace it
    pd.vertex_layout.stride = kStride;
    pd.vertex_layout.attributes = kAttrs;
    pd.bindings = bindings;
    pd.debug_name = "hud";
    pipeline_ = device.create_graphics_pipeline(pd);

    capacity_ = kMaxQuads * 6u; // two triangles a quad, unindexed
    buffers_.resize(kRing);
    for (std::size_t i = 0; i < kRing; ++i) {
        rhi::BufferDesc bd{};
        bd.size = capacity_ * kStride;
        bd.usage = rhi::BufferUsage::Vertex;
        bd.memory = rhi::MemoryUsage::CpuToGpu;
        bd.debug_name = "hud-vertices";
        buffers_[i] = device.create_buffer(bd);
    }
    vertices_.reserve(capacity_ * kFloatsPerVertex);
}

HudRenderer::~HudRenderer() {
    for (const rhi::BufferHandle b : buffers_) {
        device_.destroy(b);
    }
    device_.destroy(pipeline_);
    device_.destroy(fragment_shader_);
    device_.destroy(vertex_shader_);
    device_.destroy(sampler_);
    device_.destroy(atlas_texture_);
}

void HudRenderer::begin(rhi::Extent2D screen) {
    screen_ = screen;
    vertices_.clear();
    quads_ = 0;
}

void HudRenderer::push_quad(float x0,
                            float y0,
                            float x1,
                            float y1,
                            float u0,
                            float v0,
                            float u1,
                            float v1,
                            Color c,
                            float mode) {
    if (screen_.width == 0 || screen_.height == 0 || quads_ >= kMaxQuads) {
        return; // silently dropping past the cap is fine: quad_count() is what reports it
    }
    // Pixels (origin top-left) to clip space (origin centre, +y DOWN in Vulkan's convention, which
    // is why y is not flipped here). Done on the CPU so the vertex stage needs no uniform at all.
    const auto to_clip = [this](float x, float y) {
        return std::pair<float, float>{x / static_cast<float>(screen_.width) * 2.0f - 1.0f,
                                       y / static_cast<float>(screen_.height) * 2.0f - 1.0f};
    };
    const auto [cx0, cy0] = to_clip(x0, y0);
    const auto [cx1, cy1] = to_clip(x1, y1);

    const float corners[6][4] = {
        {cx0, cy0, u0, v0},
        {cx1, cy0, u1, v0},
        {cx1, cy1, u1, v1},
        {cx0, cy0, u0, v0},
        {cx1, cy1, u1, v1},
        {cx0, cy1, u0, v1},
    };
    for (const auto& v : corners) {
        vertices_.insert(vertices_.end(), {v[0], v[1], v[2], v[3], mode, c.r, c.g, c.b, c.a});
    }
    ++quads_;
}

void HudRenderer::panel(float x, float y, float width, float height, Color color) {
    push_quad(x, y, x + width, y + height, 0.0f, 0.0f, 0.0f, 0.0f, color, 0.0f);
}

float HudRenderer::text_width(std::string_view s, float size) noexcept {
    return static_cast<float>(s.size()) * size * kAdvanceRatio;
}

void HudRenderer::text(float x, float y, std::string_view s, Color color, float size) {
    if (!atlas_.valid() || atlas_.scale == 0) {
        return;
    }
    // THE DRAWN QUAD IS THE WHOLE ATLAS CELL, glyph plus padding — and that is the detail this gets
    // wrong if you are not careful. The padding is where the distance ramp lives, so cropping the
    // quad to the inked 5x7 body would clip the antialiased edge off every glyph and hand back the
    // hard stair-stepping the field exists to remove. So the quad covers the cell and is offset up
    // and left by the padding, which puts the glyph BODY exactly at (x, y) sized `size` tall.
    const float px_per_source = size / static_cast<float>(kGlyphHeight);
    const float scale = static_cast<float>(atlas_.scale);
    const float draw_w = static_cast<float>(atlas_.cell_width) / scale * px_per_source;
    const float draw_h = static_cast<float>(atlas_.cell_height) / scale * px_per_source;
    const float pad_px = static_cast<float>(atlas_.padding) / scale * px_per_source;
    const float advance = size * kAdvanceRatio;

    float pen = x;
    for (const char c : s) {
        if (c != ' ') { // the blank glyph would cost a quad and draw nothing
            const GlyphUv uv = glyph_uv(atlas_, c);
            push_quad(pen - pad_px,
                      y - pad_px,
                      pen - pad_px + draw_w,
                      y - pad_px + draw_h,
                      uv.u0,
                      uv.v0,
                      uv.u1,
                      uv.v1,
                      color,
                      1.0f);
        }
        pen += advance;
    }
}

void HudRenderer::declare(RenderGraph& graph, RGTexture target) {
    if (!pipeline_.is_valid() || quads_ == 0 || !target.is_valid()) {
        return; // nothing queued costs not even a pass
    }
    const rhi::BufferHandle vb = buffers_[ring_slot_];
    ring_slot_ = (ring_slot_ + 1u) % buffers_.size();
    device_.write_buffer(vb, vertices_.data(), vertices_.size() * sizeof(float), 0);

    // LOAD the finished frame and draw over it with NO depth attachment: always-on-top by
    // construction, the same policy GizmoRenderer's overlay pass uses (gizmo_renderer.hpp).
    const RGColorAttachment colors[] = {{target, rhi::LoadOp::Load, rhi::StoreOp::Store, {}}};
    RenderGraph::RasterPassDesc desc{};
    desc.colors = colors;
    const auto count = static_cast<std::uint32_t>(quads_ * 6u);
    graph.add_raster_pass("hud", desc, [this, vb, count](rhi::CommandBuffer& cmd) {
        cmd.bind_pipeline(pipeline_);
        cmd.bind_texture(0, atlas_texture_, sampler_);
        cmd.bind_vertex_buffer(vb);
        cmd.draw(count);
    });
}

} // namespace rime::render::text
