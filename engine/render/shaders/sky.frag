// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The sky — the physical sky-view LUT plus a separately resolved sun disc, composited over the
// scene wherever nothing was drawn.
//
// The LUT is the same physical single-scattering-plus-bounded-MS body that lights forward shading,
// DDGI and SSR. It deliberately excludes the sub-texel solar disc; the disc stays analytic here so
// its edge remains sharp and cannot double-light geometry.
//
// Aerial perspective (this file, geometry branch of main): distant geometry is fogged by the same
// atmosphere that paints the sky. A froxel volume (Hillaire 2020) is not built -- see the comment
// at the geometry branch for the cheaper closed form used instead and what it cannot do.
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

// ---- aerial perspective -----------------------------------------------------------------------
// Looking through kilometres of air, a distant hill is (a) dimmed, because part of its light is
// scattered out of the ray (transmittance T) and (b) tinted, because air in the ray scatters
// sunlight INTO it (in-scattering). Hillaire (2020) stores both in a camera-aligned froxel volume.
// Here we use the closed form that is exact for a homogeneous medium: if the source term along the
// ray were constant, L_in(d) = L_inf * (1 - T(d)), where L_inf is the sky radiance seen along that
// ray all the way out -- which is precisely what the sky-view LUT already holds. So one LUT tap plus
// one analytic optical depth replaces the volume. What this gives up is the variation of the source
// along the ray (a sun-lit haze layer in front of a shadowed one), which matters for god rays and
// valley fog and not for ordinary distant-hill haze.
//
// The medium is NOT homogeneous in height: density falls as exp(-h/H). For a ray leaving height h0
// with vertical slope y, the optical depth to distance d integrates in closed form,
//   tau = sigma0 * H * exp(-h0/H) * (1 - exp(-y d / H)) / y        (-> sigma0 d exp(-h0/H) as y->0)
// evaluated separately for the Rayleigh (H ~ 8 km) and Mie (H ~ 1.2 km) species. sigma0 arrives
// premultiplied by SkyParams::aerial_density, the art control for how much haze a game scene wants.
float aerial_species_tau(float sigma_unit, float h0, float scale_h, float y, float d) {
    const float base = exp(-max(h0, 0.0) / scale_h);
    // Use the y -> 0 limit when the ray is nearly level; the general expression is 0/0 there.
    if (abs(y) < 1e-3) {
        return sigma_unit * d * base;
    }
    return sigma_unit * scale_h * base * (1.0 - exp(-y * d / scale_h)) / y;
}

vec3 aerial_perspective(vec3 scene, float depth, ivec2 px) {
    // Cheap exit when disabled, so a zero density is a no-op rather than a multiply by one.
    if (sky.aerial_rayleigh.x + sky.aerial_rayleigh.y + sky.aerial_rayleigh.z +
            sky.aerial_mie.x + sky.aerial_mie.y + sky.aerial_mie.z <= 0.0) {
        return scene;
    }
    const vec2 ndc = (vec2(px) + 0.5) / vec2(textureSize(scene_color, 0)) * 2.0 - 1.0;
    const vec4 wp = sky.inv_view_proj * vec4(ndc, depth, 1.0);
    const vec3 delta = wp.xyz / wp.w - sky.camera_pos.xyz;
    const float d = length(delta);
    const vec3 dir = delta / max(d, 1e-4);

    vec3 tau = vec3(0.0);
    for (int c = 0; c < 3; ++c) {
        tau[c] = aerial_species_tau(sky.aerial_rayleigh[c], sky.camera_pos.y,
                                    sky.aerial_rayleigh.w, dir.y, d) +
                 aerial_species_tau(sky.aerial_mie[c], sky.camera_pos.y, sky.aerial_mie.w, dir.y, d);
    }
    const vec3 T = exp(-tau);

    // L_inf: the sky along this ray. Rays at or below the horizon would sample the LUT's dark
    // ground-hit region, but the haze in front of a distant hill is horizon-coloured air, so the
    // lookup is clamped to just above the horizon. The scene-resolution cloud layer fades to zero
    // there, so no cloud is smeared across the landscape.
    vec3 sample_dir = dir;
    sample_dir.y = max(sample_dir.y, 0.005);
    const vec3 sky_inf = texture(skyview_lut, skyview_uv_from_direction(normalize(sample_dir))).rgb;
    return scene * T + sky_inf * (vec3(1.0) - T);
}

void main() {
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const vec3 scene = texelFetch(scene_color, px, 0).rgb;
    const float depth = texelFetch(scene_depth, px, 0).r;

    // Anything the forward pass drew keeps its colour EXACTLY. The sky only fills what the depth
    // buffer says is still at the far plane -- so with an opaque scene this pass is a no-op on every
    // shaded pixel, which is what makes "sky on" safe to compare against "sky off".
    if (depth < 1.0) {
        out_color = vec4(aerial_perspective(scene, depth, px), 1.0);
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
    // The same transmittance the DirectionalLight is scaled by on the CPU (sky_sun_cloud_transmittance
    // and its sky.cpp mirror), so cloud over the sun dims the disc and the lighting as one.
    const float disc_visibility = sky_sun_cloud_transmittance(dir);
    out_color = vec4(atmosphere + sky_sun_disc_radiance_physical(dir) * disc_visibility, 1.0);
}
