// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// m16.1 — block-compressed formats and the device capability that gates them (ADR-0039).
//
// Two halves, deliberately separated:
//
//   1. The SIZE MATHS is pure and needs no GPU. It is the part that goes wrong quietly: the cooked
//      texture reader currently validates `size == width * height * 4`, and a block format breaks
//      that rule in a way that produces a plausible-but-wrong number rather than an obvious error.
//      A 5-wide BC7 image is two blocks per row, not 1.25, and a 1x1 mip still costs a whole
//      16-byte block.
//
//   2. WHETHER THIS MACHINE CAN SAMPLE THEM is a question about the driver, and it is the one the
//      planning spike could not answer: BC support was measured on this workstation's two hardware
//      devices, but CI runs lavapipe, which was not installed locally. Rather than assume in either
//      direction, the case below REPORTS what the device says and, if it says yes, proves the claim
//      by actually creating a BC texture. So CI answers the question permanently, in its own logs.

#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "rime/rhi/rhi.hpp"

using namespace rime;

TEST_CASE("m16.1: block-format size maths rounds up to whole blocks") {
    // Uncompressed formats are 1x1 blocks, so the familiar product still holds.
    CHECK(rhi::format_image_size(rhi::Format::RGBA8Unorm, 8, 4) == 8u * 4u * 4u);
    CHECK(rhi::format_image_size(rhi::Format::R8Unorm, 8, 4) == 32u);
    CHECK_FALSE(rhi::is_block_compressed(rhi::Format::RGBA8Unorm));

    // BC7: 16 bytes per 4x4 block ⇒ 8 bits per texel at exact multiples.
    CHECK(rhi::is_block_compressed(rhi::Format::BC7Unorm));
    CHECK(rhi::format_image_size(rhi::Format::BC7Unorm, 8, 8) == 4u * 16u); // 2x2 blocks
    CHECK(rhi::format_image_size(rhi::Format::BC7Unorm, 8, 8) ==
          rhi::format_image_size(rhi::Format::RGBA8Unorm, 8, 8) / 4);

    // THE ROUNDING, which is the whole reason this helper exists. A 5x5 image is 2x2 blocks, not
    // "1.25 squared"; a 1x1 mip still costs one full block. A reader that computed w*h*bpp would
    // demand 25 bytes for the first and reject the correct 64.
    CHECK(rhi::format_image_size(rhi::Format::BC7Unorm, 5, 5) == 4u * 16u);
    CHECK(rhi::format_image_size(rhi::Format::BC7Unorm, 1, 1) == 16u);
    CHECK(rhi::format_image_size(rhi::Format::BC7Unorm, 4, 1) == 16u);
    CHECK(rhi::format_image_size(rhi::Format::BC5Unorm, 1, 1) == 16u);

    // sRGB is a view of the same bits — same size, or a chain cooked once could not be read twice.
    CHECK(rhi::format_image_size(rhi::Format::BC7Srgb, 64, 64) ==
          rhi::format_image_size(rhi::Format::BC7Unorm, 64, 64));

    // A vertex-attribute-only format has no image size, and says so with 0 rather than guessing:
    // a caller sizing an allocation gets an obviously wrong answer it can check.
    CHECK(rhi::format_image_size(rhi::Format::Undefined, 4, 4) == 0u);
}

TEST_CASE("m16.1: the device reports whether it can sample block-compressed textures") {
    auto device = rhi::create_device({});
    if (!device) {
        MESSAGE("no Vulkan device available — skipping the block-compression capability probe");
        return;
    }

    const rhi::AdapterInfo& adapter = device->adapter();
    // This MESSAGE is the point of the case: it puts the answer in every CI log, for the driver CI
    // actually runs, without anyone having to install lavapipe to find out.
    // std::string, not a bare `const char*` ternary: doctest stringifies a char pointer as its
    // ADDRESS, which turned this line into "block_compression = 0x55c6a53495dd" — a report that
    // reads like a value and carries none. The reporting is this case's whole purpose.
    MESSAGE("adapter '" << adapter.name << "' (driver '" << adapter.driver_name
                        << "') block_compression = "
                        << std::string(adapter.block_compression ? "YES" : "NO"));

    if (!adapter.block_compression) {
        // Not a failure: the Vulkan spec requires BC *or* ETC2 *or* ASTC, so a legitimate device
        // may say no. What must never happen is a BC load silently succeeding with placeholder
        // pixels — ADR-0039 requires a named counter and a warn-once instead, and that is m16.7's
        // to enforce.
        MESSAGE("device does not support BC — the cooked pipeline must refuse BC assets loudly");
        return;
    }

    // The capability claim, made good: if the device says yes, a BC texture must actually create.
    // A flag that is true while creation fails would be worse than a flag that is false.
    rhi::TextureDesc td{};
    td.extent = {8, 8};
    td.format = rhi::Format::BC7Srgb;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "bc7-capability-probe";
    const rhi::TextureHandle tex = device->create_texture(td);
    CHECK(tex.is_valid());
    if (tex.is_valid()) {
        device->destroy(tex);
    }
}
