// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/platform/filesystem.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

// Windows: GetModuleFileNameW(nullptr, ...) returns the running executable's path (UTF-16). It does
// not tell us up front how big a buffer to use, so we grow and retry until it is not truncated. The
// resulting wide string constructs a std::filesystem::path directly (it stores native wchar_t). The
// user base dirs come from %APPDATA%/%LOCALAPPDATA% (see env_dir below).
namespace rime::platform {

std::filesystem::path executable_path() {
    std::vector<wchar_t> buffer(1024);
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return {};
        }
        if (n < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), n));
        }
        buffer.resize(buffer.size() * 2); // truncated (n == size): grow and try again
    }
}

namespace {

// Read an environment variable as a path via the Win32 API (not the CRT _wgetenv, which trips C4996
// under /WX). The first call sizes the buffer (incl. the null terminator); the second fills it.
// Empty if the variable is unset. We read %APPDATA%/%LOCALAPPDATA% directly rather than
// SHGetKnownFolderPath to avoid the COM allocation and extra link libraries — these variables are
// reliably set in a logged-in user session.
std::filesystem::path env_dir(const wchar_t* variable, std::string_view app_name) {
    const DWORD needed = GetEnvironmentVariableW(variable, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::vector<wchar_t> buffer(needed);
    const DWORD written = GetEnvironmentVariableW(variable, buffer.data(), needed);
    if (written == 0 || written >= needed) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), written)) / app_name;
}

} // namespace

// Windows splits per-user storage into Roaming (%APPDATA%, follows the user between machines) and
// Local (%LOCALAPPDATA%, machine-specific). There is no separate config location, so data and
// config both map to Roaming; cache is regenerable and machine-specific, so it maps to Local.
std::filesystem::path user_data_dir(std::string_view app_name) {
    return env_dir(L"APPDATA", app_name);
}

std::filesystem::path user_config_dir(std::string_view app_name) {
    return env_dir(L"APPDATA", app_name);
}

std::filesystem::path user_cache_dir(std::string_view app_name) {
    return env_dir(L"LOCALAPPDATA", app_name);
}

} // namespace rime::platform
