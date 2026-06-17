// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/platform/window.hpp"

#include <cstdint>

#include "platform_backend.hpp"
#include "rime/platform/event.hpp"

// OS-agnostic window glue: the create_window factory, the pump dispatcher, the headless switch,
// and window-id allocation. The decision "null backend vs the OS-native backend" lives here, in
// one place, so the public interface and the backends stay ignorant of each other.
namespace rime::platform {
namespace {

bool g_headless = false;
std::uint32_t g_next_window_index = 0;

} // namespace

void set_headless(bool enabled) noexcept {
    g_headless = enabled;
}

bool headless() noexcept {
    return g_headless;
}

namespace detail {

WindowId allocate_window_id() {
    // Sequential indices, generation 0. (A future SlotMap-backed registry would supply generations
    // that detect use-after-free; today windows are not recycled, so 0 is correct and stable.)
    return WindowId{g_next_window_index++, 0};
}

} // namespace detail

std::unique_ptr<Window> create_window(const WindowDesc& desc) {
    // headless() forces the null backend (unit tests / display-less CI); otherwise the OS-native
    // backend handles it — Cocoa today, Win32/X11/Wayland as bricks M2.2b-d land.
    return headless() ? detail::null_create_window(desc) : detail::native_create_window(desc);
}

bool pump_events() {
    // The native backend translates pending OS events and pushes them via post_event() during its
    // pump; the headless/null path has nothing to pump (input is injected via post_event()
    // directly). Either way the loop ends once a close/quit has been requested.
    if (!headless()) {
        detail::native_pump();
    }
    return !detail::quit_requested();
}

} // namespace rime::platform
