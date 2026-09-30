// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Browser physical key (`KeyboardEvent.code`) -> USB HID usage ID, Keyboard/Keypad page (0x07).
//
// MEASURED against the engine, not inferred: `InputEvent.code` for a key event is a HID usage
// (ADR-0054, decided 2026-09-29), and `engine/platform/src/hid_keys.cpp` is the table that reads
// it back into `rime::platform::Key`. This file's previous version sent `platform::Key` ORDINALS,
// which it flagged itself as a guess — `keyboard.hpp` says "values are arbitrary and stable; do
// not rely on them numerically", so there was no stable numbering to send and nothing on the
// server decoded them anyway.
//
// WHY HID. A `KeyboardEvent.code` identifies a key by POSITION ("KeyA" is the key where A sits on
// a US board, whatever the layout prints on it), and so does `platform::Key`, and so does the HID
// page — the three agree about what a key IS, which is the only thing that has to survive the
// wire. The page is a published standard that has not moved since USB 1.1, so neither end has to
// ship a version of it.
//
// The names in the comments are the standard's own ("Keyboard a and A", etc.), abbreviated.
const CODE_TO_HID_USAGE = Object.freeze({
  // 0x04..0x1D — letters, in alphabetical order.
  KeyA: 0x04, KeyB: 0x05, KeyC: 0x06, KeyD: 0x07, KeyE: 0x08, KeyF: 0x09, KeyG: 0x0a,
  KeyH: 0x0b, KeyI: 0x0c, KeyJ: 0x0d, KeyK: 0x0e, KeyL: 0x0f, KeyM: 0x10, KeyN: 0x11,
  KeyO: 0x12, KeyP: 0x13, KeyQ: 0x14, KeyR: 0x15, KeyS: 0x16, KeyT: 0x17, KeyU: 0x18,
  KeyV: 0x19, KeyW: 0x1a, KeyX: 0x1b, KeyY: 0x1c, KeyZ: 0x1d,
  // 0x1E..0x27 — the number row. The page runs 1..9 and puts 0 LAST, so this is not an offset.
  Digit1: 0x1e, Digit2: 0x1f, Digit3: 0x20, Digit4: 0x21, Digit5: 0x22,
  Digit6: 0x23, Digit7: 0x24, Digit8: 0x25, Digit9: 0x26, Digit0: 0x27,
  // 0x28..0x38 — whitespace, editing, and the US punctuation positions.
  Enter: 0x28, Escape: 0x29, Backspace: 0x2a, Tab: 0x2b, Space: 0x2c,
  Minus: 0x2d, Equal: 0x2e, BracketLeft: 0x2f, BracketRight: 0x30, Backslash: 0x31,
  Semicolon: 0x33, Quote: 0x34, Backquote: 0x35, Comma: 0x36, Period: 0x37, Slash: 0x38,
  // 0x39..0x45 — CapsLock then F1..F12.
  CapsLock: 0x39,
  F1: 0x3a, F2: 0x3b, F3: 0x3c, F4: 0x3d, F5: 0x3e, F6: 0x3f,
  F7: 0x40, F8: 0x41, F9: 0x42, F10: 0x43, F11: 0x44, F12: 0x45,
  // 0x46..0x52 — the system / navigation cluster and the arrows.
  PrintScreen: 0x46, ScrollLock: 0x47, Pause: 0x48,
  Insert: 0x49, Home: 0x4a, PageUp: 0x4b, Delete: 0x4c, End: 0x4d, PageDown: 0x4e,
  ArrowRight: 0x4f, ArrowLeft: 0x50, ArrowDown: 0x51, ArrowUp: 0x52,
  // 0x53..0x63, 0x67 — the keypad, 1..9 then 0 like the number row.
  NumLock: 0x53, NumpadDivide: 0x54, NumpadMultiply: 0x55, NumpadSubtract: 0x56,
  NumpadAdd: 0x57, NumpadEnter: 0x58,
  Numpad1: 0x59, Numpad2: 0x5a, Numpad3: 0x5b, Numpad4: 0x5c, Numpad5: 0x5d,
  Numpad6: 0x5e, Numpad7: 0x5f, Numpad8: 0x60, Numpad9: 0x61, Numpad0: 0x62,
  NumpadDecimal: 0x63, ContextMenu: 0x65, NumpadEqual: 0x67,
  // 0xE0..0xE7 — the modifiers, the one block every OS and browser agrees on exactly.
  ControlLeft: 0xe0, ShiftLeft: 0xe1, AltLeft: 0xe2, MetaLeft: 0xe3,
  ControlRight: 0xe4, ShiftRight: 0xe5, AltRight: 0xe6, MetaRight: 0xe7,
});

/** The HID usage for a `KeyboardEvent`, or 0 for a key this table does not cover — never
 * `undefined`, so a caller can encode it straight onto the wire. 0 is the page's own "Reserved
 * (no event indicated)", and the engine reads it as `Key::Unknown` and counts it.
 *
 * `event.code` (the physical key) and not `event.key` (the character the layout produced): the
 * engine identifies keys by position, so WASD stays WASD on an AZERTY board. */
export function hidUsageOf(event) {
  return CODE_TO_HID_USAGE[event.code] ?? 0;
}
