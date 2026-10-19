// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The FX particle pass, vertex stage (m13.1a, ADR-0035 §5 fx1a). Expands one camera-facing quad
// per particle out of NOTHING — no vertex buffer, no index buffer, no per-particle mesh.
//
// WHY THE QUAD IS SYNTHESISED RATHER THAN FETCHED. A billboard's four corners are the same four
// corners for every particle; only the centre, the size and the colour differ. Uploading six
// vertices per particle would multiply the bandwidth by six to carry information that is already
// in `gl_VertexIndex`. So the draw is `draw(6, particle_count)`: the vertex index picks a corner,
// the instance index picks the particle, and the particle array is a storage buffer the vertex
// stage reads directly. This is the standard modern shape and it is why the pass needs no vertex
// layout at all.
//
// WHY THE BILLBOARD BASIS COMES FROM THE CPU. `right` and `up` are the camera's world-space axes,
// pushed once per pass. Deriving them in the shader from the view matrix would work too, but it
// would re-derive the same two vectors for every vertex of every particle, and it would put a
// second copy of "which way is the camera facing" in a place that can silently disagree with the
// one the rest of the frame used.
#version 450

// Must match GpuParticle in fx_pass.hpp — position_size.xyz is the world centre, .w the half-size
// in metres; color_alpha.rgb is linear radiance, .a the fade.
struct Particle {
    vec4 position_size;
    vec4 color_alpha;
};

layout(std430, binding = 0) readonly buffer Particles {
    Particle particles[];
} fx;

layout(push_constant) uniform Pc {
    mat4 view_proj;  // clip-from-world
    vec4 camera_right; // xyz = camera right in world space
    vec4 camera_up;    // xyz = camera up in world space
} pc;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) flat out int out_mode;
layout(location = 3) flat out vec2 out_noise_origin;

void main() {
    // Two triangles from six indices, in the corner order (-1,-1) (1,-1) (-1,1) / (-1,1) (1,-1)
    // (1,1). Written as a lookup rather than as bit arithmetic because a reader can check it.
    const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0),
                                     vec2(1.0, -1.0),
                                     vec2(-1.0, 1.0),
                                     vec2(-1.0, 1.0),
                                     vec2(1.0, -1.0),
                                     vec2(1.0, 1.0));

    const Particle p = fx.particles[gl_InstanceIndex];
    const vec2 corner = kCorners[gl_VertexIndex];

    const vec3 centre = p.position_size.xyz;
    const float half_size = p.position_size.w;
    const int mode = pc.camera_right.w > 1.5 ? 2 : (pc.camera_right.w > 0.5 ? 1 : 0);
    // Flame needs vertical room for a tongue, rather than the square puff used by dust/smoke.
    // Keep the original additive geometry expression intact for its pixel regression bridge.
    vec3 world = centre + pc.camera_right.xyz * (corner.x * half_size) +
                 pc.camera_up.xyz * (corner.y * half_size);
    if (mode == 2) {
        world = centre + pc.camera_right.xyz * (corner.x * half_size * 0.55) +
                pc.camera_up.xyz * (corner.y * half_size * 1.5);
    }

    gl_Position = pc.view_proj * vec4(world, 1.0);
    out_uv = corner;            // [-1,1]² — the fragment stage's radial falloff domain
    out_color = p.color_alpha;
    out_mode = mode;
    // Continuous world-space offsets give nearby puffs related but different detail, without a
    // clock, random frame seed, or quantized position hash that pops when a puff moves.
    out_noise_origin = centre.xz * 0.71 + vec2(centre.y * 0.37, half_size * 0.83);
}
