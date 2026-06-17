// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#include "rime/platform/filesystem.hpp"

// Linux: the kernel exposes the running executable as the symlink /proc/self/exe; readlink resolves
// it to the real path. (readlink does not null-terminate, so we use its byte count.) The user base
// dirs follow the XDG Base Directory spec (see xdg_dir below).
namespace rime::platform {

std::filesystem::path executable_path() {
    std::array<char, 4096> buffer{};
    const ssize_t n = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (n <= 0) {
        return {};
    }
    return std::filesystem::path(std::string(buffer.data(), static_cast<std::size_t>(n)));
}

namespace {

// XDG Base Directory spec: each base dir is taken from $XDG_*_HOME when that is set to an
// *absolute* path, otherwise from the spec's default under $HOME. (The spec says a relative or
// unset value must be ignored in favour of the default.) Returns empty only if even $HOME is
// unavailable.
std::filesystem::path
xdg_dir(const char* xdg_var, const char* home_relative, std::string_view app_name) {
    if (const char* xdg = std::getenv(xdg_var); xdg != nullptr && xdg[0] != '\0') {
        const std::filesystem::path base(xdg);
        if (base.is_absolute()) {
            return base / app_name;
        }
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    return std::filesystem::path(home) / home_relative / app_name;
}

} // namespace

std::filesystem::path user_data_dir(std::string_view app_name) {
    return xdg_dir("XDG_DATA_HOME", ".local/share", app_name);
}

std::filesystem::path user_config_dir(std::string_view app_name) {
    return xdg_dir("XDG_CONFIG_HOME", ".config", app_name);
}

std::filesystem::path user_cache_dir(std::string_view app_name) {
    return xdg_dir("XDG_CACHE_HOME", ".cache", app_name);
}

} // namespace rime::platform
