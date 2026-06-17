// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "rime/core/containers/handle.hpp"
#include "rime/platform/native_window.hpp"

// The Window seam: an OS-agnostic handle to an on-screen window. Concrete backends (Cocoa, Win32,
// X11, Wayland — and a headless "null" backend for tests) implement this interface; the engine
// only ever sees `Window`. A window does no rendering itself — it exposes its native handles
// (native_handle()) for the M3 RHI to build a Vulkan surface on, and it produces events that the
// app drains via pump_events()/poll_event() (see event.hpp).
namespace rime::platform {

// Size in whole pixels. We distinguish the *framebuffer* size (real device pixels, what the
// swapchain is sized to) from the *logical* size (points / DIPs), because on HiDPI/Retina
// displays they differ by content_scale().
struct Extent2D {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

class Window;

// A generational id naming a window, so an Event can refer to its origin window without holding a
// raw Window* (which could dangle). Reuses core's Handle: today windows are few and ids are handed
// out sequentially, but this is ready for a SlotMap-backed window registry when multi-window grows.
using WindowId = rime::core::Handle<Window>;

struct WindowDesc {
    std::string_view title = "Rime";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool resizable = true;
    bool fullscreen = false;
    bool high_dpi = true; // create a HiDPI-aware (Retina) window when the display supports it
};

class Window {
public:
    virtual ~Window() = default;

    virtual void set_title(std::string_view title) = 0;
    virtual void set_size(Extent2D size) = 0;

    // Framebuffer pixels (size the swapchain to this); logical points/DIPs; their ratio.
    [[nodiscard]] virtual Extent2D framebuffer_size() const = 0;
    [[nodiscard]] virtual Extent2D logical_size() const = 0;
    [[nodiscard]] virtual float content_scale() const = 0;

    // True once the user (close box) or request_close() has asked the window to close. The main
    // loop typically runs `while (!window->should_close())`.
    [[nodiscard]] virtual bool should_close() const = 0;
    virtual void request_close() = 0;

    virtual void show() = 0;

    // The native handles for surface creation (see native_window.hpp). Stable for the window's
    // life.
    [[nodiscard]] virtual NativeWindow native_handle() const = 0;

    [[nodiscard]] virtual WindowId id() const = 0;

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

protected:
    Window() = default; // construct only through create_window()
};

// Create a window. Call on the main thread, after platform::init(). Returns the headless/null
// window when headless() is set (see below); otherwise the OS-native window. nullptr on failure.
[[nodiscard]] std::unique_ptr<Window> create_window(const WindowDesc& desc);

// Force the headless ("null") backend regardless of OS — for unit tests and for CI/runners with
// no display. Call before create_window() (and before init(), to skip native OS setup). Off by
// default. This is the seam that lets the whole platform layer be exercised without a window
// server, which is how the event queue and (M2.3) input state machine get deterministic tests.
void set_headless(bool enabled) noexcept;
[[nodiscard]] bool headless() noexcept;

} // namespace rime::platform
