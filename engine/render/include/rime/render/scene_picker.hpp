// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "rime/ecs/world.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

// The **ID-buffer picker** (M9.6): "which entity is under this viewport pixel?", answered by the
// GPU instead of CPU ray casting. The technique — the classic editor pick pass:
//
//   1. Re-draw the scene's drawable entities (the same WorldTransform+MeshRef+MaterialRef set the
//      SceneRenderer extracts, through the same camera), but write each draw's *id* into an
//      R32Uint target instead of shading. The depth test (Less, like the forward pass) makes the
//      NEAREST surface's id survive at each pixel — occlusion answered by hardware.
//   2. Read the one pixel back. Its integer is the answer: 0 = empty space (the clear value), k =
//      the k-1'th draw, mapped to its entity through the extraction's parallel entity array.
//
// Why the GPU and not the physics raycast: picking must agree with what the user *sees* — every
// drawn entity is pickable, including scenery with no collider — and it inherits the renderer's
// exact projection for free. The physics raycast remains the gameplay path (M7).
//
// Two cost tricks keep a pick ~free:
//   * The target is **1×1**. Rendering "the whole frame, but only this pixel" is a viewport
//     shift: point the viewport at the full W×H rectangle but offset by (−x, −y), so the pixel of
//     interest lands on the single-texel target; the 1×1 scissor (= the render area) discards the
//     rest at rasterization. Vertex work still runs (fine — editor scenes), fragment work is one
//     pixel, and the readback is 4 bytes.
//   * The readback is **asynchronous** (the s1.1 SubmitTicket seam, like FrameStreamer):
//     begin_pick() submits without waiting; try_resolve() polls the ticket. The host's render
//     loop keeps streaming, and the answer is a frame late — the documented, acceptable pick
//     latency (the m9.6 plan).
//
// The empty-space sentinel is 0 by construction: the RHI's ClearColor is float-shaped, and an
// all-zero float clear is bit-identical to an all-zero uint clear (IEEE 754 0.0f is all zero
// bits), so an integer target can be cleared "nothing" through the ordinary clear path. Draw ids
// are therefore 1-based.
namespace rime::render {

class ScenePicker {
public:
    // Bakes the pick pipeline and creates the 4-byte readback buffer (the 1×1 render targets are
    // graph transients, recycled across picks by the graph's cache). `meshes` is borrowed for the
    // picker's lifetime (draws bind its buffers).
    ScenePicker(rhi::Device& device, const MeshRegistry& meshes);
    ~ScenePicker(); // drains any in-flight pick before destroying its resources (s1.1 contract)

    ScenePicker(const ScenePicker&) = delete;
    ScenePicker& operator=(const ScenePicker&) = delete;

    // Start an asynchronous pick at pixel (x, y) of an `extent`-sized view of `world` — the same
    // extent the viewport renders at, so editor click coordinates map 1:1. Extracts the drawable
    // set + camera NOW (the world may change before the GPU finishes; the captured entity map
    // keeps the resolve honest), declares the pass into a private graph, and submits without
    // waiting. Out-of-bounds pixels, a camera-less world, or an empty scene short-circuit to an
    // immediate miss — no GPU work, but still exactly one result to collect.
    //
    // One pick in flight at a time: if one is already pending, its result is waited out and
    // DISCARDED (latest wins). Callers that want every request answered (the editor host does)
    // should queue and begin the next only after try_resolve() delivered.
    void begin_pick(ecs::World& world, rhi::Extent2D extent, std::int32_t x, std::int32_t y);

    // Poll for the pending pick's answer. nullopt = still on the GPU (render on, ask again next
    // frame). Otherwise the picked entity — ecs::kNullEntity for empty space — and the picker is
    // ready for the next begin_pick(). Exactly one non-null result per begin_pick.
    [[nodiscard]] std::optional<ecs::Entity> try_resolve();

    [[nodiscard]] bool pending() const noexcept { return pending_; }

private:
    rhi::Device& device_;
    const MeshRegistry& meshes_;
    RenderGraph graph_; // private: a pick is its own tiny frame, independent of the app's graph

    rhi::ShaderHandle vertex_shader_;
    rhi::ShaderHandle fragment_shader_;
    rhi::PipelineHandle pipeline_;
    rhi::BufferHandle readback_; // 4 bytes, host-visible

    rhi::SubmitTicket ticket_;
    bool pending_ = false;
    bool immediate_miss_ = false; // resolved on the CPU (out of bounds / no camera / no draws)

    // Draw index → source entity for the pick in flight (moved out of the extraction), held until
    // resolve so a world change mid-flight cannot skew the mapping.
    std::vector<ecs::Entity> pick_entities_;
};

} // namespace rime::render
