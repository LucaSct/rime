// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <mach-o/dyld.h>
#include <pwd.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <system_error>
#include <vector>

#include "rime/platform/filesystem.hpp"

// macOS: _NSGetExecutablePath fills a caller-sized buffer with the executable path. Called with a
// null buffer it reports the required size, so we size exactly, then canonicalize to resolve any
// symlinks / "." segments. Plain C/C++ (no Objective-C needed), hence a .cpp in the cocoa dir.
//
// The user base dirs live under ~/Library per Apple's convention; home discovery is likewise plain
// POSIX (see home_dir below) to keep this file Objective-C-free.
namespace rime::platform {

std::filesystem::path executable_path() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // size now holds the required buffer length
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    const std::filesystem::path raw(buffer.data());
    std::error_code ec;
    std::filesystem::path canonical = std::filesystem::canonical(raw, ec);
    return ec ? raw : canonical;
}

namespace {

// The user's home directory, plain-C++ (no Foundation): $HOME, falling back to the passwd database
// for the rare login context where $HOME is unset. NSHomeDirectory would pull in Objective-C and,
// under the App Sandbox, return a container path — neither is what this engine context wants.
std::filesystem::path home_dir() {
    if (const char* h = std::getenv("HOME"); h != nullptr && h[0] != '\0') {
        return std::filesystem::path(h);
    }
    if (const passwd* pw = ::getpwuid(::getuid());
        pw != nullptr && pw->pw_dir != nullptr && pw->pw_dir[0] != '\0') {
        return std::filesystem::path(pw->pw_dir);
    }
    return {};
}

std::filesystem::path under_home(const char* relative, std::string_view app_name) {
    const std::filesystem::path home = home_dir();
    return home.empty() ? std::filesystem::path{} : home / relative / app_name;
}

} // namespace

// macOS keeps per-app files under ~/Library: Application Support for data the user "owns", Caches
// for regenerable data the OS may purge. There is no general user-config directory (Preferences is
// reserved for the defaults/plist system), so config maps to Application Support alongside data.
std::filesystem::path user_data_dir(std::string_view app_name) {
    return under_home("Library/Application Support", app_name);
}

std::filesystem::path user_config_dir(std::string_view app_name) {
    return under_home("Library/Application Support", app_name);
}

std::filesystem::path user_cache_dir(std::string_view app_name) {
    return under_home("Library/Caches", app_name);
}

} // namespace rime::platform
