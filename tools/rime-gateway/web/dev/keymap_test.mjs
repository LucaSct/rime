// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

// keymap.js against the USB HID Usage Tables §10 "Keyboard/Keypad Page (0x07)", and against the
// engine's own copy of that page in `engine/platform/src/hid_keys.cpp`.
//
// WHAT THIS CAN AND CANNOT PROVE. The two tables are hand-derived from the same standard in two
// languages, so this file checks the values a *reader* can verify against the standard — the
// anchors every keyboard page quotes (a = 0x04, Enter = 0x28, Space = 0x2C, the modifier block at
// 0xE0), the three places the numbering is counter-intuitive and a transcription is most likely
// to be wrong, and the structural invariants (no duplicates, nothing outside the page, no
// `event.key` strings mistaken for `event.code` ones). It cannot prove the C++ side maps the same
// usage to the same key — nothing here can link the two files — so the engine carries its own
// round-trip test (tests/platform/hid_keys_test.cpp) and a static_assert that every Key has a
// usage. The two together are what keeps the pair honest.

import assert from "node:assert/strict";
import { hidUsageOf } from "../keymap.js";

let failures = 0;

function check(name, actual, expected) {
  if (actual === expected) {
    console.log(`ok - ${name}`);
  } else {
    failures += 1;
    console.error(`FAIL - ${name}\n  expected 0x${expected.toString(16)}\n  actual   0x${actual.toString(16)}`);
  }
}

// ── The anchors. These four are the values quoted in the standard's own examples and in every
// HID keyboard descriptor; if the table were shifted by one, all four would be wrong.
check("KeyA -> 0x04", hidUsageOf({ code: "KeyA" }), 0x04);
check("Enter -> 0x28", hidUsageOf({ code: "Enter" }), 0x28);
check("Space -> 0x2C", hidUsageOf({ code: "Space" }), 0x2c);
check("ArrowRight -> 0x4F", hidUsageOf({ code: "ArrowRight" }), 0x4f);
check("ControlLeft -> 0xE0", hidUsageOf({ code: "ControlLeft" }), 0xe0);

// ── The three counter-intuitive places.
// 1. The number row is 1..9 then 0 — Digit0 is 0x27, NOT 0x1D + 0.
check("Digit1 -> 0x1E", hidUsageOf({ code: "Digit1" }), 0x1e);
check("Digit0 -> 0x27 (zero comes LAST)", hidUsageOf({ code: "Digit0" }), 0x27);
// 2. The keypad repeats that shape: Numpad1..9 then Numpad0.
check("Numpad1 -> 0x59", hidUsageOf({ code: "Numpad1" }), 0x59);
check("Numpad0 -> 0x62 (zero comes LAST here too)", hidUsageOf({ code: "Numpad0" }), 0x62);
// 3. 0x32 (the non-US "# and ~") is skipped: `platform::Key` has one Backslash and a keyboard
//    never has both keys, so only 0x31 is mapped and the ISO key falls through to Unknown.
check("Backslash -> 0x31", hidUsageOf({ code: "Backslash" }), 0x31);
// 4. Arrows are Right, Left, Down, Up — not the reading order the names suggest.
check("ArrowLeft -> 0x50", hidUsageOf({ code: "ArrowLeft" }), 0x50);
check("ArrowDown -> 0x51", hidUsageOf({ code: "ArrowDown" }), 0x51);
check("ArrowUp -> 0x52", hidUsageOf({ code: "ArrowUp" }), 0x52);

// ── WASD, because it is what the demo is actually played with.
check("KeyW -> 0x1A", hidUsageOf({ code: "KeyW" }), 0x1a);
check("KeyS -> 0x16", hidUsageOf({ code: "KeyS" }), 0x16);
check("KeyD -> 0x07", hidUsageOf({ code: "KeyD" }), 0x07);

// ── Unmapped keys are 0 ("Reserved (no event indicated)"), never undefined: the caller writes
// the result straight into a u32 field, and `undefined >>> 0` is 0 by accident rather than by
// contract, which is exactly the kind of accident that survives until a different field uses it.
check("an unknown code -> 0", hidUsageOf({ code: "MediaPlayPause" }), 0);
check("a missing code -> 0", hidUsageOf({}), 0);
// `event.key` is the LAYOUT-mapped character; sending it would break the whole point of using
// physical positions, so a lowercase "a" must not resolve to anything.
check('the event.key string "a" -> 0', hidUsageOf({ code: "a" }), 0);

// ── Structural invariants over the whole table, via the same public entry point.
const codes = [
  "KeyA", "KeyB", "KeyC", "KeyD", "KeyE", "KeyF", "KeyG", "KeyH", "KeyI", "KeyJ", "KeyK", "KeyL",
  "KeyM", "KeyN", "KeyO", "KeyP", "KeyQ", "KeyR", "KeyS", "KeyT", "KeyU", "KeyV", "KeyW", "KeyX",
  "KeyY", "KeyZ",
  "Digit0", "Digit1", "Digit2", "Digit3", "Digit4", "Digit5", "Digit6", "Digit7", "Digit8",
  "Digit9",
  "Enter", "Escape", "Backspace", "Tab", "Space", "Minus", "Equal", "BracketLeft", "BracketRight",
  "Backslash", "Semicolon", "Quote", "Backquote", "Comma", "Period", "Slash", "CapsLock",
  "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
  "PrintScreen", "ScrollLock", "Pause", "Insert", "Home", "PageUp", "Delete", "End", "PageDown",
  "ArrowRight", "ArrowLeft", "ArrowDown", "ArrowUp",
  "NumLock", "NumpadDivide", "NumpadMultiply", "NumpadSubtract", "NumpadAdd", "NumpadEnter",
  "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6", "Numpad7", "Numpad8",
  "Numpad9", "Numpad0", "NumpadDecimal", "ContextMenu", "NumpadEqual",
  "ControlLeft", "ShiftLeft", "AltLeft", "MetaLeft",
  "ControlRight", "ShiftRight", "AltRight", "MetaRight",
];
assert.equal(codes.length, 105, "the list above must cover every mapped key");

const seen = new Map();
for (const code of codes) {
  const usage = hidUsageOf({ code });
  assert.ok(usage !== 0, `${code} must be mapped`);
  // Two codes sharing a usage would make one of them unreachable on the engine side, silently.
  assert.ok(!seen.has(usage), `0x${usage.toString(16)} is used by both ${seen.get(usage)} and ${code}`);
  // 0x04..0xE7 is the whole of the page this build uses; anything outside it decodes to Unknown.
  assert.ok(usage >= 0x04 && usage <= 0xe7, `${code} -> 0x${usage.toString(16)} is outside the page`);
  seen.set(usage, code);
}
console.log(`ok - ${codes.length} codes map to distinct in-page usages`);

if (failures > 0) {
  console.error(`${failures} mapping(s) did not match`);
  process.exit(1);
}
