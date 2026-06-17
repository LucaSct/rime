// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M2.1 thread naming. The OS gives us no portable way to read a thread's name back, so
// this is a smoke test: naming the current thread, an over-long name (forcing the per-OS
// truncation path), an empty name, and a name on a spawned std::thread must all run without
// crashing or throwing.
#include <doctest/doctest.h>

#include <string>
#include <thread>

#include "rime/platform/threads.hpp"

TEST_CASE("set_thread_name is a safe, best-effort no-throw") {
    using rime::platform::set_thread_name;

    set_thread_name("rime-test");
    set_thread_name(std::string(128, 'x')); // longer than every OS limit: exercises truncation
    set_thread_name("");                    // empty: must be a harmless no-op

    std::thread worker([] { rime::platform::set_thread_name("rime-worker"); });
    worker.join();

    CHECK(true); // reaching here without a crash/throw is the assertion
}
