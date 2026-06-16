// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/diagnostics/assert.hpp"

#include <fmt/format.h>

#include <cstdlib>
#include <mutex>
#include <utility>

#include "rime/core/diagnostics/log.hpp"

// Drop into the debugger if one is attached; otherwise this halts the process and we fall
// through to abort(). __builtin_trap is the GCC/Clang spelling, __debugbreak the MSVC one.
#if defined(_MSC_VER)
#include <intrin.h>
#define RIME_DEBUG_BREAK() __debugbreak()
#elif defined(__GNUC__) || defined(__clang__)
#define RIME_DEBUG_BREAK() __builtin_trap()
#else
#define RIME_DEBUG_BREAK() ((void)0)
#endif

namespace rime::core {
namespace {

std::mutex g_handler_mutex;
AssertHandler g_handler{}; // empty => default_assert_handler

[[noreturn]] void
default_assert_handler(const char* condition, std::string_view message, SourceLocation where) {
    // Report the failure through the normal logging path (so it lands wherever logs go),
    // then break/abort. The engine forbids exceptions on hot paths, so a failed assert ends
    // the process rather than throwing.
    if (message.empty()) {
        log_message(LogLevel::Error, fmt::format("assertion failed: {}", condition), where);
    } else {
        log_message(
            LogLevel::Error, fmt::format("assertion failed: {} - {}", condition, message), where);
    }
    RIME_DEBUG_BREAK();
    std::abort();
}

} // namespace

void set_assert_handler(AssertHandler handler) {
    const std::lock_guard<std::mutex> lock(g_handler_mutex);
    g_handler = std::move(handler);
}

void assert_failed(const char* condition, std::string_view message, SourceLocation where) {
    // Copy the handler out under the lock, then call it unlocked: the handler may run for a
    // while (or, in tests, reinstall itself), and we must not hold the lock across it.
    AssertHandler handler;
    {
        const std::lock_guard<std::mutex> lock(g_handler_mutex);
        handler = g_handler;
    }
    if (handler) {
        handler(condition, message, where);
    } else {
        default_assert_handler(condition, message, where);
    }
}

} // namespace rime::core
