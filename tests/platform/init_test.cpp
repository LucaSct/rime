// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M2.1 platform lifetime + the main-thread contract. doctest runs test bodies on the
// thread that called main(), which is our "main thread" — so after init(), on_main_thread() is
// true here and false on a spawned thread.
#include <doctest/doctest.h>

#include <thread>

#include "rime/platform/init.hpp"

TEST_CASE("init/shutdown lifetime and the main-thread guard") {
    using namespace rime::platform;

    CHECK(init()); // first init succeeds
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
