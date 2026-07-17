// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/stream/frame_streamer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

#include "rime/core/diagnostics/log.hpp"
#include "rime/rhi/device.hpp"

// The frame tap's implementation. The synchronous capture() is the same handful of RHI calls every
// pixel-proof test uses — copy_texture_to_buffer → submit → wait → read_buffer — wrapped in a ring
// and timing. The asynchronous begin_capture()/try_get_frame() pair (ADR-0030, s1.1) splits the
// submit from the wait so a live viewport never pays the readback stall: capture N is still on the
// GPU while the CPU submits N+1 and drains N-1. See frame_streamer.hpp for the contract and
// docs/design/graphics-streaming.md for how this feeds the encode/transport bricks.
namespace rime::stream {
namespace {

// Bytes per pixel for the formats we can capture in S0. 0 == unsupported.
std::uint32_t bytes_per_pixel_of(rhi::Format format) {
    switch (format) {
        case rhi::Format::RGBA8Unorm:
        case rhi::Format::RGBA8Srgb:
        case rhi::Format::BGRA8Unorm:
        case rhi::Format::BGRA8Srgb:
            return 4;
        default:
            return 0;
    }
}

} // namespace

std::optional<FrameStreamer>
FrameStreamer::create(rhi::Device& device, rhi::Extent2D extent, rhi::Format format) {
    const std::uint32_t bpp = bytes_per_pixel_of(format);
    if (bpp == 0) {
        RIME_ERROR("FrameStreamer: unsupported capture format (S0 supports 8-bit RGBA/BGRA only)");
        return std::nullopt;
    }
    if (extent.width == 0 || extent.height == 0) {
        RIME_ERROR("FrameStreamer: zero extent ({}x{})", extent.width, extent.height);
        return std::nullopt;
    }

    FrameStreamer fs;
    fs.device_ = &device;
    fs.extent_ = extent;
    fs.format_ = format;
    fs.bytes_per_pixel_ = bpp;
    fs.frame_bytes_ = static_cast<std::size_t>(extent.width) * extent.height * bpp;

    // Pre-create the ring's readback targets: host-visible (GpuToCpu) so the CPU can read them,
    // TransferDst so the copy can write them. Doing it once here keeps the capture paths
    // allocation-free on the hot path.
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        rhi::BufferDesc rbd{};
        rbd.size = fs.frame_bytes_;
        rbd.usage = rhi::BufferUsage::TransferDst;
        rbd.memory = rhi::MemoryUsage::GpuToCpu;
        rbd.debug_name = "framestreamer-readback";
        fs.slots_[i].readback = device.create_buffer(rbd);
        if (!fs.slots_[i].readback.is_valid()) {
            RIME_ERROR("FrameStreamer: failed to create readback buffer {} ({} bytes)",
                       i,
                       fs.frame_bytes_);
            fs.release(); // free any buffers already made
            return std::nullopt;
        }
        fs.slots_[i].cpu.resize(fs.frame_bytes_);
    }
    return fs;
}

void FrameStreamer::begin_capture(rhi::TextureHandle color) {
    const std::uint32_t slot = next_slot_;
    next_slot_ = (next_slot_ + 1) % kSlots;

    // Back-pressure: if this slot still holds an un-retrieved in-flight capture, we must finish its
    // GPU copy before overwriting its buffer. Force-wait it and DROP that frame (latest-wins,
    // ADR-0030 §2). In steady state the slot is already idle and this costs nothing.
    if (slots_[slot].pending) {
        const auto b0 = std::chrono::steady_clock::now();
        device_->wait(slots_[slot].ticket);
        const auto b1 = std::chrono::steady_clock::now();
        stats_.last_ms = std::chrono::duration<double, std::milli>(b1 - b0).count();
        slots_[slot].pending = false;
        slots_[slot].ticket = {};
        stats_.dropped += 1;
    } else {
        stats_.last_ms = 0.0;
    }

    // Record the copy into this slot's readback buffer and submit it WITHOUT waiting. The backend
    // inserts the image-layout transition (color-attachment -> transfer-source) and keeps the
    // command buffer alive behind the ticket until the copy completes.
    auto cmd = device_->begin_commands();
    cmd->copy_texture_to_buffer(color, slots_[slot].readback);
    slots_[slot].ticket = device_->submit(std::move(cmd));
    slots_[slot].pending = true;
    slots_[slot].frame_index = index_++;
}

std::optional<FrameView> FrameStreamer::try_get_frame() {
    // Find the newest slot whose GPU copy has completed. Latest-wins: if several finished, return
    // the most recent and drop the older ones — a viewport wants the freshest frame, not a backlog.
    std::array<bool, kSlots> done{};
    int best = -1;
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        if (slots_[i].pending && device_->is_complete(slots_[i].ticket)) {
            done[i] = true;
            const auto b = static_cast<std::uint32_t>(best);
            if (best < 0 || slots_[i].frame_index > slots_[b].frame_index)
                best = static_cast<int>(i);
        }
    }
    if (best < 0)
        return std::nullopt;

    const auto bslot = static_cast<std::uint32_t>(best);

    // Drop older completed slots (they are superseded by `bslot`).
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        if (i != bslot && done[i]) {
            slots_[i].pending = false;
            slots_[i].ticket = {};
            stats_.dropped += 1;
        }
    }

    Slot& s = slots_[bslot];
    device_->read_buffer(s.readback, s.cpu.data(), frame_bytes_, 0);
    s.pending = false;
    s.ticket = {};
    stats_.frames += 1;

    FrameView view{};
    view.pixels = std::span<const std::byte>(s.cpu.data(), frame_bytes_);
    view.extent = extent_;
    view.format = format_;
    view.bytes_per_pixel = bytes_per_pixel_;
    view.index = s.frame_index;
    return view;
}

FrameView FrameStreamer::capture(rhi::TextureHandle color) {
    const auto t0 = std::chrono::steady_clock::now();

    // Synchronous convenience built on the async primitives: submit into the next slot, then block
    // for exactly that frame. This is the S0 contract every one-shot pixel-proof relies on (render
    // a frame, get its pixels back now); the stall it pays is the very cost begin_capture() hides.
    const std::uint32_t slot = next_slot_;
    begin_capture(color);
    device_->wait(slots_[slot].ticket);
    device_->read_buffer(slots_[slot].readback, slots_[slot].cpu.data(), frame_bytes_, 0);
    slots_[slot].pending = false;
    slots_[slot].ticket = {};
    const std::uint64_t fi = slots_[slot].frame_index;

    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (stats_.frames == 0) {
        stats_.min_ms = ms;
        stats_.max_ms = ms;
    } else {
        stats_.min_ms = std::min(stats_.min_ms, ms);
        stats_.max_ms = std::max(stats_.max_ms, ms);
    }
    stats_.avg_ms = (stats_.avg_ms * static_cast<double>(stats_.frames) + ms) /
                    static_cast<double>(stats_.frames + 1);
    stats_.frames += 1;
    stats_.last_ms = ms;

    FrameView view{};
    view.pixels = std::span<const std::byte>(slots_[slot].cpu.data(), frame_bytes_);
    view.extent = extent_;
    view.format = format_;
    view.bytes_per_pixel = bytes_per_pixel_;
    view.index = fi;
    return view;
}

void FrameStreamer::release() noexcept {
    if (device_ != nullptr) {
        for (auto& s : slots_) {
            if (s.pending) {
                device_->wait(s.ticket); // finish the GPU copy before freeing its target buffer
                s.pending = false;
                s.ticket = {};
            }
            device_->destroy(s.readback);
            s.readback = {};
        }
    }
}

FrameStreamer::~FrameStreamer() {
    release();
}

FrameStreamer::FrameStreamer(FrameStreamer&& other) noexcept {
    *this = std::move(other);
}

FrameStreamer& FrameStreamer::operator=(FrameStreamer&& other) noexcept {
    if (this != &other) {
        release(); // free what we currently hold
        device_ = other.device_;
        extent_ = other.extent_;
        format_ = other.format_;
        bytes_per_pixel_ = other.bytes_per_pixel_;
        frame_bytes_ = other.frame_bytes_;
        slots_ = std::move(other.slots_);
        next_slot_ = other.next_slot_;
        index_ = other.index_;
        stats_ = other.stats_;

        // Neuter the moved-from streamer so its destructor won't wait tickets or free the buffers
        // we just took (release() guards on device_, so clearing it suffices; clear the handles
        // too).
        other.device_ = nullptr;
        for (auto& s : other.slots_) {
            s.readback = {};
            s.pending = false;
            s.ticket = {};
        }
    }
    return *this;
}

} // namespace rime::stream
