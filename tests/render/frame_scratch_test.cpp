// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the per-frame CPU→GPU scratch ring (m17.4, ADR-0041) — the thing that has to exist
// before the frame loop is allowed to stop waiting for the GPU.
//
// The hazard, stated precisely, because it is easy to state loosely and then defend the wrong
// thing. Submissions are GPU work and the graph already orders GPU work against GPU work with
// barriers and queue order. A CPU write to host-visible memory is not on the queue at all. So the
// moment the loop pipelines, frame N+1's `write_buffer` into a system's own uniform buffer lands
// while the GPU is still reading that buffer for frame N, and frame N renders with frame N+1's
// numbers. `submit_blocking` hides it perfectly — which is how seven lighting systems came to own
// fourteen unringed host-visible buffers with nothing ever looking wrong.
//
// Four claims, and the fourth is the one that makes the design non-obvious:
//
//   1. The ring turns over. Consecutive frames land in DIFFERENT memory, and the slot is only
//      reused after a full lap — which is the whole safety argument, since a lap is longer than
//      the GPU is ever behind.
//   2. Slices within one frame do not overlap, and every offset is legally alignable as a uniform
//      binding offset.
//   3. What was pushed is what is there. A ring that hands out tidy addresses and drops the copy
//      passes claims 1 and 2 without rendering anything correctly — the vacuity guard.
//   4. A push that does not fit takes a FRESH BLOCK and leaves earlier slices alone. The obvious
//      implementation grows the buffer, and growing means destroying the buffer that passes
//      earlier in this same frame are still pointing at. That is a use-after-free the ring would
//      have introduced while fixing a race.

#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/rhi/rhi.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::vulkan_required;

namespace {

// Read a slice back through the device, so the check is "what the GPU would see", not "what the
// CPU remembers writing".
[[nodiscard]] std::vector<std::uint32_t>
read_slice(rhi::Device& device, RenderGraph::FrameSlice slice, std::size_t count) {
    std::vector<std::uint32_t> out(count, 0xDEADBEEFu);
    device.read_buffer(slice.buffer, out.data(), count * sizeof(std::uint32_t), slice.offset);
    return out;
}

} // namespace

TEST_CASE("frame scratch: the ring turns over, and a slice survives the frame that made it") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available — skipping the frame-scratch proof");
        return;
    }

    RenderGraph graph(*device);
    graph.set_frames_in_flight(2); // => 3 slots, the swapchain rule
    REQUIRE(graph.frame_slot_count() == 3);

    SUBCASE("consecutive frames land in different memory, and the ring returns after one lap") {
        // The safety argument itself. If two consecutive frames shared memory, pipelining would
        // corrupt the older one the moment the CPU ran ahead.
        const std::uint32_t slots = graph.frame_slot_count();
        std::vector<RenderGraph::FrameSlice> firsts;
        for (std::uint32_t f = 0; f < slots * 2; ++f) {
            graph.reset();
            const std::uint32_t payload = f;
            firsts.push_back(graph.push_frame_data(&payload, sizeof(payload)));
        }

        // Within a lap every frame has its own buffer…
        for (std::uint32_t a = 0; a < slots; ++a) {
            for (std::uint32_t b = a + 1; b < slots; ++b)
                CHECK_FALSE(firsts[a].buffer == firsts[b].buffer);
        }
        // …and a lap later the ring is back where it started, which is what bounds the memory.
        for (std::uint32_t a = 0; a < slots; ++a) {
            CHECK(firsts[a].buffer == firsts[a + slots].buffer);
            CHECK(firsts[a].offset == firsts[a + slots].offset);
        }
    }

    SUBCASE("slices in one frame are disjoint and 256-aligned") {
        graph.reset();
        std::vector<RenderGraph::FrameSlice> slices;
        for (std::uint32_t i = 0; i < 8; ++i) {
            const std::uint32_t payload = i;
            slices.push_back(graph.push_frame_data(&payload, sizeof(payload)));
        }
        for (const RenderGraph::FrameSlice& s : slices)
            CHECK(s.offset % 256 == 0);
        std::size_t collisions = 0;
        for (std::size_t a = 0; a < slices.size(); ++a) {
            for (std::size_t b = a + 1; b < slices.size(); ++b) {
                if (slices[a].buffer == slices[b].buffer && slices[a].offset == slices[b].offset)
                    ++collisions;
            }
        }
        CHECK(collisions == 0);
    }

    SUBCASE("what was pushed is what is there — the vacuity guard") {
        graph.reset();
        const std::vector<std::uint32_t> a{1u, 2u, 3u, 4u};
        const std::vector<std::uint32_t> b{9u, 8u, 7u, 6u};
        const auto sa = graph.push_frame_data(a.data(), a.size() * sizeof(std::uint32_t));
        const auto sb = graph.push_frame_data(b.data(), b.size() * sizeof(std::uint32_t));
        CHECK(read_slice(*device, sa, a.size()) == a);
        CHECK(read_slice(*device, sb, b.size()) == b);
    }

    SUBCASE("a push that overflows the block takes a fresh one and does NOT move the old slices") {
        // The claim that rules out the obvious implementation. Grow-in-place would free the buffer
        // `first` points into while this frame's earlier passes still hold it.
        graph.reset();
        const std::vector<std::uint32_t> first{0xA1u, 0xA2u, 0xA3u, 0xA4u};
        const auto sfirst =
            graph.push_frame_data(first.data(), first.size() * sizeof(std::uint32_t));

        // Fill well past one 64 KiB block, in chunks big enough to get there quickly.
        const std::vector<std::uint32_t> chunk(4096, 0x5Au); // 16 KiB
        bool took_new_block = false;
        for (int i = 0; i < 8; ++i) {
            const auto s =
                graph.push_frame_data(chunk.data(), chunk.size() * sizeof(std::uint32_t));
            if (!(s.buffer == sfirst.buffer))
                took_new_block = true;
        }
        CHECK(took_new_block); // it really did chain, so the case below is not vacuous

        // The original slice still reads back its own bytes: not freed, not overwritten, not moved.
        CHECK(read_slice(*device, sfirst, first.size()) == first);
    }

    SUBCASE("the ring depth follows the frames-in-flight rule, and rebuilding is idempotent") {
        graph.set_frames_in_flight(1);
        CHECK(graph.frame_slot_count() == 2);
        graph.set_frames_in_flight(1); // no-op, must not destroy live buffers
        CHECK(graph.frame_slot_count() == 2);
        graph.set_frames_in_flight(3);
        CHECK(graph.frame_slot_count() == 4);
    }
}
