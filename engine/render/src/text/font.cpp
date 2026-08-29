// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/render/text/font.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rime::render::text {
namespace {

// The 5x7 glyph set, packed as five COLUMN masks per glyph with bit 0 the top row.
//
// Authored as ASCII art and mechanically packed — the shapes were drawn on a 5x7 grid and
// converted, so no byte here was typed by hand. The compact form is what lives in the source; the
// HUMAN-LEGIBLE form lives in the test, which renders glyphs back to art and compares them against
// letters written out longhand. That split is deliberate: a table of 475 hex bytes is unreviewable
// by eye, and a table of 665 string literals is unreadable in a diff, so the data is compact and
// the PROOF is legible.
constexpr std::uint8_t kGlyphs[kGlyphCount][kGlyphWidth] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, // ' '
    {0x00, 0x00, 0x5F, 0x00, 0x00}, // '!'
    {0x00, 0x03, 0x00, 0x03, 0x00}, // 'double quote'
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, // '#'
    {0x24, 0x2A, 0x7F, 0x2A, 0x12}, // '$'
    {0x23, 0x13, 0x08, 0x64, 0x62}, // '%'
    {0x36, 0x49, 0x49, 0x36, 0x50}, // '&'
    {0x00, 0x00, 0x03, 0x00, 0x00}, // 'apostrophe'
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // '('
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ')'
    {0x2A, 0x1C, 0x3E, 0x1C, 0x2A}, // '*'
    {0x08, 0x08, 0x3E, 0x08, 0x08}, // '+'
    {0x00, 0x40, 0x30, 0x10, 0x00}, // ','
    {0x08, 0x08, 0x08, 0x08, 0x08}, // '-'
    {0x00, 0x00, 0x60, 0x60, 0x00}, // '.'
    {0x60, 0x10, 0x08, 0x04, 0x03}, // '/'
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // '0'
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // '1'
    {0x42, 0x61, 0x51, 0x49, 0x46}, // '2'
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // '3'
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // '4'
    {0x27, 0x45, 0x45, 0x45, 0x39}, // '5'
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // '6'
    {0x01, 0x71, 0x09, 0x05, 0x03}, // '7'
    {0x36, 0x49, 0x49, 0x49, 0x36}, // '8'
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // '9'
    {0x00, 0x00, 0x36, 0x36, 0x00}, // ':'
    {0x00, 0x40, 0x36, 0x16, 0x00}, // ';'
    {0x08, 0x14, 0x22, 0x41, 0x00}, // '<'
    {0x14, 0x14, 0x14, 0x14, 0x14}, // '='
    {0x00, 0x41, 0x22, 0x14, 0x08}, // '>'
    {0x02, 0x01, 0x51, 0x09, 0x06}, // '?'
    {0x32, 0x49, 0x79, 0x41, 0x7E}, // '@'
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, // 'A'
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // 'B'
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // 'C'
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // 'D'
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // 'E'
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // 'F'
    {0x3E, 0x41, 0x41, 0x49, 0x7A}, // 'G'
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // 'H'
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // 'I'
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // 'J'
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // 'K'
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // 'L'
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // 'M'
    {0x7F, 0x02, 0x04, 0x08, 0x7F}, // 'N'
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // 'O'
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // 'P'
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // 'Q'
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // 'R'
    {0x46, 0x49, 0x49, 0x49, 0x31}, // 'S'
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // 'T'
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // 'U'
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // 'V'
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, // 'W'
    {0x63, 0x14, 0x08, 0x14, 0x63}, // 'X'
    {0x03, 0x04, 0x78, 0x04, 0x03}, // 'Y'
    {0x61, 0x51, 0x49, 0x45, 0x43}, // 'Z'
    {0x00, 0x7F, 0x41, 0x41, 0x00}, // '['
    {0x03, 0x04, 0x08, 0x10, 0x60}, // 'backslash'
    {0x00, 0x41, 0x41, 0x7F, 0x00}, // ']'
    {0x04, 0x02, 0x01, 0x02, 0x04}, // '^'
    {0x40, 0x40, 0x40, 0x40, 0x40}, // '_'
    {0x00, 0x01, 0x02, 0x00, 0x00}, // '`'
    {0x20, 0x54, 0x54, 0x54, 0x78}, // 'a'
    {0x7F, 0x44, 0x44, 0x44, 0x38}, // 'b'
    {0x38, 0x44, 0x44, 0x44, 0x00}, // 'c'
    {0x38, 0x44, 0x44, 0x44, 0x7F}, // 'd'
    {0x38, 0x54, 0x54, 0x54, 0x18}, // 'e'
    {0x08, 0x7E, 0x09, 0x09, 0x00}, // 'f'
    {0x0C, 0x52, 0x52, 0x52, 0x3E}, // 'g'
    {0x7F, 0x04, 0x04, 0x04, 0x78}, // 'h'
    {0x00, 0x44, 0x7D, 0x40, 0x00}, // 'i'
    {0x20, 0x40, 0x40, 0x3D, 0x00}, // 'j'
    {0x7F, 0x10, 0x28, 0x44, 0x00}, // 'k'
    {0x00, 0x41, 0x7F, 0x40, 0x00}, // 'l'
    {0x7C, 0x04, 0x18, 0x04, 0x78}, // 'm'
    {0x7C, 0x04, 0x04, 0x04, 0x78}, // 'n'
    {0x38, 0x44, 0x44, 0x44, 0x38}, // 'o'
    {0x7E, 0x12, 0x12, 0x12, 0x0C}, // 'p'
    {0x0C, 0x12, 0x12, 0x12, 0x7E}, // 'q'
    {0x7C, 0x08, 0x04, 0x04, 0x00}, // 'r'
    {0x48, 0x54, 0x54, 0x54, 0x24}, // 's'
    {0x04, 0x3F, 0x44, 0x44, 0x20}, // 't'
    {0x3C, 0x40, 0x40, 0x40, 0x7C}, // 'u'
    {0x1C, 0x20, 0x40, 0x20, 0x1C}, // 'v'
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, // 'w'
    {0x44, 0x28, 0x10, 0x28, 0x44}, // 'x'
    {0x0E, 0x50, 0x50, 0x50, 0x3E}, // 'y'
    {0x44, 0x64, 0x54, 0x4C, 0x44}, // 'z'
    {0x00, 0x08, 0x36, 0x41, 0x00}, // '{'
    {0x00, 0x00, 0x7F, 0x00, 0x00}, // '|'
    {0x00, 0x41, 0x36, 0x08, 0x00}, // '}'
    {0x18, 0x04, 0x08, 0x10, 0x0C}, // '~'
};

// Squared length of a 2-D offset, in a form that saturates rather than overflows for the "very far
// away" sentinel the sweep starts from.
struct Offset {
    int dx = 0;
    int dy = 0;

    [[nodiscard]] int dist2() const noexcept { return dx * dx + dy * dy; }
};

constexpr Offset kFar{4096, 4096};

// 8SSEDT — Danielsson's Euclidean distance transform in Leymarie & Levine's two-pass form.
//
// Each cell holds the OFFSET to its nearest seed, not just a distance, which is what makes the
// answer Euclidean instead of chessboard: propagating a vector lets a diagonal neighbour contribute
// its true distance rather than a step count. Two sweeps — forward (top-left to bottom-right,
// consulting the neighbours already visited) and backward — converge for practical grids. See
// docs/math/distance-fields.md.
void propagate(std::vector<Offset>& grid, int w, int h, int x, int y, int ox, int oy) {
    const int nx = x + ox;
    const int ny = y + oy;
    if (nx < 0 || ny < 0 || nx >= w || ny >= h) {
        return;
    }
    Offset candidate = grid[static_cast<std::size_t>(ny) * w + nx];
    candidate.dx -= ox;
    candidate.dy -= oy;
    Offset& here = grid[static_cast<std::size_t>(y) * w + x];
    if (candidate.dist2() < here.dist2()) {
        here = candidate;
    }
}

void distance_transform(std::vector<Offset>& grid, int w, int h) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            propagate(grid, w, h, x, y, -1, 0);
            propagate(grid, w, h, x, y, 0, -1);
            propagate(grid, w, h, x, y, -1, -1);
            propagate(grid, w, h, x, y, 1, -1);
        }
        for (int x = w - 1; x >= 0; --x) {
            propagate(grid, w, h, x, y, 1, 0);
        }
    }
    for (int y = h - 1; y >= 0; --y) {
        for (int x = w - 1; x >= 0; --x) {
            propagate(grid, w, h, x, y, 1, 0);
            propagate(grid, w, h, x, y, 0, 1);
            propagate(grid, w, h, x, y, 1, 1);
            propagate(grid, w, h, x, y, -1, 1);
        }
        for (int x = 0; x < w; ++x) {
            propagate(grid, w, h, x, y, -1, 0);
        }
    }
}

} // namespace

std::span<const std::uint8_t> glyph_bitmap(char c) noexcept {
    const std::size_t index =
        has_glyph(c) ? static_cast<std::size_t>(c - kFirstGlyph) : 0u; // 0 == space == blank
    return {kGlyphs[index], kGlyphWidth};
}

FontAtlas build_font_atlas(const FontAtlasDesc& desc) {
    FontAtlas atlas;
    const std::uint32_t scale = std::max(1u, desc.scale);
    const std::uint32_t pad = desc.padding;

    atlas.cell_width = kGlyphWidth * scale + 2u * pad;
    atlas.cell_height = kGlyphHeight * scale + 2u * pad;
    atlas.columns = 16u; // 95 glyphs -> a 16 x 6 grid, both powers-of-two-friendly and compact
    const std::uint32_t rows =
        (static_cast<std::uint32_t>(kGlyphCount) + atlas.columns - 1u) / atlas.columns;
    atlas.width = atlas.columns * atlas.cell_width;
    atlas.height = rows * atlas.cell_height;
    atlas.scale = scale;
    atlas.padding = pad;
    atlas.distance_range = desc.distance_range;
    atlas.pixels.assign(static_cast<std::size_t>(atlas.width) * atlas.height, 0u);

    const int cw = static_cast<int>(atlas.cell_width);
    const int ch = static_cast<int>(atlas.cell_height);
    // The 0..255 range spans this many ATLAS texels, since `distance_range` is quoted in source
    // pixels and one source pixel is `scale` texels.
    const float range_texels = std::max(1.0f, desc.distance_range * static_cast<float>(scale));

    std::vector<Offset> inside(static_cast<std::size_t>(cw) * ch);
    std::vector<Offset> outside(inside.size());

    for (std::size_t g = 0; g < kGlyphCount; ++g) {
        // Seed the two fields from the upscaled bitmap. A cell that is INK seeds the "inside" field
        // with a zero offset and is far from the "outside" one, and vice versa — the two transforms
        // then give distance-to-edge from either side, and their difference is the SIGNED distance.
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                const int sx = (x - static_cast<int>(pad)) / static_cast<int>(scale);
                const int sy = (y - static_cast<int>(pad)) / static_cast<int>(scale);
                bool ink = false;
                if (x >= static_cast<int>(pad) && y >= static_cast<int>(pad) &&
                    sx < static_cast<int>(kGlyphWidth) && sy < static_cast<int>(kGlyphHeight)) {
                    ink = (kGlyphs[g][sx] >> sy) & 1u;
                }
                const std::size_t i = static_cast<std::size_t>(y) * cw + x;
                inside[i] = ink ? Offset{0, 0} : kFar;
                outside[i] = ink ? kFar : Offset{0, 0};
            }
        }
        distance_transform(inside, cw, ch);
        distance_transform(outside, cw, ch);

        const std::uint32_t cell_x = static_cast<std::uint32_t>(g % atlas.columns);
        const std::uint32_t cell_y = static_cast<std::uint32_t>(g / atlas.columns);
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * cw + x;
                // Positive INSIDE the glyph. `outside` measures how far this texel is from ink, so
                // for an ink texel it is 0 and `inside` carries the distance to the nearest gap.
                const float d = std::sqrt(static_cast<float>(outside[i].dist2())) -
                                std::sqrt(static_cast<float>(inside[i].dist2()));
                const float t = std::clamp(0.5f + 0.5f * (d / range_texels), 0.0f, 1.0f);
                const std::uint32_t px = cell_x * atlas.cell_width + static_cast<std::uint32_t>(x);
                const std::uint32_t py = cell_y * atlas.cell_height + static_cast<std::uint32_t>(y);
                atlas.pixels[static_cast<std::size_t>(py) * atlas.width + px] =
                    static_cast<std::uint8_t>(std::lround(t * 255.0f));
            }
        }
    }
    return atlas;
}

GlyphUv glyph_uv(const FontAtlas& atlas, char c) noexcept {
    GlyphUv uv;
    if (!atlas.valid() || atlas.columns == 0) {
        return uv;
    }
    const std::size_t g = has_glyph(c) ? static_cast<std::size_t>(c - kFirstGlyph) : 0u;
    const auto cell_x =
        static_cast<float>(g % atlas.columns) * static_cast<float>(atlas.cell_width);
    const auto cell_y =
        static_cast<float>(g / atlas.columns) * static_cast<float>(atlas.cell_height);
    uv.u0 = cell_x / static_cast<float>(atlas.width);
    uv.v0 = cell_y / static_cast<float>(atlas.height);
    uv.u1 = (cell_x + static_cast<float>(atlas.cell_width)) / static_cast<float>(atlas.width);
    uv.v1 = (cell_y + static_cast<float>(atlas.cell_height)) / static_cast<float>(atlas.height);
    return uv;
}

} // namespace rime::render::text
