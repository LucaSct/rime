// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M2.1 platform lifetime + the main-thread contract. doctest runs test bodies on the
// thread that called main(), which is our "main thread" — so after init(), on_main_thread() is
// true here and false on a spawned thread.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <thread>

#include "rime/platform/init.hpp"
#include "rime/platform/window.hpp"

TEST_CASE("init/shutdown lifetime and the main-thread guard") {
    using namespace rime::platform;

    set_headless(true); // a unit test must never bring up a real window server
    CHECK(init());      // first init succeeds
    CHECK(is_initialized());
    CHECK_FALSE(init());     // double-init is rejected, leaving the layer initialized
    CHECK(on_main_thread()); // the test body runs on the thread that called init()

    bool worker_on_main = true;
    std::thread worker([&] { worker_on_main = on_main_thread(); });
    worker.join();
    CHECK_FALSE(worker_on_main); // a different thread is not the main thread

    shutdown();
    CHECK_FALSE(is_initialized());
}

TEST_CASE("TimerResolutionScope makes a 1 ms sleep about 1 ms, with no window and no init()") {
    using namespace rime::platform;
    using clock = std::chrono::steady_clock;

    const TimerResolutionScope fine_timer;
    std::array<clock::duration, 21> samples{};
    for (auto& s : samples) {
        const auto t0 = clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        s = clock::now() - t0;
    }
    std::sort(samples.begin(), samples.end());
    const auto median = samples[samples.size() / 2];
    MESSAGE("median sleep_for(1ms): " << std::chrono::duration<double, std::milli>(median).count()
                                      << " ms");
    // 8 ms is half the default Windows period: the result is bimodal (~1-2 ms raised, ~16 ms
    // not), so this is a mode test, not a fine timing threshold.
    CHECK(median < std::chrono::milliseconds(8));
}
