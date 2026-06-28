// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Turntable export (E4): the pure helpers behind `--turntable N` — render N lit snapshots orbiting the
// part through a full 360° to a numbered PPM sequence (turn into a GIF/MP4 to showcase a computed part).
// Only the *novel* bits live here so they unit-test without a GPU: the per-frame camera yaw and the frame
// file naming. The render of each frame reuses the proven off-screen path in main.cpp. See
// docs/math/orbit-camera.md for the camera the yaw drives.
#pragma once

#include <cstdio>
#include <string>

namespace rime::viewer {

// The camera yaw (radians) for frame `i` of an `n`-frame, full-revolution turntable starting at `yaw0`.
// Frame i sits at yaw0 + 2π·i/n, so frame 0 is the start pose and frame n would close the loop back onto
// it (we render i ∈ [0,n), n distinct evenly-spaced views). n is clamped to ≥ 1 to avoid a divide-by-zero.
[[nodiscard]] inline float turntable_yaw(int i, int n, float yaw0) {
    constexpr float kTwoPi = 6.283185307179586f;
    return yaw0 + kTwoPi * static_cast<float>(i) / static_cast<float>(n < 1 ? 1 : n);
}

// The PPM path for frame `i`, derived from the output template by inserting a zero-padded index on its
// stem: ("icem_view.ppm", 3) → "icem_view_003.ppm"; ("out/spin", 12) → "out/spin_012.ppm". An extension
// on the final path component is stripped first (so the index lands on the stem, not after ".ppm"); a dot
// that belongs to a directory, not the filename, is left alone.
[[nodiscard]] inline std::string turntable_frame_path(const std::string& out_template, int i) {
    std::string stem = out_template;
    const std::size_t slash = stem.find_last_of("/\\");
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        stem = stem.substr(0, dot);
    char idx[24];
    std::snprintf(idx, sizeof(idx), "_%03d.ppm", i);
    return stem + idx;
}

} // namespace rime::viewer
