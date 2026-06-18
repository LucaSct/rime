// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include "rime/platform/native_window.hpp"
#include "rime/rhi/command_buffer.hpp"
#include "rime/rhi/types.hpp"

// Presentation (M3.4). A Swapchain turns a platform window into a small queue of images the GPU
// renders into and the window system displays. It is created from the Device and a
// platform::NativeWindow, and is the *only* RHI object that knows a window exists — the Device
// itself stays headless / window-agnostic, which is exactly what lets the M3.1–M3.3 off-screen
// proof run GPU-free in CI. See docs/adr/0009 and docs/design/rhi.md.
//
// The frame loop is the simplest correct shape; frames-in-flight pipelining (overlapping CPU
// recording with GPU execution) is handled *inside* the backend, paced by present:
//
//   rhi::TextureHandle target = swapchain->acquire_next_image();
//   if (!target.is_valid()) { swapchain->recreate(window->framebuffer_size()); continue; }
//   auto cmd = device->begin_commands();
//   // ... record a render into `target` ...
//   if (!swapchain->present(*cmd)) swapchain->recreate(window->framebuffer_size());
namespace rime::rhi {

struct SwapchainDesc {
    platform::NativeWindow window;  // the surface to present to (from Window::native_handle())
    Extent2D extent;                // framebuffer size in pixels (Window::framebuffer_size())
    bool vsync = true;              // FIFO present (tear-free, always supported); mailbox comes later
};

class Swapchain {
public:
    virtual ~Swapchain() = default;

    // Acquire the next backbuffer to render into; the returned handle is an ordinary color target
    // (begin_rendering / copy etc. accept it). Returns an **invalid handle** when the swapchain is
    // out of date — typically the window resized — in which case call recreate() with the new
    // framebuffer size and try again next frame.
    [[nodiscard]] virtual TextureHandle acquire_next_image() = 0;

    // Submit the commands recorded for this frame and present the acquired backbuffer. The backend
    // records the backbuffer's transition to a presentable layout, submits with this frame's
    // synchronization (wait image-acquired, signal render-finished, in-flight fence), then queues
    // the present. Returns false if the swapchain became out of date — recreate() before next frame.
    virtual bool present(CommandBuffer& commands) = 0;

    // Rebuild the swapchain for a new framebuffer size (on resize or an out-of-date result). Waits
    // for the GPU to go idle first, so in-flight frames finish before the old images are freed.
    virtual void recreate(Extent2D extent) = 0;

    [[nodiscard]] virtual Format format() const = 0;   // the backbuffer color format
    [[nodiscard]] virtual Extent2D extent() const = 0; // current size in pixels

protected:
    Swapchain() = default; // obtained only from Device::create_swapchain()

public:
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
};

} // namespace rime::rhi
