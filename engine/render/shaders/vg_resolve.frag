// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The virtual-geometry material resolve (M18 step 2), a fullscreen pass over the visibility
// target. Per pixel: decode the visibility ID (ABI version 2: triangle [6:0], slot [22:7],
// generation [27:23], version [31:28]), find the cluster record by slot, fetch the triangle's
// three vertices from the same storage buffers the visibility pass pulled them from, and
// reconstruct everything a forward pass would have got from the rasterizer's interpolators.
//
// ANALYTIC BARYCENTRICS (the standard visibility-buffer technique — Schied & Dachsbacher 2015,
// "Deferred Attribute Interpolation"; the form below is the one Wihlidal popularised for
// visibility buffers). The rasterizer is gone, so the shader re-derives the pixel's position
// inside the triangle. Project the three vertices to clip space again. In NDC (after the divide)
// the screen-space barycentrics b_i are AFFINE in the pixel position, so each has a constant
// gradient: for vertex 0, ∇b0 = (n1.y - n2.y, n2.x - n1.x) / det, where det is the triangle's
// signed NDC area (and cyclically for 1 and 2). Screen-space barycentrics are not what the
// forward pass interpolates with, though — attributes are linear in OBJECT space, so the
// perspective-correct weight is λ_i = (b_i / w_i) / Σ_j (b_j / w_j): interpolate b/w and 1/w
// linearly on screen and divide, the same thing hardware perspective-correct interpolation does.
//
// UV GRADIENTS for texture LOD come out of the same algebra for free: step the pixel one to the
// right (Δndc.x = 2 / width) and one down, recompute λ at each, and difference. That is exactly
// the one-pixel finite difference dFdx/dFdy would have taken in a forward pass, but with no 2x2
// quad — which matters, because neighbouring pixels here may belong to different triangles or
// materials, so hardware derivatives across the quad would be garbage. textureGrad with these
// explicit gradients is also legal inside the per-material branch below, where implicit-derivative
// texture() is not (non-uniform control flow).
#version 450

layout(set = 0, binding = 0) uniform usampler2D visibility;
layout(std430, set = 0, binding = 1) readonly buffer Vertices { uint words[]; } vertices;
layout(std430, set = 0, binding = 2) readonly buffer Indices { uint indices[]; } index_data;

struct Cluster {
    uint vertex_base;
    uint index_base;
    uint triangle_count;
    uint material_slot;
    uint generation;
    uint valid;
    uint pad0;
    uint pad1;
};
layout(std430, set = 0, binding = 3) readonly buffer Clusters { Cluster clusters[]; } table;

// One albedo texture per material slot. Separate bindings rather than a sampler array: indexing
// an array with a per-pixel value needs the non-uniform-indexing feature; a branch does not.
layout(set = 0, binding = 4) uniform sampler2D albedo0;
layout(set = 0, binding = 5) uniform sampler2D albedo1;
layout(set = 0, binding = 6) uniform sampler2D albedo2;
layout(set = 0, binding = 7) uniform sampler2D albedo3;

layout(push_constant) uniform Pc {
    mat4 mvp;            // clip-from-object, the same matrix the visibility pass drew with
    vec2 viewport;       // target size in pixels
    uint cluster_count;  // entries in `table`
    uint stride_words;   // cooked vertex stride in u32 words
    uint uv_word;        // word offset of the UV inside a vertex
    uint material_count; // bound albedo textures (1..4)
} pc;

layout(location = 0) out uint out_material;  // material_slot + 1; 0 = empty; ~0u = stale/bad
layout(location = 1) out vec4 out_uv;        // (u, v, du/dx, dv/dy)
layout(location = 2) out vec4 out_albedo;

const uint kStale = 0xffffffffu;

vec3 position_of(uint v) {
    uint w = v * pc.stride_words;
    return vec3(uintBitsToFloat(vertices.words[w]), uintBitsToFloat(vertices.words[w + 1u]),
                uintBitsToFloat(vertices.words[w + 2u]));
}

vec2 uv_of(uint v) {
    uint w = v * pc.stride_words + pc.uv_word;
    return vec2(uintBitsToFloat(vertices.words[w]), uintBitsToFloat(vertices.words[w + 1u]));
}

struct Barycentrics {
    vec3 lambda; // perspective-correct weights at the pixel centre
    vec3 ddx;    // change in lambda one pixel to the right
    vec3 ddy;    // change in lambda one pixel down
};

Barycentrics analytic_barycentrics(vec4 c0, vec4 c1, vec4 c2, vec2 ndc) {
    vec3 inv_w = 1.0 / vec3(c0.w, c1.w, c2.w);
    vec2 n0 = c0.xy * inv_w.x;
    vec2 n1 = c1.xy * inv_w.y;
    vec2 n2 = c2.xy * inv_w.z;
    float inv_det = 1.0 / determinant(mat2(n2 - n1, n0 - n1));
    // Gradients of b_i / w_i over NDC.
    vec3 gx = vec3(n1.y - n2.y, n2.y - n0.y, n0.y - n1.y) * inv_det * inv_w;
    vec3 gy = vec3(n2.x - n1.x, n0.x - n2.x, n1.x - n0.x) * inv_det * inv_w;
    float gx_sum = gx.x + gx.y + gx.z;
    float gy_sum = gy.x + gy.y + gy.z;

    // b_i / w_i at the pixel, from its value at vertex 0 (b = (1,0,0) there) plus the gradient.
    vec2 d = ndc - n0;
    vec3 num = vec3(inv_w.x, 0.0, 0.0) + d.x * gx + d.y * gy;
    float den = inv_w.x + d.x * gx_sum + d.y * gy_sum; // interpolated 1/w

    Barycentrics b;
    b.lambda = num / den;
    // One pixel is 2/size in NDC (Vulkan's viewport maps NDC y = -1 to row 0, as gl_FragCoord.y
    // counts, so no sign flip).
    vec2 px = 2.0 / pc.viewport;
    b.ddx = (num + gx * px.x) / (den + gx_sum * px.x) - b.lambda;
    b.ddy = (num + gy * px.y) / (den + gy_sum * px.y) - b.lambda;
    return b;
}

void main() {
    out_material = 0u;
    out_uv = vec4(0.0);
    out_albedo = vec4(0.0);

    uint id = texelFetch(visibility, ivec2(gl_FragCoord.xy), 0).r;
    if (id == 0u) {
        return; // the ABI's empty sentinel: nothing was drawn here
    }
    uint version = id >> 28;
    uint triangle = id & 0x7fu;
    uint slot = (id >> 7) & 0xffffu;
    uint generation = (id >> 23) & 0x1fu;
    // Every way a covered pixel can fail to resolve is VISIBLE (kStale), never silently empty.
    if (version != 2u || slot >= pc.cluster_count) {
        out_material = kStale;
        return;
    }
    Cluster c = table.clusters[slot];
    if (c.valid == 0u || c.generation != generation || triangle >= c.triangle_count) {
        out_material = kStale;
        return;
    }

    uint i = c.index_base + triangle * 3u;
    uint v0 = c.vertex_base + index_data.indices[i];
    uint v1 = c.vertex_base + index_data.indices[i + 1u];
    uint v2 = c.vertex_base + index_data.indices[i + 2u];

    vec2 ndc = gl_FragCoord.xy / pc.viewport * 2.0 - 1.0;
    Barycentrics b = analytic_barycentrics(pc.mvp * vec4(position_of(v0), 1.0),
                                           pc.mvp * vec4(position_of(v1), 1.0),
                                           pc.mvp * vec4(position_of(v2), 1.0), ndc);

    mat3x2 uvs = mat3x2(uv_of(v0), uv_of(v1), uv_of(v2));
    vec2 uv = uvs * b.lambda;
    vec2 duv_dx = uvs * b.ddx;
    vec2 duv_dy = uvs * b.ddy;

    out_material = c.material_slot + 1u;
    out_uv = vec4(uv, duv_dx.x, duv_dy.y);
    if (c.material_slot >= pc.material_count) {
        return; // no texture for this slot: albedo stays 0, the material id still says which
    }
    switch (c.material_slot) {
        case 0u: out_albedo = textureGrad(albedo0, uv, duv_dx, duv_dy); break;
        case 1u: out_albedo = textureGrad(albedo1, uv, duv_dx, duv_dy); break;
        case 2u: out_albedo = textureGrad(albedo2, uv, duv_dx, duv_dy); break;
        default: out_albedo = textureGrad(albedo3, uv, duv_dx, duv_dy); break;
    }
}
