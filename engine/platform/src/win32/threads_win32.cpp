// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/platform/threads.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX // keep <windows.h> from defining min()/max() macros that break std::min/max.
#endif
#include <windows.h>
// clang-format off
#include <timeapi.h> // timeBeginPeriod / timeEndPeriod (winmm); must follow <windows.h>
// clang-format on

#include <string>

// Windows thread naming via SetThreadDescription (Windows 10 1607+) — the modern, supported
// replacement for the old SEH "raise exception 0x406D1388" hack that only a debugger could see.
// The Win32 API speaks UTF-16, so we widen the incoming UTF-8 name with MultiByteToWideChar.
namespace rime::platform {

void set_thread_name(std::string_view name) {
    if (name.empty()) {
        return;
    }
    const int wlen =
        MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (wlen <= 0) {
        return;
    }
    std::wstring wide(static_cast<std::size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), wlen);
    // Best-effort: ignore the HRESULT — a failed rename must never disturb the caller.
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

namespace detail {
namespace {

// Not every SDK's processthreadsapi.h declares it (older ones predate Windows 11); its value is
// fixed by the Win32 ABI.
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif

bool g_timer_period_raised = false; // true only if timeBeginPeriod(1) succeeded.

} // namespace

// Windows wakes sleeping threads on a periodic timer interrupt whose default period is 15.625 ms
// (64 Hz). A sleep_for() can only end on one of those ticks, so it rounds UP to the next tick:
// we measured sleep_for(1ms) taking 15.94 / 16.03 / 16.12 ms, which turned a job system's idle
// 100 us nap and a sample's "1 ms" poll into 16 ms stalls. timeBeginPeriod(1) asks for a 1 ms
// period instead. Since Windows 10 2004 the request is per-process (before, it was global and any
// program could speed up everyone's clock), so each process must make it for itself. Windows 11
// adds a second catch: a process that owns no visible window, or whose window is occluded, may be
// throttled and have the request silently ignored, which is exactly the situation of a
// dedicated server or a minimised game. Setting PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
// with StateMask 0 opts this process out of that throttling. Both calls are best-effort: failure
// just leaves the coarse default timer, which is slower but not an error.
void timer_resolution_begin() {
    g_timer_period_raised = (timeBeginPeriod(1) == TIMERR_NOERROR);

    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state));
}

void timer_resolution_end() {
    if (g_timer_period_raised) {
        timeEndPeriod(1);
        g_timer_period_raised = false;
    }
}

} // namespace detail

} // namespace rime::platform
