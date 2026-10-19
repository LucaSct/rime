// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Proof for the optional play-control codec. The byte vectors below are the SAME literals asserted
// by tools/rime-protocol/src/play_control.rs, so C++ and Rust cannot drift apart silently. This is
// a codec test only: whether a session id is stale is the runtime's judgement, not the wire's.

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "rime/editorhost/editor_host.hpp"
#include "rime/editorhost/play_control.hpp"

namespace {
using namespace rime::editorhost;

std::vector<std::byte> bytes(std::initializer_list<unsigned> v) {
    std::vector<std::byte> out;
    for (unsigned b : v)
        out.push_back(static_cast<std::byte>(b));
    return out;
}

PlayControlStateMsg make(std::uint8_t sup,
                         std::uint8_t mode,
                         std::uint8_t phase,
                         std::uint8_t status,
                         bool cap,
                         std::uint64_t sid) {
    PlayControlStateMsg m;
    m.supported_modes = sup;
    m.mode = mode;
    m.phase = phase;
    m.status = status;
    m.captured = cap;
    m.session_id = sid;
    return m;
}

bool accepts(const std::vector<std::byte>& b) {
    PlayControlStateMsg s;
    return parse_play_control_state(b, s);
}
} // namespace

TEST_CASE("play_control: message tags and frame classification") {
    CHECK(static_cast<unsigned>(EditorMessage::PlayControlState) == 0x020B);
    CHECK(static_cast<unsigned>(EditorMessage::PlayControl) == 0x021E);
    CHECK(static_cast<unsigned>(EditorMessage::SaveScene) == 0x021D);
    CHECK(message_affects_frame(EditorMessage::PlayControl));
    CHECK_FALSE(message_affects_frame(EditorMessage::PlayControlState));
}

TEST_CASE("play_control: command bytes are pinned and round-trip for every action") {
    PlayControlMsg m{PlayControlAction::SelectPlayer, 0x0102030405060708ull};
    CHECK(serialize_play_control(m) == bytes({1, 8, 7, 6, 5, 4, 3, 2, 1}));
    for (std::uint8_t a = 0; a <= 3; ++a) {
        PlayControlMsg in{static_cast<PlayControlAction>(a), 0xFFFFFFFFFFFFFFFFull};
        PlayControlMsg out;
        CHECK(parse_play_control(serialize_play_control(in), out));
        CHECK(out.action == in.action);
        CHECK(out.session_id == in.session_id); // stale ids are carried, not judged
    }
}

TEST_CASE("play_control: command rejects bad action, truncation, trailing bytes; out unchanged") {
    const PlayControlMsg sentinel{PlayControlAction::Release, 99};
    PlayControlMsg out = sentinel;
    auto good = serialize_play_control({PlayControlAction::Capture, 5});
    for (std::size_t n = 0; n < good.size(); ++n) {
        CHECK_FALSE(parse_play_control({good.data(), n}, out));
        CHECK(out.session_id == 99);
    }
    auto extra = good;
    extra.push_back(std::byte{0});
    CHECK_FALSE(parse_play_control(extra, out));
    auto bad = good;
    bad[0] = std::byte{4};
    CHECK_FALSE(parse_play_control(bad, out));
    CHECK(out.action == PlayControlAction::Release);
    CHECK(out.session_id == 99);
    CHECK(serialize_play_control({static_cast<PlayControlAction>(4), 1}).empty());
}

TEST_CASE("play_control: state bytes are pinned") {
    auto m = make(3, 1, 1, 1, true, 10);
    m.tick_count = 0x0102;
    CHECK(serialize_play_control_state(m) ==
          bytes({3, 1, 1, 1, 1, 10, 0, 0, 0, 0, 0, 0, 0, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    auto r = make(3, 0, 0, 2, false, 0);
    r.reason = "no";
    CHECK(serialize_play_control_state(r) == bytes({3, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,   0,  0,
                                                    0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 'n', 'o'}));
}

TEST_CASE("play_control: Ready, Active, Paused and Refused states round-trip") {
    std::vector<PlayControlStateMsg> states{
        make(1, 0, 0, 0, false, 0), // Ready, inspect-only host
        make(3, 1, 0, 0, false, 0), // Ready, player selected
        make(3, 1, 1, 1, true, 7),  // Active + captured
        make(3, 1, 1, 1, false, 7), // Active, released
        make(3, 1, 2, 1, false, 7), // Paused
        make(1, 0, 1, 1, false, 0xFFFFFFFFFFFFFFFFull),
        make(3, 1, 0, 2, false, 3)}; // Refused (stale session id preserved)
    states.back().reason = "player unavailable \xE2\x9C\x93";
    states.back().tick_count = 123456789012ull;
    for (const auto& in : states) {
        auto b = serialize_play_control_state(in);
        REQUIRE_FALSE(b.empty());
        PlayControlStateMsg out;
        REQUIRE(parse_play_control_state(b, out));
        CHECK(out.supported_modes == in.supported_modes);
        CHECK(out.mode == in.mode);
        CHECK(out.phase == in.phase);
        CHECK(out.status == in.status);
        CHECK(out.captured == in.captured);
        CHECK(out.session_id == in.session_id);
        CHECK(out.tick_count == in.tick_count);
        CHECK(out.reason == in.reason);
    }
}

TEST_CASE("play_control: inconsistent states are refused on both sides of the wire") {
    std::vector<PlayControlStateMsg> bad{
        make(2, 0, 0, 0, false, 0), // Inspect bit missing
        make(0, 0, 0, 0, false, 0),
        make(7, 0, 0, 0, false, 0), // unknown cap bit
        make(1, 1, 0, 0, false, 0), // Player mode without Player support
        make(3, 2, 0, 0, false, 0), // mode
        make(3, 0, 3, 0, false, 0), // phase
        make(3, 0, 0, 3, false, 0), // status
        make(3, 1, 1, 0, false, 0), // Ready outside Edit
        make(3, 1, 1, 2, false, 0), // Refused outside Edit
        make(3, 1, 0, 0, true, 0),  // Ready + captured
        make(3, 1, 0, 2, true, 0),  // Refused + captured
        make(3, 1, 0, 1, false, 4), // Active in Edit
        make(3, 1, 1, 1, false, 0), // Active without session
        make(3, 0, 1, 1, true, 4),  // captured in Inspect
        make(3, 1, 2, 1, true, 4)}; // captured while Paused
    for (const auto& m : bad)
        CHECK(serialize_play_control_state(m).empty());
    // Same states hand-encoded, so the parser is judged independently of the serializer.
    auto b = bytes({3, 1, 1, 1, 1, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    REQUIRE(accepts(b));
    auto t = b;
    t[4] = std::byte{2}; // invalid boolean
    CHECK_FALSE(accepts(t));
    t = b;
    t[4] = std::byte{1};
    t[2] = std::byte{2}; // captured while Paused
    CHECK_FALSE(accepts(t));
    t = b;
    t[0] = std::byte{1}; // Player mode, Inspect-only host
    CHECK_FALSE(accepts(t));
    t = b;
    t[5] = std::byte{0}; // Active captured, session 0
    CHECK_FALSE(accepts(t));
}

TEST_CASE("play_control: truncation, trailing bytes, text cap, UTF-8, atomic failure") {
    auto in = make(3, 0, 0, 2, false, 0);
    in.reason = "refused";
    const auto good = serialize_play_control_state(in);
    PlayControlStateMsg sentinel = make(1, 0, 0, 0, false, 0);
    sentinel.reason = "keep";
    PlayControlStateMsg out = sentinel;
    for (std::size_t n = 0; n < good.size(); ++n) {
        CHECK_FALSE(parse_play_control_state({good.data(), n}, out));
        CHECK(out.reason == "keep");
        CHECK(out.session_id == 0);
    }
    auto extra = good;
    extra.push_back(std::byte{0});
    CHECK_FALSE(parse_play_control_state(extra, out));
    CHECK(out.reason == "keep");

    in.reason.assign(PlayControlStateMsg::max_reason_bytes, 'x');
    auto at_cap = serialize_play_control_state(in);
    CHECK(at_cap.size() == 25 + 1024);
    CHECK(parse_play_control_state(at_cap, out));
    in.reason.assign(PlayControlStateMsg::max_reason_bytes + 1, 'x');
    CHECK(serialize_play_control_state(in).empty());
    auto over = at_cap; // forge a 1025-byte length with matching body
    over[21] = std::byte{0x01};
    over[22] = std::byte{0x04};
    over.push_back(std::byte{'x'});
    CHECK_FALSE(accepts(over));

    for (const char* s : {"\xC3", "\xFF", "\xC0\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80"}) {
        in.reason = s;
        CHECK(serialize_play_control_state(in).empty());
    }
    out = sentinel;
    in.reason = "ab";
    auto text = serialize_play_control_state(in);
    text[25] = std::byte{0xFF};
    CHECK_FALSE(parse_play_control_state(text, out));
    CHECK(out.reason == "keep");
}
