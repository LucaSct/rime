// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "rime/render/render_graph.hpp"
#include "rime/render/text/font.hpp"
#include "rime/rhi/resources.hpp"
#include "rime/rhi/types.hpp"

namespace rime::rhi {
class Device;
}

// The HUD overlay (m13.3b): panels and distance-field text drawn over a finished frame.
//
// WHY THIS EXISTS AT ALL. Nothing in the C++ engine could draw a letter. Every number the engine
// knows about itself — parts alive, live debris, submitted vs culled, active lights, frame time —
// was reachable only from a log line or the Rust editor's egui shell, which means it could not be
// in the picture. Luca ruled (2026-08-29) that the vision demo gets a NATIVE HUD rather than stats
// in someone else's window, and that made this a rendering subsystem rather than a detail of the
// playable-client brick (ADR-0035 amendment E4).
//
// IMMEDIATE MODE, deliberately. `begin()` then a few `panel()`/`text()` calls then `declare()`; no
// retained widget tree, no ids, no layout engine. A debug HUD's content changes every frame and its
// layout is written by the person reading it, so a retained model would be pure ceremony. The whole
// state is one vertex vector that is cleared and refilled.
//
// ALWAYS ON TOP, like the gizmo overlay it borrows its shape from: one raster pass that LOADS the
// finished LDR and draws with alpha blending and NO depth attachment. Nothing can occlude the HUD,
// because nothing it draws over is in the same space.
namespace rime::render::text {

// Linear RGBA, premultiplied by nothing — the blend is straight `Alpha`, so `a` is coverage.
struct Color {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
};

// ── The default look ─────────────────────────────────────────────────────────────────────────────
// A HUD is read at a glance over a moving image, so the palette is chosen for CONTRAST AGAINST
// ANYTHING rather than for prettiness in isolation: a near-black translucent panel to sit the text
// on (text alone over a bright frame is unreadable no matter what colour it is), near-white body
// text, and a single cool accent for the values the eye should find first. One accent, not five —
// a HUD where everything is highlighted highlights nothing.
namespace style {
inline constexpr Color kPanel{0.05f, 0.06f, 0.08f, 0.72f};
inline constexpr Color kText{0.88f, 0.90f, 0.94f, 1.0f};
inline constexpr Color kLabel{0.55f, 0.59f, 0.66f, 1.0f}; // dimmer: the noun, not the number
inline constexpr Color kAccent{0.45f, 0.78f, 1.0f, 1.0f};
inline constexpr Color kWarn{1.0f, 0.68f, 0.28f, 1.0f};
inline constexpr Color kBad{1.0f, 0.42f, 0.42f, 1.0f};

inline constexpr float kTextSize = 16.0f; // cap height in pixels
inline constexpr float kLineHeight = 22.0f;
inline constexpr float kPadding = 12.0f; // panel inset
} // namespace style

// Advance between glyph origins, as a multiple of the drawn glyph HEIGHT (the size argument).
//
// The arithmetic matters and the first value here got it wrong. A glyph body is kGlyphWidth /
// kGlyphHeight = 5/7 ≈ 0.714 of `size` wide, so an advance of 0.62 was NARROWER than the glyph —
// consecutive letters overlapped and the text read as cramped. 6/7 is the classic terminal cell:
// a 5-wide glyph in a 6-wide box, leaving exactly one source pixel of tracking.
inline constexpr float kAdvanceRatio =
    static_cast<float>(kGlyphWidth + 1) / static_cast<float>(kGlyphHeight);

class HudRenderer {
public:
    // Builds the atlas on the CPU, uploads it once, and bakes the overlay pipeline. `target_format`
    // is the format of the image the HUD will be drawn onto (a pipeline bakes its colour format in
    // — the same reason PresentPass takes one).
    HudRenderer(rhi::Device& device, rhi::Format target_format, const FontAtlasDesc& font = {});
    ~HudRenderer();

    HudRenderer(const HudRenderer&) = delete;
    HudRenderer& operator=(const HudRenderer&) = delete;

    [[nodiscard]] bool valid() const noexcept { return pipeline_.is_valid(); }

    // Start a frame. `screen` is the pixel size everything below is positioned in, with the origin
    // at the TOP-LEFT — the convention every 2-D UI uses and the one a reader expects when they
    // write `text(12, 12, ...)` and mean "near the top-left corner".
    void begin(rhi::Extent2D screen);

    // A filled rectangle, in pixels.
    void panel(float x, float y, float width, float height, Color color = style::kPanel);

    // Draw `utf8` (ASCII only — see font.hpp) with `x, y` the TOP-LEFT of the first glyph cell and
    // `size` the drawn cell height in pixels. Unprintable characters draw as blanks rather than as
    // whatever glyph their byte happens to index.
    void text(float x,
              float y,
              std::string_view s,
              Color color = style::kText,
              float size = style::kTextSize);

    // What `text` would occupy horizontally. Exposed because a HUD that right-aligns its numbers is
    // enormously easier to read than one that does not, and the caller cannot compute this without
    // knowing the advance.
    [[nodiscard]] static float text_width(std::string_view s,
                                          float size = style::kTextSize) noexcept;

    // Declare the overlay pass over `target`. No-op with nothing queued, so a frame that draws no
    // HUD costs nothing — not even a pass.
    void declare(RenderGraph& graph, RGTexture target);

    // What the frame queued. `quads` is the honest cost number; a HUD that silently stopped drawing
    // reads exactly like one with nothing to say, and this is the difference.
    [[nodiscard]] std::size_t quad_count() const noexcept { return quads_; }

    [[nodiscard]] const FontAtlas& atlas() const noexcept { return atlas_; }

private:
    void push_quad(float x0,
                   float y0,
                   float x1,
                   float y1,
                   float u0,
                   float v0,
                   float u1,
                   float v1,
                   Color c,
                   float mode);

    rhi::Device& device_;
    FontAtlas atlas_;
    rhi::TextureHandle atlas_texture_{};
    rhi::SamplerHandle sampler_{};
    rhi::ShaderHandle vertex_shader_{};
    rhi::ShaderHandle fragment_shader_{};
    rhi::PipelineHandle pipeline_{};

    // A RING of vertex buffers, not one. Writing a host-visible buffer the GPU is still reading is
    // the same class of bug m13.3a spent four fixes on: `Swapchain::present` does not wait, so the
    // previous frame can still be consuming last frame's vertices. Three is comfortably above the
    // backend's two frames in flight, and the cost is three small buffers.
    static constexpr std::size_t kRing = 3;
    std::vector<rhi::BufferHandle> buffers_;
    std::size_t ring_slot_ = 0;
    std::size_t capacity_ = 0; // vertices per buffer

    std::vector<float> vertices_;
    std::size_t quads_ = 0;
    rhi::Extent2D screen_{};
};

} // namespace rime::render::text
