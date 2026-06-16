// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/core/diagnostics/profile.hpp"

#include <mutex>
#include <utility>

namespace rime::core {
namespace {
std::mutex g_zone_mutex;
ZoneSink g_zone_sink{}; // empty => no-op (zones are free when nothing is listening)
} // namespace

void set_zone_sink(ZoneSink sink) {
    const std::lock_guard<std::mutex> lock(g_zone_mutex);
    g_zone_sink = std::move(sink);
}

void report_zone(std::string_view name, double ms) {
    // Copy the sink out under the lock, then call it unlocked (a zone sink may itself be
    // doing real work, e.g. appending to a trace buffer).
    ZoneSink sink;
    {
        const std::lock_guard<std::mutex> lock(g_zone_mutex);
        sink = g_zone_sink;
    }
    if (sink) {
        sink(name, ms);
    }
}

} // namespace rime::core
