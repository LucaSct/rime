// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/platform/keyboard.hpp"

// USB HID usage IDs (Keyboard/Keypad page, 0x07) <-> `platform::Key` (m18 Track H, ADR-0054).
//
// WHY A SECOND KEY NUMBERING EXISTS. `Key` is the engine's *internal* vocabulary and
// `keyboard.hpp` says in so many words that its values are "arbitrary and stable; do not rely on
// them numerically" — the enum stays free to grow a key in the middle. That freedom is
// incompatible with putting a `Key` on a wire, and m18's browser page has to put *something* on
// one: `stream::InputEvent::code` is four bytes of key identity travelling from a JavaScript
// `KeyboardEvent.code` to this engine.
//
// The USB HID Keyboard/Keypad page is the numbering that solves it. It is a published standard
// (HID Usage Tables §10) that has not moved since USB 1.1, every OS keymap in this repo is already
// derived from it one step removed (evdev scancodes ARE HID usages plus 8; Win32 scancodes and
// macOS virtual keys are the same physical table under other names), and — the reason it is the
// right choice rather than merely a workable one — it identifies keys BY POSITION, which is
// exactly what `Key` means. `KeyA` is the key where A sits on a US board, on an AZERTY keyboard
// too. Neither end has to agree about layout, only about geometry.
//
// This header is in `platform` and not in `stream` or `app` because the mapping needs nothing but
// `Key`: a HID usage is an integer from a standard, not a stream concept. Putting it here means
// the browser page, a native remote-view client, and any future input backend all translate
// through ONE table instead of three, and `stream` does not grow a dependency it does not have.
namespace rime::platform {

// The HID usage for `key` on the Keyboard/Keypad page, or 0 when there is none (`Key::Unknown`,
// `Key::Count`, or a key the table does not cover). 0 is "Reserved (no event indicated)" in the
// standard, so it is a legal way to say "nothing" on the wire as well as here.
[[nodiscard]] std::uint32_t key_to_hid_usage(Key key) noexcept;

// The `Key` for a HID usage on the Keyboard/Keypad page, or `Key::Unknown` for a usage this build
// does not map — including every usage outside the page's range. NEVER a crash and never an
// out-of-range `Key`: the wire is attacker-controlled (a browser on the public internet), so an
// unmapped usage is ordinary data, not an error condition. Callers COUNT what they drop; see
// `app::StreamInputTranslator::unknown_usages()`.
[[nodiscard]] Key hid_usage_to_key(std::uint32_t usage) noexcept;

} // namespace rime::platform
