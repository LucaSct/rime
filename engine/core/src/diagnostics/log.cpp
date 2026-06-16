// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/diagnostics/log.hpp"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <utility>

namespace rime::core {
namespace {

// The active threshold. atomic because any thread may log or change the level; relaxed
// ordering suffices: we need each load/store to be coherent, not synchronized with other
// memory. A log racing a level change simply sees the old or the new value, which is fine.
std::atomic<LogLevel> g_level{LogLevel::Info};

// Guards the sink pointer *and* serializes sink calls, so concurrent logs from worker
// threads (arriving in M1.6) don't interleave mid-line. A sink therefore must not call back
// into the logger.
std::mutex g_sink_mutex;
LogSink g_sink{}; // empty => default_sink

const char* level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warn:
            return "WARN";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Off:
            return "OFF";
    }
    return "?"; // unreachable for a valid LogLevel; keeps -Wreturn-type / MSVC C4715 quiet
}

void default_sink(const LogRecord& rec) {
    // "[LEVEL] message (file:line)" to stderr. Called with g_sink_mutex held, so lines from
    // different threads stay whole.
    fmt::print(stderr,
               "[{}] {} ({}:{})\n",
               level_name(rec.level),
               rec.message,
               rec.where.file,
               rec.where.line);
}

} // namespace

void set_log_level(LogLevel level) noexcept {
    g_level.store(level, std::memory_order_relaxed);
}

LogLevel log_level() noexcept {
    return g_level.load(std::memory_order_relaxed);
}

bool log_enabled(LogLevel level) noexcept {
    // Never "log at Off"; otherwise show the message only if its severity meets the current
    // threshold. Comparing the underlying bytes keeps this to a single relaxed load.
    if (level == LogLevel::Off) {
        return false;
    }
    return static_cast<std::uint8_t>(level) >=
           static_cast<std::uint8_t>(g_level.load(std::memory_order_relaxed));
}

void set_log_sink(LogSink sink) {
    const std::lock_guard<std::mutex> lock(g_sink_mutex);
    g_sink = std::move(sink);
}

void log_message(LogLevel level, std::string_view message, SourceLocation where) {
    const LogRecord rec{level, message, where};
    const std::lock_guard<std::mutex> lock(g_sink_mutex);
    if (g_sink) {
        g_sink(rec);
    } else {
        default_sink(rec);
    }
}

} // namespace rime::core
