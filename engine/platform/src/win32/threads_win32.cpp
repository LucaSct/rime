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

} // namespace rime::platform
