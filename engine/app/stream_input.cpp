// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/app/stream_input.hpp"

#include "rime/platform/hid_keys.hpp"

namespace rime::app {
namespace {

using platform::Event;
using platform::EventType;
using platform::Key;
using platform::KeyMods;
using platform::MouseButton;

Event make_key_event(EventType type, Key key, KeyMods mods, bool repeat) {
    Event e{};
    e.type = type;
    e.key = Event::KeyData{key, mods, repeat};
    return e;
}

Event make_button_event(MouseButton button, KeyMods mods, bool down) {
    Event e{};
    e.type = EventType::MouseButton;
    e.button = Event::MouseButtonData{button, mods, down};
    return e;
}

} // namespace

KeyMods StreamInputTranslator::mods_from_wire(std::uint32_t mods) noexcept {
    // Mask to the four bits `KeyMods` defines. `KeyMods` is a uint8 enum, so a blind cast of a u32
    // would be a narrowing conversion with an unspecified result for bit 8 and above — masking
    // first makes "reserved bits are ignored" a property of the code rather than of the compiler.
    constexpr std::uint32_t kKnown = 0x0Fu;
    return static_cast<KeyMods>(static_cast<std::uint8_t>(mods & kKnown));
}

bool StreamInputTranslator::button_from_wire(std::uint32_t code, MouseButton& out) noexcept {
    switch (code) {
        case 0:
            out = MouseButton::Left;
            return true;
        case 1:
            out = MouseButton::Middle; // DOM's 1 is the WHEEL button, not the right one
            return true;
        case 2:
            out = MouseButton::Right;
            return true;
        case 3:
            out = MouseButton::X1; // "back"
            return true;
        case 4:
            out = MouseButton::X2; // "forward"
            return true;
        default:
            return false;
    }
}

bool StreamInputTranslator::key_held(Key key) const noexcept {
    const auto index = static_cast<std::size_t>(key);
    return index < keys_.size() && keys_[index];
}

bool StreamInputTranslator::button_held(MouseButton button) const noexcept {
    const auto index = static_cast<std::size_t>(button);
    return index < buttons_.size() && buttons_[index];
}

std::size_t StreamInputTranslator::held_count() const noexcept {
    std::size_t n = 0;
    for (const bool held : keys_) {
        n += held ? 1u : 0u;
    }
    for (const bool held : buttons_) {
        n += held ? 1u : 0u;
    }
    return n;
}

void StreamInputTranslator::translate(const stream::InputEvent& event,
                                      std::vector<platform::Event>& out) {
    using Kind = stream::InputEvent::Kind;
    const KeyMods mods = mods_from_wire(event.mods);

    // Latch the echo BEFORE the switch, so an event whose usage this build does not map still
    // counts as "input the server has seen". Echoing only mapped keys would make the measured
    // latency depend on which key was pressed, which is the kind of quiet bias a latency ledger
    // exists to not have.
    if (event.seq != 0) {
        last_seq_ = event.seq;
        last_client_us_ = event.client_us;
    }

    switch (event.kind) {
        case Kind::KeyDown:
        case Kind::KeyUp: {
            const Key key = platform::hid_usage_to_key(event.code);
            if (key == Key::Unknown) {
                ++unknown_usages_; // a usage this build does not map; counted, never fatal
                return;
            }
            const auto index = static_cast<std::size_t>(key);
            const bool down = event.kind == Kind::KeyDown;
            // An auto-repeat is a KeyDown for a key we already hold. Deriving it here rather than
            // carrying a wire bit keeps the browser's `KeyboardEvent.repeat` and a native sender's
            // OS repeat flag from having to agree about anything.
            const bool repeat = down && keys_[index];
            keys_[index] = down;
            out.push_back(
                make_key_event(down ? EventType::KeyDown : EventType::KeyUp, key, mods, repeat));
            return;
        }
        case Kind::PointerDown:
        case Kind::PointerUp: {
            MouseButton button{};
            if (!button_from_wire(event.code, button)) {
                ++unknown_buttons_;
                return;
            }
            const bool down = event.kind == Kind::PointerDown;
            buttons_[static_cast<std::size_t>(button)] = down;
            out.push_back(make_button_event(button, mods, down));
            return;
        }
        case Kind::PointerMove: {
            const auto x = static_cast<float>(event.x);
            const auto y = static_cast<float>(event.y);
            Event e{};
            e.type = EventType::MouseMove;
            // The wire carries the position only (stream-frame pixels, see protocol.hpp), so the
            // delta the camera reads is reconstructed here. The FIRST move of a session reports a
            // zero delta rather than a delta from the origin — see `have_pointer_`.
            e.mouse_move = Event::MouseMove{
                x, y, have_pointer_ ? x - last_x_ : 0.0f, have_pointer_ ? y - last_y_ : 0.0f};
            last_x_ = x;
            last_y_ = y;
            have_pointer_ = true;
            out.push_back(e);
            return;
        }
        case Kind::PointerScroll: {
            Event e{};
            e.type = EventType::MouseWheel;
            e.wheel = Event::Wheel{event.scroll_x, event.scroll_y};
            out.push_back(e);
            return;
        }
    }
    // No default: `decode` already rejected any kind byte outside the enum, and a total switch
    // makes a future Kind a compile error here rather than a silently ignored event.
}

void StreamInputTranslator::release_all(std::vector<platform::Event>& out) {
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i]) {
            keys_[i] = false;
            // No modifiers on a synthetic release: we are not being told the shift state, we are
            // asserting that nothing is held any more, and claiming Shift on the way out would
            // leave a consumer that latches `mods` believing it still is.
            out.push_back(make_key_event(
                EventType::KeyUp, static_cast<Key>(i), KeyMods::None, /*repeat=*/false));
        }
    }
    for (std::size_t i = 0; i < buttons_.size(); ++i) {
        if (buttons_[i]) {
            buttons_[i] = false;
            out.push_back(
                make_button_event(static_cast<MouseButton>(i), KeyMods::None, /*down=*/false));
        }
    }
}

InputDispatch dispatch_input_message(stream::MessageType type,
                                     std::span<const std::byte> payload,
                                     StreamInputTranslator& translator,
                                     std::vector<platform::Event>& out) {
    if (type != stream::MessageType::Input) {
        return InputDispatch::NotInput;
    }
    stream::InputEvent event;
    if (!event.decode(payload)) {
        // One bad message, not a dead session: the sender is a browser on the public internet and
        // a truncated DataChannel message must cost a counter, not the stream.
        return InputDispatch::Malformed;
    }
    translator.translate(event, out);
    return InputDispatch::Applied;
}

} // namespace rime::app
