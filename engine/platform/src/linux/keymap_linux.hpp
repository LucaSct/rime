// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

#include "rime/platform/keyboard.hpp"

// Shared Linux key mapping. Both Linux backends speak Linux evdev key codes (the KEY_* constants in
// <linux/input-event-codes.h>): X11 delivers them as keycode = evdev + 8, Wayland delivers the raw
// evdev code. Mapping the *physical* evdev code — not a layout-dependent keysym — keeps WASD on the
// WASD keys for any layout, the same physical-key promise the Cocoa and Win32 backends keep.
namespace rime::platform::detail {

[[nodiscard]] Key key_from_evdev(std::uint32_t evdev_code);

} // namespace rime::platform::detail
