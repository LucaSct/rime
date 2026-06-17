// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Filesystem helpers.
//
// Path manipulation and bulk file I/O are built on std::filesystem / std::fstream — which already
// wrap the OS-native calls — so there is no reason to hand-roll them (see ADR-0006: "native" is for
// windowing/input, not for things the standard library already does well). The genuinely per-OS
// pieces are the ones the standard library cannot answer: locating the running executable, and the
// per-user base directories (data/config/cache) that must follow each OS's own conventions. Those
// live in the backend files (src/<platform>/paths_*.cpp) — _NSGetExecutablePath + ~/Library on
// macOS, GetModuleFileNameW + %APPDATA%/%LOCALAPPDATA% on Windows, /proc/self/exe + XDG on Linux.
namespace rime::platform {

// Read an entire file as bytes. nullopt if it cannot be opened/read.
[[nodiscard]] std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path);

// Write bytes to a file, truncating any existing contents. Returns false on failure.
[[nodiscard]] bool write_file(const std::filesystem::path& path, std::span<const std::byte> data);

// Does the path exist? Never throws (errors are treated as "does not exist").
[[nodiscard]] bool file_exists(const std::filesystem::path& path);

// Absolute path to the currently running executable (per-OS native). Empty path on failure. Useful
// as the anchor for locating assets/config relative to the binary.
[[nodiscard]] std::filesystem::path executable_path();

// Per-user base directories for this application, each namespaced under app_name (its last path
// component) so two applications never collide. They follow each OS's conventions so files land
// where users and backup/sync tools expect:
//   • user_data_dir   — persistent app data (saves, generated content)
//   • user_config_dir — user settings
//   • user_cache_dir  — regenerable data, safe to delete (kept off synced/roaming storage)
// These are pure path queries: the directory is *not* created — call std::filesystem::create_-
// directories when you first write. Empty path if the location cannot be determined (e.g. no HOME).
// On macOS and Windows the OS has no separate user-config location, so data and config coincide
// (Application Support / %APPDATA%); on Linux the XDG spec gives three distinct directories.
[[nodiscard]] std::filesystem::path user_data_dir(std::string_view app_name);
[[nodiscard]] std::filesystem::path user_config_dir(std::string_view app_name);
[[nodiscard]] std::filesystem::path user_cache_dir(std::string_view app_name);

} // namespace rime::platform
