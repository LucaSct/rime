// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <chrono>
#include <functional>
#include <string_view>

// Timing and a lightweight profiling hook. `Stopwatch` measures durations; `ScopedZone`
// (via RIME_PROFILE_ZONE) times a scope and reports it to a swappable sink. This is the hook
// the job system (M1.6) uses to surface per-worker busy time. With no sink installed a zone
// costs only a Stopwatch, so the hook can stay in hot code.
namespace rime::core {

// A monotonic wall-clock stopwatch. steady_clock never jumps backward (unlike system_clock,
// which the OS can adjust), which is exactly what you want for measuring elapsed time.
class Stopwatch {
public:
    Stopwatch() noexcept : start_(Clock::now()) {}

    void restart() noexcept { start_ = Clock::now(); }

    [[nodiscard]] double elapsed_us() const noexcept {
        return std::chrono::duration<double, std::micro>(Clock::now() - start_).count();
    }

    [[nodiscard]] double elapsed_ms() const noexcept { return elapsed_us() / 1000.0; }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_;
};

// A profiling "zone" reports how long a scope took. A real profiler nests these into a flame
// graph; for now a zone hands (name, milliseconds) to the sink when it closes.
using ZoneSink = std::function<void(std::string_view name, double ms)>;

void set_zone_sink(ZoneSink sink);
void report_zone(std::string_view name, double ms);

// RAII zone: times from construction to destruction, reports on scope exit. The name must
// outlive the zone (a string literal is the intended use). Non-copyable and non-movable: a
// zone is tied to exactly one scope.
class ScopedZone {
public:
    explicit ScopedZone(std::string_view name) noexcept : name_(name) {}

    ~ScopedZone() { report_zone(name_, watch_.elapsed_ms()); }

    ScopedZone(const ScopedZone&) = delete;
    ScopedZone& operator=(const ScopedZone&) = delete;
    ScopedZone(ScopedZone&&) = delete;
    ScopedZone& operator=(ScopedZone&&) = delete;

private:
    std::string_view name_;
    Stopwatch watch_;
};

} // namespace rime::core

// Reach a zone through this macro so each gets a unique variable name (two zones can share a
// scope) and unused-variable warnings stay quiet.
#define RIME_PROFILE_ZONE_CONCAT_(a, b) a##b
#define RIME_PROFILE_ZONE_NAME_(line) RIME_PROFILE_ZONE_CONCAT_(rime_zone_, line)
#define RIME_PROFILE_ZONE(name) ::rime::core::ScopedZone RIME_PROFILE_ZONE_NAME_(__LINE__)(name)
