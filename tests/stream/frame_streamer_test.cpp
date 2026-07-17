// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Proof for the S0.2 frame tap. We render a known solid colour off-screen (a clear needs no
// pipeline or shaders), capture it with rime::stream::FrameStreamer, and check the CPU-side pixels.
// A second capture of a different colour proves the double-buffering: the two views must be
// independent (a consumer can hold frame N while frame N+1 is captured). Like the RHI proofs this
// needs a device, so it runs on lavapipe in CI and skips cleanly when there is none — unless
// RIME_REQUIRE_VULKAN is set (CI sets it), which turns "no device" into a failure.

// This is the only translation unit in rime_stream_tests, so it supplies doctest's runtime and
// main() — the header ships neither (see tests/CMakeLists.txt).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>

#include "rime/rhi/rhi.hpp"
#include "rime/stream/frame_streamer.hpp"

using namespace rime;

namespace {

bool require_vulkan() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// Render a solid colour into `color` with a clear-only pass (LoadOp::Clear + Store, no draws).
void render_solid(rhi::Device& device, rhi::TextureHandle color, rhi::ClearColor clear) {
    auto cmd = device.begin_commands();
    rhi::RenderingInfo ri{};
    ri.color.target = color;
    ri.color.load_op = rhi::LoadOp::Clear;
    ri.color.store_op = rhi::StoreOp::Store;
    ri.color.clear = clear;
    cmd->begin_rendering(ri);
    cmd->end_rendering();
    device.submit_blocking(*cmd);
}

// A captured 8-bit channel is within rounding tolerance of the float clear value it came from.
bool channel_is(std::byte b, float unorm) {
    const int got = static_cast<int>(std::to_integer<std::uint8_t>(b));
    const int want = static_cast<int>(unorm * 255.0f + 0.5f);
    const int diff = got > want ? got - want : want - got;
    return diff <= 2; // unorm rounding + software-rasteriser slack
}

} // namespace

TEST_CASE("FrameStreamer captures a rendered frame and double-buffers it") {
    auto device = rhi::create_device({});
    if (!device) {
        if (require_vulkan()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the frame-tap capture");
        return;
    }

    // A non-square target catches width/height mix-ups in the readback stride.
    const rhi::Extent2D extent{64, 48};
    rhi::TextureDesc td{};
    td.extent = extent;
    td.format = rhi::Format::RGBA8Unorm;
    td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    const rhi::TextureHandle color = device->create_texture(td);
    REQUIRE(color.is_valid());

    auto streamer = stream::FrameStreamer::create(*device, extent);
    REQUIRE(streamer.has_value());
    CHECK(streamer->extent().width == 64);
    CHECK(streamer->extent().height == 48);

    // A representative interior pixel; RGBA8 is 4 bytes, rows are tightly packed.
    const std::size_t p = (10u * 64u + 10u) * 4u;

    // Frame 0 — clear to colour A, capture, verify.
    render_solid(*device, color, {0.20f, 0.40f, 0.60f, 1.0f});
    const stream::FrameView f0 = streamer->capture(color);
    CHECK(f0.index == 0);
    CHECK(f0.extent.width == 64);
    CHECK(f0.pixels.size() == 64u * 48u * 4u);
    CHECK(channel_is(f0.pixels[p + 0], 0.20f));
    CHECK(channel_is(f0.pixels[p + 1], 0.40f));
    CHECK(channel_is(f0.pixels[p + 2], 0.60f));
    CHECK(channel_is(f0.pixels[p + 3], 1.00f));

    // The stall was measured.
    CHECK(streamer->stats().frames == 1);
    CHECK(streamer->stats().last_ms >= 0.0);

    // Frame 1 — a different colour lands in the OTHER slot. f1 shows B; f0 must still show A,
    // because double buffering gave them independent CPU buffers.
    render_solid(*device, color, {0.80f, 0.10f, 0.30f, 1.0f});
    const stream::FrameView f1 = streamer->capture(color);
    CHECK(f1.index == 1);
    CHECK(channel_is(f1.pixels[p + 0], 0.80f));
    CHECK(channel_is(f1.pixels[p + 1], 0.10f));
    CHECK(channel_is(f1.pixels[p + 2], 0.30f));

    CHECK(channel_is(f0.pixels[p + 0], 0.20f)); // f0 unchanged by the second capture
    CHECK(channel_is(f0.pixels[p + 1], 0.40f));

    CHECK(streamer->stats().frames == 2);

    device->destroy(color);
}

TEST_CASE("FrameStreamer async ring: begin_capture/try_get_frame are correct and latest-wins") {
    auto device = rhi::create_device({});
    if (!device) {
        if (require_vulkan()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the async frame-tap");
        return;
    }

    const rhi::Extent2D extent{64, 48};
    rhi::TextureDesc td{};
    td.extent = extent;
    td.format = rhi::Format::RGBA8Unorm;
    td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    const rhi::TextureHandle color = device->create_texture(td);
    REQUIRE(color.is_valid());

    auto streamer = stream::FrameStreamer::create(*device, extent);
    REQUIRE(streamer.has_value());

    // Each frame gets a distinct red level = f(index), so a retrieved frame's pixels prove the slot
    // holds exactly the frame its index names (no torn or aliased slots). Green is a constant tag.
    const auto red_for = [](std::uint64_t i) {
        return static_cast<float>((i * 17u + 8u) % 200u) / 255.0f;
    };
    const std::size_t p = (5u * 64u + 5u) * 4u;

    constexpr int kN = 16;
    int retrieved = 0;
    bool have_prev = false;
    std::uint64_t prev_index = 0;
    const auto verify = [&](const stream::FrameView& fv) {
        CHECK(channel_is(fv.pixels[p + 0], red_for(fv.index))); // the right frame is in this slot
        CHECK(channel_is(fv.pixels[p + 1], 0.25f));
        if (have_prev) {
            CHECK(fv.index > prev_index); // strictly monotonic: older frames drop, never reorder
        }
        prev_index = fv.index;
        have_prev = true;
        ++retrieved;
    };

    // Drive a realistic capture loop: render frame i, submit it NON-BLOCKING, collect whatever the
    // GPU has finished. The ring is only kSlots deep, so submitting faster than we drain exercises
    // the latest-wins back-pressure drop inside begin_capture().
    for (int i = 0; i < kN; ++i) {
        render_solid(*device, color, {red_for(static_cast<std::uint64_t>(i)), 0.25f, 0.5f, 1.0f});
        streamer->begin_capture(color);
        if (auto fv = streamer->try_get_frame()) {
            verify(*fv);
        }
    }

    // Drain the tail. wait_idle first so every in-flight copy is finished — then try_get_frame sees
    // them all and the frames+dropped bookkeeping is exact.
    device->wait_idle();
    while (auto fv = streamer->try_get_frame()) {
        verify(*fv);
    }

    CHECK(retrieved >= 1);
    // Every submitted frame is accounted for exactly once — returned or dropped, none lost/dupe'd.
    CHECK(streamer->stats().frames == static_cast<std::uint64_t>(retrieved));
    CHECK(streamer->stats().frames + streamer->stats().dropped == static_cast<std::uint64_t>(kN));

    device->destroy(color);
}

TEST_CASE("FrameStreamer async begin_capture hides the synchronous readback stall") {
    auto device = rhi::create_device({});
    if (!device) {
        if (require_vulkan()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the stall measurement");
        return;
    }

    // Big enough that the glass-to-CPU readback round-trip is measurable on this software
    // rasteriser.
    const rhi::Extent2D extent{256, 256};
    rhi::TextureDesc td{};
    td.extent = extent;
    td.format = rhi::Format::RGBA8Unorm;
    td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    const rhi::TextureHandle color = device->create_texture(td);
    REQUIRE(color.is_valid());

    using clock = std::chrono::steady_clock;
    const auto as_ms = [](clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    constexpr int kWarm = 8;
    constexpr int kIter = 200;

    // Before: the synchronous capture() — submit the readback copy and WAIT for it. Time only the
    // tap call (the render is separate and untimed) so we measure the capture stall, not the draw.
    double sync_ms = 0.0;
    {
        auto s = stream::FrameStreamer::create(*device, extent);
        REQUIRE(s.has_value());
        double acc = 0.0;
        for (int i = 0; i < kWarm + kIter; ++i) {
            render_solid(*device, color, {0.3f, 0.4f, 0.5f, 1.0f});
            const auto t0 = clock::now();
            (void)s->capture(color);
            const double dt = as_ms(clock::now() - t0);
            if (i >= kWarm) {
                acc += dt;
            }
        }
        sync_ms = acc / kIter;
    }

    // After: async begin_capture() — time only the non-blocking submit (the per-frame hot path a
    // live viewport calls); try_get_frame drains off that path and is not timed.
    double async_ms = 0.0;
    {
        auto s = stream::FrameStreamer::create(*device, extent);
        REQUIRE(s.has_value());
        double acc = 0.0;
        for (int i = 0; i < kWarm + kIter; ++i) {
            render_solid(*device, color, {0.3f, 0.4f, 0.5f, 1.0f});
            const auto t0 = clock::now();
            s->begin_capture(color);
            const double dt = as_ms(clock::now() - t0);
            if (i >= kWarm) {
                acc += dt;
            }
            (void)s->try_get_frame();
        }
        async_ms = acc / kIter;
    }

    MESSAGE("readback stall per frame — sync capture(): "
            << sync_ms << " ms, async begin_capture(): " << async_ms << " ms");
    CHECK(async_ms >= 0.0);
    // Only assert the improvement when the sync stall is above timing noise (lavapipe can complete
    // a small copy nearly instantly, which would make the comparison flaky). When it IS measurable,
    // the non-blocking submit must be cheaper — this catches a regression that reintroduces the
    // wait.
    if (sync_ms > 0.2) {
        CHECK(async_ms < sync_ms);
    }

    device->destroy(color);
}

TEST_CASE("FrameStreamer::create rejects an unsupported format") {
    auto device = rhi::create_device({});
    if (!device) {
        if (require_vulkan()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping");
        return;
    }
    // D32Float is not a capturable colour format for S0 — create() must decline, not crash.
    auto bad = stream::FrameStreamer::create(*device, {16, 16}, rhi::Format::D32Float);
    CHECK_FALSE(bad.has_value());
}
