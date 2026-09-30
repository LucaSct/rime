// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The engine's half of "input is actions, never keys" (m20.1): the table-driven InputMapper that
// turns a frame's platform events into the ActionState a game's fixed tick consumes. See
// game_definition.hpp for why the two halves run at different rates.

#include "rime/app/game_definition.hpp"

#include <utility>

namespace rime::app {
namespace {

[[nodiscard]] constexpr std::size_t key_index(platform::Key key) noexcept {
    return static_cast<std::size_t>(key);
}

[[nodiscard]] constexpr bool key_in_range(platform::Key key) noexcept {
    return key_index(key) < key_index(platform::Key::Count);
}

} // namespace

InputMapper::InputMapper(InputMap map) : map_(std::move(map)) {
    // Validate the table ONCE, here, and drop what cannot be honoured — counted, so a game whose
    // binding table names axis 9 of 8 finds out from a number rather than from a key that silently
    // does nothing. Checking on every event instead would pay per keystroke for a mistake that can
    // only be made at build time.
    auto bad_axis = [](const AxisBinding& b) {
        return b.axis >= ActionState::kAxes || !key_in_range(b.key);
    };
    auto bad_button = [](const ButtonBinding& b) {
        return b.button >= ActionState::kButtons || !key_in_range(b.key);
    };
    rejected_ += std::erase_if(map_.axes, bad_axis);
    rejected_ += std::erase_if(map_.buttons, bad_button);
}

void InputMapper::update(std::span<const platform::Event> events) {
    for (const platform::Event& e : events) {
        const bool down = e.type == platform::EventType::KeyDown;
        if (!down && e.type != platform::EventType::KeyUp) {
            continue; // not a key: window/mouse events are the engine's business, not the map's
        }
        if (!key_in_range(e.key.key)) {
            continue;
        }
        // A press is an EDGE: up→down, and never an auto-repeat. Recorded before the held state
        // changes so the comparison sees the state the key was in before this event.
        const bool was_down = down_[key_index(e.key.key)];
        if (down && !was_down && !e.key.repeat) {
            for (const ButtonBinding& b : map_.buttons) {
                if (b.key == e.key.key) {
                    pressed_ |= 1u << b.button;
                }
            }
        }
        down_[key_index(e.key.key)] = down;
    }
}

ActionState InputMapper::take() noexcept {
    // Recomputed from the held keys each time rather than accumulated per event, so the answer is
    // a pure function of "which keys are down now" — a missed KeyUp cannot leave an axis stuck at a
    // value no key is producing.
    ActionState s{};
    for (const AxisBinding& b : map_.axes) {
        if (down_[key_index(b.key)]) {
            s.axes[b.axis] += b.value;
        }
    }
    for (const ButtonBinding& b : map_.buttons) {
        if (down_[key_index(b.key)]) {
            s.held |= 1u << b.button;
        }
    }
    s.pressed = pressed_;
    pressed_ = 0; // an edge is delivered to exactly one tick
    return s;
}

} // namespace rime::app
