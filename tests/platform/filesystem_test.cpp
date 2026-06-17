// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the M2.4 filesystem helpers: write/read round-trips byte-for-byte, exists reports
// correctly, a missing file reads as nullopt, executable_path() points at a real file (here, the
// running test binary), and the per-user base dirs are absolute and namespaced by app — together
// exercising the per-OS native path lookups.
#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "rime/platform/filesystem.hpp"

using namespace rime::platform;

TEST_CASE("write_file/read_file round-trip and file_exists") {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() / "rime_fs_roundtrip.bin";
    std::error_code ec;
    fs::remove(path, ec);

    CHECK_FALSE(file_exists(path));

    const std::vector<std::byte> data{
        std::byte{0x01}, std::byte{0x02}, std::byte{0xFE}, std::byte{0x00}, std::byte{0xFF}};
    REQUIRE(write_file(path, data));
    CHECK(file_exists(path));

    const auto read = read_file(path);
    REQUIRE(read.has_value());
    CHECK(read->size() == data.size());
    CHECK(*read == data);

    fs::remove(path, ec);
}

TEST_CASE("read_file of a missing path is nullopt") {
    CHECK_FALSE(read_file("/no/such/rime/path/zzz.bin").has_value());
}

TEST_CASE("executable_path points at an existing file") {
    const auto exe = executable_path();
    CHECK_FALSE(exe.empty());
    CHECK(file_exists(exe));
}

TEST_CASE("user base dirs are absolute and namespaced by app") {
    namespace fs = std::filesystem;
    const std::string app = "RimeUnitTest";
    const fs::path data = user_data_dir(app);
    const fs::path config = user_config_dir(app);
    const fs::path cache = user_cache_dir(app);

    // The standard CI runners always provide HOME / %APPDATA%, so a real path is expected here.
    REQUIRE_FALSE(data.empty());
    REQUIRE_FALSE(config.empty());
    REQUIRE_FALSE(cache.empty());

    // Each is absolute and ends in the app name (so two apps never share a directory). These are
    // pure queries — we deliberately do not create or touch anything on disk.
    CHECK(data.is_absolute());
    CHECK(config.is_absolute());
    CHECK(cache.is_absolute());
    CHECK(data.filename() == app);
    CHECK(config.filename() == app);
    CHECK(cache.filename() == app);

    // Cache is a distinct, throwaway location from persistent data on every platform we target.
    CHECK(cache != data);
}
