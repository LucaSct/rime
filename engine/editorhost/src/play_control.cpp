// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/editorhost/play_control.hpp"

#include <utility>

#include "rime/core/byte_cursor.hpp"

namespace rime::editorhost {
namespace {
// Strict UTF-8 (the same set Rust's std::str::from_utf8 accepts): no overlongs, no surrogates,
// nothing above U+10FFFF. The older editor-channel strings are length-prefixed bytes with no
// check on the C++ side; this one is validated because the reason is shown to a person.
bool valid_utf8(const std::string& s) {
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = p[i];
        std::size_t len = 0;
        unsigned lo = 0x80, hi = 0xBF;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            if (c == 0xE0)
                lo = 0xA0;
            if (c == 0xED)
                hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            if (c == 0xF0)
                lo = 0x90;
            if (c == 0xF4)
                hi = 0x8F;
        } else {
            return false;
        }
        if (n - i < len)
            return false;
        if (p[i + 1] < lo || p[i + 1] > hi)
            return false;
        for (std::size_t k = 2; k < len; ++k)
            if (p[i + k] < 0x80 || p[i + k] > 0xBF)
                return false;
        i += len;
    }
    return true;
}

bool valid(const PlayControlStateMsg& m) {
    if (m.supported_modes != 1 && m.supported_modes != 3)
        return false;
    if (m.mode > 1 || (m.mode == 1 && (m.supported_modes & 2) == 0))
        return false;
    if (m.phase > 2 || m.status > 2)
        return false;
    if (m.status != 1 && (m.phase != 0 || m.captured))
        return false; // Ready / Refused are Edit-phase, uncaptured
    if (m.status == 1 && (m.phase == 0 || m.session_id == 0))
        return false; // Active needs a running session
    if (m.captured && (m.mode != 1 || m.phase != 1 || m.status != 1 || m.session_id == 0))
        return false;
    return m.reason.size() <= PlayControlStateMsg::max_reason_bytes && valid_utf8(m.reason);
}
} // namespace

std::vector<std::byte> serialize_play_control(const PlayControlMsg& m) {
    if (static_cast<std::uint8_t>(m.action) > 3)
        return {};
    std::vector<std::byte> out;
    out.reserve(9);
    core::ByteWriter w(out);
    w.u8(static_cast<std::uint8_t>(m.action));
    w.u64(m.session_id);
    return out;
}

bool parse_play_control(std::span<const std::byte> payload, PlayControlMsg& out) {
    core::ByteReader r(payload);
    std::uint8_t action = 0;
    PlayControlMsg m;
    if (!r.u8(action) || action > 3 || !r.u64(m.session_id) || r.remaining() != 0)
        return false;
    m.action = static_cast<PlayControlAction>(action);
    out = m;
    return true;
}

std::vector<std::byte> serialize_play_control_state(const PlayControlStateMsg& m) {
    if (!valid(m))
        return {};
    std::vector<std::byte> out;
    out.reserve(25 + m.reason.size());
    core::ByteWriter w(out);
    w.u8(m.supported_modes);
    w.u8(m.mode);
    w.u8(m.phase);
    w.u8(m.status);
    w.u8(m.captured ? 1 : 0);
    w.u64(m.session_id);
    w.u64(m.tick_count);
    w.u32(static_cast<std::uint32_t>(m.reason.size()));
    for (const char c : m.reason)
        w.u8(static_cast<std::uint8_t>(c));
    return out;
}

bool parse_play_control_state(std::span<const std::byte> payload, PlayControlStateMsg& out) {
    core::ByteReader r(payload);
    PlayControlStateMsg m;
    std::uint8_t captured = 0, b = 0;
    std::uint32_t len = 0;
    if (!r.u8(m.supported_modes) || !r.u8(m.mode) || !r.u8(m.phase) || !r.u8(m.status) ||
        !r.u8(captured) || captured > 1 || !r.u64(m.session_id) || !r.u64(m.tick_count) ||
        !r.u32(len) || len > PlayControlStateMsg::max_reason_bytes || r.remaining() != len)
        return false;
    m.captured = captured == 1;
    m.reason.reserve(len);
    for (std::uint32_t i = 0; i < len; ++i) {
        if (!r.u8(b))
            return false;
        m.reason.push_back(static_cast<char>(b));
    }
    if (!valid(m))
        return false;
    out = std::move(m);
    return true;
}
} // namespace rime::editorhost
