// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>

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
// its contents on entry/exit. (Depth and multiple color targets arrive with the render graph.)
struct ColorAttachment {
    TextureHandle target;
    LoadOp load_op = LoadOp::Clear;
    StoreOp store_op = StoreOp::Store;
    ClearColor clear = {};
};

// Describes a dynamic-rendering scope. The render area defaults to the full target extent (the
// backend reads it from the texture), so the common case needs only the color attachment.
struct RenderingInfo {
    ColorAttachment color;
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

    // Viewport/scissor are dynamic pipeline state, so they're set per-recording rather than baked
    // into the pipeline — the same pipeline can draw to differently sized targets.
    virtual void set_viewport(const Viewport& viewport) = 0;
    virtual void set_scissor(const Rect2D& scissor) = 0;

    virtual void draw(std::uint32_t vertex_count,
                      std::uint32_t instance_count = 1,
                      std::uint32_t first_vertex = 0,
                      std::uint32_t first_instance = 0) = 0;

    // Copy a (TransferSrc) texture's pixels into a (TransferDst, host-visible) buffer, tightly
    // packed. This is how the M3 proof gets rendered pixels back to the CPU to verify them. The
    // backend transitions the texture to a transfer-source layout first.
    virtual void copy_texture_to_buffer(TextureHandle src, BufferHandle dst) = 0;

protected:
    CommandBuffer() = default; // obtained only from Device::begin_commands()

public:
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
};

} // namespace rime::rhi
