// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <memory>

#include "rime/platform/window.hpp"

// Linux has two windowing systems in play — Wayland (modern, the default on current desktops) and
// X11 (still ubiquitous) — so unlike the single-backend OSes, the Linux backend is chosen at
// runtime. Each system implements this small vtable; window_linux.cpp probes the session at init()
// (Wayland first, X11 fallback), remembers the winner, and forwards the native_* seam calls to it.
// A struct of function pointers (rather than another abstract base) keeps the selection a plain,
// branch-free dispatch and keeps each backend's types confined to its own translation unit — which
// matters because <X11/Xlib.h> and <wayland-client.h> define clashing names (e.g. X11's `Window`).
namespace rime::platform::detail {

struct LinuxBackend {
    bool (*init)();     // connect to the display server; false if unavailable (e.g. no session)
    void (*shutdown)(); // disconnect and release global state
    void (*pump)();     // drain pending server events, translating them onto our queue
    std::unique_ptr<Window> (*create_window)(const WindowDesc&);
};

[[nodiscard]] const LinuxBackend& x11_backend();     // defined in window_x11.cpp
[[nodiscard]] const LinuxBackend& wayland_backend(); // defined in window_wayland.cpp

} // namespace rime::platform::detail
