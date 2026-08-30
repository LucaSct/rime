// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// m13.3b — the font and its distance-field atlas. GPU-free: this is all CPU data.
//
// THE DIVISION OF LABOUR WITH THE SOURCE. `font.cpp` holds 95 glyphs as 475 packed hex bytes,
// which no reviewer can check by eye. So the human-legible form lives HERE: a handful of glyphs are
// rendered back into ASCII art and compared against letters written out longhand. If the packing,
// the bit order, or the column/row convention is ever wrong, these fail with a picture of the wrong
// letter rather than a hex mismatch.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "rime/render/text/font.hpp"

using namespace rime::render::text;

namespace {

// Render a glyph back to art: one line per row, '#' for ink. The inverse of the packing step, so a
// mismatch names the exact convention that drifted.
[[nodiscard]] std::vector<std::string> art(char c) {
    const std::span<const std::uint8_t> cols = glyph_bitmap(c);
    std::vector<std::string> rows;
    for (std::uint32_t y = 0; y < kGlyphHeight; ++y) {
        std::string line;
        for (std::uint32_t x = 0; x < kGlyphWidth; ++x) {
            line += ((cols[x] >> y) & 1u) ? '#' : ' ';
        }
        rows.push_back(line);
    }
    return rows;
}

} // namespace

TEST_CASE("m13.3b: the packed glyphs are the letters they claim to be") {
    CHECK(art('A') ==
          std::vector<std::string>{"  #  ", " # # ", "#   #", "#   #", "#####", "#   #", "#   #"});
    CHECK(art('E') ==
          std::vector<std::string>{"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"});
    CHECK(art('0') ==
          std::vector<std::string>{" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "});
    CHECK(art('7') ==
          std::vector<std::string>{"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "});
    // The one that catches a transposed row/column convention: '.' is ink only at the BOTTOM.
    CHECK(art('.') ==
          std::vector<std::string>{"     ", "     ", "     ", "     ", "     ", "  ## ", "  ## "});
    // …and the one that catches a flipped row order, since ' is ink only at the TOP.
    CHECK(art('\'') ==
          std::vector<std::string>{"  #  ", "  #  ", "     ", "     ", "     ", "     ", "     "});
}

TEST_CASE("m13.3b: every printable character has a glyph, and space is the only blank one") {
    std::size_t blanks = 0;
    for (char c = kFirstGlyph; c <= kLastGlyph; ++c) {
        CHECK(has_glyph(c));
        const std::span<const std::uint8_t> cols = glyph_bitmap(c);
        REQUIRE(cols.size() == kGlyphWidth);
        std::uint8_t any = 0;
        for (const std::uint8_t m : cols) {
            any = static_cast<std::uint8_t>(any | m);
            // Only the low 7 bits are the 7 rows; a set high bit means the packing overflowed.
            CHECK((m & 0x80u) == 0u);
        }
        if (any == 0) {
            ++blanks;
        }
    }
    // A blank glyph in the middle of the alphabet is the failure this catches: it renders as a hole
    // and is easy to miss in a HUD full of numbers.
    CHECK(blanks == 1);

    // Out of range maps to the blank cell, never to a wrong character.
    CHECK_FALSE(has_glyph('\n'));
    CHECK_FALSE(has_glyph(static_cast<char>(127)));
    std::uint8_t any = 0;
    for (const std::uint8_t m : glyph_bitmap('\n')) {
        any = static_cast<std::uint8_t>(any | m);
    }
    CHECK(any == 0);
}

TEST_CASE("m13.3b: the distance field is signed, centred on the edge, and monotone") {
    const FontAtlas atlas = build_font_atlas();
    REQUIRE(atlas.valid());
    CHECK(atlas.columns == 16);
    CHECK(atlas.pixels.size() == static_cast<std::size_t>(atlas.width) * atlas.height);

    const auto sample = [&atlas](char c, std::uint32_t x, std::uint32_t y) {
        const std::size_t g = static_cast<std::size_t>(c - kFirstGlyph);
        const std::uint32_t cx =
            static_cast<std::uint32_t>(g % atlas.columns) * atlas.cell_width + x;
        const std::uint32_t cy =
            static_cast<std::uint32_t>(g / atlas.columns) * atlas.cell_height + y;
        return atlas.pixels[static_cast<std::size_t>(cy) * atlas.width + cx];
    };

    SUBCASE("ink reads above the 128 mid-point and empty space below it") {
        // 'I' is a full-width bar on its top row, so the centre of that bar is deep inside ink and
        // the padding margin is deep outside it. 128 is the edge; the shader thresholds there.
        const FontAtlasDesc desc{};
        const std::uint32_t mid_of_top_bar = desc.padding + desc.scale / 2u;
        CHECK(sample('I', desc.padding + 2u * desc.scale, mid_of_top_bar) > 128);
        CHECK(sample('I', 0, 0) < 128); // the corner of the padding: far outside
    }

    SUBCASE("the field decreases monotonically as you leave the glyph") {
        // '_' is ink on its BOTTOM row only, so "away from the glyph" is UPWARD from that row —
        // decreasing y from the ink's top edge toward the cell's top padding. (The first draft of
        // this case walked the other way, from the bottom padding upward, which moves TOWARD the
        // ink and correctly reported an increase.)
        //
        // A field that is not monotone away from its edge produces visible banding in the rendered
        // glyph, and non-monotonicity is the signature of a sweep that missed a direction.
        const FontAtlasDesc desc{};
        const std::uint32_t x = desc.padding + 2u * desc.scale;
        const std::uint32_t top_ink_row = desc.padding + (kGlyphHeight - 1u) * desc.scale;
        int previous = sample('_', x, top_ink_row);
        CHECK(previous > 128); // non-vacuity: we really did start inside the ink
        for (std::uint32_t y = top_ink_row; y-- > 0;) {
            const int here = sample('_', x, y);
            CHECK(here <= previous);
            previous = here;
        }
        CHECK(previous < 128); // …and really did end outside it
    }

    SUBCASE("space is uniformly outside") {
        // No ink anywhere means every texel is maximally far from a glyph — and crucially NOT the
        // 128 an unseeded field would produce.
        for (std::uint32_t y = 0; y < atlas.cell_height; ++y) {
            for (std::uint32_t x = 0; x < atlas.cell_width; ++x) {
                CHECK(sample(' ', x, y) < 128);
            }
        }
    }

    SUBCASE("generation is deterministic — the atlas is content, and content must not drift") {
        const FontAtlas again = build_font_atlas();
        CHECK(again.pixels == atlas.pixels);
    }
}

TEST_CASE("m13.3b: glyph UVs tile the atlas without overlap or gap") {
    const FontAtlas atlas = build_font_atlas();
    const GlyphUv a = glyph_uv(atlas, 'A');
    const GlyphUv b = glyph_uv(atlas, 'B');

    // Adjacent glyphs on a row are exactly one cell apart, and a cell's own span matches.
    CHECK(b.u0 == doctest::Approx(a.u1));
    CHECK(b.v0 == doctest::Approx(a.v0));
    CHECK(a.u1 - a.u0 ==
          doctest::Approx(static_cast<float>(atlas.cell_width) / static_cast<float>(atlas.width)));
    CHECK(a.v1 - a.v0 == doctest::Approx(static_cast<float>(atlas.cell_height) /
                                         static_cast<float>(atlas.height)));

    // Everything stays inside [0,1] — a UV that ran past the edge would wrap onto another glyph and
    // print the wrong letter, which is exactly the failure has_glyph()'s blank fallback avoids.
    for (char c = kFirstGlyph; c <= kLastGlyph; ++c) {
        const GlyphUv uv = glyph_uv(atlas, c);
        CHECK(uv.u0 >= 0.0f);
        CHECK(uv.v0 >= 0.0f);
        CHECK(uv.u1 <= doctest::Approx(1.0f));
        CHECK(uv.v1 <= doctest::Approx(1.0f));
    }
}
