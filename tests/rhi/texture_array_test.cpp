// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the layered-texture RHI lift (m10.1a, ADR-0032 §10): real array/cube images plus a
// depth-compare sampler — the primitive cascaded shadow maps (m10.1) and cube point-light shadows
// (m10.2) store their slices in, rather than atlas-hacking around the RHI. Two claims:
//
//  (1) Per-layer render targets work. Rendering a distinct clear into two layers of ONE array
//      texture, then reading each layer back, must show each layer holding exactly its own value —
//      i.e. begin_rendering aimed at ColorAttachment::layer wrote that layer and no other, AND the
//      whole-image layout transition covered every layer (the earlier code moved only layer 0). No
//      golden image: two analytically-known clears, compared per texel.
//
//  (2) The creation paths a layered/shadow technique needs — a cube (6 faces), a depth array (N
//      cascades), and a depth-compare (sampler2DShadow) sampler — all build valid Vulkan objects,
//      and a malformed cube is rejected. With validation on (CI sets RIME_REQUIRE_VULKAN), a bad
//      VkImage/VkImageView/VkSampler create-info would fail here, so a valid returned handle IS the
//      structural proof.
//
// GPU-free on lavapipe, like every RHI proof since M3.3.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi: an array texture renders into each layer independently (m10.1a)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping array-texture proof");
        return;
    }

    constexpr std::uint32_t kSize = 4;
    constexpr std::uint32_t kLayers = 2;
    constexpr std::uint32_t kTexelBytes = 4; // RGBA8

    TextureDesc td{};
    td.extent = {kSize, kSize};
    td.array_layers = kLayers;
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc | TextureUsage::Sampled;
    td.debug_name = "m10.1a-array";
    const TextureHandle arr = device->create_texture(td);
    REQUIRE(arr.is_valid());

    // Distinct, analytically-known clears: layer 0 red, layer 1 green.
    const std::array<ClearColor, kLayers> clears = {ClearColor{1.0f, 0.0f, 0.0f, 1.0f},
                                                    ClearColor{0.0f, 1.0f, 0.0f, 1.0f}};

    const std::uint64_t layer_bytes = static_cast<std::uint64_t>(kSize) * kSize * kTexelBytes;
    BufferDesc rbd{};
    rbd.size = layer_bytes;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "m10.1a-readback";

    auto cmd = device->begin_commands();
    // Render each layer in its own scope, aimed at that layer via ColorAttachment::layer. A
    // Clear+Store scope fills the layer with its clear value even with no draw.
    for (std::uint32_t layer = 0; layer < kLayers; ++layer) {
        RenderingInfo ri{};
        ri.color.target = arr;
        ri.color.layer = layer;
        ri.color.load_op = LoadOp::Clear;
        ri.color.store_op = StoreOp::Store;
        ri.color.clear = clears[layer];
        cmd->begin_rendering(ri);
        cmd->end_rendering();
    }
    // Read each layer back into its own buffer (copy_texture_to_buffer's base_layer picks the
    // layer).
    std::array<BufferHandle, kLayers> rb{};
    for (std::uint32_t layer = 0; layer < kLayers; ++layer) {
        rb[layer] = device->create_buffer(rbd);
        cmd->copy_texture_to_buffer(arr, rb[layer], layer);
    }
    device->submit_blocking(*cmd);

    for (std::uint32_t layer = 0; layer < kLayers; ++layer) {
        std::vector<std::uint8_t> px(layer_bytes);
        device->read_buffer(rb[layer], px.data(), px.size(), 0);
        // Every texel of this layer must be its own clear: layer 0 = red (255,0,0), layer 1 =
        // green.
        const std::uint8_t want_r = layer == 0 ? 255 : 0;
        const std::uint8_t want_g = layer == 0 ? 0 : 255;
        bool all_match = true;
        for (std::uint32_t t = 0; t < kSize * kSize; ++t) {
            const std::uint8_t* texel = px.data() + static_cast<std::size_t>(t) * kTexelBytes;
            if (texel[0] != want_r || texel[1] != want_g || texel[2] != 0) {
                all_match = false;
                break;
            }
        }
        CHECK_MESSAGE(all_match, "layer ", layer, " did not hold exactly its own clear");
        device->destroy(rb[layer]);
    }
    device->destroy(arr);
}

TEST_CASE("rhi: cube, depth-array, and depth-compare sampler create cleanly (m10.1a)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping layered-creation proof");
        return;
    }

    SUBCASE("a cube color texture (6 faces) with a samplerCube view") {
        TextureDesc td{};
        td.extent = {8, 8};
        td.array_layers = 6;
        td.cube = true;
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::ColorAttachment | TextureUsage::Sampled;
        td.debug_name = "m10.1a-cube";
        const TextureHandle cube = device->create_texture(td);
        CHECK(cube.is_valid());
        device->destroy(cube);
    }

    SUBCASE("a depth array (CSM cascade storage)") {
        TextureDesc td{};
        td.extent = {16, 16};
        td.array_layers = 3;
        td.format = Format::D32Float;
        td.usage = TextureUsage::DepthStencil | TextureUsage::Sampled;
        td.debug_name = "m10.1a-depth-array";
        const TextureHandle csm = device->create_texture(td);
        CHECK(csm.is_valid());
        device->destroy(csm);
    }

    SUBCASE("a depth-compare (sampler2DShadow) sampler") {
        SamplerDesc sd{};
        sd.compare_enable = true;
        sd.compare_op = CompareOp::LessEqual;
        sd.address_mode = AddressMode::ClampToEdge;
        sd.debug_name = "m10.1a-shadow-sampler";
        const SamplerHandle s = device->create_sampler(sd);
        CHECK(s.is_valid());
        device->destroy(s);
    }

    SUBCASE("a cube whose layer count is not a multiple of 6 is rejected") {
        TextureDesc td{};
        td.extent = {8, 8};
        td.array_layers = 4; // invalid for a cube
        td.cube = true;
        td.format = Format::RGBA8Unorm;
        td.usage = TextureUsage::Sampled;
        td.debug_name = "m10.1a-bad-cube";
        const TextureHandle bad = device->create_texture(td); // logs an error (expected)
        CHECK_FALSE(bad.is_valid());
    }
}
