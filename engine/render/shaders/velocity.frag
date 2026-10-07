// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The motion-vector pass's fragment shader (ADR-0078 step 1c): current NDC minus previous NDC.
//
// NDC UNITS, NOT PIXELS. The offset is a fraction of the screen's half-extent ([-1, 1] across it),
// so it means the same thing at 720p and at 4K: a resolve that reprojects history after a
// resolution change multiplies by its own half-extent instead of inheriting a stale pixel scale.
// Half precision resolves an NDC delta well: it keeps ~11 significant bits, so a delta of a few
// pixels at 1080p (about 4e-3) is stored to better than a hundredth of a pixel.
//
// A STATIC point under a STATIC camera has bit-identical current and previous clip positions (same
// matrices, same model, same arithmetic), so this subtraction is exactly zero, not "small".
#version 450

layout(location = 0) in vec4 v_clip_current;
layout(location = 1) in vec4 v_clip_previous;

layout(location = 0) out vec2 out_velocity;

void main() {
    // `precise`: no FMA contraction, so equal inputs give an exactly zero difference (see above).
    precise vec2 current_ndc = v_clip_current.xy / v_clip_current.w;
    precise vec2 previous_ndc = v_clip_previous.xy / v_clip_previous.w;
    precise vec2 velocity = current_ndc - previous_ndc;
    out_velocity = velocity;
}
