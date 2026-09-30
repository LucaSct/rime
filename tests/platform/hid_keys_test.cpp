// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the m18 Track H key numbering (ADR-0054): the HID Keyboard/Keypad page (0x07) <->
// `platform::Key` table.
//
// The table is the only sanctioned route between the two numberings, and both directions of it
// are used in anger — the engine decodes a browser's usage into a `Key`, and a native client
// (samples/04-remote-view) encodes a `Key` into a usage. A table that is not a bijection over its
// mapped set means those two disagree for some key, which presents as "the remote client's X does
// something else", the hardest class of input bug to see.
#include <doctest/doctest.h>

#include <cstdint>
#include <set>

#include "rime/platform/hid_keys.hpp"
#include "rime/platform/keyboard.hpp"

using namespace rime::platform;

TEST_CASE("hid_keys: every named Key round-trips Key -> usage -> Key") {
    // Walks the ENUM, not a hand-written list, so a key added to keyboard.hpp is covered here the
    // day it is added rather than the day someone remembers to extend a fixture.
    std::set<std::uint32_t> seen;
    for (std::uint16_t i = 1; i < static_cast<std::uint16_t>(Key::Count); ++i) {
        const auto key = static_cast<Key>(i);
        const std::uint32_t usage = key_to_hid_usage(key);
        CAPTURE(i);
        CAPTURE(usage);
        // A named key with no usage is a key no remote client can ever send. The library carries a
        // static_assert for this too; the check here is what names the offender at run time.
        REQUIRE(usage != 0);
        // Two keys sharing a usage would make one of them unreachable, silently.
        CHECK(seen.insert(usage).second);
        CHECK(hid_usage_to_key(usage) == key);
    }
}

TEST_CASE("hid_keys: every mapped usage round-trips usage -> Key -> usage") {
    // The other direction over the whole page, including the holes: a usage that maps to a Key
    // must map back to ITSELF, or the reverse table has an entry the forward table disagrees with.
    std::uint32_t mapped = 0;
    for (std::uint32_t usage = 0; usage <= 0xFF; ++usage) {
        const Key key = hid_usage_to_key(usage);
        if (key == Key::Unknown) {
            continue;
        }
        ++mapped;
        CAPTURE(usage);
        CHECK(key_to_hid_usage(key) == usage);
    }
    // 105 = every named Key but Unknown. Asserting the COUNT is what makes the two loops above a
    // bijection claim rather than two one-sided ones.
    CHECK(mapped == static_cast<std::uint32_t>(Key::Count) - 1);
}

TEST_CASE("hid_keys: the standard's anchor values") {
    // The five values the standard's own examples quote, spelled out so a reader can check this
    // table against HID Usage Tables §10 without running anything. A table shifted by one would
    // still be a perfect bijection and would fail here.
    CHECK(hid_usage_to_key(0x04) == Key::A);
    CHECK(hid_usage_to_key(0x28) == Key::Enter);
    CHECK(hid_usage_to_key(0x2C) == Key::Space);
    CHECK(hid_usage_to_key(0x4F) == Key::Right);
    CHECK(hid_usage_to_key(0xE0) == Key::LeftCtrl);

    // The number row runs 1..9 THEN 0 — the one place the page is not a straight offset, and the
    // single most likely transcription error on either side of the wire.
    CHECK(hid_usage_to_key(0x1E) == Key::Num1);
    CHECK(hid_usage_to_key(0x27) == Key::Num0);
    CHECK(hid_usage_to_key(0x59) == Key::KP1);
    CHECK(hid_usage_to_key(0x62) == Key::KP0);
}

TEST_CASE("hid_keys: an unmapped usage is Unknown, never a crash") {
    // The wire is attacker-controlled. Every one of these is a legal u32 a peer may send.
    CHECK(hid_usage_to_key(0x00) == Key::Unknown);        // "Reserved (no event indicated)"
    CHECK(hid_usage_to_key(0x01) == Key::Unknown);        // ErrorRollOver — a real usage, not a key
    CHECK(hid_usage_to_key(0x32) == Key::Unknown);        // the non-US "# and ~"; see hid_keys.cpp
    CHECK(hid_usage_to_key(0x64) == Key::Unknown);        // the non-US "\ and |"
    CHECK(hid_usage_to_key(0x66) == Key::Unknown);        // Power
    CHECK(hid_usage_to_key(0xE8) == Key::Unknown);        // one past the modifier block
    CHECK(hid_usage_to_key(0xFFFF) == Key::Unknown);      // off the page entirely
    CHECK(hid_usage_to_key(0xFFFFFFFFu) == Key::Unknown); // the largest value the field can hold
}

TEST_CASE("hid_keys: Unknown and Count have no usage") {
    CHECK(key_to_hid_usage(Key::Unknown) == 0);
    // Not a key, but it is a value of the enum type and therefore something a caller can pass.
    CHECK(key_to_hid_usage(Key::Count) == 0);
}
