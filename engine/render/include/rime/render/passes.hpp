// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

#include "rime/core/math/mat.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"

// The reusable pass library (M5.6, ADR-0022): the three passes of the forward-PBR frame —
//
//     depth pre-pass  →  forward PBR (HDR)  →  tonemap + sRGB encode (LDR)
//
// Each pass object is created ONCE (it owns its pipelines and shaders); each frame it *declares*
// itself into a RenderGraph with add(). The passes are deliberately independent of the ECS — they
// consume a flat SceneDrawData (draw items + uniform buffers someone else filled), so a test can
// drive them without a World and the SceneRenderer (scene_renderer.hpp) is just the standard
// composer, not the only one. The tonemap pass is the template for every post pass to come
// (M5.8's ten-line vignette rides the same shape: sample an input, write a target, fullscreen
// triangle).
//
// Why depth pre-pass + forward (and not deferred), why these BRDF terms, why HDR-then-tonemap:
// ADR-0022 records the decisions, docs/math/pbr.md derives the math.
namespace rime::render {

// ── The frame's fixed format choices ──────────────────────────────────────────────────────────
// One place, so every pipeline/attachment agrees. HDR scene color is RGBA16Float: radiance needs
// range above 1.0, and float16 carries it at half the bandwidth of float32 (M5.1b brought the
// format). Depth is 32-bit float — the modern default, no stencil until a pass needs one. LDR
// output is plain RGBA8Unorm with the sRGB encode done in the tonemap shader (see tonemap.frag
// for why that is explicit rather than a hardware Srgb target).
inline constexpr rhi::Format kHdrFormat = rhi::Format::RGBA16Float;
inline constexpr rhi::Format kDepthFormat = rhi::Format::D32Float;
inline constexpr rhi::Format kLdrFormat = rhi::Format::RGBA8Unorm;

// Fixed light budgets for the per-frame uniform block. Uniform blocks are statically sized, so
// the caps are compile-time; 4 suns and 16 point lights are generous for M5 scenes. Past these,
// lights are dropped (the scene renderer warns once) — culling lights *per view* into a bigger
// structure is the M10 many-lights problem, and these constants are where it will splice in.
inline constexpr std::uint32_t kMaxDirectionalLights = 4;
inline constexpr std::uint32_t kMaxPointLights = 16;

// ── GPU uniform-block mirrors (std140) ────────────────────────────────────────────────────────
// C++ twins of the uniform blocks in pbr_forward.vert/.frag and depth_only.vert. std140 is the
// GLSL block layout with *fixed, documented* rules — the only sane choice for CPU-written blocks.
// The classic std140 trap is vec3 (it pads to 16 bytes and drags the next member with it); these
// structs dodge it by construction: every member is a mat4, a [4]-array, or a 32-byte struct, so
// C++'s natural layout and std140 agree exactly. The static_asserts below are the tripwire if
// anyone edits one side without the other.
struct GpuDirectionalLight {
    float direction[4] = {0.0f, 0.0f, -1.0f, 0.0f}; // xyz = travel direction (world), w unused
    float radiance[4] = {0.0f, 0.0f, 0.0f, 0.0f};   // rgb = color * intensity (linear)
};

struct GpuPointLight {
    float position[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // xyz = world position, w = falloff radius
    float radiance[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // rgb = color * intensity (linear)
};

struct GpuFrameUniforms {
    core::Mat4 view_proj;                           // clip-from-world
    float camera_pos[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // xyz = eye (world)
    float ambient[4] = {0.0f, 0.0f, 0.0f, 0.0f};    // rgb = constant ambient radiance
    std::uint32_t light_counts[4] = {0, 0, 0, 0};   // x = directional, y = point
    GpuDirectionalLight dir_lights[kMaxDirectionalLights];
    GpuPointLight point_lights[kMaxPointLights];
};

struct GpuDrawUniforms {
    core::Mat4 model;                               // world-from-object
    core::Mat4 normal_matrix;                       // inverse-transpose of model (for normals)
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // linear factor
    float params[4] = {0.0f,
                       0.5f,
                       1.0f,
                       1.0f}; // x metallic, y roughness, z normal_scale, w AO strength
    float emissive[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // rgb = emissive factor (linear), w unused (M6.4)
};

static_assert(std::is_standard_layout_v<GpuFrameUniforms> && sizeof(GpuFrameUniforms) == 752 &&
                  offsetof(GpuFrameUniforms, dir_lights) == 112 &&
                  offsetof(GpuFrameUniforms, point_lights) == 240,
              "GpuFrameUniforms no longer matches the std140 FrameUniforms block in the shaders");
static_assert(std::is_standard_layout_v<GpuDrawUniforms> && sizeof(GpuDrawUniforms) == 176 &&
                  offsetof(GpuDrawUniforms, base_color) == 128 &&
                  offsetof(GpuDrawUniforms, params) == 144 &&
                  offsetof(GpuDrawUniforms, emissive) == 160,
              "GpuDrawUniforms no longer matches the std140 DrawUniforms block in the shaders");

// Per-draw uniform data lives as SLICES of one buffer, re-bound at a new offset per draw
// (bind_uniform_buffer's documented pattern). 256 is the universally-valid uniform-offset
// alignment (minUniformBufferOffsetAlignment ≤ 256 on every Vulkan device), so slice i sits at
// i * kDrawUniformStride without querying limits.
inline constexpr std::uint32_t kDrawUniformStride = 256;
static_assert(sizeof(GpuDrawUniforms) <= kDrawUniformStride);

// ── The draw list ─────────────────────────────────────────────────────────────────────────────
// One drawable thing, fully resolved to registry ids + a world matrix. This is what scene
// extraction produces and what the geometry passes consume — deliberately flat and GPU-agnostic.
struct DrawItem {
    MeshId mesh = kInvalidMeshId;
    MaterialId material = kInvalidMaterialId;
    core::Mat4 model;
};

// Everything the geometry passes need to record their draws. The spans/pointers are BORROWED and
// must stay alive until RenderGraph::execute() returns (pass lambdas capture this struct by
// value, but not the arrays it points into) — the SceneRenderer keeps them as members for
// exactly that reason. The five `*_textures` spans are each parallel to `draws`: the per-draw map
// for that slot with the material's fallback already resolved (1x1 white, or flat-normal for the
// normal slot), so recording never branches on "has a texture?". `frame_ubo` holds one
// GpuFrameUniforms; `draw_ubo` holds one GpuDrawUniforms slice per draw at kDrawUniformStride.
struct SceneDrawData {
    const MeshRegistry* meshes = nullptr;
    std::span<const DrawItem> draws = {};
    std::span<const rhi::TextureHandle> base_color_textures = {};
    std::span<const rhi::TextureHandle> metallic_roughness_textures = {};
    std::span<const rhi::TextureHandle> normal_textures = {};
    std::span<const rhi::TextureHandle> occlusion_textures = {};
    std::span<const rhi::TextureHandle> emissive_textures = {};
    rhi::BufferHandle frame_ubo;
    rhi::BufferHandle draw_ubo;
    rhi::SamplerHandle material_sampler;
};

// ── Depth pre-pass ────────────────────────────────────────────────────────────────────────────
// Rasterize every opaque draw with NO fragment shader and NO color attachment, writing only
// depth. The forward pass then depth-tests with CompareOp::Equal and shades each pixel exactly
// once — overdraw pays a vertex transform here instead of a full BRDF there. The win is
// workload-dependent (measure!), which is why the pass is optional per frame.
class DepthPrepass {
public:
    explicit DepthPrepass(rhi::Device& device);
    ~DepthPrepass();

    DepthPrepass(const DepthPrepass&) = delete;
    DepthPrepass& operator=(const DepthPrepass&) = delete;

    // Declare the pass: clears `depth` and fills it with the scene's nearest surfaces.
    void add(RenderGraph& graph, RGTexture depth, const SceneDrawData& data) const;

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::PipelineHandle pipeline_;
};

// ── Forward PBR pass ──────────────────────────────────────────────────────────────────────────
// Shade every draw with the Cook-Torrance BRDF into the HDR target. Two depth modes, two baked
// pipelines: after a pre-pass it LOADS depth and tests Equal without writing (read-only depth —
// the graph rewards that with weaker ordering); standalone it clears and writes with Less like
// any classic forward renderer.
class ForwardPbrPass {
public:
    explicit ForwardPbrPass(rhi::Device& device);
    ~ForwardPbrPass();

    ForwardPbrPass(const ForwardPbrPass&) = delete;
    ForwardPbrPass& operator=(const ForwardPbrPass&) = delete;

    void add(RenderGraph& graph,
             RGTexture hdr,
             RGTexture depth,
             bool depth_prepassed,
             const SceneDrawData& data) const;

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_after_prepass_; // depth Load + Equal + no write
    rhi::PipelineHandle pipeline_standalone_;    // depth Clear + Less + write
};

// ── Tonemap pass ──────────────────────────────────────────────────────────────────────────────
// Fullscreen triangle: read HDR radiance, apply the ACES curve, sRGB-encode into the LDR target.
// The shape every post pass copies (declare input sampled, output as the color attachment, draw 3
// vertices).
class TonemapPass {
public:
    explicit TonemapPass(rhi::Device& device);
    ~TonemapPass();

    TonemapPass(const TonemapPass&) = delete;
    TonemapPass& operator=(const TonemapPass&) = delete;

    void add(RenderGraph& graph, RGTexture hdr, RGTexture ldr) const;

private:
    rhi::Device& device_;
    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_; // texelFetch ignores filtering, but a binding needs a sampler
};

} // namespace rime::render
