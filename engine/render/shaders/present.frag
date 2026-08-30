// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Present (m13.3a): copy a finished LDR image onto the swapchain backbuffer. The last pass of a
// windowed frame, and deliberately the dumbest one in the engine.
//
// WHY A PASS AND NOT A COPY. The RHI has `copy_texture_to_buffer` but no texture-to-texture copy,
// and adding one would mean a new virtual on the command-buffer interface plus a layout-transition
// story in every backend — to move pixels the raster path already moves for free. A fullscreen
// triangle sampling one image into another needs no new RHI surface at all, and it is the same
// idiom `tonemap.frag` already uses.
//
// WHY NO COLOUR MANAGEMENT HERE, which is the part worth getting right rather than guessing:
// `tonemap.frag` already applies the sRGB transfer function in the shader (see its comment — it
// anticipated exactly this pass), and the Vulkan backend picks a **UNORM** surface format
// (`swapchain_vulkan.cpp` asks for `VK_FORMAT_B8G8R8A8_UNORM`), so the hardware does no encode of
// its own. Passing the value straight through is therefore correct. If the backend ever selects an
// _SRGB surface, this pass must stop being a pass-through or the image double-encodes and every
// midtone washes out — that is the one change that would silently break it.
//
// Channel order needs no thought: the source is RGBA8 and the target BGRA8, and a fragment shader
// writes *components*, not bytes — the format decides where each lands.
//
// texelFetch, not a sampled uv: output pixel (x, y) reads source texel (x, y) exactly, with no
// filtering and no half-texel offsets. When the window and the render target differ in size the
// fetch is clamped rather than scaled, which is honest — the loop resizes the render target to the
// framebuffer instead of pretending a stretch is free.
#version 450

layout(set = 0, binding = 0) uniform sampler2D source;

layout(location = 0) out vec4 out_color;

void main() {
    const ivec2 at = ivec2(gl_FragCoord.xy);
    const ivec2 limit = textureSize(source, 0) - ivec2(1);
    out_color = vec4(texelFetch(source, clamp(at, ivec2(0), limit), 0).rgb, 1.0);
}
