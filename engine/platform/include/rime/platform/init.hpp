// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

// Platform lifetime + the "UI thread" contract.
//
// Every desktop OS pins window and event work to a single thread: Win32 delivers window
// messages only to the thread that created the window; Cocoa requires all NSApplication /
// NSWindow calls on the main thread; X11 needs XInitThreads() before any multithreaded use.
// Rather than fight that, the platform layer *names* the rule: init() is called once, on the
// thread that will own the window and pump events (the "main thread"), and later windowing
// calls assert they run there. This mirrors the JobSystem's single-thread submit/wait contract
// (engine/core/include/rime/core/jobs/job_system.hpp) — a deliberate, documented constraint
// beats a hidden one.
namespace rime::platform {

// Initialize the platform layer. Call once, on the main thread, before creating any window.
// Records the calling thread as the main/UI thread and gives it a debugger-visible name.
// Returns false if already initialized. (Per-OS heavy setup — DPI awareness, NSApplication,
// the X11/Wayland connection — lands with the window backends in M2.2.)
bool init();

// Tear down the platform layer. Call once, on the main thread, after the last window is gone.
void shutdown();

// True if the calling thread is the one that called init(). Cheap; safe to call from asserts.
[[nodiscard]] bool on_main_thread() noexcept;

// True between a successful init() and shutdown().
[[nodiscard]] bool is_initialized() noexcept;

} // namespace rime::platform
