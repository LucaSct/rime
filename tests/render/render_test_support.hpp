// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Shared helpers for the render module's GPU pixel proofs (the M5.6 pattern): read a rendered
// texture back to the CPU, decode the RGBA16F HDR target to linear radiance, and project a world
// point to a pixel with the renderer's own matrices. Promoted out of pbr_pipeline_test.cpp so the
// shadow proof (m10.1) and later lighting proofs share one source of truth instead of copying it.
// GPU-free except read_texture (which needs a device); no golden images — every proof is
// structural.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/rhi/rhi.hpp"

namespace rime::render::test {

// CI sets RIME_REQUIRE_VULKAN so a missing device is a failure, not a skip.
[[nodiscard]] inline bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// IEEE 754 half → float (sign, rebiased exponent, normalized mantissa). The HDR target is
// RGBA16Float; the CPU decodes it to assert on radiance.
[[nodiscard]] inline float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Copy a rendered texture to CPU bytes (copy → host buffer → read; the tests/rhi readback pattern).
[[nodiscard]] inline std::vector<std::uint8_t> read_texture(rhi::Device& device,
                                                            rhi::TextureHandle texture,
                                                            std::uint32_t width,
                                                            std::uint32_t height,
                                                            std::uint32_t bytes_per_pixel) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * bytes_per_pixel;
    rhi::BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = rhi::BufferUsage::TransferDst;
    rbd.memory = rhi::MemoryUsage::GpuToCpu;
    rbd.debug_name = "render-test-readback";
    const rhi::BufferHandle rb = device.create_buffer(rbd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(texture, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

// A decoded HDR image: linear radiance per channel.
struct HdrImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgb; // 3 floats per pixel

    [[nodiscard]] float luminance(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
        return 0.2126f * rgb[i] + 0.7152f * rgb[i + 1] + 0.0722f * rgb[i + 2];
    }
};

[[nodiscard]] inline HdrImage
decode_hdr(const std::vector<std::uint8_t>& bytes, std::uint32_t width, std::uint32_t height) {
    HdrImage img;
    img.width = width;
    img.height = height;
    img.rgb.resize(static_cast<std::size_t>(width) * height * 3);
    const auto* half = reinterpret_cast<const std::uint16_t*>(bytes.data());
    for (std::size_t p = 0; p < static_cast<std::size_t>(width) * height; ++p) {
        img.rgb[p * 3 + 0] = half_to_float(half[p * 4 + 0]);
        img.rgb[p * 3 + 1] = half_to_float(half[p * 4 + 1]);
        img.rgb[p * 3 + 2] = half_to_float(half[p * 4 + 2]);
    }
    return img;
}

// Project a world point to pixel coordinates with the SAME matrices the renderer uses (perspective
// already bakes Vulkan's y-down NDC, so world +y lands at a LOW row index; no flips here).
struct Pixel {
    float x = 0.0f;
    float y = 0.0f;
};

[[nodiscard]] inline Pixel
project(const core::Mat4& view_proj, core::Vec3 world, std::uint32_t size) {
    const core::Vec4 clip = view_proj * core::Vec4{world.x, world.y, world.z, 1.0f};
    return {(clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(size),
            (clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(size)};
}

} // namespace rime::render::test
