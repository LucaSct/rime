// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <memory>

#include "rime/platform/event.hpp"
#include "rime/platform/window.hpp"

// Private seam (not installed) between the OS-agnostic platform glue — platform.cpp, window.cpp,
// event_queue.cpp — and the concrete backends. Keeping these declarations out of the public
// headers is what lets the public interface stay free of backend detail.
namespace rime::platform::detail {

// --- event queue + run-loop state (event_queue.cpp) ---
void request_quit(); // mark that the app should stop; pump_events() then returns false
[[nodiscard]] bool quit_requested();
void reset_quit();
void clear_events(); // drop any pending events (on init/shutdown)

// --- window bookkeeping (window.cpp) ---
[[nodiscard]] WindowId allocate_window_id(); // hand out sequential generational ids to new windows

// --- headless/null backend (null/window_null.cpp) ---
[[nodiscard]] std::unique_ptr<Window> null_create_window(const WindowDesc& desc);

// --- native OS backend (cocoa/win32/linux; exactly one compiled per OS) ---
// Declared here, defined by the per-OS backend. window.cpp / platform.cpp call these by name only
// when !headless(); naming them also forces the linker to keep the backend's object file from the
// static library (self-registration would risk being dropped).
void native_init();
void native_shutdown();
void native_pump();
[[nodiscard]] std::unique_ptr<Window> native_create_window(const WindowDesc& desc);

} // namespace rime::platform::detail
