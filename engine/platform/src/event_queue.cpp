// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include <deque>

#include "platform_backend.hpp"
#include "rime/platform/event.hpp"

// The event queue: a plain FIFO drained on the main thread. There is no lock because the contract
// (events are produced by pump_events() and consumed by poll_event(), both on the main/UI thread)
// makes this single-threaded by construction — the same "state the rule, don't pay for a mutex"
// discipline the rest of the platform layer uses. A std::deque is the natural fit: O(1) push-back
// (enqueue) and pop-front (dequeue), and it never invalidates the elements we are not touching.
namespace rime::platform {
namespace {

std::deque<Event> g_queue;
bool g_quit = false;

} // namespace

void post_event(const Event& e) {
    g_queue.push_back(e);
}

bool poll_event(Event& out) {
    if (g_queue.empty()) {
        return false;
    }
    out = g_queue.front();
    g_queue.pop_front();
    return true;
}

namespace detail {

void request_quit() {
    g_quit = true;
}

bool quit_requested() {
    return g_quit;
}

void reset_quit() {
    g_quit = false;
}

void clear_events() {
    g_queue.clear();
}

} // namespace detail
} // namespace rime::platform
