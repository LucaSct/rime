// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.1 assertions: a recording handler lets us verify that a failing assert fires
// (with the right condition/message/site) and a passing one stays quiet, without aborting and
// without exceptions. RIME_VERIFY must always evaluate its expression; RIME_PANIC must always
// fire. The assert-firing cases depend on RIME_ENABLE_ASSERTS (on in dev/Debug); the
// VERIFY/PANIC cases hold in every configuration.

#include <doctest/doctest.h>

#include <string>
#include <string_view>

#include "rime/core/diagnostics/assert.hpp"

namespace {
struct Record {
    bool fired = false;
    std::string condition;
    std::string message;
    int line = 0;
};

Record g_last;

// Installs a recording handler and restores the default on scope exit, so a failure in one
// case never leaks into the next.
struct RecordingHandler {
    RecordingHandler() {
        g_last = {};
        rime::core::set_assert_handler(
            [](const char* cond, std::string_view msg, rime::core::SourceLocation where) {
                g_last.fired = true;
                g_last.condition = cond;
                g_last.message = std::string(msg);
                g_last.line = where.line;
            });
    }

    ~RecordingHandler() { rime::core::set_assert_handler({}); }
};
} // namespace

#if defined(RIME_ENABLE_ASSERTS) && RIME_ENABLE_ASSERTS
TEST_CASE("a failing assertion invokes the handler with its condition") {
    RecordingHandler guard;
    RIME_ASSERT(1 + 1 == 3);
    CHECK(g_last.fired);
    CHECK(g_last.condition == "1 + 1 == 3");
    CHECK(g_last.line > 0);
}

TEST_CASE("a passing assertion does nothing") {
    RecordingHandler guard;
    RIME_ASSERT(1 + 1 == 2);
    CHECK_FALSE(g_last.fired);
}

TEST_CASE("RIME_ASSERT_MSG carries a formatted message") {
    RecordingHandler guard;
    RIME_ASSERT_MSG(false, "x={}", 7);
    CHECK(g_last.fired);
    CHECK(g_last.message == "x=7");
}
#endif

TEST_CASE("RIME_VERIFY always evaluates its expression") {
    RecordingHandler guard;
    int side_effect = 0;
    RIME_VERIFY((side_effect = 5) == 5);
    CHECK(side_effect == 5);
    CHECK_FALSE(g_last.fired); // the condition was true, so nothing should have fired
}

TEST_CASE("RIME_PANIC always fires") {
    RecordingHandler guard;
    RIME_PANIC("boom {}", 1);
    CHECK(g_last.fired);
    CHECK(g_last.message == "boom 1");
}
