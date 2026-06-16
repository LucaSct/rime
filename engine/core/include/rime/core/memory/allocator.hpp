// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

// The allocator seam for the whole engine. Engine code allocates through an `Allocator`
// rather than raw new/delete (a hard rule in CLAUDE.md) so that each subsystem can pick a
// memory strategy matching its access pattern, and so allocations can be tracked. This header
// also defines make_in/destroy_in, the typed new/delete built on top of the raw interface.
namespace rime::core {

// Round n up to the next multiple of `alignment`, which MUST be a power of two.
//
// Why the bit trick works: for a power of two a, (a - 1) is a mask of the low bits (e.g.
// 16 -> 0b1111). Adding (a - 1) pushes any non-zero remainder up into the next multiple, and
// AND-ing with ~(a - 1) clears those low bits back down to that multiple. If a were not a
// power of two, ~(a - 1) would not be a clean low-bit mask and the result would be garbage.
[[nodiscard]] constexpr std::size_t align_up(std::size_t n, std::size_t alignment) noexcept {
    return (n + (alignment - 1)) & ~(alignment - 1);
}

// The same rounding applied to a pointer's address; returns a pointer at or after p.
[[nodiscard]] inline void* align_ptr(void* p, std::size_t alignment) noexcept {
    const auto addr = reinterpret_cast<std::uintptr_t>(p);
    const auto aligned = (addr + (alignment - 1)) & ~(static_cast<std::uintptr_t>(alignment) - 1);
    return reinterpret_cast<void*>(aligned);
}

// The interface every engine allocator implements. Allocators hand out raw, uninitialized
// storage; turning that into live objects is make_in's job. They never throw (no exceptions
// on hot paths), so a failed request is reported as nullptr.
struct Allocator {
    Allocator() = default;
    virtual ~Allocator() = default;

    // At least `bytes` of storage aligned to `alignment` (a power of two), or nullptr if the
    // request can't be satisfied.
    [[nodiscard]] virtual void* allocate(std::size_t bytes,
                                         std::size_t alignment = alignof(std::max_align_t)) = 0;

    // Return storage previously obtained from allocate(). Some allocators (the arena) reclaim
    // in bulk and make this a no-op; `bytes` is passed so allocators need not store a header
    // to recover the size.
    virtual void deallocate(void* p, std::size_t bytes) noexcept = 0;

    // An allocator owns a region plus an offset / free-list into it; copying one would alias
    // that state. They are always held and passed by reference, so copy and move are deleted.
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;
    Allocator(Allocator&&) = delete;
    Allocator& operator=(Allocator&&) = delete;
};

// Construct a T in storage from `alloc`, forwarding constructor arguments: the engine's
// stand-in for `new T(...)`. Allocates correctly-aligned space and placement-news the object,
// or returns nullptr if the allocator is out of memory. Pair with destroy_in.
template <class T, class... Args> [[nodiscard]] T* make_in(Allocator& alloc, Args&&... args) {
    void* mem = alloc.allocate(sizeof(T), alignof(T));
    if (mem == nullptr) {
        return nullptr;
    }
    return ::new (mem) T(std::forward<Args>(args)...);
}

// Run T's destructor and return its storage to the allocator: the counterpart to make_in
// (the engine's `delete`). A null pointer is ignored.
template <class T> void destroy_in(Allocator& alloc, T* obj) noexcept {
    if (obj == nullptr) {
        return;
    }
    obj->~T();
    alloc.deallocate(obj, sizeof(T));
}

} // namespace rime::core
