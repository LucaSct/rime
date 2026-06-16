// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <functional>
#include <string_view>

#include "rime/core/diagnostics/source_location.hpp"

// Leveled logging for the engine. The everyday entry points are the RIME_* macros at the
// bottom; they short-circuit below the active level (so a disabled log costs one atomic
// load, not a fmt::format) and capture the call site automatically. Output goes through a
// swappable sink: dependency injection that also makes logging testable.
namespace rime::core {

// Severity, low to high. `Off` is a threshold sentinel: set the level to Off to silence
// everything. One byte, because it travels inside every LogRecord.
enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error, Off };

// One log message handed to the active sink. The message is a view, not a string: the
// macros format into a temporary std::string that outlives the (synchronous) sink call, so
// the view never dangles, and a sink that only counts or filters pays nothing for a copy.
struct LogRecord {
    LogLevel level;
    std::string_view message;
    SourceLocation where;
};

// Where formatted records go: stderr by default, but also a file, a test buffer, or the
// editor console. A std::function so it can be swapped at runtime.
using LogSink = std::function<void(const LogRecord&)>;

// --- Configuration (all thread-safe) ---------------------------------------------------
void set_log_level(LogLevel level) noexcept;
[[nodiscard]] LogLevel log_level() noexcept;

// True if a message at `level` would currently be emitted. The macros test this first so
// that, below the threshold, the fmt::format never runs.
[[nodiscard]] bool log_enabled(LogLevel level) noexcept;

// Replace the sink; pass an empty std::function to restore the default stderr sink.
void set_log_sink(LogSink sink);

// --- Emission --------------------------------------------------------------------------
// Build a LogRecord and hand it to the active sink. Usually reached through the macros, but
// exposed for code that already holds a formatted string.
void log_message(LogLevel level, std::string_view message, SourceLocation where);

} // namespace rime::core

// RIME_LOG and its level-named wrappers are the public interface. The level check guards the
// formatting so disabled logs are nearly free; fmt's compile-time format checking still
// applies to the arguments.
#define RIME_LOG(level, ...)                                                                       \
    do {                                                                                           \
        if (::rime::core::log_enabled(level)) {                                                    \
            ::rime::core::log_message((level), ::fmt::format(__VA_ARGS__), RIME_SRC_LOC);          \
        }                                                                                          \
    } while (false)

#define RIME_TRACE(...) RIME_LOG(::rime::core::LogLevel::Trace, __VA_ARGS__)
#define RIME_DEBUG(...) RIME_LOG(::rime::core::LogLevel::Debug, __VA_ARGS__)
#define RIME_INFO(...) RIME_LOG(::rime::core::LogLevel::Info, __VA_ARGS__)
#define RIME_WARN(...) RIME_LOG(::rime::core::LogLevel::Warn, __VA_ARGS__)
#define RIME_ERROR(...) RIME_LOG(::rime::core::LogLevel::Error, __VA_ARGS__)
