// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// FNV-1a, the engine's one small non-cryptographic hash. It is five lines because the whole idea
// is: start from a fixed 64-bit "offset basis", then for every input byte XOR it in and multiply by
// a fixed 64-bit prime. That XOR-then-multiply order (1a) mixes each byte through the whole 64-bit
// state before the next arrives, so a one-byte change avalanches across the result — good enough
// for the jobs we need it for: content-hashed asset identity, the cook cache key, reflection schema
// fingerprints (ADR-0024). It is NOT collision-resistant against an adversary; where that matters
// (never, in v1 — asset files are our own build products) the container header's version field
// admits a stronger hash later. Chosen over xxHash/BLAKE3 precisely because it needs no dependency
// and reads clearly (VISION: teach from the code). Derivation: docs/design/reflection.md links
// here.
namespace rime::core {

// The canonical 64-bit FNV-1a constants (Fowler–Noll–Vo). These specific numbers are the published
// ones; do not "tidy" them.
inline constexpr std::uint64_t kFnv1a64OffsetBasis = 14695981039346656037ull;
inline constexpr std::uint64_t kFnv1a64Prime = 1099511628211ull;

// Hash a run of bytes. `seed` defaults to the offset basis; passing a previous result as the seed
// *continues* the hash, so fnv1a_64(b, fnv1a_64(a)) == fnv1a_64(a followed by b). That chaining is
// what lets callers fold several fields into one fingerprint without concatenating buffers.
[[nodiscard]] constexpr std::uint64_t fnv1a_64(std::span<const std::byte> bytes,
                                               std::uint64_t seed = kFnv1a64OffsetBasis) noexcept {
    std::uint64_t hash = seed;
    for (const std::byte b : bytes) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b));
        hash *= kFnv1a64Prime;
    }
    return hash;
}

// Same, over the bytes of a string (its characters, not any trailing NUL). A separate overload
// rather than a reinterpret_cast so it stays usable in a constexpr context (e.g. hashing a string
// literal at compile time).
[[nodiscard]] constexpr std::uint64_t fnv1a_64(std::string_view text,
                                               std::uint64_t seed = kFnv1a64OffsetBasis) noexcept {
    std::uint64_t hash = seed;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= kFnv1a64Prime;
    }
    return hash;
}

} // namespace rime::core
