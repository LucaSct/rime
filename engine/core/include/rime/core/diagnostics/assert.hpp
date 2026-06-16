// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <fmt/format.h>

#include <functional>
#include <string_view>

#include "rime/core/diagnostics/source_location.hpp"

// Assertions: cheap invariant checks that are active in debug-style builds and compiled out
// otherwise. On failure they route through a swappable handler: the default logs and aborts;
// tests install a recording handler to verify a failure fired without crashing (and without
// exceptions, which the engine forbids on hot paths).
namespace rime::core {

// Args: the stringified condition, an optional formatted message, and the call site.
using AssertHandler =
    std::function<void(const char* condition, std::string_view message, SourceLocation where)>;

// Empty handler restores the default (log at Error, then trap/abort).
void set_assert_handler(AssertHandler handler);

// The slow path behind the macros: invoke the current handler. Exposed for the macros; not
// usually called directly.
void assert_failed(const char* condition, std::string_view message, SourceLocation where);

} // namespace rime::core

#if defined(RIME_ENABLE_ASSERTS) && RIME_ENABLE_ASSERTS

// Checked builds: evaluate the condition and, if it is false, report it.
#define RIME_ASSERT(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::rime::core::assert_failed(#cond, {}, RIME_SRC_LOC);                                  \
        }                                                                                          \
    } while (false)

#define RIME_ASSERT_MSG(cond, ...)                                                                 \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::rime::core::assert_failed(#cond, ::fmt::format(__VA_ARGS__), RIME_SRC_LOC);          \
        }                                                                                          \
    } while (false)

#define RIME_VERIFY(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ::rime::core::assert_failed(#cond, {}, RIME_SRC_LOC);                                  \
        }                                                                                          \
    } while (false)

#else

// Unchecked builds: asserts vanish. RIME_VERIFY still EVALUATES its expression (only the
// check is dropped), so it is safe for conditions with side effects you always want to run,
// e.g. `RIME_VERIFY(initialize())`. The cast to void silences unused-result warnings.
#define RIME_ASSERT(cond) ((void)0)
#define RIME_ASSERT_MSG(cond, ...) ((void)0)
#define RIME_VERIFY(cond) ((void)(cond))

#endif

// Always active: an unconditional, unrecoverable failure ("can't happen" branches, the
// default of a switch that should be exhaustive). Routes through the same handler so it is
// observable in tests.
#define RIME_PANIC(...)                                                                            \
    ::rime::core::assert_failed("RIME_PANIC", ::fmt::format(__VA_ARGS__), RIME_SRC_LOC)
