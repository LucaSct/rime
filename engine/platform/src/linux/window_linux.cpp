// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <memory>

#include "platform_backend.hpp"
#include "rime/platform/window.hpp"

// Linux window backend — placeholder. The real backends (Xlib and Wayland, with runtime
// selection) land in bricks M2.2c and M2.2d. For now this satisfies the backend seam so the
// platform library links on Linux; non-headless create_window returns nullptr there, and the unit
// tests run through the headless null backend on every OS.
namespace rime::platform::detail {

void native_init() {}

void native_shutdown() {}

void native_pump() {}

std::unique_ptr<Window> native_create_window(const WindowDesc&) {
    return nullptr;
}

} // namespace rime::platform::detail
