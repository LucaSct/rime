// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "rime/core/containers/handle.hpp"
#include "rime/platform/mouse.hpp"
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
};

// GONE, NOT FORGOTTEN (m15.5): `fullscreen` and `high_dpi` used to sit here. Neither was ever read
// by a backend or set by a caller — they were a promise the engine had no code behind, and this
// repo's rule is that a declared knob the engine ignores is worse than an absent one, because it
// reads as "tried and it did not work". Fullscreen is a real brick (four backends, four different
// state machines) and belongs in one. HiDPI is partly live already but not as a toggle: Cocoa sizes
// its layer to `backingScaleFactor`, and X11 reports content_scale() == 1.0 with a comment saying
// why (there is no single honest DPI source on X11). Re-add either as the first line of the brick
// that implements it.

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

    // ── Pointer capture (m15.5) ──────────────────────────────────────────────────────────────
    //
    // Ask the window for a cursor mode; get back the mode ACTUALLY IN EFFECT, which may be weaker
    // than the request. That return value is the whole design, and it is not defensive politeness:
    // a Wayland compositor is free not to advertise `zwp_pointer_constraints_v1` at all, and a
    // window that reported Locked anyway would leave the caller steering a camera with a cursor
    // that is still walking off the screen — the failure looking exactly like a broken camera
    // rather than a missing capability. The engine's rule about never strengthening a claim on
    // anything short of confirmation (CLAUDE.md guardrail 5) is the same rule one layer down.
    //
    // Locked means: cursor hidden, pinned, and MouseMove events carry relative motion in dx/dy with
    // x/y frozen. Every backend that supports it delivers that; how it gets there differs
    // (constrained pointer, warp-and-recentre, RAWINPUT), and those differences stay inside the
    // backends. Hidden means hidden and free — the mode a drag-look wants while the button is held.
    //
    // A backend may drop out of Locked on its own when the window loses focus: an app that keeps
    // the pointer of a background window is a bug the OS blames on the app. Callers must therefore
    // treat `cursor_mode()` as live state and re-request on focus gain rather than assume the mode
    // they set is the mode they still have.
    virtual CursorMode set_cursor_mode(CursorMode mode) = 0;
    [[nodiscard]] virtual CursorMode cursor_mode() const = 0;

    // The native handles for surface creation (see native_window.hpp). Stable for the window's
    // life.
    //
    // Ask for this ONLY to build a rendering surface on the window. It is not a neutral getter
    // everywhere: the Wayland backend reads the call as "a renderer owns this surface's pixels from
    // now on" and stands down the placeholder buffer it would otherwise use to make a rendererless
    // window visible (a Wayland surface with no buffer is never mapped at all). Querying the handle
    // of a window you will never render to therefore leaves it invisible on Wayland.
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
