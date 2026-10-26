// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "rime/replication/prediction.hpp"

// ADR-0109 §2's versioned payload is a leaf codec: the crate's original 52 bytes remain opaque
// to the wire format, while their floats and quaternion are checked before a restore can use them.
namespace rime::replication {

inline constexpr std::uint8_t kContentionContextVersion = 2;
inline constexpr std::size_t kContentionCrateBytes = 52;
inline constexpr std::size_t kContentionHeaderBytes = 12;
inline constexpr std::size_t kContentionProxyBytes = 48;
inline constexpr std::size_t kContentionMaxProxies = 3;
inline constexpr std::size_t kContentionMaxBytes =
    kContentionHeaderBytes + kContentionCrateBytes + kContentionMaxProxies * kContentionProxyBytes;
static_assert(kContentionMaxBytes == 208);
// Squared quaternion length must differ from one by at most 0.01. This admits normal float
// roundoff, but refuses an orientation whose scale would change contact geometry.
inline constexpr float kContentionQuaternionNormTolerance = 0.01f;

struct ContentionProxy {
    std::uint16_t seat = 0;
    std::uint16_t seat_generation = 0;
    PredictedEntityId net_id{};
    std::array<float, 3> position{};
    std::array<float, 3> velocity{};
    std::array<float, 2> axes{};
    std::uint32_t held = 0;
    [[nodiscard]] bool operator==(const ContentionProxy&) const = default;
};

struct ContentionContext {
    std::uint16_t owner_seat = 0;
    std::uint16_t owner_generation = 0;
    std::uint32_t context_revision = 0;
    std::array<std::byte, kContentionCrateBytes> crate{};
    std::vector<ContentionProxy> proxies;
    [[nodiscard]] bool operator==(const ContentionContext&) const = default;
};

enum class ContentionRefusal : std::uint8_t {
    None,
    BadLength,
    UnknownVersion,
    ReservedNonZero,
    TooManyProxies,
    NonFinite,
    NonUnitQuaternion,
    DuplicateProxyIdentity,
    ProxyIsOwner,
    SeatsNotAscending,
    Count, // not a refusal: bounds caller-side reason counters
};

inline constexpr std::size_t kContentionRefusalCount =
    static_cast<std::size_t>(ContentionRefusal::Count);

// Both operations leave their output untouched on refusal. The caller counts the named reason.
[[nodiscard]] ContentionRefusal validate_contention_crate(std::span<const std::byte> crate);
[[nodiscard]] ContentionRefusal encode_contention_context(const ContentionContext& value,
                                                          std::span<std::byte> out,
                                                          std::size_t& written);
[[nodiscard]] ContentionRefusal decode_contention_context(std::span<const std::byte> bytes,
                                                          ContentionContext& out);

} // namespace rime::replication
