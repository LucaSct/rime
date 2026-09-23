// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky — the physical sky-view LUT plus a separately resolved sun disc, composited over the
// scene wherever nothing was drawn.
//
// The LUT is the same physical single-scattering-plus-bounded-MS body that lights forward shading,
// DDGI and SSR. It deliberately excludes the sub-texel solar disc; the disc stays analytic here so
// its edge remains sharp and cannot double-light geometry. Full aerial perspective is still the
// later m17.7e froxel brick, not implied by this background replacement.
//
// The technique for the clouds is the standard one: fractional Brownian motion (fBm) over a
// value-noise basis, evaluated where the view ray pierces a flat cloud slab, thresholded by a
// coverage parameter. Real volumetric clouds ray-march a 3-D density field and are a different
// (much more expensive) animal; this is the 2-D projection that reads correctly for a sky you look
// at rather than fly through.
#version 450
#extension GL_GOOGLE_include_directive : require

// The SkyParams block and every function the comment above describes -- the value-noise/fBm basis,
// sky_radiance() and the cloud layer -- live in sky_common.glsl as of m17.7a, so the passes that
// BAKE this sky evaluate the same body this one composites rather than a copy of it. What stays
// here is only what makes this the COMPOSITE pass: the scene inputs, and the main() that decides
// per pixel whether the sky is allowed to touch it.
#include "sky_common.glsl"
#include "sky_mapping.glsl"

layout(set = 0, binding = 0) uniform sampler2D scene_color; // the lit HDR frame
layout(set = 0, binding = 1) uniform sampler2D scene_depth; // D32, Vulkan NDC z in [0,1]
layout(set = 0, binding = 3) uniform sampler2D skyview_lut;

layout(location = 0) out vec4 out_color;

void main() {
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const vec3 scene = texelFetch(scene_color, px, 0).rgb;
    const float depth = texelFetch(scene_depth, px, 0).r;

    // Anything the forward pass drew keeps its colour EXACTLY. The sky only fills what the depth
    // buffer says is still at the far plane -- so with an opaque scene this pass is a no-op on every
    // shaded pixel, which is what makes "sky on" safe to compare against "sky off".
    if (depth < 1.0) {
        out_color = vec4(scene, 1.0);
        return;
    }

    // Reconstruct the world-space view ray. z = 1 is the far plane in Vulkan NDC; w-divide then
    // subtract the camera to get a direction. Doing this from the inverse view-projection means the
    // ray is correct for any projection the camera happens to have, including the editor's.
    vec2 ndc = (gl_FragCoord.xy / vec2(textureSize(scene_color, 0))) * 2.0 - 1.0;
    vec4 far = sky.inv_view_proj * vec4(ndc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - sky.camera_pos.xyz);

    // The physical LUT carries the broad atmosphere and clouds. It cannot resolve the ~0.7 degree
    // sun disc, so the disc is added separately and muted where the authored cloud slab covers it.
    // That disc is presentation-only: direct sunlight still comes from DirectionalLight.
    const vec3 atmosphere = texture(skyview_lut, skyview_uv_from_direction(dir)).rgb;
    const float disc_visibility = 1.0 - clouds(dir).x;
    out_color = vec4(atmosphere + sky_sun_disc_radiance(dir, kSceneSolarScale) * disc_visibility,
                     1.0);
}
