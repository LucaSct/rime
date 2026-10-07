// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The TAA resolve (ADR-0078 step 1d): the pass that turns jittered frames into one supersampled one.
//
// ---- What this computes ---------------------------------------------------------------------
// Temporal jitter (temporal_jitter.hpp) renders every frame with the sampling grid nudged by a
// different sub-pixel amount, so across 8 frames each pixel is sampled at 8 distinct positions
// inside its own area. The average of those samples is TEMPORAL SUPERSAMPLING: it converges to the
// integral of the scene over the pixel's footprint, which is the correct value that any single
// sample only ESTIMATES. Aliasing is the error of that estimate: it changes with the sample offset,
// while the true value does not, so averaging removes it. That is why this is not "a blur that hides
// jaggies" -- a blur mixes neighbouring pixels' values, this mixes one pixel's own value across
// time.
//
// Averaging 8 frames literally would need 8 frames stored. Instead the resolve keeps ONE history
// image and blends it with the new frame, history weight 0.9: result = 0.9 * history + 0.1 * now.
// That is an exponential moving average, and with a periodic input it settles to a periodic output
// whose average over one period equals the plain mean of the period's frames (every input frame
// contributes the same total weight across the period). tests/render/taa_resolve_test.cpp proves
// exactly that, against a mean rendered the slow way.
//
// ---- Why it is hard: the world moves -----------------------------------------------------------
// "Last frame's value for this pixel" is only at the same pixel if nothing moved. When the camera
// or an object moves, the surface visible here was somewhere ELSE in last frame's image; the
// velocity buffer (velocity.frag) says where: current NDC minus previous NDC of the same surface
// point. Every TAA artefact is a failure of that lookup: ghosting when wrong history is used
// anyway, smearing when it is reused too strongly, sparkle when it is thrown away too often.
//
// The conventions, because an inverted y here gives an image that looks almost right and ghosts
// upward:
//   * velocity is in NDC units (a fraction of the half-extent: [-1,1] spans the screen);
//   * uv = ndc * 0.5 + 0.5, so a UV offset is HALF the NDC offset;
//   * Vulkan's NDC y and framebuffer y both point DOWN, so there is no y flip anywhere;
//   * history lives where the surface WAS: prev_uv = uv - 0.5 * velocity.
//
// ---- Three defences, each against one failure ---------------------------------------------------
// 1. OFF-SCREEN REJECTION. A pixel whose history position lies outside the screen has no history:
//    sampling there returns either the clamped edge texel (garbage that looks plausible) or a
//    wrapped texel from the far side. Those pixels take the current frame whole.
// 2. NEIGHBOURHOOD CLAMP (Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014, Advances in
//    Real-Time Rendering). Whatever the history holds, it must be a colour that the CURRENT frame
//    could plausibly have produced here, i.e. lie within the min/max of the current frame's 3x3
//    neighbourhood. History from a surface that has since been disoccluded (an object moved away,
//    revealing background) is a colour the neighbourhood no longer contains; the clamp drags it to
//    the nearest plausible colour instead of letting it ghost. The clamp is done in YCoCg: that
//    space separates luminance (Y) from chroma (Co, Cg), so the box is mostly a bound on
//    brightness, which is where the visible artefact lives. An RGB box clamps each channel
//    independently and shifts hue at the very edges the pass exists to stabilise.
// 3. DEPTH REPROJECTION FOR BACKGROUND. The velocity pass only writes surfaces, so the sky and any
//    other pixel the depth pre-pass left at the far value carries zero velocity. Zero velocity
//    means "this pixel did not move", which is wrong for the sky under a camera turn (the whole sky
//    moves) and would smear it spectacularly. Those pixels are reprojected analytically instead:
//    unproject the pixel to a point on the far plane with the current unjittered view-projection,
//    project that point with last frame's. (Sky is effectively at infinity, so a camera
//    TRANSLATION barely moves it, and the far-plane point has the same property to within
//    translation / far-distance; a turn is handled exactly.)
//
// ---- Why this runs BEFORE tonemap ----------------------------------------------------------------
// It averages radiance. Tonemapping first would make the average depend on the operator (the mean
// of tonemapped values is not the tonemap of the mean), and tonemapping destroys the information
// needed to undo it, so the order cannot be fixed afterwards. History is kept in RGBA16F for the
// same reason as the HDR target: convergence is made of very small differences that an 8-bit
// history would quantise away.
#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_color; // the jittered HDR frame (texelFetch)
layout(set = 0, binding = 1) uniform sampler2D velocity;    // RG16F NDC-unit delta (texelFetch)
layout(set = 0, binding = 2) uniform sampler2D scene_depth; // D32, Vulkan z in [0,1]; 1.0 == far
layout(set = 0, binding = 3) uniform sampler2D history;     // last frame's resolved HDR (bilinear)

layout(std140, set = 0, binding = 4) uniform TaaParams {
    mat4 inv_view_proj;  // clip -> world, UNJITTERED current camera
    mat4 prev_view_proj; // world -> clip, UNJITTERED previous camera
    vec4 params;         // x = history weight, y = history valid (0/1), z,w = unused
} taa;

layout(location = 0) out vec4 out_resolved; // what the tonemap reads
layout(location = 1) out vec4 out_history;  // next frame's history (identical value)

// RGB <-> YCoCg. Y = (R + 2G + B)/4 is luminance-like; Co/Cg are orange/green chroma axes.
vec3 rgb_to_ycocg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

vec3 ycocg_to_rgb(vec3 c) {
    return vec3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

void main() {
    const ivec2 size = textureSize(scene_color, 0);
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const vec2 uv = (vec2(px) + 0.5) / vec2(size);

    const vec4 current = texelFetch(scene_color, px, 0);

    // 3x3 neighbourhood of the CURRENT frame, in YCoCg. Edge pixels clamp their taps to the image.
    vec3 box_min = vec3(1e30);
    vec3 box_max = vec3(-1e30);
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const ivec2 q = clamp(px + ivec2(dx, dy), ivec2(0), size - 1);
            const vec3 s = rgb_to_ycocg(texelFetch(scene_color, q, 0).rgb);
            box_min = min(box_min, s);
            box_max = max(box_max, s);
        }
    }

    // Where was this surface point last frame? Background (depth at the far plane) is reprojected
    // from depth; everything else uses the velocity buffer. See the header for why.
    vec2 prev_uv;
    if (texelFetch(scene_depth, px, 0).r >= 1.0) {
        const vec2 ndc = uv * 2.0 - 1.0;
        vec4 world = taa.inv_view_proj * vec4(ndc, 1.0, 1.0);
        world /= world.w;
        const vec4 prev_clip = taa.prev_view_proj * world;
        prev_uv = (prev_clip.xy / prev_clip.w) * 0.5 + 0.5;
    } else {
        prev_uv = uv - 0.5 * texelFetch(velocity, px, 0).rg;
    }

    // No history (first frame, after a resize) or history from outside the screen: current, whole.
    const bool on_screen = all(greaterThanEqual(prev_uv, vec2(0.0))) &&
                           all(lessThanEqual(prev_uv, vec2(1.0)));
    vec4 result = current;
    if (taa.params.y > 0.5 && on_screen) {
        const vec4 hist = texture(history, prev_uv);
        const vec3 clamped = ycocg_to_rgb(clamp(rgb_to_ycocg(hist.rgb), box_min, box_max));
        // The history weight is the knob a per-pixel reactive mask will later modulate (a pixel
        // that just fractured must forget its history faster). It is a uniform for now.
        const float w = taa.params.x;
        result = mix(current, vec4(clamped, hist.a), w);
    }

    out_resolved = result;
    out_history = result;
}
