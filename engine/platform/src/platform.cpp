// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <thread>

#include "rime/platform/init.hpp"
#include "rime/platform/threads.hpp"

// OS-agnostic platform lifetime. The main-thread identity is captured here, once, and read from
// anywhere via on_main_thread(). We keep it in plain file-scope globals guarded by the
// documented contract — init() runs on the main thread before any other platform thread exists,
// so the single write happens-before every later read. (This is the same single-owner discipline
// the JobSystem relies on for its per-worker deques; we state the rule rather than pay for a lock
// on a value that is written exactly once at startup.)
namespace rime::platform {
namespace {

bool g_initialized = false;
std::thread::id g_main_thread_id{};

} // namespace

bool init() {
    if (g_initialized) {
        return false;
    }
    g_main_thread_id = std::this_thread::get_id();
    g_initialized = true;
    // Name the main thread so it is identifiable in a debugger/profiler — alongside the job
    // system's workers once those are named too.
    set_thread_name("rime-main");
    return true;
}

void shutdown() {
    g_initialized = false;
    g_main_thread_id = std::thread::id{};
}

bool on_main_thread() noexcept {
    return std::this_thread::get_id() == g_main_thread_id;
}

bool is_initialized() noexcept {
    return g_initialized;
}

} // namespace rime::platform
