// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// The runtime, in-memory form of a cooked texture (M6.3). Textures cook as RGBA8 with a **full,
// offline-generated mip chain** and an explicit colour-space tag (ADR-0024, decision 5). The chain
// is generated in the Rust pipeline in *linear* space — sRGB inputs are linearized before averaging
// and re-encoded after — so minified surfaces don't darken (the classic gamma-wrong-mips bug). The
// engine uploads each level verbatim through the RHI's per-mip path; it never regenerates the
// chain.
namespace rime::assets {

// The pixel format of a cooked texture. RGBA8, tagged sRGB or linear by *semantic*: baseColor and
// emissive are sRGB (perceptual data); normal, metallic-roughness, and occlusion are linear (data,
// not colour). Compressed formats are reserved — adopted only when a measured GPU-memory need
// appears.
enum class TextureFormat : std::uint32_t {
    Rgba8Unorm = 0, // linear data (normal / metallic-roughness / occlusion)
    Rgba8Srgb = 1,  // perceptual colour (baseColor / emissive)
    // 2 and 3 stay reserved for Bc1/Bc3 (ADR-0024's original list); m16.7 makes 4 and 5 real and
    // appends 6, because BC7 needs both colour spaces exactly as RGBA8 does.
    Bc5Unorm = 4, // two channels, 8 bpp — normal maps, with Z reconstructed in the shader
    Bc7Unorm = 5, // RGBA, 8 bpp, linear data
    Bc7Srgb = 6,  // RGBA, 8 bpp, perceptual colour
};

// Is this format stored as 4x4 blocks rather than individual texels? Block formats round their
// extents UP to whole blocks, which is the rule a size computation has to obey — a 5-wide image is
// two blocks per row, not 1.25, and a 1x1 mip still costs a whole block.
[[nodiscard]] constexpr bool is_block_format(TextureFormat f) noexcept {
    return f == TextureFormat::Bc5Unorm || f == TextureFormat::Bc7Unorm ||
           f == TextureFormat::Bc7Srgb;
}

// Bytes one mip level of `format` at `width` x `height` occupies, tightly packed. This is the
// single definition the reader validates against and the uploader sizes from; before m16.7 it was
// the literal `width * height * 4` spelled out at each site, which a block format silently breaks.
[[nodiscard]] constexpr std::uint64_t
texture_level_bytes(TextureFormat format, std::uint32_t width, std::uint32_t height) noexcept {
    if (!is_block_format(format)) {
        return std::uint64_t{width} * height * 4;
    }
    const std::uint64_t bw = (std::uint64_t{width} + 3) / 4;
    const std::uint64_t bh = (std::uint64_t{height} + 3) / 4;
    return bw * bh * 16; // every BC format here is 16 bytes per 4x4 block
}

// One mip level: its extent and the byte range of its pixels within TextureAsset::pixels.
struct TextureMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t offset = 0; // byte offset into TextureAsset::pixels
    std::uint32_t size = 0;   // width * height * 4 for RGBA8
};

// A cooked texture in memory: the base extent, the colour-space-tagged format, the mip table (level
// 0 first), and every level's pixels concatenated. The layout is what the RHI's
// `write_texture_mips` consumes — `mips[i]` slices `pixels` directly, so upload is a set of
// memcpys, not a transform.
struct TextureAsset {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    TextureFormat format = TextureFormat::Rgba8Unorm;
    std::vector<TextureMip> mips;
    std::vector<std::byte> pixels;
};

// RGBA8: four bytes per texel, whatever the colour space. A named constant so the reader, the cook,
// and the tests all size mips the same way (and so a future BCn block size has one place to
// branch).
inline constexpr std::uint32_t kTextureBytesPerPixel = 4;

// The number of mip levels a full chain has for a base extent: each level halves (floor) until 1×1,
// i.e. floor(log2(max(width, height))) + 1. This is the single definition of "full chain" shared by
// the cooker (which generates it), this reader (which requires it), and the RHI create_texture
// clamp — so all three agree without a magic number. A v1 cooked texture always carries a full
// chain.
[[nodiscard]] constexpr std::uint32_t full_mip_count(std::uint32_t width,
                                                     std::uint32_t height) noexcept {
    std::uint32_t max_dim = width > height ? width : height;
    std::uint32_t levels = 1;
    while (max_dim > 1) {
        max_dim >>= 1;
        ++levels;
    }
    return levels;
}

// One mip level's extent: the base dimension shifted right by the level, floored at 1 (a 1-wide
// texture stays 1 wide as its height keeps halving — matching how the GPU addresses the chain).
[[nodiscard]] constexpr std::uint32_t mip_extent(std::uint32_t base, std::uint32_t level) noexcept {
    const std::uint32_t d = base >> level;
    return d == 0 ? 1u : d;
}

} // namespace rime::assets
