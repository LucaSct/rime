// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The virtual-geometry visibility pass, vertex stage (M18 step 2). **Vertex pulling**: no vertex
// or index buffer is bound. Each invocation is one triangle corner, gl_VertexIndex = 3 * triangle
// + corner, and fetches its own index and position from the cluster storage buffers the resolve
// pass also reads. The triangle index leaves as a flat varying — the portable replacement for
// gl_PrimitiveID in a fragment shader, which Vulkan gates behind the geometryShader feature.
// Must match VisibilityPush in virtual_geometry_visibility_pass.cpp.
#version 450

layout(std430, set = 0, binding = 0) readonly buffer Vertices { uint words[]; } vertices;
layout(std430, set = 0, binding = 1) readonly buffer Indices { uint indices[]; } index_data;

layout(push_constant) uniform Pc {
    mat4 mvp;          // clip-from-object
    uvec2 id;          // packed v3 visibility ID (lo, hi) with triangle = 0; frag ORs it in
    uint index_base;   // this cluster's first index in `indices`
    uint vertex_base;  // this cluster's first vertex in `vertices`
    uint stride_words; // cooked vertex stride in u32 words; position is the first three
} pc;

layout(location = 0) flat out uint out_triangle;

// Same coverage as any other draw of these positions with this matrix (the step-2 parity proof
// compares against a conventional forward draw pixel for pixel).
invariant gl_Position;

void main() {
    uint v = pc.vertex_base + index_data.indices[pc.index_base + uint(gl_VertexIndex)];
    uint w = v * pc.stride_words;
    vec3 p = vec3(uintBitsToFloat(vertices.words[w]),
                  uintBitsToFloat(vertices.words[w + 1u]),
                  uintBitsToFloat(vertices.words[w + 2u]));
    out_triangle = uint(gl_VertexIndex) / 3u;
    gl_Position = pc.mvp * vec4(p, 1.0);
}
