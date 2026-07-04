// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "rime/rhi/types.hpp"

// The command-recording seam. A CommandBuffer is an *encoder*: you describe a sequence of GPU work
// — start a render, bind state, draw, copy — and the backend records it into a real command list to
// be submitted later. Recording is explicit and ordered, which is exactly the shape the M5 render
// graph wants to drive (each pass records into a command buffer). Nothing here mentions Vulkan;
// the backend translates each call to vkCmd*.
//
// A deliberate simplification for M3: image-layout transitions and pipeline barriers are inserted
// *inside* the backend (e.g. when a texture becomes a render target, then a copy source). Exposing
// barriers in the public API is a power-user feature we defer until the render graph needs explicit
// control; until then the RHI keeps the caller out of the synchronization weeds.
namespace rime::rhi {

// One color attachment for a dynamic-rendering pass: the texture to draw into and what to do with
// its contents on entry/exit. (Multiple color targets arrive with the render graph.)
struct ColorAttachment {
    TextureHandle target;
    LoadOp load_op = LoadOp::Clear;
    StoreOp store_op = StoreOp::Store;
    ClearColor clear = {};
};

// The depth(-stencil) attachment for a dynamic-rendering pass: the depth texture to test/write
// against and what to do with its contents on entry/exit. This is what makes opaque 3-D draw
// correctly — without it, the painter's-order last-drawn fragment wins. `clear_depth = 1.0` clears
// to the far plane (the right default for a `Less` depth test). `clear_stencil` is carried now but
// only takes effect once the stencil aspect is wired in the cross-section brick; depth is enough
// for 3-D.
struct DepthStencilAttachment {
    TextureHandle target;
    LoadOp load_op = LoadOp::Clear;
    StoreOp store_op = StoreOp::DontCare; // depth is usually transient; Store only if sampled later
    float clear_depth = 1.0f;
    std::uint32_t clear_stencil = 0;
};

// Describes a dynamic-rendering scope. The render area defaults to the full target extent (the
// backend reads it from the first color target), so the common case needs only the color
// attachment. `depth_stencil` is optional: leave it unset for a flat 2-D pass (the M3
// triangle/quad), set it to enable depth-tested 3-D. The pipeline bound inside the pass must
// agree (GraphicsPipelineDesc depth fields) — depth on here ⇔ depth_test + matching depth_format
// on the pipeline.
//
// Multiple render targets (M5.1b): fill `colors` with 2..kMaxColorAttachments attachments to have
// one pass write several images at once (the fragment shader's location-i output lands in
// colors[i]); it wins over `color` when non-empty, and the bound pipeline must declare matching
// `color_formats`. All attachments share one extent. The span is read during begin_rendering
// only (it may reference a caller-owned temporary).
struct RenderingInfo {
    ColorAttachment color;
    std::span<const ColorAttachment> colors = {};
    std::optional<DepthStencilAttachment> depth_stencil;
};

class CommandBuffer {
public:
    virtual ~CommandBuffer() = default;

    // Begin/end a dynamic-rendering scope. All draws happen between these. begin_rendering
    // transitions the target into a color-attachment layout for you; end_rendering closes the
    // scope. (Vulkan 1.3 dynamic rendering — no render-pass/framebuffer objects; see ADR-0007.)
    virtual void begin_rendering(const RenderingInfo& info) = 0;
    virtual void end_rendering() = 0;

    virtual void bind_pipeline(PipelineHandle pipeline) = 0;
    virtual void bind_vertex_buffer(BufferHandle buffer, std::uint64_t offset = 0) = 0;

    // Bind an index buffer for indexed drawing; `type` (16- or 32-bit) must match how the indices
    // were written. Pair with draw_indexed(). Indexed draws let vertices be shared between
    // triangles (a quad is 4 vertices + 6 indices instead of 6 vertices).
    virtual void
    bind_index_buffer(BufferHandle buffer, IndexType type, std::uint64_t offset = 0) = 0;

    // Attach a texture + sampler to a CombinedImageSampler `binding` the currently bound
    // pipeline declared (GraphicsPipelineDesc::bindings, or the sampled_texture sugar). Call
    // after bind_pipeline(). Like bind_uniform_buffer, the attachment takes effect at the next
    // draw, when all pending bindings are baked into one transient descriptor set (ADR-0020) —
    // so the same binding may point at different resources from draw to draw.
    virtual void
    bind_texture(std::uint32_t binding, TextureHandle texture, SamplerHandle sampler) = 0;

    // Attach `size` bytes at `offset` of a uniform buffer (BufferUsage::Uniform) to a
    // UniformBuffer `binding` the currently bound pipeline declared. `size` 0 means "through the
    // end of the buffer". Takes effect at the next draw (one transient descriptor set per draw —
    // ADR-0020), so per-draw data can live as slices of one buffer, re-bound at a new offset
    // between draws (respect the device's uniform-offset alignment; 256 is universally safe).
    virtual void bind_uniform_buffer(std::uint32_t binding,
                                     BufferHandle buffer,
                                     std::uint64_t offset = 0,
                                     std::uint64_t size = 0) = 0;

    // Attach a storage buffer (BufferUsage::Storage) to a StorageBuffer `binding` — read-write
    // bulk shader data, the compute path's staple (M5.2). Same attach-then-bake-at-dispatch/draw
    // semantics as the other bind_* calls.
    virtual void bind_storage_buffer(std::uint32_t binding,
                                     BufferHandle buffer,
                                     std::uint64_t offset = 0,
                                     std::uint64_t size = 0) = 0;

    // Attach a texture created with TextureUsage::Storage to a StorageImage `binding` — written
    // and read by shaders via imageStore/imageLoad (GLSL `image2D`), not filtered sampling. The
    // image must already be in the general layout (the backend puts it there the first time a
    // dispatch touches it — see bind_storage_image's implementation notes).
    virtual void bind_storage_image(std::uint32_t binding, TextureHandle texture) = 0;

    // Upload `size` bytes of push-constant data (from `offset`) to the currently bound pipeline,
    // visible to its vertex and fragment stages. Call after bind_pipeline; the pipeline must have
    // been created with a matching `push_constant_size`. This is the per-draw fast path for a small
    // block such as an MVP matrix — no descriptor set, no buffer. Keep within the pipeline's
    // declared size.
    virtual void push_constants(const void* data, std::uint32_t size, std::uint32_t offset = 0) = 0;

    // Viewport/scissor are dynamic pipeline state, so they're set per-recording rather than baked
    // into the pipeline — the same pipeline can draw to differently sized targets.
    virtual void set_viewport(const Viewport& viewport) = 0;
    virtual void set_scissor(const Rect2D& scissor) = 0;

    virtual void draw(std::uint32_t vertex_count,
                      std::uint32_t instance_count = 1,
                      std::uint32_t first_vertex = 0,
                      std::uint32_t first_instance = 0) = 0;

    // Draw using the bound index buffer: `index_count` indices from `first_index`, with
    // `vertex_offset` added to each index before the vertex is fetched.
    virtual void draw_indexed(std::uint32_t index_count,
                              std::uint32_t instance_count = 1,
                              std::uint32_t first_index = 0,
                              std::int32_t vertex_offset = 0,
                              std::uint32_t first_instance = 0) = 0;

    // Copy a (TransferSrc) texture's pixels into a (TransferDst, host-visible) buffer, tightly
    // packed. This is how the M3 proof gets rendered pixels back to the CPU to verify them. The
    // backend transitions the texture to a transfer-source layout first.
    virtual void copy_texture_to_buffer(TextureHandle src, BufferHandle dst) = 0;

    // ── Compute (M5.2, ADR-0021) ───────────────────────────────────────────────────────────
    // Bind a compute pipeline (created with create_compute_pipeline). Compute has its own bind
    // call because Vulkan keeps graphics and compute bind points separate on one command buffer;
    // pending resource attachments are shared, so a buffer attached once can feed a compute
    // dispatch and then a draw.
    virtual void bind_compute_pipeline(PipelineHandle pipeline) = 0;

    // Launch `gx × gy × gz` workgroups of the bound compute pipeline (the workgroup size itself
    // is baked into the shader's local_size_*). Call OUTSIDE begin/end_rendering — compute is not
    // part of a raster pass. Pending bindings are baked exactly as at a draw. v0 follows every
    // dispatch with a conservative barrier so its writes are visible to whatever comes next
    // (draws, copies, more dispatches) — precise, graph-derived barriers replace that blanket at
    // M5.4 (ADR-0019).
    virtual void dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz = 1) = 0;

    // ── GPU timing + debug labels (M5.3) ──────────────────────────────────────────────────
    // Stamp the GPU clock into `slot` (< kMaxTimestamps) when all prior work completes. Call
    // outside begin/end_rendering. The render graph brackets every pass with a pair of these —
    // "measure before optimize" made structural (ADR-0019). No-op when the device cannot
    // timestamp (read_timestamps then returns false).
    virtual void write_timestamp(std::uint32_t slot) = 0;

    // After this command buffer's submission has completed (submit_blocking has returned), fetch
    // the stamped slots into `out_ns` — nanoseconds on the GPU clock (timestampPeriod applied),
    // comparable within one submission: pass_ms = (out_ns[end] - out_ns[begin]) / 1e6. Reads
    // out_ns.size() slots from slot 0. Returns false (out untouched) if the device cannot
    // timestamp or nothing was stamped.
    [[nodiscard]] virtual bool read_timestamps(std::span<std::uint64_t> out_ns) = 0;

    // Bracket a region of GPU work with a named label — what makes a RenderDoc/validation capture
    // read like the render graph that recorded it ("depth-prepass", "forward-pbr", …). Nestable;
    // no-ops when VK_EXT_debug_utils is absent (a release-driver norm, so never rely on them for
    // correctness).
    virtual void begin_debug_label(std::string_view name) = 0;
    virtual void end_debug_label() = 0;

protected:
    CommandBuffer() = default; // obtained only from Device::begin_commands()

public:
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
};

} // namespace rime::rhi
