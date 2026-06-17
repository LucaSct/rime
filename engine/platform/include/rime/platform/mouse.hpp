// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

// Mouse input vocabulary.
namespace rime::platform {

// Mouse buttons. X1/X2 are the "back"/"forward" side buttons. Count sizes polled-state arrays.
enum class MouseButton : std::uint8_t { Left, Right, Middle, X1, X2, Count };

// How the cursor behaves over a window. Locked is the first-person-camera mode: the cursor is
// hidden and pinned, and the app reads relative motion (dx/dy) instead of an absolute position —
// each backend implements the capture natively (raw input / pointer constraints) in M2.3.
enum class CursorMode : std::uint8_t {
    Normal, // visible, moves freely
    Hidden, // hidden while over the window, still moves freely
    Locked, // hidden + locked to center; report relative motion (FPS camera)
};

} // namespace rime::platform
