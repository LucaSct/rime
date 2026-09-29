// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// Browser physical key (`KeyboardEvent.code`) -> `rime::platform::Key` ordinal
// (engine/platform/include/rime/platform/keyboard.hpp).
//
// INFERRED, not measured: `input.rs`'s doc comment says an `InputEvent.code` for a key event "is
// a key code (Key*)", but nothing in the repository defines a wire-stable numbering for that enum
// — `keyboard.hpp` says the opposite in so many words: "Values are arbitrary and stable; do not
// rely on them numerically." No engine-side input-injection path exists yet to decode this page's
// `code` field back into a `Key` at all (grep for `InputEvent` under `engine/` turns up only the
// wire structs, never a producer or consumer tied to `platform::Key`), so there is nothing to
// conform this table against. What follows is this page's own choice: the enum's declaration
// order, read by hand from `keyboard.hpp` (`Unknown` = 0, then each named key in file order), used
// as the ordinal. If a future engine-side injector picks a different numbering, this table moves,
// not the wire format — flagged in the brick's report for exactly that reason.
const KEY_ORDER = [
  "Unknown",
  "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
  "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
  "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
  "Space", "Enter", "Tab", "Backspace", "Escape", "Insert", "Delete",
  "Left", "Right", "Up", "Down", "Home", "End", "PageUp", "PageDown",
  "Minus", "Equal", "LeftBracket", "RightBracket", "Backslash", "Semicolon", "Apostrophe",
  "Grave", "Comma", "Period", "Slash",
  "LeftShift", "RightShift", "LeftCtrl", "RightCtrl", "LeftAlt", "RightAlt",
  "LeftSuper", "RightSuper", "CapsLock", "NumLock", "ScrollLock",
  "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
  "PrintScreen", "Pause", "Menu",
  "KP0", "KP1", "KP2", "KP3", "KP4", "KP5", "KP6", "KP7", "KP8", "KP9",
  "KPDecimal", "KPDivide", "KPMultiply", "KPSubtract", "KPAdd", "KPEnter", "KPEqual",
];

const KEY_TO_ORDINAL = Object.freeze(
  Object.fromEntries(KEY_ORDER.map((name, index) => [name, index])),
);

/** `KeyboardEvent.code` (the physical-key string; `event.key` is the layout-mapped character and
 * the wrong thing to send — `keyboard.hpp` identifies keys "by position, not the character it
 * produces") to this table's key name. */
const CODE_TO_KEY_NAME = Object.freeze({
  KeyA: "A", KeyB: "B", KeyC: "C", KeyD: "D", KeyE: "E", KeyF: "F", KeyG: "G",
  KeyH: "H", KeyI: "I", KeyJ: "J", KeyK: "K", KeyL: "L", KeyM: "M", KeyN: "N",
  KeyO: "O", KeyP: "P", KeyQ: "Q", KeyR: "R", KeyS: "S", KeyT: "T", KeyU: "U",
  KeyV: "V", KeyW: "W", KeyX: "X", KeyY: "Y", KeyZ: "Z",
  Digit0: "Num0", Digit1: "Num1", Digit2: "Num2", Digit3: "Num3", Digit4: "Num4",
  Digit5: "Num5", Digit6: "Num6", Digit7: "Num7", Digit8: "Num8", Digit9: "Num9",
  Space: "Space", Enter: "Enter", Tab: "Tab", Backspace: "Backspace", Escape: "Escape",
  Insert: "Insert", Delete: "Delete",
  ArrowLeft: "Left", ArrowRight: "Right", ArrowUp: "Up", ArrowDown: "Down",
  Home: "Home", End: "End", PageUp: "PageUp", PageDown: "PageDown",
  Minus: "Minus", Equal: "Equal", BracketLeft: "LeftBracket", BracketRight: "RightBracket",
  Backslash: "Backslash", Semicolon: "Semicolon", Quote: "Apostrophe", Backquote: "Grave",
  Comma: "Comma", Period: "Period", Slash: "Slash",
  ShiftLeft: "LeftShift", ShiftRight: "RightShift", ControlLeft: "LeftCtrl",
  ControlRight: "RightCtrl", AltLeft: "LeftAlt", AltRight: "RightAlt",
  MetaLeft: "LeftSuper", MetaRight: "RightSuper",
  CapsLock: "CapsLock", NumLock: "NumLock", ScrollLock: "ScrollLock",
  F1: "F1", F2: "F2", F3: "F3", F4: "F4", F5: "F5", F6: "F6",
  F7: "F7", F8: "F8", F9: "F9", F10: "F10", F11: "F11", F12: "F12",
  PrintScreen: "PrintScreen", Pause: "Pause", ContextMenu: "Menu",
  Numpad0: "KP0", Numpad1: "KP1", Numpad2: "KP2", Numpad3: "KP3", Numpad4: "KP4",
  Numpad5: "KP5", Numpad6: "KP6", Numpad7: "KP7", Numpad8: "KP8", Numpad9: "KP9",
  NumpadDecimal: "KPDecimal", NumpadDivide: "KPDivide", NumpadMultiply: "KPMultiply",
  NumpadSubtract: "KPSubtract", NumpadAdd: "KPAdd", NumpadEnter: "KPEnter",
  NumpadEqual: "KPEqual",
});

/** The `Key` ordinal for a `KeyboardEvent`, or `Key::Unknown` (0) for anything not in the table —
 * never `undefined`, so a caller can encode it straight onto the wire. */
export function keyOrdinalOf(event) {
  const name = CODE_TO_KEY_NAME[event.code];
  return name ? KEY_TO_ORDINAL[name] : KEY_TO_ORDINAL.Unknown;
}
