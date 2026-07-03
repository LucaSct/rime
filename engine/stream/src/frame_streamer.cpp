// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/stream/frame_streamer.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

#include "rime/core/diagnostics/log.hpp"
#include "rime/rhi/device.hpp"

// The frame tap's implementation. The capture path is the same three RHI calls every pixel-proof
// test uses — copy_texture_to_buffer → submit_blocking → read_buffer — wrapped in double buffering
// and timing. See frame_streamer.hpp for the contract and docs/design/graphics-streaming.md for how
// this feeds the encode/transport bricks.
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

    // Pre-create the double-buffered readback targets: host-visible (GpuToCpu) so the CPU can read
    // them, TransferDst so the copy can write them. Doing it once here keeps capture()
    // allocation-free.
    for (std::uint32_t i = 0; i < kSlots; ++i) {
        rhi::BufferDesc rbd{};
        rbd.size = fs.frame_bytes_;
        rbd.usage = rhi::BufferUsage::TransferDst;
        rbd.memory = rhi::MemoryUsage::GpuToCpu;
        rbd.debug_name = "framestreamer-readback";
        fs.readback_[i] = device.create_buffer(rbd);
        if (!fs.readback_[i].is_valid()) {
            RIME_ERROR("FrameStreamer: failed to create readback buffer {} ({} bytes)",
                       i,
                       fs.frame_bytes_);
            fs.release(); // free any buffers already made
            return std::nullopt;
        }
        fs.cpu_[i].resize(fs.frame_bytes_);
    }
    return fs;
}

FrameView FrameStreamer::capture(rhi::TextureHandle color) {
    const std::uint32_t slot = next_slot_;
    next_slot_ = (next_slot_ + 1) % kSlots;

    const auto t0 = std::chrono::steady_clock::now();

    // The tap itself: record a copy of the rendered texture into this slot's readback buffer,
    // submit and wait (the v0 stall), then read the host-visible bytes into CPU staging. The
    // backend inserts the image-layout transition (color-attachment -> transfer-source) for us.
    auto cmd = device_->begin_commands();
    cmd->copy_texture_to_buffer(color, readback_[slot]);
    device_->submit_blocking(*cmd);
    device_->read_buffer(readback_[slot], cpu_[slot].data(), frame_bytes_, 0);

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
    view.pixels = std::span<const std::byte>(cpu_[slot].data(), frame_bytes_);
    view.extent = extent_;
    view.format = format_;
    view.bytes_per_pixel = bytes_per_pixel_;
    view.index = index_++;
    return view;
}

void FrameStreamer::release() noexcept {
    if (device_ != nullptr) {
        for (auto& handle : readback_) {
            device_->destroy(handle);
            handle = {};
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
        readback_ = other.readback_;
        cpu_ = std::move(other.cpu_);
        next_slot_ = other.next_slot_;
        index_ = other.index_;
        stats_ = other.stats_;

        // Neuter the moved-from streamer so its destructor doesn't free the buffers we just took.
        other.device_ = nullptr;
        for (auto& handle : other.readback_) {
            handle = {};
        }
    }
    return *this;
}

} // namespace rime::stream
