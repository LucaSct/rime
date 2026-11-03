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

// Raises the OS timer resolution for as long as the object lives (RAII), so that a short
// sleep_for() sleeps about as long as asked. On Windows the default timer period is 15.625 ms and
// every sleep rounds UP to it (sleep_for(1ms) measured ~16 ms); the request lowers it to 1 ms.
// Linux and macOS timers are already fine-grained, so there it does nothing. It is deliberately
// NOT part of init(): init() belongs to windowing and only a windowed app calls it, while the
// processes that sleep most (a dedicated server, a headless client) have no window at all. Hold
// one for the life of anything that sleeps or runs a JobSystem; rime::app::Application does.
// Nesting is safe: Windows counts the requests and the period stays raised until the last ends.
class TimerResolutionScope {
public:
    TimerResolutionScope();
    ~TimerResolutionScope();
    TimerResolutionScope(const TimerResolutionScope&) = delete;
    TimerResolutionScope& operator=(const TimerResolutionScope&) = delete;

private:
    bool raised_; // the OS accepted the request, so the destructor owes it one release
};

} // namespace rime::platform
