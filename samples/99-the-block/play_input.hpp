// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <span>

#include "rime/gameplay/character.hpp"
#include "rime/platform/event.hpp"

namespace rime::block_play {

// A rendered frame can run zero fixed ticks, or several. Keep the newest levels/view angles,
// but accumulate action edges until ONE tick consumes them. Replacing the whole command each
// frame loses clicks at high frame rates; replaying its pressed bits each tick fires repeatedly.
class PlayInput {
public:
    void frame(const gameplay::CharacterInput& input,
               std::span<const platform::Event> events,
               platform::MouseButton fire_button,
               std::uint32_t fire_bit) noexcept {
        const auto pending = latest_.pressed;
        latest_ = input;
        latest_.pressed |= pending;
        // A fast click can press AND release inside one render frame. The device's final held
        // level is then false; use the actual down event so that tap still reaches the tick.
        for (const auto& e : events) {
            if (e.type == platform::EventType::WindowFocus && !e.focus.focused) {
                latest_.pressed = latest_.held = 0;
                latest_.move_x = latest_.move_y = 0;
            }
            if (e.type == platform::EventType::MouseButton && e.button.button == fire_button &&
                e.button.down)
                latest_.pressed |= fire_bit;
        }
    }

    [[nodiscard]] gameplay::CharacterInput tick() noexcept {
        const auto result = latest_;
        latest_.pressed = 0;
        return result;
    }

private:
    gameplay::CharacterInput latest_{};
};

} // namespace rime::block_play
