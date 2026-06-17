// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

// Keyboard input vocabulary.
//
// Key identifies a *physical* key by its position, not the character it produces — so WASD is
// WASD on a French AZERTY board, which is what game controls want. (Each OS backend maps its
// native scancodes/virtual-keys to these in a keymap_*.cpp.) Typed *text* is a separate concern,
// delivered as UTF-8 via EventType::TextInput, because one keypress can yield zero, one, or many
// characters once layout, modifiers, and IME composition are taken into account.
namespace rime::platform {

// Physical keys (US-QWERTY position names). Values are arbitrary and stable; do not rely on them
// numerically. Unknown is the catch-all for keys we do not map.
enum class Key : std::uint16_t {
    Unknown = 0,

    // Letters (by position).
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,

    // Number row.
    Num0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Num9,

    // Whitespace / editing.
    Space,
    Enter,
    Tab,
    Backspace,
    Escape,
    Insert,
    Delete,

    // Navigation.
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,

    // Punctuation (US layout positions).
    Minus,
    Equal,
    LeftBracket,
    RightBracket,
    Backslash,
    Semicolon,
    Apostrophe,
    Grave,
    Comma,
    Period,
    Slash,

    // Modifiers + locks.
    LeftShift,
    RightShift,
    LeftCtrl,
    RightCtrl,
    LeftAlt,
    RightAlt,
    LeftSuper,
    RightSuper,
    CapsLock,
    NumLock,
    ScrollLock,

    // Function keys.
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,

    // System.
    PrintScreen,
    Pause,
    Menu,

    // Keypad.
    KP0,
    KP1,
    KP2,
    KP3,
    KP4,
    KP5,
    KP6,
    KP7,
    KP8,
    KP9,
    KPDecimal,
    KPDivide,
    KPMultiply,
    KPSubtract,
    KPAdd,
    KPEnter,
    KPEqual,

    Count // number of named keys; useful for sizing polled-state arrays (M2.3)
};

// Modifier keys held during an event, as a bitmask. (See operator| / operator& below.)
enum class KeyMods : std::uint8_t {
    None = 0,
    Shift = 1u << 0,
    Ctrl = 1u << 1,
    Alt = 1u << 2,
    Super = 1u << 3, // Windows / Command / Meta
};

[[nodiscard]] constexpr KeyMods operator|(KeyMods a, KeyMods b) noexcept {
    return static_cast<KeyMods>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

[[nodiscard]] constexpr KeyMods operator&(KeyMods a, KeyMods b) noexcept {
    return static_cast<KeyMods>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}

constexpr KeyMods& operator|=(KeyMods& a, KeyMods b) noexcept {
    return a = a | b;
}

[[nodiscard]] constexpr bool any(KeyMods m) noexcept {
    return m != KeyMods::None;
}

} // namespace rime::platform
