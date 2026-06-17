// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/platform/input.hpp"

#include <cstddef>

namespace rime::platform {
namespace {

// Map an enum to its array slot, clamping anything out of range (e.g. Key::Unknown, or a value a
// future enum addition introduces) to slot 0 so indexing is always in bounds.
std::size_t key_index(Key k) {
    const auto i = static_cast<std::size_t>(k);
    return i < static_cast<std::size_t>(Key::Count) ? i : 0;
}

std::size_t button_index(MouseButton b) {
    const auto i = static_cast<std::size_t>(b);
    return i < static_cast<std::size_t>(MouseButton::Count) ? i : 0;
}

} // namespace

void Input::new_frame() {
    keys_prev_ = keys_cur_;
    mouse_prev_ = mouse_cur_;
    mouse_dx_ = 0.0f;
    mouse_dy_ = 0.0f;
    wheel_x_ = 0.0f;
    wheel_y_ = 0.0f;
    text_.clear();
}

void Input::process(const Event& e) {
    switch (e.type) {
        case EventType::KeyDown:
            keys_cur_[key_index(e.key.key)] = true;
            break;
        case EventType::KeyUp:
            keys_cur_[key_index(e.key.key)] = false;
            break;
        case EventType::TextInput:
            text_ += e.text.utf8;
            break;
        case EventType::MouseButton:
            mouse_cur_[button_index(e.button.button)] = e.button.down;
            break;
        case EventType::MouseMove:
            mouse_x_ = e.mouse_move.x;
            mouse_y_ = e.mouse_move.y;
            mouse_dx_ += e.mouse_move.dx;
            mouse_dy_ += e.mouse_move.dy;
            break;
        case EventType::MouseWheel:
            wheel_x_ += e.wheel.dx;
            wheel_y_ += e.wheel.dy;
            break;
        default:
            break;
    }
}

bool Input::key_down(Key k) const {
    return keys_cur_[key_index(k)];
}

bool Input::key_pressed(Key k) const {
    const std::size_t i = key_index(k);
    return keys_cur_[i] && !keys_prev_[i];
}

bool Input::key_released(Key k) const {
    const std::size_t i = key_index(k);
    return !keys_cur_[i] && keys_prev_[i];
}

bool Input::mouse_down(MouseButton b) const {
    return mouse_cur_[button_index(b)];
}

bool Input::mouse_pressed(MouseButton b) const {
    const std::size_t i = button_index(b);
    return mouse_cur_[i] && !mouse_prev_[i];
}

bool Input::mouse_released(MouseButton b) const {
    const std::size_t i = button_index(b);
    return !mouse_cur_[i] && mouse_prev_[i];
}

} // namespace rime::platform
