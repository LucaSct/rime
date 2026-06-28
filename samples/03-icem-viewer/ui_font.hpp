// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// A tiny built-in bitmap font for the from-scratch UI (E2). Each glyph is a 5×7 cell drawn as
// **string art** — seven rows of '#'/' ' you can read straight from the source — so the font is
// self-documenting and there is no opaque hex array to mis-transcribe (the teaching rule). The
// atlas builder rasterises the printable ASCII range into one R-coverage texture (one 8×8 cell per
// code point, glyph in the top-left 5×7), which the UI samples per glyph quad. Uppercase + digits +
// the punctuation a control panel needs; lower-case code points fall back to their upper-case
// glyph, and anything undefined is blank. See docs/math/ui-text-layout.md.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace rime::viewer::ui {

constexpr std::uint32_t kGlyphW = 5; // drawn glyph width  (pixels)
constexpr std::uint32_t kGlyphH = 7; // drawn glyph height (pixels)
constexpr std::uint32_t kCell = 8;   // atlas cell (square, glyph in the top-left, the rest padding)
constexpr std::uint32_t kFirst = 32; // first code point in the atlas (space)
constexpr std::uint32_t kLast = 126; // last code point (~)
constexpr std::uint32_t kCols = 16;  // cells per atlas row
constexpr std::uint32_t kCount = kLast - kFirst + 1;
constexpr std::uint32_t kRows = (kCount + kCols - 1) / kCols;
constexpr std::uint32_t kAtlasW = kCols * kCell;
constexpr std::uint32_t kAtlasH = kRows * kCell;

namespace detail {

// The 5×7 art for an ASCII code point, as 7 row strings (each ≥5 chars), or nullptr for a blank
// cell. Lower-case letters are mapped to their upper-case glyph by the caller. The table is
// deliberately one readable line per character, so clang-format is turned off across it.
// clang-format off
[[nodiscard]] inline const char* const* glyph(char c) {
    switch (c) {
    case 'A': { static const char* g[7] = {" ### ", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}; return g; }
    case 'B': { static const char* g[7] = {"#### ", "#   #", "#   #", "#### ", "#   #", "#   #", "#### "}; return g; }
    case 'C': { static const char* g[7] = {" ### ", "#   #", "#    ", "#    ", "#    ", "#   #", " ### "}; return g; }
    case 'D': { static const char* g[7] = {"###  ", "#  # ", "#   #", "#   #", "#   #", "#  # ", "###  "}; return g; }
    case 'E': { static const char* g[7] = {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#####"}; return g; }
    case 'F': { static const char* g[7] = {"#####", "#    ", "#    ", "#### ", "#    ", "#    ", "#    "}; return g; }
    case 'G': { static const char* g[7] = {" ### ", "#   #", "#    ", "# ###", "#   #", "#   #", " ### "}; return g; }
    case 'H': { static const char* g[7] = {"#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #"}; return g; }
    case 'I': { static const char* g[7] = {" ### ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}; return g; }
    case 'J': { static const char* g[7] = {"  ###", "   # ", "   # ", "   # ", "#  # ", "#  # ", " ##  "}; return g; }
    case 'K': { static const char* g[7] = {"#   #", "#  # ", "# #  ", "##   ", "# #  ", "#  # ", "#   #"}; return g; }
    case 'L': { static const char* g[7] = {"#    ", "#    ", "#    ", "#    ", "#    ", "#    ", "#####"}; return g; }
    case 'M': { static const char* g[7] = {"#   #", "## ##", "# # #", "# # #", "#   #", "#   #", "#   #"}; return g; }
    case 'N': { static const char* g[7] = {"#   #", "##  #", "# # #", "# # #", "#  ##", "#   #", "#   #"}; return g; }
    case 'O': { static const char* g[7] = {" ### ", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}; return g; }
    case 'P': { static const char* g[7] = {"#### ", "#   #", "#   #", "#### ", "#    ", "#    ", "#    "}; return g; }
    case 'Q': { static const char* g[7] = {" ### ", "#   #", "#   #", "#   #", "# # #", "#  # ", " ## #"}; return g; }
    case 'R': { static const char* g[7] = {"#### ", "#   #", "#   #", "#### ", "# #  ", "#  # ", "#   #"}; return g; }
    case 'S': { static const char* g[7] = {" ####", "#    ", "#    ", " ### ", "    #", "    #", "#### "}; return g; }
    case 'T': { static const char* g[7] = {"#####", "  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "  #  "}; return g; }
    case 'U': { static const char* g[7] = {"#   #", "#   #", "#   #", "#   #", "#   #", "#   #", " ### "}; return g; }
    case 'V': { static const char* g[7] = {"#   #", "#   #", "#   #", "#   #", "#   #", " # # ", "  #  "}; return g; }
    case 'W': { static const char* g[7] = {"#   #", "#   #", "#   #", "# # #", "# # #", "## ##", "#   #"}; return g; }
    case 'X': { static const char* g[7] = {"#   #", "#   #", " # # ", "  #  ", " # # ", "#   #", "#   #"}; return g; }
    case 'Y': { static const char* g[7] = {"#   #", "#   #", " # # ", "  #  ", "  #  ", "  #  ", "  #  "}; return g; }
    case 'Z': { static const char* g[7] = {"#####", "    #", "   # ", "  #  ", " #   ", "#    ", "#####"}; return g; }
    case '0': { static const char* g[7] = {" ### ", "#   #", "#  ##", "# # #", "##  #", "#   #", " ### "}; return g; }
    case '1': { static const char* g[7] = {"  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### "}; return g; }
    case '2': { static const char* g[7] = {" ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####"}; return g; }
    case '3': { static const char* g[7] = {" ### ", "#   #", "    #", "  ## ", "    #", "#   #", " ### "}; return g; }
    case '4': { static const char* g[7] = {"   # ", "  ## ", " # # ", "#  # ", "#####", "   # ", "   # "}; return g; }
    case '5': { static const char* g[7] = {"#####", "#    ", "#### ", "    #", "    #", "#   #", " ### "}; return g; }
    case '6': { static const char* g[7] = {" ### ", "#    ", "#    ", "#### ", "#   #", "#   #", " ### "}; return g; }
    case '7': { static const char* g[7] = {"#####", "    #", "   # ", "  #  ", " #   ", " #   ", " #   "}; return g; }
    case '8': { static const char* g[7] = {" ### ", "#   #", "#   #", " ### ", "#   #", "#   #", " ### "}; return g; }
    case '9': { static const char* g[7] = {" ### ", "#   #", "#   #", " ####", "    #", "    #", " ### "}; return g; }
    case '.': { static const char* g[7] = {"     ", "     ", "     ", "     ", "     ", " ##  ", " ##  "}; return g; }
    case ',': { static const char* g[7] = {"     ", "     ", "     ", "     ", " ##  ", " ##  ", " #   "}; return g; }
    case ':': { static const char* g[7] = {"     ", " ##  ", " ##  ", "     ", " ##  ", " ##  ", "     "}; return g; }
    case '-': { static const char* g[7] = {"     ", "     ", "     ", "#####", "     ", "     ", "     "}; return g; }
    case '+': { static const char* g[7] = {"     ", "  #  ", "  #  ", "#####", "  #  ", "  #  ", "     "}; return g; }
    case '=': { static const char* g[7] = {"     ", "     ", "#####", "     ", "#####", "     ", "     "}; return g; }
    case '/': { static const char* g[7] = {"    #", "    #", "   # ", "  #  ", " #   ", "#    ", "#    "}; return g; }
    case '%': { static const char* g[7] = {"##  #", "##  #", "   # ", "  #  ", " #   ", "#  ##", "#  ##"}; return g; }
    case '(': { static const char* g[7] = {"  ## ", " #   ", " #   ", " #   ", " #   ", " #   ", "  ## "}; return g; }
    case ')': { static const char* g[7] = {" ##  ", "   # ", "   # ", "   # ", "   # ", "   # ", " ##  "}; return g; }
    case '!': { static const char* g[7] = {"  #  ", "  #  ", "  #  ", "  #  ", "  #  ", "     ", "  #  "}; return g; }
    case '?': { static const char* g[7] = {" ### ", "#   #", "    #", "   # ", "  #  ", "     ", "  #  "}; return g; }
    case '#': { static const char* g[7] = {" # # ", "#####", " # # ", " # # ", "#####", " # # ", "     "}; return g; }
    case '<': { static const char* g[7] = {"   # ", "  #  ", " #   ", "#    ", " #   ", "  #  ", "   # "}; return g; }
    case '>': { static const char* g[7] = {" #   ", "  #  ", "   # ", "    #", "   # ", "  #  ", " #   "}; return g; }
    default: return nullptr; // space and anything undefined: a blank cell
    }
}

// clang-format on

} // namespace detail

// Rasterise the font into an RGBA8 coverage atlas (kAtlasW × kAtlasH). Each pixel is white where
// the glyph bit is set and transparent-black elsewhere; the UI samples the red channel as glyph
// coverage and alpha-tests it (no blending needed). Code point p sits in cell (p − kFirst): column
// %16, row /16.
[[nodiscard]] inline std::vector<std::uint8_t> build_font_atlas() {
    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(kAtlasW) * kAtlasH * 4, 0);
    for (std::uint32_t p = kFirst; p <= kLast; ++p) {
        char c = static_cast<char>(p);
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A'); // fold lower → upper
        const char* const* rows = detail::glyph(c);
        if (!rows)
            continue;
        const std::uint32_t cell = p - kFirst;
        const std::uint32_t cx = (cell % kCols) * kCell;
        const std::uint32_t cy = (cell / kCols) * kCell;
        for (std::uint32_t gy = 0; gy < kGlyphH; ++gy) {
            const char* row = rows[gy];
            for (std::uint32_t gx = 0; gx < kGlyphW; ++gx) {
                if (row[gx] == '\0')
                    break;
                if (row[gx] != '#')
                    continue;
                const std::size_t px =
                    (static_cast<std::size_t>(cy + gy) * kAtlasW + (cx + gx)) * 4;
                atlas[px + 0] = atlas[px + 1] = atlas[px + 2] = atlas[px + 3] = 255;
            }
        }
    }
    return atlas;
}

} // namespace rime::viewer::ui
