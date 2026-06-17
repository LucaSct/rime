// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "rime/platform/event.hpp"
#include "rime/platform/keyboard.hpp"
#include "rime/platform/mouse.hpp"

// Polled input state with per-frame edge detection, layered on top of the event stream.
//
// The backends and the event queue speak in transitions ("key A went down"); gameplay code usually
// wants questions answered ("is A held this frame?", "was A pressed *this* frame?"). Answering the
// edge questions means remembering last frame and diffing — so Input keeps a current and a previous
// snapshot. Each frame: call new_frame() (roll current -> previous, clear per-frame accumulators),
// feed that frame's events to process(), then query in O(1).
//
// This layer is deliberately OS-agnostic with no per-OS code, so it is fully unit-tested by feeding
// synthetic events — the same path the null backend and a real backend both drive. (Per-OS cursor
// capture / raw-motion modes are a separate concern handled in the backends.)
namespace rime::platform {

class Input {
public:
    // Start a new frame: snapshot current key/button state into "previous" and clear the per-frame
    // accumulators (mouse delta, wheel, typed text). Call once per frame, before draining events.
    void new_frame();

    // Fold one event into the current state. Events Input does not care about are ignored, so it is
    // safe to forward every event from poll_event().
    void process(const Event& e);

    // Keyboard. "down" = held now; "pressed"/"released" = the up->down / down->up edge this frame.
    [[nodiscard]] bool key_down(Key k) const;
    [[nodiscard]] bool key_pressed(Key k) const;
    [[nodiscard]] bool key_released(Key k) const;

    // Mouse buttons, same semantics as keys.
    [[nodiscard]] bool mouse_down(MouseButton b) const;
    [[nodiscard]] bool mouse_pressed(MouseButton b) const;
    [[nodiscard]] bool mouse_released(MouseButton b) const;

    // Mouse position is the latest absolute location (framebuffer pixels); delta is the relative
    // motion accumulated this frame (the right signal for a first-person camera).
    [[nodiscard]] float mouse_x() const noexcept { return mouse_x_; }

    [[nodiscard]] float mouse_y() const noexcept { return mouse_y_; }

    [[nodiscard]] float mouse_dx() const noexcept { return mouse_dx_; }

    [[nodiscard]] float mouse_dy() const noexcept { return mouse_dy_; }

    // Scroll accumulated this frame.
    [[nodiscard]] float wheel_x() const noexcept { return wheel_x_; }

    [[nodiscard]] float wheel_y() const noexcept { return wheel_y_; }

    // UTF-8 text typed this frame (layout/IME applied; may be several codepoints, or empty).
    [[nodiscard]] std::string_view text() const noexcept { return text_; }

private:
    static constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);
    static constexpr std::size_t kButtonCount = static_cast<std::size_t>(MouseButton::Count);

    std::array<bool, kKeyCount> keys_cur_{};
    std::array<bool, kKeyCount> keys_prev_{};
    std::array<bool, kButtonCount> mouse_cur_{};
    std::array<bool, kButtonCount> mouse_prev_{};
    float mouse_x_ = 0.0f;
    float mouse_y_ = 0.0f;
    float mouse_dx_ = 0.0f;
    float mouse_dy_ = 0.0f;
    float wheel_x_ = 0.0f;
    float wheel_y_ = 0.0f;
    std::string text_;
};

} // namespace rime::platform
