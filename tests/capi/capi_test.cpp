// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Tests for the C ABI (rime/capi/rime.h, M6.9), exercised from C++ and linked against the real
// librime_capi shared library. This complements tools/rime-ffi's Rust tests: those prove the ABI
// works from another language, these run on every CI OS and — crucially — under the ASan/UBSan job
// (which builds C++ only), giving the trust-nothing validate path and the app lifecycle a sanitizer
// pass across the FFI boundary. RIME_CAPI_FIXTURE_DIR (the shared assets fixtures) is injected by
// CMake; the mesh fixture is the same quad.rmesh the cross-language tests use.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "rime/capi/rime.h"

TEST_CASE("rime_version reports the engine version (0.0.1)") {
    const RimeVersion v = rime_version();
    CHECK(v.major == 0);
    CHECK(v.minor == 0);
    CHECK(v.patch == 1);
}

TEST_CASE("rime_asset_validate accepts the golden mesh fixture and reports real counts") {
    RimeAssetInfo info{};
    const RimeStatus s = rime_asset_validate(RIME_CAPI_FIXTURE_DIR "/quad.rmesh", &info);
    REQUIRE(s == RIME_OK);
    CHECK(info.kind == static_cast<uint32_t>(RIME_ASSET_MESH));
    // The same mesh schema hash both languages pin — the cross-language drift alarm, now over the
    // ABI.
    CHECK(info.schema_hash == 0x198738A2DDE250ACull);
    CHECK(info.vertex_count == 4); // a quad: 4 unique vertices
    CHECK(info.index_count == 6);  // two triangles
}

TEST_CASE("rime_asset_validate reports the right status for bad inputs, with a message") {
    RimeAssetInfo info{};

    SUBCASE("a missing file is an I/O error") {
        CHECK(rime_asset_validate("/no/such/path/quad.rmesh", &info) == RIME_ERR_IO);
        CHECK(std::strlen(rime_last_error_message()) > 0);
    }
    SUBCASE("null arguments are rejected, not dereferenced") {
        CHECK(rime_asset_validate(nullptr, &info) == RIME_ERR_INVALID_ARGUMENT);
        CHECK(rime_asset_validate("anything", nullptr) == RIME_ERR_INVALID_ARGUMENT);
    }
    SUBCASE("garbage bytes fail the RMA1 reader (not a crash)") {
        const auto path = std::filesystem::temp_directory_path() / "rime_capi_corrupt.bin";
        {
            std::ofstream out(path, std::ios::binary);
            out << "definitely not an RMA1 cooked asset payload";
        }
        CHECK(rime_asset_validate(path.string().c_str(), &info) == RIME_ERR_ASSET_INVALID);
        CHECK(std::strlen(rime_last_error_message()) > 0);
        std::filesystem::remove(path);
    }
}

TEST_CASE("rime_last_error_message is empty after a successful call") {
    RimeVersion v = rime_version();
    (void)v;
    CHECK(std::strlen(rime_last_error_message()) == 0);
}

TEST_CASE("headless application lifecycle through the C ABI") {
    RimeApp* app = rime_app_create_headless();
    REQUIRE(app != nullptr);
    CHECK(rime_app_tick(app, 3) == RIME_OK);
    CHECK(rime_app_tick(nullptr, 1) == RIME_ERR_INVALID_ARGUMENT); // null-checked, not dereferenced
    rime_app_destroy(app);
    rime_app_destroy(nullptr); // destroy is null-safe
}
