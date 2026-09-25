// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The virtual-geometry visibility pass, vertex stage (M18 step 2). **Vertex pulling**: no vertex
// buffer is bound; an identity index buffer is bound so gl_VertexIndex is the global cluster
// index. The vertex shader reads the per-draw record (std430 storage buffer indexed by
// gl_DrawIDARB) and fetches position from the cluster storage buffers the resolve pass also reads.
// The triangle index and packed visibility ID leave as flat varyings.
//
// Identity index buffer trick: gl_PrimitiveID in a fragment shader needs the Vulkan
// geometryShader feature, which MoltenVK does not have, so we derive the triangle from
// gl_VertexIndex using the per-draw index_base instead.
#version 450
#extension GL_ARB_shader_draw_parameters : require

struct DrawRecord {
    uint id_lo;
    uint id_hi;
    uint index_base;
    uint vertex_base;
    uint stride_words;
    uint cluster_slot;
    uint pad[2];
};

layout(std430, set = 0, binding = 0) readonly buffer Vertices { uint words[]; } vertices;
layout(std430, set = 0, binding = 1) readonly buffer Indices { uint indices[]; } index_data;
layout(std430, set = 0, binding = 2) readonly buffer DrawRecords {
    DrawRecord records[];
} draw_records;

layout(push_constant) uniform Pc {
    mat4 mvp; // clip-from-object, same for every cluster in the request
} pc;

layout(location = 0) flat out uvec2 out_id;
layout(location = 1) flat out uint out_triangle;

// Same coverage as any other draw of these positions with this matrix (the step-2 parity proof
// compares against a conventional forward draw pixel for pixel).
invariant gl_Position;

void main() {
    DrawRecord rec = draw_records.records[gl_DrawIDARB];
    uint global_index = uint(gl_VertexIndex);
    uint v = rec.vertex_base + index_data.indices[global_index];
    uint w = v * rec.stride_words;
    vec3 p = vec3(uintBitsToFloat(vertices.words[w]),
                  uintBitsToFloat(vertices.words[w + 1u]),
                  uintBitsToFloat(vertices.words[w + 2u]));
    out_id = uvec2(rec.id_lo, rec.id_hi);
    out_triangle = (global_index - rec.index_base) / 3u;
    gl_Position = pc.mvp * vec4(p, 1.0);
}
