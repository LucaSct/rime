// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The engine's first unit test, and the proof for brick M0.2: it drives rime::core
// through its public header and runs under CTest. Defining the macro below makes this
// translation unit also provide doctest's main(), so the test executable is
// self-contained (doctest is header-only and ships no prebuilt main).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "rime/core/version.hpp"

TEST_CASE("engine_name is the engine's display name") {
    CHECK(rime::core::engine_name() == "Rime");
}

TEST_CASE("version_string reflects the compile-time version") {
    // Pin the human-readable form so an accidental change to the formatting is caught.
    CHECK(rime::core::version_string() == "0.0.1");

    // And prove the string is actually derived from kVersion rather than a hand-typed
    // literal, so the two can never silently drift: bumping kVersion alone must move the
    // string with it. (Built with std::to_string to keep this test free of fmt.)
    const auto& v = rime::core::kVersion;
    const std::string expected =
        std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    CHECK(rime::core::version_string() == expected);
}
