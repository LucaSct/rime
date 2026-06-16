// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.1 logging: a custom sink captures records, the level threshold drops messages
// below it, and log_enabled mirrors the threshold. doctest runs cases sequentially, so the
// global logger state we poke here is restored at the end of each case.

#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "rime/core/diagnostics/log.hpp"

namespace {
// LogRecord's message is a view into a temporary, so we copy what we need into owning fields
// the moment the sink fires.
struct Captured {
    rime::core::LogLevel level;
    std::string message;
    std::string file;
    int line;
};
} // namespace

TEST_CASE("a log record passes through a custom sink with its call site") {
    std::vector<Captured> captured;
    rime::core::set_log_level(rime::core::LogLevel::Trace);
    rime::core::set_log_sink([&](const rime::core::LogRecord& r) {
        captured.push_back(
            {r.level, std::string(r.message), std::string(r.where.file), r.where.line});
    });

    RIME_INFO("answer is {}", 42);

    REQUIRE(captured.size() == 1);
    CHECK(captured[0].level == rime::core::LogLevel::Info);
    CHECK(captured[0].message == "answer is 42");
    CHECK(captured[0].line > 0);

    rime::core::set_log_sink({});
    rime::core::set_log_level(rime::core::LogLevel::Info);
}

TEST_CASE("messages below the active level are dropped") {
    int count = 0;
    rime::core::set_log_level(rime::core::LogLevel::Warn);
    rime::core::set_log_sink([&](const rime::core::LogRecord&) { ++count; });

    RIME_DEBUG("dropped"); // Debug < Warn
    RIME_INFO("dropped");  // Info  < Warn
    RIME_WARN("kept");     // Warn >= Warn
    RIME_ERROR("kept");    // Error >= Warn

    CHECK(count == 2);

    rime::core::set_log_sink({});
    rime::core::set_log_level(rime::core::LogLevel::Info);
}

TEST_CASE("log_enabled mirrors the active threshold") {
    rime::core::set_log_level(rime::core::LogLevel::Error);
    CHECK_FALSE(rime::core::log_enabled(rime::core::LogLevel::Info));
    CHECK(rime::core::log_enabled(rime::core::LogLevel::Error));
    CHECK_FALSE(rime::core::log_enabled(rime::core::LogLevel::Off));
    rime::core::set_log_level(rime::core::LogLevel::Info);
}
