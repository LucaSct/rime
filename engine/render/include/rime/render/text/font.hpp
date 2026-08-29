// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The engine's own font (m13.3b): a compact bitmap glyph set, turned into a SIGNED DISTANCE FIELD
// atlas so text stays crisp at any size.
//
// WHY WE OWN THIS. third_party/README states the rule plainly — "we don't pull in a library for
// something we should understand and own" — and a HUD font is squarely inside that line. FreeType
// and stb_truetype are both fine libraries; neither is worth a dependency, a license entry and a
// build-graph edge for what a HUD needs, and a distance field is one of the more instructive things
// a renderer does.
//
// WHY A DISTANCE FIELD RATHER THAN A BITMAP. A bitmap font is exactly one size. Scale it up and you
// get stair-stepped blocks; scale it down and it aliases into mush. An SDF stores, per texel, the
// signed distance to the nearest glyph edge, so the fragment shader recovers a smooth analytic edge
// at ANY scale with one `smoothstep` — Green (2007), "Improved Alpha-Tested Magnification for
// Vector Textures and Special Effects", the technique Valve shipped in Team Fortress 2 and which
// every engine has used for HUD text since. Text can then be sized by the frame rather than by the
// asset, which is the whole reason a 5x7 source bitmap is enough to look good at 24 px.
//
// The distance transform itself is 8SSEDT (Danielsson's algorithm, in Leymarie & Levine's
// two-pass form): sweep the grid twice propagating the nearest-seed VECTOR to each cell, once
// forward and once backward. It is O(pixels), exact for practical purposes, and the derivation is
// in docs/math/distance-fields.md.
namespace rime::render::text {

// The glyph grid the source bitmaps are authored on. 5x7 is the classic terminal cell: the smallest
// that renders every printable ASCII form legibly (a 5-wide 'M' and 'W' work; 4 does not).
inline constexpr std::uint32_t kGlyphWidth = 5;
inline constexpr std::uint32_t kGlyphHeight = 7;

// Printable ASCII, space (32) through '~' (126). Anything outside maps to a blank cell rather than
// to a wrong glyph — a HUD that quietly prints the wrong character is worse than one with a hole.
inline constexpr char kFirstGlyph = ' ';
inline constexpr char kLastGlyph = '~';
inline constexpr std::size_t kGlyphCount = static_cast<std::size_t>(kLastGlyph - kFirstGlyph) + 1u;

// Is `c` a character the font can draw?
[[nodiscard]] constexpr bool has_glyph(char c) noexcept {
    return c >= kFirstGlyph && c <= kLastGlyph;
}

// The raw 5x7 bitmap for `c`, as `kGlyphWidth` COLUMN masks with bit 0 the TOP row. Returns the
// blank glyph for anything unprintable. Exposed for the font's own proof, which renders a handful
// back into ASCII art and compares against the letter shapes written out longhand — the table below
// is compact so the code stays readable, and the test carries the human-legible form.
[[nodiscard]] std::span<const std::uint8_t> glyph_bitmap(char c) noexcept;

// ── The atlas ────────────────────────────────────────────────────────────────────────────────────

// A generated SDF atlas: one cell per glyph in a fixed grid, single channel, 8-bit.
//
// `distance_range` is how many SOURCE pixels the stored 0..255 range spans, centred on 128 at the
// edge. It is the one number that couples this to the shader: too small and the edge cannot be
// antialiased at large sizes; too large and thin strokes lose contrast. 4 source pixels across a
// 5x7 glyph is generous and is what the default `padding` is sized for.
struct FontAtlas {
    std::vector<std::uint8_t> pixels; // width * height, R8
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t cell_width = 0; // per-glyph cell, including padding
    std::uint32_t cell_height = 0;
    std::uint32_t columns = 0;   // glyph cells per row
    std::uint32_t scale = 0;     // atlas texels per SOURCE pixel — needed to size a glyph on screen
    std::uint32_t padding = 0;   // atlas texels of margin around each glyph
    float distance_range = 0.0f; // in SOURCE pixels

    [[nodiscard]] bool valid() const noexcept { return width > 0 && height > 0; }
};

// How finely the source bitmap is supersampled into the field, and how much empty margin surrounds
// each glyph so its distance ramp has somewhere to live.
struct FontAtlasDesc {
    std::uint32_t scale = 4;     // source pixel -> `scale` atlas texels
    std::uint32_t padding = 4;   // atlas texels of margin on every side of a glyph
    float distance_range = 4.0f; // source pixels the 0..255 range spans
};

// Build the atlas. Pure CPU and deterministic — no device, no time, no randomness — so the whole
// thing is testable headless and produces byte-identical output on every run, which is what lets a
// test assert distances rather than eyeball a picture.
[[nodiscard]] FontAtlas build_font_atlas(const FontAtlasDesc& desc = {});

// Where glyph `c` sits in `atlas`, in NORMALIZED texture coordinates. `u0/v0` is the top-left of
// the cell and `u1/v1` the bottom-right; the glyph's inked area is inset by the atlas padding.
struct GlyphUv {
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
};

[[nodiscard]] GlyphUv glyph_uv(const FontAtlas& atlas, char c) noexcept;

} // namespace rime::render::text
