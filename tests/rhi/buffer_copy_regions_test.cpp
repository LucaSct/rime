// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for CommandBuffer::copy_buffer_regions (m18.5), the upload twin of copy_buffer that a
// staging ring and a slotted page pool need: bytes at one offset of one buffer land at another
// offset of another, several ranges per call. Structural, like every readback proof here: the
// destination is pre-filled with a sentinel, so a region that landed at the wrong offset, a byte
// the copy should not have touched, and a copy that silently did not run all read differently.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "rime/rhi/rhi.hpp"

TEST_CASE("rhi: copy_buffer_regions moves each range to its own offset, and nothing else") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping copy_buffer_regions proof");
        return;
    }

    constexpr std::size_t kBytes = 256;
    constexpr std::uint8_t kSentinel = 0xEE;
    std::vector<std::uint8_t> source(kBytes);
    std::iota(source.begin(), source.end(), std::uint8_t{0}); // byte i holds i
    const std::vector<std::uint8_t> sentinel(kBytes, kSentinel);

    BufferDesc sd{};
    sd.size = kBytes;
    sd.usage = BufferUsage::TransferSrc;
    sd.memory = MemoryUsage::CpuToGpu;
    sd.initial_data = source.data();
    const BufferHandle src = device->create_buffer(sd);
    BufferDesc dd{};
    dd.size = kBytes;
    dd.usage = BufferUsage::TransferDst;
    dd.memory = MemoryUsage::GpuToCpu;
    dd.initial_data = sentinel.data();
    const BufferHandle dst = device->create_buffer(dd);
    REQUIRE(src.is_valid());
    REQUIRE(dst.is_valid());

    const auto run = [&](std::span<const BufferCopyRegion> regions) {
        device->write_buffer(dst, sentinel.data(), kBytes);
        auto cmd = device->begin_commands();
        cmd->copy_buffer_regions(src, dst, regions);
        device->submit_blocking(*cmd);
        std::vector<std::uint8_t> out(kBytes);
        device->read_buffer(dst, out.data(), kBytes);
        return out;
    };

    SUBCASE("two ranges land at their own destination offsets") {
        // Deliberately unaligned sizes and offsets: vkCmdCopyBuffer has no alignment rule, and a
        // page's byte size is whatever the cooker wrote.
        const BufferCopyRegion regions[] = {{10, 100, 7}, {200, 3, 33}};
        const std::vector<std::uint8_t> out = run(regions);
        for (std::size_t i = 0; i < kBytes; ++i) {
            std::uint8_t expected = kSentinel;
            if (i >= 100 && i < 107) {
                expected = static_cast<std::uint8_t>(10 + (i - 100));
            } else if (i >= 3 && i < 36) {
                expected = static_cast<std::uint8_t>(200 + (i - 3));
            }
            CHECK(out[i] == expected);
        }
    }

    SUBCASE("one out-of-range region records nothing at all, not a partial upload") {
        const BufferCopyRegion regions[] = {{0, 0, 16}, {250, 0, 16}}; // second overruns src
        const std::vector<std::uint8_t> out = run(regions);
        CHECK(out == sentinel);
    }

    device->destroy(dst);
    device->destroy(src);
}
