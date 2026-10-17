// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#ifndef RIME_DDGI_SUPPORT_GLSL
#define RIME_DDGI_SUPPORT_GLSL

// A scrolling atlas has a whole cell of snap phase. Its changing outer layers must have ZERO
// influence at recenter, even on receivers beyond its geometric bounds. A ramp against snapped
// atlas bounds would itself jump, so centre this envelope on the continuous camera instead.
// H-s is the symmetric intersection of all possible floor-snapped volumes; a one-cell inward
// ramp excludes edge interpolation too. The margin reserves the SSR surface-query bias.
// Thin authored grids have no safe interior: when explicitly enabled they return fallback.
float ddgi_spatial_influence(vec3 world_pos, vec3 camera_pos) {
    if (ddgi.enabled_pad.y == 0u)
        return 1.0;
    const float spacing = max(ddgi.grid_origin_spacing.w, 1e-4);
    const vec3 half_extent = (vec3(ddgi.grid_dims_perrow.xyz) - 1.0) * spacing * 0.5;
    const float margin = max(0.01, spacing * 0.025);
    const vec3 outer = half_extent - vec3(spacing + margin);
    if (any(lessThanEqual(outer, vec3(0.0))))
        return 0.0;
    const vec3 edge = outer - abs(world_pos - camera_pos);
    const vec3 axis = smoothstep(vec3(0.0), vec3(spacing), edge);
    return min(axis.x, min(axis.y, axis.z));
}
#endif
