// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for M1.6a (Chase-Lev work-stealing deque). Single-threaded: the owner sees LIFO via
// push/pop, thieves see FIFO via steal, the buffer grows transparently, and an empty deque
// reports Empty. Concurrent: one owner pushing and popping while many thieves steal, every item
// is consumed EXACTLY once — no losses (a missed happens-before) and no duplicates (a lost
// last-element race). The concurrent run is the real test of the memory orderings.
//
// doctest's assertion macros are not thread-safe, so worker threads only touch atomics; all
// CHECK/REQUIRE run on the main thread after the workers join.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "rime/core/jobs/chase_lev_deque.hpp"

using namespace rime::core;

TEST_CASE("single-threaded owner sees LIFO via push/pop") {
    ChaseLevDeque<int> dq(4);
    CHECK(dq.empty_approx());
    dq.push(1);
    dq.push(2);
    dq.push(3);
    CHECK(dq.size_approx() == 3);

    auto a = dq.pop();
    auto b = dq.pop();
    auto c = dq.pop();
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(c.ok());
    CHECK(a.value == 3); // last in, first out
    CHECK(b.value == 2);
    CHECK(c.value == 1);

    auto d = dq.pop();
    CHECK(d.status == DequeStatus::Empty);
}

TEST_CASE("steal takes from the opposite end (FIFO)") {
    ChaseLevDeque<int> dq(4);
    dq.push(10);
    dq.push(20);
    dq.push(30);
    // Thieves take the OLDEST first; pushed 10,20,30 -> stolen 10,20,30.
    auto a = dq.steal();
    auto b = dq.steal();
    auto c = dq.steal();
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    REQUIRE(c.ok());
    CHECK(a.value == 10);
    CHECK(b.value == 20);
    CHECK(c.value == 30);
    CHECK(dq.steal().status == DequeStatus::Empty);
}

TEST_CASE("the circular buffer grows transparently") {
    ChaseLevDeque<int> dq(2); // tiny: forces several growths
    constexpr int n = 1000;
    for (int i = 0; i < n; ++i) {
        dq.push(i);
    }
    CHECK(dq.capacity() >= static_cast<std::size_t>(n));
    CHECK(dq.size_approx() == n);
    // All values survive the growth, in LIFO order.
    bool ok = true;
    for (int i = n - 1; i >= 0; --i) {
        auto r = dq.pop();
        if (!r.ok() || r.value != i) {
            ok = false;
            break;
        }
    }
    CHECK(ok);
    CHECK(dq.empty_approx());
}

TEST_CASE("empty deque reports Empty for both pop and steal") {
    ChaseLevDeque<int> dq(8);
    CHECK(dq.pop().status == DequeStatus::Empty);
    CHECK(dq.steal().status == DequeStatus::Empty);
}

TEST_CASE("concurrent owner + thieves consume every item exactly once") {
    constexpr int kItems = 100000;
    ChaseLevDeque<int> dq(32); // small start -> exercises growth under contention

    std::vector<std::atomic<int>> seen(kItems); // times each value was consumed (want exactly 1)
    std::atomic<int> consumed{0};
    std::atomic<bool> out_of_range{false};
    std::atomic<bool> duplicate{false};
    std::atomic<bool> go{false};

    auto consume = [&](int v) {
        if (v < 0 || v >= kItems) {
            out_of_range.store(true, std::memory_order_relaxed);
            return;
        }
        if (seen[v].fetch_add(1, std::memory_order_relaxed) != 0) {
            duplicate.store(true, std::memory_order_relaxed);
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
    };

    // Thieves: spin-steal until everything has been consumed. Empty/Abort -> back off and retry.
    const unsigned thief_count = std::max(2u, std::thread::hardware_concurrency());
    std::vector<std::thread> thieves;
    thieves.reserve(thief_count);
    for (unsigned i = 0; i < thief_count; ++i) {
        thieves.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            while (consumed.load(std::memory_order_relaxed) < kItems) {
                auto r = dq.steal();
                if (r.status == DequeStatus::Success) {
                    consume(r.value);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    // Owner (this thread): push everything, occasionally popping to contend with the thieves on
    // the bottom, then help drain whatever the thieves did not steal.
    go.store(true, std::memory_order_release);
    for (int i = 0; i < kItems; ++i) {
        dq.push(i);
        if ((i & 15) == 0) {
            auto r = dq.pop();
            if (r.status == DequeStatus::Success) {
                consume(r.value);
            }
        }
    }
    while (consumed.load(std::memory_order_relaxed) < kItems) {
        auto r = dq.pop();
        if (r.status == DequeStatus::Success) {
            consume(r.value);
        } else {
            std::this_thread::yield();
        }
    }

    for (auto& t : thieves) {
        t.join();
    }

    // All assertions back on the main thread.
    CHECK_FALSE(out_of_range.load());
    CHECK_FALSE(duplicate.load());
    CHECK(consumed.load() == kItems);
    int total = 0;
    bool each_once = true;
    for (int i = 0; i < kItems; ++i) {
        const int c = seen[i].load(std::memory_order_relaxed);
        total += c;
        if (c != 1) {
            each_once = false;
        }
    }
    CHECK(each_once);
    CHECK(total == kItems);
}
