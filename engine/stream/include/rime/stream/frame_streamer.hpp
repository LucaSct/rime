// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "rime/rhi/types.hpp"

// The **frame tap** — the first brick of the graphics-streaming track (Track S / S0.2). It takes a
// texture the engine has already rendered off-screen and copies its pixels back to the CPU, so a
// later brick can encode them (S0.3) and send them over the platform TCP sockets (S0.1) to a thin
// client (S0.5). This is the in-engine start of the "engine renders → capture → encode → transport
// → present" pipeline that ADR-0016 shows serving dev streaming now and the editor viewport /
// remote play later. Design: docs/design/graphics-streaming.md.
//
// It depends only on the **RHI interface** (`rime::rhi`), never a backend, so it captures from
// Vulkan today and any future backend unchanged. It lives in its own module because the plan makes
// streaming a first-class, removable engine feature (engine/stream over the engine/net transport).
namespace rime::rhi {
class Device;
}

namespace rime::stream {

// A read-only view of one captured frame's pixels: tightly packed, top row first, in `format`.
// Lifetime: the streamer keeps two CPU buffers and alternates between them, so a view stays valid
// across exactly **one** following capture() (the consumer can read frame N while frame N+1 is
// captured), and is overwritten on the second following capture(). Copy the bytes out if you need
// them longer.
struct FrameView {
    std::span<const std::byte> pixels;
    rhi::Extent2D extent;
    rhi::Format format = rhi::Format::RGBA8Unorm;
    std::uint32_t bytes_per_pixel = 4;
    std::uint64_t index = 0; // 0-based, increments once per capture()
};

// Rolling capture cost in milliseconds — this is the "measure the stall, don't pre-optimize it"
// the S0 plan asks for. capture() is **synchronous** in v0 (it submits a copy and waits for the
// GPU, then reads host-visible memory), so `last_ms` is the glass-to-CPU stall we later hide with
// asynchronous readback (S1). Recording it now means the day we make it async, we can prove it.
struct CaptureStats {
    std::uint64_t frames = 0;
    double last_ms = 0.0;
    double avg_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
};

class FrameStreamer {
public:
    // Create a streamer for `extent` frames in `format`. S0 supports only 8-bit RGBA/BGRA (4
    // bytes/pixel — what the offscreen color target and every codec we plan speak); other formats
    // are rejected. Pre-allocates the double-buffered GPU readback buffers + CPU staging so
    // capture() allocates nothing on the hot path. Returns nullopt (and logs) if the extent is
    // zero, the format is unsupported, or a readback buffer can't be created.
    [[nodiscard]] static std::optional<FrameStreamer>
    create(rhi::Device& device, rhi::Extent2D extent, rhi::Format format = rhi::Format::RGBA8Unorm);

    FrameStreamer() = default; // an empty streamer (captures nothing); use create()
    ~FrameStreamer();
    FrameStreamer(FrameStreamer&&) noexcept;
    FrameStreamer& operator=(FrameStreamer&&) noexcept;
    FrameStreamer(const FrameStreamer&) = delete;
    FrameStreamer& operator=(const FrameStreamer&) = delete;

    // Capture `color`: an already-rendered texture of this streamer's extent+format that carries
    // rhi::TextureUsage::TransferSrc. Copies it to the next readback buffer, waits, reads it into
    // CPU memory, updates stats(), and returns a view of those bytes (see FrameView lifetime). The
    // caller renders into `color`, then hands it here — the tap is decoupled from *how* the frame
    // was drawn.
    [[nodiscard]] FrameView capture(rhi::TextureHandle color);

    [[nodiscard]] rhi::Extent2D extent() const noexcept { return extent_; }

    [[nodiscard]] rhi::Format format() const noexcept { return format_; }

    [[nodiscard]] const CaptureStats& stats() const noexcept { return stats_; }

private:
    void release() noexcept; // destroy the readback buffers (owner-only)

    static constexpr std::uint32_t kSlots = 2; // double-buffered

    rhi::Device* device_ = nullptr;
    rhi::Extent2D extent_{};
    rhi::Format format_ = rhi::Format::RGBA8Unorm;
    std::uint32_t bytes_per_pixel_ = 4;
    std::size_t frame_bytes_ = 0;

    std::array<rhi::BufferHandle, kSlots> readback_{}; // GPU->CPU host-visible buffers
    std::array<std::vector<std::byte>, kSlots> cpu_{}; // CPU staging, one per slot
    std::uint32_t next_slot_ = 0;
    std::uint64_t index_ = 0;
    CaptureStats stats_{};
};

} // namespace rime::stream
