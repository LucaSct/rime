// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <cstdlib>
#include <memory>

#include "linux_backend.hpp"
#include "platform_backend.hpp"
#include "rime/platform/window.hpp"

// Linux windowing backend selector. Unlike the single-backend OSes, Linux chooses its windowing
// system at runtime and forwards the native_* seam calls to the winner. Wayland is preferred when
// the session advertises one ($WAYLAND_DISPLAY); X11 is the fallback (and, until brick M2.2d lands
// the Wayland backend, the only choice). If neither connects — a display-less box that did not opt
// into the null backend — g_backend stays null and create_window returns nullptr, exactly as the
// earlier placeholder did.
namespace rime::platform::detail {
namespace {

const LinuxBackend* g_backend = nullptr;

} // namespace

void native_init() {
    // Wayland-first selection arrives with the Wayland backend (M2.2d). For now: X11 only.
    const LinuxBackend& x11 = x11_backend();
    if (x11.init()) {
        g_backend = &x11;
    }
}

void native_shutdown() {
    if (g_backend != nullptr) {
        g_backend->shutdown();
        g_backend = nullptr;
    }
}

void native_pump() {
    if (g_backend != nullptr) {
        g_backend->pump();
    }
}

std::unique_ptr<Window> native_create_window(const WindowDesc& desc) {
    return g_backend != nullptr ? g_backend->create_window(desc) : nullptr;
}

} // namespace rime::platform::detail
