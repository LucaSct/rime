// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/platform/filesystem.hpp"

#include <fstream>
#include <ios>
#include <system_error>

// OS-agnostic filesystem helpers on top of the standard library. executable_path() is the one
// per-OS piece and lives in the backend files (src/<platform>/paths_*.cpp).
namespace rime::platform {

std::optional<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    // Open at the end (ate) so tellg() gives the size in one shot, then rewind and read.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    std::vector<std::byte> buffer(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (size > 0 &&
        !file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return buffer;
}

bool write_file(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()),
                   static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(file);
}

bool file_exists(const std::filesystem::path& path) {
    std::error_code ec; // the non-throwing overload: on error, returns false
    return std::filesystem::exists(path, ec);
}

} // namespace rime::platform
