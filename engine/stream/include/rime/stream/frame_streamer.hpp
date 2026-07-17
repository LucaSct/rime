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
// Lifetime: the streamer keeps a small ring of CPU buffers (kSlots), so a returned view stays valid
// until its slot is reused — the next kSlots-1 captures. Read frame N while later frames are still
// in flight; copy the bytes out if you need them beyond that window.
struct FrameView {
    std::span<const std::byte> pixels;
    rhi::Extent2D extent;
    rhi::Format format = rhi::Format::RGBA8Unorm;
    std::uint32_t bytes_per_pixel = 4;
    std::uint64_t index = 0; // 0-based, increments once per capture()
};

// Rolling capture cost in milliseconds — the "measure the stall, don't pre-optimize it" the S0 plan
// asks for, now cashed in. The synchronous capture() submits a copy and waits for the GPU, so its
// `last_ms` is the full glass-to-CPU stall. The asynchronous begin_capture()/try_get_frame() path
// (ADR-0030, s1.1) hides that wait behind the kSlots-deep ring: there `last_ms` is only the time
// begin_capture() was *forced* to block for back-pressure (≈0 in steady state), and `dropped`
// counts frames discarded latest-wins when the consumer fell behind. `frames` counts retrieved
// frames either way.
struct CaptureStats {
    std::uint64_t frames = 0;  // frames successfully captured / retrieved
    std::uint64_t dropped = 0; // frames discarded under back-pressure (async path, latest-wins)
    double last_ms = 0.0;      // last capture cost (sync: the stall; async: forced-block time, ≈0)
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
    // rhi::TextureUsage::TransferSrc. **Synchronous convenience** — copies it to the next readback
    // buffer, submits and WAITS, reads it into CPU memory, updates stats(), and returns a view of
    // those bytes (see FrameView lifetime). This is what a one-shot proof or a render-then-encode
    // headless sample wants: give me this exact frame's pixels now. A live viewport at rate should
    // use begin_capture()/try_get_frame() instead, which hide the wait.
    [[nodiscard]] FrameView capture(rhi::TextureHandle color);

    // Asynchronous capture (ADR-0030, s1.1). begin_capture() records a copy of `color` into the
    // next ring slot and submits it NON-BLOCKING — it does not wait for the GPU. Pair it with
    // try_get_frame() to collect frames as they finish. If every ring slot is still in flight, the
    // oldest is force-waited and DROPPED (latest-wins back-pressure): the newest frame always beats
    // a backlog, keeping an interactive viewport low-latency. `color` has the same requirements as
    // in capture(). **Lifetime:** a submitted copy references `color` until it completes, so drain
    // the streamer (destroy/reset it, or wait_idle) before destroying a texture you captured from —
    // freeing an image an in-flight copy still reads is a use-after-free.
    void begin_capture(rhi::TextureHandle color);

    // Return the newest capture whose GPU copy has completed, reading it into CPU memory; older
    // completed captures are dropped (latest-wins). Returns nullopt if nothing has finished yet —
    // the caller renders/submits more and calls again. The returned view follows FrameView's
    // lifetime rule (valid until its slot is reused).
    [[nodiscard]] std::optional<FrameView> try_get_frame();

    [[nodiscard]] rhi::Extent2D extent() const noexcept { return extent_; }

    [[nodiscard]] rhi::Format format() const noexcept { return format_; }

    [[nodiscard]] const CaptureStats& stats() const noexcept { return stats_; }

private:
    void release() noexcept; // drain in-flight captures + destroy the readback buffers (owner-only)

    // Ring depth: how many captures may be in flight (submitted, not yet retrieved) at once. Three
    // gives the GPU room to work on frame N while the CPU submits N+1 and drains N-1 without the
    // capture call blocking in steady state — one more slot than the backend's frames-in-flight.
    // (S0's synchronous path used two.)
    static constexpr std::uint32_t kSlots = 3;

    // One ring slot: a persistent GPU->CPU readback buffer + its CPU staging, plus the in-flight
    // submission filling it (a valid ticket == a copy is pending for this slot).
    struct Slot {
        rhi::BufferHandle readback;    // GPU->CPU host-visible buffer (created once)
        std::vector<std::byte> cpu;    // CPU staging, frame_bytes_ long
        rhi::SubmitTicket ticket;      // the copy submission in flight (invalid == idle)
        std::uint64_t frame_index = 0; // which capture this slot holds while pending
        bool pending = false;          // a submit is in flight for this slot
    };

    rhi::Device* device_ = nullptr;
    rhi::Extent2D extent_{};
    rhi::Format format_ = rhi::Format::RGBA8Unorm;
    std::uint32_t bytes_per_pixel_ = 4;
    std::size_t frame_bytes_ = 0;

    std::array<Slot, kSlots> slots_{};
    std::uint32_t next_slot_ = 0;
    std::uint64_t index_ = 0; // monotonic capture ordinal (FrameView::index / Slot::frame_index)
    CaptureStats stats_{};
};

} // namespace rime::stream
