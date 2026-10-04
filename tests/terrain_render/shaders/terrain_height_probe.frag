// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The m19.3 proof's fragment stage. It is paired with the ENGINE's own terrain.vert (compiled into
// this test target from engine/render/shaders — one source of truth per shader, the rule
// tests/render already follows for the viewer's and the RHI's shaders), and its only job is to
// report, losslessly, the world height that vertex stage reconstructed.
//
// ── WHY THE TARGET IS R32Uint AND THE HEIGHT TRAVELS AS ITS BIT PATTERN ──────────────────────
//
// The number under test is a metre height around 20 m that has to be compared against a physics
// raycast to well under a millimetre. An RGBA16Float target (the HDR format the real pass writes)
// has a 10-bit mantissa: at 20 m its step is ~16 mm, which would be hundreds of times coarser than
// the margin and would make the proof measure the TARGET's quantisation instead of the terrain's.
//
// `floatBitsToUint` into an R32_UINT target is the standard fix: integer colour attachments are
// never blended, never filtered and never format-converted, so the 32 bits the fragment writes are
// the 32 bits the readback reads — the f32 arrives on the CPU exactly. (ScenePicker's id buffer
// uses the same format for the same "these bits are data, not colour" reason.)
//
// The target is cleared to all-zero bits, which decode as +0.0 m. The proof places the tile so no
// world height anywhere on it can be 0, so "exactly 0.0" unambiguously means NO FRAGMENT LANDED
// HERE — which is what lets the test count its own coverage instead of silently comparing against
// a pixel the terrain never reached.
#version 450

layout(location = 0) in vec3 v_world;

layout(location = 0) out uint out_height_bits;

void main() {
    out_height_bits = floatBitsToUint(v_world.y);
}
