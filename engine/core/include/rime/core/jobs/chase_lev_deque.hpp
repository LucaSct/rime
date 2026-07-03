// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

#include "rime/core/diagnostics/assert.hpp"

// A Chase-Lev lock-free work-stealing deque — the per-worker queue the job system (M1.6b) is
// built on. It has ONE owner thread and MANY thieves:
//   - the owner pushes/pops at the BOTTOM (LIFO), which is cache-friendly and contention-free in
//     the common case (just relaxed loads/stores);
//   - idle worker threads STEAL from the TOP (FIFO), contending only with each other and with the
//     owner over the single last element.
// This asymmetry is the whole point of work stealing: the hot owner path is nearly free, and
// synchronization is paid only when a thief actually races for work.
//
// The implementation follows Chase & Lev (2005), "Dynamic Circular Work-Stealing Deque," with
// the memory orderings of Lê, Pop, Cohen & Nardelli (2013), "Correct and Efficient Work-Stealing
// for Weak Memory Models" — the peer-reviewed C11-atomics version that is correct on ARM/POWER,
// not just x86. The ordering argument is written up in docs/design/work-stealing-deque.md; the
// comments below name each fence and why it is there. Get the orderings wrong and this breaks
// only rarely, only on weak hardware, only under load — so we follow the proof exactly.
namespace rime::core {

enum class DequeStatus {
    Success, // got an item
    Empty,   // the deque was observably empty
    Abort,   // a thief lost the race for an item; the caller should retry (not "empty")
};

template <class T> struct DequeResult {
    DequeStatus status = DequeStatus::Empty;
    T value{};

    [[nodiscard]] bool ok() const noexcept { return status == DequeStatus::Success; }
};

template <class T> class ChaseLevDeque {
    // T is stored in lock-free atomic slots; restrict to trivially-copyable, lock-free types
    // (in practice a pointer or small handle — exactly what a job queue holds).
    static_assert(std::is_trivially_copyable_v<T>,
                  "ChaseLevDeque<T>: T must be trivially copyable");

public:
    explicit ChaseLevDeque(std::size_t initial_capacity = 1024) {
        const std::size_t cap = round_up_pow2(initial_capacity < 2 ? 2 : initial_capacity);
        buffers_.push_back(std::make_unique<Buffer>(cap));
        array_.store(buffers_.back().get(), std::memory_order_relaxed);
    }

    ChaseLevDeque(const ChaseLevDeque&) = delete;
    ChaseLevDeque& operator=(const ChaseLevDeque&) = delete;
    ChaseLevDeque(ChaseLevDeque&&) = delete;
    ChaseLevDeque& operator=(ChaseLevDeque&&) = delete;
    ~ChaseLevDeque() = default; // buffers_ frees every (current + retired) buffer

    // OWNER ONLY. Append an item at the bottom, growing the circular buffer if it is full.
    void push(T item) {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed);
        const std::int64_t t = top_.load(std::memory_order_acquire);
        Buffer* buf = array_.load(std::memory_order_relaxed);

        if (b - t > static_cast<std::int64_t>(buf->cap) - 1) {
            // Full: copy live elements [t, b) into a 2x buffer and publish it. The old buffer is
            // retired (kept alive), not freed, because an in-flight thief may still read from it.
            buf = grow(buf, b, t);
            array_.store(buf, std::memory_order_release);
        }

        buf->put(b, item);
        // Release fence: make the slot write visible to a thief BEFORE it can observe the new
        // bottom and try to take this index.
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    // OWNER ONLY. Take from the bottom (LIFO). Returns Empty if there was nothing to take. The
    // owner never returns Abort: losing the last-element race is, for it, simply Empty.
    DequeResult<T> pop() noexcept {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed) - 1;
        Buffer* buf = array_.load(std::memory_order_relaxed);
        bottom_.store(b, std::memory_order_relaxed);
        // Seq-cst fence: this is the crux. It orders our bottom-- against a thief's top++ so that
        // the owner and a thief can never both take the same single element.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::int64_t t = top_.load(std::memory_order_relaxed);

        DequeResult<T> result;
        if (t <= b) {
            // Non-empty.
            result = {DequeStatus::Success, buf->get(b)};
            if (t == b) {
                // Exactly one element: race the thieves for it via CAS on top.
                if (!top_.compare_exchange_strong(
                        t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    result = {DequeStatus::Empty, T{}}; // a thief won it
                }
                bottom_.store(b + 1, std::memory_order_relaxed); // deque is now empty
            }
        } else {
            // Empty: restore bottom to where it was.
            result = {DequeStatus::Empty, T{}};
            bottom_.store(b + 1, std::memory_order_relaxed);
        }
        return result;
    }

    // THIEF (any non-owner thread). Steal from the top (FIFO). Returns Success, Empty, or Abort
    // (lost the CAS race — caller should retry; the deque is not necessarily empty).
    DequeResult<T> steal() noexcept {
        std::int64_t t = top_.load(std::memory_order_acquire);
        // Seq-cst fence pairs with the owner's pop() fence: it orders our top read against the
        // owner's bottom write, so we see a consistent (top, bottom) pair.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const std::int64_t b = bottom_.load(std::memory_order_acquire);

        DequeResult<T> result; // defaults to Empty
        if (t < b) {
            // Non-empty. Acquire-load the buffer so we see the slot write the owner released in
            // push(). Read the value FIRST, then try to claim the index with a CAS on top.
            Buffer* buf = array_.load(std::memory_order_acquire);
            const T value = buf->get(t);
            if (top_.compare_exchange_strong(
                    t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed)) {
                result = {DequeStatus::Success, value};
            } else {
                result = {DequeStatus::Abort, T{}}; // another thread moved top; retry
            }
        }
        return result;
    }

    // Approximate count (bottom - top). Racy by nature — for diagnostics/heuristics only, never
    // for correctness decisions.
    [[nodiscard]] std::size_t size_approx() const noexcept {
        const std::int64_t b = bottom_.load(std::memory_order_relaxed);
        const std::int64_t t = top_.load(std::memory_order_relaxed);
        return b > t ? static_cast<std::size_t>(b - t) : 0;
    }

    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

    [[nodiscard]] std::size_t capacity() const noexcept {
        return array_.load(std::memory_order_relaxed)->cap;
    }

private:
    // A power-of-two circular array of atomic slots. Power-of-two capacity turns the modulo into a
    // bit-mask.
    //
    // Slot ordering — release/acquire, a note the paper doesn't need but a *pointer-carrying*
    // deque does. Lê et al. access the array with relaxed atomics and let the top/bottom fences
    // carry all ordering; that is sufficient when the element IS the payload (say an int). But this
    // deque carries POINTERS (the job system stores Job*), and the pointed-to object must be
    // published too: a thief that steals a Job* must also see the Job's fields the owner wrote
    // before pushing. The index fences already guarantee that by the standard — the release fence
    // before push()'s bottom store synchronizes-with a thief's acquire load of bottom
    // ([atomics.fences]/3), so everything the owner wrote before the push happens-before the
    // thief's use — and the code is race-free with relaxed slots. We nonetheless publish on the
    // slot itself, a release store paired with an acquire load, for two reasons: it makes the
    // payload happens-before explicit exactly where the pointer is transferred, and ThreadSanitizer
    // cannot follow synchronization through a standalone fence (a known TSan blind spot for
    // fence-based lock-free code) — so without this it reports the payload transfer as a false
    // race. This only ADDS ordering; it never weakens the index protocol, and the seq-cst
    // pop/steal fences below are untouched. Write-up: docs/design/work-stealing-deque.md.
    struct Buffer {
        std::size_t cap;
        std::size_t mask;
        std::unique_ptr<std::atomic<T>[]> slots;

        explicit Buffer(std::size_t c)
            : cap(c), mask(c - 1), slots(std::make_unique<std::atomic<T>[]>(c)) {}

        // Acquire: pairs with put()'s release so a stolen pointer's payload is visible to the
        // thief.
        [[nodiscard]] T get(std::int64_t i) const noexcept {
            return slots[static_cast<std::size_t>(i) & mask].load(std::memory_order_acquire);
        }

        // Release: publishes the element and, when it is a pointer, the object it points at.
        void put(std::int64_t i, T x) noexcept {
            slots[static_cast<std::size_t>(i) & mask].store(x, std::memory_order_release);
        }
    };

    static std::size_t round_up_pow2(std::size_t n) noexcept {
        std::size_t p = 1;
        while (p < n) {
            p <<= 1;
        }
        return p;
    }

    // Allocate a double-size buffer, copy the live range [t, b) into it (preserving each element's
    // logical index, so a stale buffer read at index i still yields the same value), retire the
    // old buffer (owner-only mutation of buffers_), and return the new one.
    Buffer* grow(Buffer* old_buf, std::int64_t b, std::int64_t t) {
        auto fresh = std::make_unique<Buffer>(old_buf->cap * 2);
        for (std::int64_t i = t; i < b; ++i) {
            fresh->put(i, old_buf->get(i));
        }
        Buffer* raw = fresh.get();
        buffers_.push_back(std::move(fresh)); // keep old buffers alive for in-flight thieves
        return raw;
    }

    // top_/bottom_ are signed: pop() transiently decrements bottom below top on an empty deque.
    // Cache-line padding to avoid false sharing between the owner's bottom and thieves' top is a
    // worthwhile later tweak (measure first); kept simple here.
    std::atomic<std::int64_t> top_{0};
    std::atomic<std::int64_t> bottom_{0};
    std::atomic<Buffer*> array_{nullptr};
    std::vector<std::unique_ptr<Buffer>> buffers_; // owns current + retired buffers; owner-only
};

} // namespace rime::core
