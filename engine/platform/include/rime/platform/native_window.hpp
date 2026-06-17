// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

// The OS-native handles a window is built from — the seam the renderer crosses to create a
// graphics surface. This is the one place the platform layer hands OS-specific pointers upward,
// and it does so *type-erased to void** so the header pulls in no OS headers (the "no #ifdef
// upward" rule). The M3 RHI Vulkan backend — the only code allowed to include Vulkan headers
// (ADR-0002) — reinterpret_casts these to the arguments of the matching vkCreate*SurfaceKHR.
namespace rime::platform {

// Which OS windowing system produced a NativeWindow. Lets the consumer pick the right surface
// constructor without the platform layer naming any OS type.
enum class WindowSystem : std::uint8_t { Null, Win32, Cocoa, Xlib, Wayland };

// Type-erased native handles. Interpretation by `system`:
//   Win32   : display = HINSTANCE,   handle = HWND
//   Cocoa   : display = nullptr,     handle = CAMetalLayer*  (the layer-backed NSView's layer)
//   Xlib    : display = Display*,    handle = (void*)(uintptr_t)Window   (an X11 window XID)
//   Wayland : display = wl_display*, handle = wl_surface*
//   Null    : both nullptr
struct NativeWindow {
    WindowSystem system = WindowSystem::Null;
    void* display = nullptr;
    void* handle = nullptr;
};

} // namespace rime::platform
