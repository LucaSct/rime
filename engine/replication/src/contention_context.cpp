// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/replication/contention_context.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <utility>

namespace rime::replication {
namespace {

struct Writer {
    std::span<std::byte> bytes;
    std::size_t at = 0;

    void u8(std::uint8_t v) { bytes[at++] = static_cast<std::byte>(v); }

    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }

    void u32(std::uint32_t v) {
        for (unsigned i = 0; i < 4; ++i)
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
};

struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at = 0;

    std::uint8_t u8() { return std::to_integer<std::uint8_t>(bytes[at++]); }

    std::uint16_t u16() { return static_cast<std::uint16_t>(u8() | (std::uint16_t{u8()} << 8)); }

    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (unsigned i = 0; i < 4; ++i)
            v |= std::uint32_t{u8()} << (8 * i);
        return v;
    }

    float f32() { return std::bit_cast<float>(u32()); }
};

ContentionRefusal validate(const ContentionContext& value) {
    if (value.proxies.size() > kContentionMaxProxies)
        return ContentionRefusal::TooManyProxies;
    if (const auto reason = validate_contention_crate(value.crate);
        reason != ContentionRefusal::None)
        return reason;
    for (std::size_t i = 0; i < value.proxies.size(); ++i) {
        const auto& proxy = value.proxies[i];
        if (proxy.seat == value.owner_seat && proxy.seat_generation == value.owner_generation)
            return ContentionRefusal::ProxyIsOwner;
        for (std::size_t j = 0; j < i; ++j) {
            if (value.proxies[j].net_id == proxy.net_id ||
                (value.proxies[j].seat == proxy.seat &&
                 value.proxies[j].seat_generation == proxy.seat_generation))
                return ContentionRefusal::DuplicateProxyIdentity;
        }
        if (i > 0 && value.proxies[i - 1].seat >= proxy.seat)
            return ContentionRefusal::SeatsNotAscending;
        for (const float v : proxy.position)
            if (!std::isfinite(v))
                return ContentionRefusal::NonFinite;
        for (const float v : proxy.velocity)
            if (!std::isfinite(v))
                return ContentionRefusal::NonFinite;
        for (const float v : proxy.axes)
            if (!std::isfinite(v))
                return ContentionRefusal::NonFinite;
    }
    return ContentionRefusal::None;
}

} // namespace

ContentionRefusal validate_contention_crate(std::span<const std::byte> crate) {
    if (crate.size() != kContentionCrateBytes)
        return ContentionRefusal::BadLength;
    Reader reader{crate};
    float norm = 0.0f;
    for (std::size_t i = 0; i < kContentionCrateBytes / 4; ++i) {
        const float v = reader.f32();
        if (!std::isfinite(v))
            return ContentionRefusal::NonFinite;
        if (i >= 3 && i <= 6)
            norm += v * v;
    }
    if (!std::isfinite(norm) || std::fabs(norm - 1.0f) > kContentionQuaternionNormTolerance)
        return ContentionRefusal::NonUnitQuaternion;
    return ContentionRefusal::None;
}

ContentionRefusal encode_contention_context(const ContentionContext& value,
                                            std::span<std::byte> out,
                                            std::size_t& written) {
    // Validate the versioned payload before writing its first byte. This keeps an encoder
    // refusal from leaving a plausible but incomplete crate state in the caller's buffer.
    if (const auto reason = validate(value); reason != ContentionRefusal::None)
        return reason;
    const auto size = kContentionHeaderBytes + kContentionCrateBytes +
                      value.proxies.size() * kContentionProxyBytes;
    if (out.size() < size)
        return ContentionRefusal::BadLength;
    Writer writer{out};
    writer.u8(kContentionContextVersion);
    writer.u8(static_cast<std::uint8_t>(value.proxies.size()));
    writer.u16(0);
    writer.u16(value.owner_seat);
    writer.u16(value.owner_generation);
    writer.u32(value.context_revision);
    std::copy(value.crate.begin(),
              value.crate.end(),
              out.begin() + static_cast<std::ptrdiff_t>(writer.at));
    writer.at += value.crate.size();
    for (const auto& proxy : value.proxies) {
        writer.u16(proxy.seat);
        writer.u16(proxy.seat_generation);
        writer.u32(proxy.net_id.index);
        writer.u32(proxy.net_id.generation);
        for (float v : proxy.position)
            writer.f32(v);
        for (float v : proxy.velocity)
            writer.f32(v);
        for (float v : proxy.axes)
            writer.f32(v);
        writer.u32(proxy.held);
    }
    written = writer.at;
    return ContentionRefusal::None;
}

ContentionRefusal decode_contention_context(std::span<const std::byte> bytes,
                                            ContentionContext& out) {
    if (bytes.size() < kContentionHeaderBytes + kContentionCrateBytes)
        return ContentionRefusal::BadLength;
    Reader reader{bytes};
    if (reader.u8() != kContentionContextVersion)
        return ContentionRefusal::UnknownVersion;
    const auto count = reader.u8();
    if (count > kContentionMaxProxies)
        return ContentionRefusal::TooManyProxies;
    if (reader.u16() != 0)
        return ContentionRefusal::ReservedNonZero;
    if (bytes.size() != kContentionHeaderBytes + kContentionCrateBytes +
                            static_cast<std::size_t>(count) * kContentionProxyBytes)
        return ContentionRefusal::BadLength;
    // Decode into a local value: even a bad LAST proxy record leaves the caller's context whole.
    ContentionContext decoded{};
    decoded.owner_seat = reader.u16();
    decoded.owner_generation = reader.u16();
    decoded.context_revision = reader.u32();
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(reader.at),
                kContentionCrateBytes,
                decoded.crate.begin());
    reader.at += kContentionCrateBytes;
    for (std::size_t i = 0; i < count; ++i) {
        ContentionProxy proxy{};
        proxy.seat = reader.u16();
        proxy.seat_generation = reader.u16();
        proxy.net_id = {reader.u32(), reader.u32()};
        for (float& v : proxy.position)
            v = reader.f32();
        for (float& v : proxy.velocity)
            v = reader.f32();
        for (float& v : proxy.axes)
            v = reader.f32();
        proxy.held = reader.u32();
        decoded.proxies.push_back(proxy);
    }
    if (const auto reason = validate(decoded); reason != ContentionRefusal::None)
        return reason;
    out = std::move(decoded);
    return ContentionRefusal::None;
}

} // namespace rime::replication
