// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// M1.6 proof: saturate every CPU core through the work-stealing job system. We compute a
// deliberately CPU-heavy function over a few million items, first serially and then with
// parallel_for, verify the two results agree, and report the speedup. On an N-core machine the
// speedup should approach N — that is the visible evidence the job system is keeping the cores
// busy and balancing the load via stealing. Run it with `build/<preset>/bin/jobs_core_saturation`.

#include <fmt/core.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <thread>
#include <vector>

#include "rime/core/jobs.hpp"

namespace {

// A purposely-expensive, branch-free per-item kernel: enough transcendental work that the loop is
// compute-bound (so parallelism shows), and deterministic so serial and parallel must match bit
// for bit.
double heavy_kernel(std::size_t i) {
    double x = static_cast<double>(i) * 1e-6 + 1.0;
    for (int k = 0; k < 64; ++k) {
        x = std::sin(x) + std::cos(x * 1.3) + 1.0;
    }
    return x;
}

template <class Clock> double ms_between(Clock a, Clock b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

} // namespace

int main() {
    using rime::core::JobSystem;
    using clock = std::chrono::steady_clock;

    constexpr std::size_t kItems = 4'000'000;
    std::vector<double> serial(kItems);
    std::vector<double> parallel(kItems);

    // --- serial baseline ---
    const auto s0 = clock::now();
    for (std::size_t i = 0; i < kItems; ++i) {
        serial[i] = heavy_kernel(i);
    }
    const auto s1 = clock::now();

    // --- parallel via the job system ---
    JobSystem jobs;
    // Aim for ~16 chunks per participant: enough granularity for stealing to balance the load,
    // without so many tiny jobs that scheduling overhead dominates.
    const std::size_t chunk = std::max<std::size_t>(1, kItems / (jobs.participant_count() * 16));
    const auto p0 = clock::now();
    jobs.parallel_for(kItems, chunk, [&](std::size_t i) { parallel[i] = heavy_kernel(i); });
    const auto p1 = clock::now();

    // --- verify the parallel result equals the serial one ---
    bool match = true;
    for (std::size_t i = 0; i < kItems; ++i) {
        if (parallel[i] != serial[i]) {
            match = false;
            break;
        }
    }

    const double serial_ms = ms_between(s0, s1);
    const double parallel_ms = ms_between(p0, p1);
    const double speedup = parallel_ms > 0.0 ? serial_ms / parallel_ms : 0.0;

    fmt::print("Rime job system — core saturation sample\n");
    fmt::print("  hardware_concurrency : {}\n", std::thread::hardware_concurrency());
    fmt::print("  workers + main       : {}\n", jobs.participant_count());
    fmt::print("  items                : {}\n", kItems);
    fmt::print("  chunk size           : {}\n", chunk);
    fmt::print("  serial               : {:.1f} ms\n", serial_ms);
    fmt::print("  parallel             : {:.1f} ms\n", parallel_ms);
    fmt::print("  speedup              : {:.2f}x\n", speedup);
    fmt::print("  results match        : {}\n", match ? "yes" : "NO");

    // Non-zero exit on mismatch so this doubles as a smoke test.
    return match ? 0 : 1;
}
