// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the m18 Track H host input path (ADR-0054): a `stream::InputEvent` off the wire
// becomes `platform::Event`s, and the game's `platform::Input` sees the key.
//
// WHY THIS IS THE RIGHT SEAM. The real consumer is the editor host's viewport loop, which needs a
// GPU, a bound socket and a live client — unprovable in CI on every OS, which is precisely why the
// decode-and-translate step was pulled out of it into `dispatch_input_message`. What is exercised
// below is the SAME code that loop calls, on the same side of the wire, ending at the same
// `platform::Input` a game reads: nothing is re-implemented here to make the test work.
//
// What it does NOT prove: that the drain loop calls this at all (that is read, plus the build),
// that a browser produces these bytes (the page's own dev/keymap_test.mjs is the other half), and
// that any of it survives a real WebRTC hop.
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rime/app/stream_input.hpp"
#include "rime/platform/event.hpp"
#include "rime/platform/input.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/platform/mouse.hpp"
#include "rime/stream/protocol.hpp"

using rime::app::dispatch_input_message;
using rime::app::InputDispatch;
using rime::app::StreamInputTranslator;
using rime::platform::Event;
using rime::platform::EventType;
using rime::platform::Input;
using rime::platform::Key;
using rime::platform::KeyMods;
using rime::platform::MouseButton;
using rime::stream::InputEvent;
using rime::stream::MessageType;

namespace {

// Build the bytes a client would actually send — encode(), not a hand-filled struct — so the test
// crosses the same decode the socket path does and a payload-layout change cannot slip past it.
std::vector<std::byte> wire(const InputEvent& e) {
    std::vector<std::byte> out;
    e.encode(out);
    return out;
}

InputEvent key(InputEvent::Kind kind, std::uint32_t usage, std::uint32_t mods = 0) {
    InputEvent e;
    e.kind = kind;
    e.code = usage;
    e.mods = mods;
    return e;
}

// One frame of the loop the host runs: roll the edges, then fold in what the drain produced.
void feed(Input& in, const std::vector<Event>& events) {
    in.new_frame();
    for (const Event& e : events) {
        in.process(e);
    }
}

} // namespace

TEST_CASE("stream input: a KeyDown for HID 0x04 makes the game's input see Key::A down") {
    StreamInputTranslator translator;
    std::vector<Event> events;
    Input in;

    // 0x04 is "Keyboard a and A" — what the browser page sends for `KeyboardEvent.code == "KeyA"`.
    const auto down = wire(key(InputEvent::Kind::KeyDown, 0x04));
    CHECK(dispatch_input_message(MessageType::Input, down, translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::KeyDown);
    CHECK(events[0].key.key == Key::A);
    CHECK_FALSE(events[0].key.repeat);

    feed(in, events);
    CHECK(in.key_down(Key::A));
    CHECK(in.key_pressed(Key::A)); // the up->down edge, this frame

    // A KeyUp releases it, and the release is an EDGE the frame after, not just an absence.
    events.clear();
    const auto up = wire(key(InputEvent::Kind::KeyUp, 0x04));
    CHECK(dispatch_input_message(MessageType::Input, up, translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::KeyUp);
    CHECK(events[0].key.key == Key::A);

    feed(in, events);
    CHECK_FALSE(in.key_down(Key::A));
    CHECK(in.key_released(Key::A));
}

TEST_CASE("stream input: modifiers, auto-repeat and an unmapped usage") {
    StreamInputTranslator translator;
    std::vector<Event> events;

    // mods is the platform::KeyMods bitmask: Shift 1, Ctrl 2, Alt 4, Super 8 (ADR-0054).
    const auto shift_ctrl = wire(key(InputEvent::Kind::KeyDown, 0x1A, 0b0011));
    CHECK(dispatch_input_message(MessageType::Input, shift_ctrl, translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].key.key == Key::W);
    CHECK(events[0].key.mods == (KeyMods::Shift | KeyMods::Ctrl));
    CHECK_FALSE(events[0].key.repeat);

    // Bits above the low four are reserved and IGNORED — a newer client that sets one is not
    // refused and does not get a garbage modifier set.
    events.clear();
    const auto reserved = wire(key(InputEvent::Kind::KeyDown, 0x16, 0xFFFFFFF0u));
    CHECK(dispatch_input_message(MessageType::Input, reserved, translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].key.mods == KeyMods::None);

    // A second KeyDown for a key already held IS an auto-repeat — the wire has no repeat bit, so
    // this is derived from the held set.
    events.clear();
    const auto again = wire(key(InputEvent::Kind::KeyDown, 0x1A));
    CHECK(dispatch_input_message(MessageType::Input, again, translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].key.repeat);

    // An unmapped usage produces NO event and is counted. Not an error: the peer is a browser on
    // the public internet and a key this build does not know must cost one counter.
    events.clear();
    CHECK(translator.unknown_usages() == 0);
    const auto power = wire(key(InputEvent::Kind::KeyDown, 0x66)); // "Keyboard Power"
    CHECK(dispatch_input_message(MessageType::Input, power, translator, events) ==
          InputDispatch::Applied);
    CHECK(events.empty());
    CHECK(translator.unknown_usages() == 1);
}

TEST_CASE("stream input: pointer buttons are DOM order and pointer moves carry a delta") {
    StreamInputTranslator translator;
    std::vector<Event> events;
    Input in;

    // DOM MouseEvent.button: 0 left, 1 MIDDLE, 2 right. platform::MouseButton is Left, Right,
    // Middle — the two differ in the middle, which is the whole reason this case exists.
    InputEvent middle;
    middle.kind = InputEvent::Kind::PointerDown;
    middle.code = 1;
    const auto middle_bytes = wire(middle);
    CHECK(dispatch_input_message(MessageType::Input, middle_bytes, translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::MouseButton);
    CHECK(events[0].button.button == MouseButton::Middle);
    CHECK(events[0].button.down);
    feed(in, events);
    CHECK(in.mouse_down(MouseButton::Middle));

    // An index neither side defines is counted and dropped, like an unmapped key usage.
    events.clear();
    InputEvent nonsense;
    nonsense.kind = InputEvent::Kind::PointerDown;
    nonsense.code = 99;
    const auto nonsense_bytes = wire(nonsense);
    CHECK(dispatch_input_message(MessageType::Input, nonsense_bytes, translator, events) ==
          InputDispatch::Applied);
    CHECK(events.empty());
    CHECK(translator.unknown_buttons() == 1);

    // Pointer moves: the wire carries a POSITION in stream-frame pixels and the delta is
    // reconstructed as (this - previous). The FIRST move of a session has no predecessor and must
    // report a zero delta rather than a delta from the origin — a camera would otherwise snap by
    // the whole screen on the player's first twitch.
    events.clear();
    InputEvent move;
    move.kind = InputEvent::Kind::PointerMove;
    move.x = 480;
    move.y = 270;
    CHECK(dispatch_input_message(MessageType::Input, wire(move), translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::MouseMove);
    CHECK(events[0].mouse_move.x == doctest::Approx(480.0f));
    CHECK(events[0].mouse_move.y == doctest::Approx(270.0f));
    CHECK(events[0].mouse_move.dx == doctest::Approx(0.0f));
    CHECK(events[0].mouse_move.dy == doctest::Approx(0.0f));

    move.x = 500;
    move.y = 260;
    CHECK(dispatch_input_message(MessageType::Input, wire(move), translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 2);
    CHECK(events[1].mouse_move.dx == doctest::Approx(20.0f));
    CHECK(events[1].mouse_move.dy == doctest::Approx(-10.0f));

    feed(in, events);
    CHECK(in.mouse_x() == doctest::Approx(500.0f));
    CHECK(in.mouse_dx() == doctest::Approx(20.0f)); // 0 + 20, accumulated over the frame

    // Scroll deltas pass straight through as a wheel event.
    events.clear();
    InputEvent scroll;
    scroll.kind = InputEvent::Kind::PointerScroll;
    scroll.scroll_x = 1.5f;
    scroll.scroll_y = -3.0f;
    CHECK(dispatch_input_message(MessageType::Input, wire(scroll), translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == EventType::MouseWheel);
    CHECK(events[0].wheel.dx == doctest::Approx(1.5f));
    CHECK(events[0].wheel.dy == doctest::Approx(-3.0f));
}

TEST_CASE("stream input: a malformed payload is counted and the session carries on") {
    StreamInputTranslator translator;
    std::vector<Event> events;

    // 37 bytes is the fixed payload; 12 is a truncated one.
    const std::vector<std::byte> truncated(12, std::byte{0});
    CHECK(dispatch_input_message(MessageType::Input, truncated, translator, events) ==
          InputDispatch::Malformed);
    CHECK(events.empty());

    // An in-range payload with an out-of-range kind byte is the other malformation `decode`
    // catches — kind 9 is not an InputEvent::Kind.
    std::vector<std::byte> bad_kind = wire(key(InputEvent::Kind::KeyDown, 0x04));
    bad_kind[0] = std::byte{9};
    CHECK(dispatch_input_message(MessageType::Input, bad_kind, translator, events) ==
          InputDispatch::Malformed);
    CHECK(events.empty());

    // The very next well-formed event still works: a bad message costs a counter, not the stream.
    const auto good = wire(key(InputEvent::Kind::KeyDown, 0x04));
    CHECK(dispatch_input_message(MessageType::Input, good, translator, events) ==
          InputDispatch::Applied);
    REQUIRE(events.size() == 1);
    CHECK(events[0].key.key == Key::A);
}

TEST_CASE("stream input: a disconnect releases every held key and button") {
    StreamInputTranslator translator;
    std::vector<Event> events;
    Input in;

    // Hold W and D and the left button — the shape of a player mid-strafe when the relay drops.
    for (const std::uint32_t usage : {0x1Au, 0x07u}) { // W, D
        CHECK(dispatch_input_message(
                  MessageType::Input, wire(key(InputEvent::Kind::KeyDown, usage)), translator,
                  events) == InputDispatch::Applied);
    }
    InputEvent click;
    click.kind = InputEvent::Kind::PointerDown;
    click.code = 0; // DOM left
    CHECK(dispatch_input_message(MessageType::Input, wire(click), translator, events) ==
          InputDispatch::Applied);

    feed(in, events);
    REQUIRE(in.key_down(Key::W));
    REQUIRE(in.key_down(Key::D));
    REQUIRE(in.mouse_down(MouseButton::Left));
    CHECK(translator.held_count() == 3);

    // Bye / dead socket.
    events.clear();
    translator.release_all(events);
    CHECK(events.size() == 3);
    CHECK(translator.held_count() == 0);

    feed(in, events);
    CHECK_FALSE(in.key_down(Key::W));
    CHECK_FALSE(in.key_down(Key::D));
    CHECK_FALSE(in.mouse_down(MouseButton::Left));
    // Released as a proper EDGE, not merely absent: gameplay that fires on `key_released` (a jump
    // on key-up, a charged shot) must still see the release rather than the key evaporating.
    CHECK(in.key_released(Key::W));
    CHECK(in.mouse_released(MouseButton::Left));

    // Idempotent: a second release-all emits nothing, so a host that calls it on both Bye and
    // socket-close does not inject a second set of spurious ups.
    events.clear();
    translator.release_all(events);
    CHECK(events.empty());
}

TEST_CASE("stream input: a non-Input message is left to the caller") {
    StreamInputTranslator translator;
    std::vector<Event> events;
    const auto payload = wire(key(InputEvent::Kind::KeyDown, 0x04));

    // The editor band (0x0200..0x02FF) and the other stream-band codes must fall through
    // UNTOUCHED — this function sits above the EditorMessage cast in the host's drain, and a
    // greedy one would swallow every edit the client sends.
    CHECK(dispatch_input_message(MessageType::Capabilities, payload, translator, events) ==
          InputDispatch::NotInput);
    CHECK(dispatch_input_message(MessageType::KeyframeRequest, payload, translator, events) ==
          InputDispatch::NotInput);
    CHECK(dispatch_input_message(static_cast<MessageType>(0x0210), payload, translator, events) ==
          InputDispatch::NotInput);
    CHECK(events.empty());
}
