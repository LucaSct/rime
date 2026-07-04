// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "rime/rhi/rhi.hpp"

// The render graph (M5.4, ADR-0019): a frame described as DATA — passes plus the resources they
// read and write — instead of a hand-maintained sequence of raw RHI calls. Each frame, render
// code DECLARES its passes; compile() derives everything hand-rolled code keeps in its author's
// head: the execution order (from resource dependencies), which passes are dead (nothing they
// write ever reaches an output), what transient GPU memory is needed (allocated from a
// cross-frame cache), and every layout/hazard barrier between passes (emitted through the RHI
// seam `texture_barrier` reserved since M3). Records serially into one command buffer on one
// queue in v0 — deliberately: pass boundaries + declared accesses are exactly the inputs
// parallel recording and async compute need later, so those arrive as backend upgrades, not API
// breaks. Every pass is bracketed with GPU timestamps and a debug label from day one.
//
//   render::RenderGraph graph(device);            // long-lived: owns the transient cache
//   ...each frame:
//   graph.reset();
//   RGTexture hdr = graph.create_texture({{w, h}, Format::RGBA16Float, "hdr"});
//   graph.add_raster_pass("scene", {.colors = {{hdr, LoadOp::Clear}}},
//                         [&](rhi::CommandBuffer& cmd) { /* bind + draw */ });
//   ...
//   graph.export_texture(result);                 // "this is a frame output — keep its producers"
//   auto cmd = device.begin_commands();
//   graph.execute(*cmd);                          // compile → transients → barriers → record
//   device.submit_blocking(*cmd);                 // (or hand to the swapchain)
//
// See docs/design/render-graph.md for the worked walkthrough and docs/adr/0019-render-graph.md
// for why this shape (frame-declared, virtual resources, graph-owned barriers) was chosen.
namespace rime::render {

inline constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

// A VIRTUAL texture: an index into this frame's resource table, not GPU memory. Physical memory
// exists only after execute() compiles the frame — created transients come from the graph's
// desc-keyed cache, imported ones wrap externally owned rhi handles. Frame-local: reset()
// invalidates every RGTexture from the previous frame (the indices restart), which is exactly
// why they are cheap to mint and impossible to leak.
struct RGTexture {
    std::uint32_t index = kInvalidIndex;

    [[nodiscard]] bool is_valid() const noexcept { return index != kInvalidIndex; }
};

// What create_texture needs: extent + format. Usage flags are deliberately ABSENT — the graph
// accumulates them from how passes actually declare the texture (a Sampled read adds Sampled, a
// color attachment adds ColorAttachment, …), so a declaration can never disagree with usage.
struct RGTextureDesc {
    rhi::Extent2D extent{};
    rhi::Format format = rhi::Format::RGBA8Unorm;
    std::string_view debug_name = {}; // copied; also stamped onto the physical for captures
};

// One declared color attachment of a raster pass. Mirrors rhi::ColorAttachment but names a
// virtual texture; the graph builds the real RenderingInfo at execute time.
struct RGColorAttachment {
    RGTexture texture;
    rhi::LoadOp load = rhi::LoadOp::Clear;
    rhi::StoreOp store = rhi::StoreOp::Store;
    rhi::ClearColor clear = {};
};

// The declared depth(-stencil) attachment. `read_only` says the pass tests against depth but
// never writes it (a forward pass running against a pre-pass's depth): that turns the hazard
// from write-after-write into read-after-write, which lets the compiler keep more passes
// unordered relative to each other.
struct RGDepthAttachment {
    RGTexture texture;
    rhi::LoadOp load = rhi::LoadOp::Clear;
    rhi::StoreOp store = rhi::StoreOp::DontCare;
    float clear_depth = 1.0f;
    std::uint32_t clear_stencil = 0;
    bool read_only = false;
};

class RenderGraph {
public:
    // The graph object is long-lived (it owns the cross-frame transient cache); the DECLARATIONS
    // are per-frame (reset() clears them). One graph per device is the expected shape.
    explicit RenderGraph(rhi::Device& device) noexcept : device_(device) {}

    ~RenderGraph();

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Begin a new frame: forget every pass and virtual resource, keep the physical cache (its
    // textures become available for this frame's transients).
    void reset();

    // Declare a transient texture. No GPU memory yet — see RGTexture.
    [[nodiscard]] RGTexture create_texture(const RGTextureDesc& desc);

    // Wrap an externally owned texture (a swapchain backbuffer, a streamed-capture target, a
    // persistent history buffer) so passes can declare against it. `state` is what the texture
    // is in RIGHT NOW (ResourceState::Undefined when freshly created / contents irrelevant).
    // Imported textures are never culled roots' victims: writes to them always survive culling —
    // the outside world can see them.
    [[nodiscard]] RGTexture import_texture(rhi::TextureHandle handle, rhi::ResourceState state);

    // Mark a created texture as a frame OUTPUT: its producer chain survives culling and its
    // physical handle is queryable after execute() (for a readback copy, a streamer tap, …).
    void export_texture(RGTexture texture);

    // The physical rhi handle behind a virtual texture. Valid after execute() for exported and
    // imported textures (transients may be recycled the moment the next frame compiles).
    [[nodiscard]] rhi::TextureHandle physical(RGTexture texture) const;

    // The recorded body of a pass: bind pipelines/resources and draw/dispatch. Everything the
    // pass DECLARED is already true when it runs — attachments bound (raster passes run inside
    // begin/end_rendering with viewport+scissor preset to the full target), sampled inputs
    // transitioned to ShaderRead, storage images to general. The λ must touch only what its pass
    // declared; that discipline is what makes every derived barrier sound.
    using ExecuteFn = std::function<void(rhi::CommandBuffer&)>;

    struct RasterPassDesc {
        std::span<const RGColorAttachment> colors; // ≤ rhi::kMaxColorAttachments
        const RGDepthAttachment* depth = nullptr;  // optional
        std::span<const RGTexture> sampled = {};   // textures this pass reads via bind_texture
        std::span<const RGTexture> storage = {};   // storage images read/written by its shaders
    };

    void add_raster_pass(std::string_view name, const RasterPassDesc& desc, ExecuteFn fn);

    struct ComputePassDesc {
        std::span<const RGTexture> sampled = {};       // read via bind_texture
        std::span<const RGTexture> storage_read = {};  // imageLoad only
        std::span<const RGTexture> storage_write = {}; // imageStore (or both) — the write set
    };

    void add_compute_pass(std::string_view name, const ComputePassDesc& desc, ExecuteFn fn);

    // Compile the declared frame and record it into `cmd`: derive the order (resource versioning
    // → dependency edges → topological order, declared order breaking ties), cull passes that
    // contribute to no imported/exported resource, satisfy transients from the cache, emit the
    // between-pass barriers, and run each live pass's λ bracketed by a debug label + GPU
    // timestamps. Serial, single queue (ADR-0019 v0).
    void execute(rhi::CommandBuffer& cmd);

    // ── Introspection (tests, tools, the samples' printouts) ──────────────────────────────
    [[nodiscard]] std::size_t pass_count() const noexcept { return passes_.size(); }

    [[nodiscard]] const std::string& pass_name(std::uint32_t pass) const {
        return passes_[pass].name;
    }

    // Declared-pass indices in the order execute() ran them (culled passes absent).
    [[nodiscard]] std::span<const std::uint32_t> execution_order() const noexcept { return order_; }

    [[nodiscard]] bool was_culled(std::uint32_t pass) const { return passes_[pass].culled; }

    // Per-pass GPU time, readable once the submission execute() recorded into has completed
    // (e.g. after submit_blocking returns). Empty when the device cannot timestamp. Names point
    // into the graph — consume before reset().
    struct PassTiming {
        std::string_view name;
        double gpu_ms = 0.0;
    };

    [[nodiscard]] std::vector<PassTiming> resolve_timings(rhi::CommandBuffer& cmd) const;

private:
    // One declared use of one resource: the state the pass needs it in, and whether it writes.
    // This little record is the whole input to ordering, culling, AND barriers — the payoff of
    // declaring accesses instead of hand-placing synchronization.
    struct Access {
        std::uint32_t resource = kInvalidIndex;
        rhi::ResourceState state = rhi::ResourceState::Undefined;
        bool write = false;
    };

    struct Pass {
        std::string name;
        bool is_raster = false;
        std::vector<RGColorAttachment> colors; // baked copies (declaration spans are temporary)
        RGDepthAttachment depth{};
        bool has_depth = false;
        std::vector<Access> accesses;
        ExecuteFn fn;
        bool culled = false;
    };

    struct Resource {
        rhi::Extent2D extent{};
        rhi::Format format = rhi::Format::Undefined;
        std::string debug_name;
        bool imported = false;
        bool exported = false;
        rhi::TextureUsage usage = rhi::TextureUsage::None;        // accumulated from accesses
        rhi::ResourceState state = rhi::ResourceState::Undefined; // tracked while recording
        rhi::TextureHandle physical{}; // imported handle, or cache-assigned at execute()
    };

    // The cross-frame physical cache: the one thing reset() keeps. A transient costs a real
    // allocation the first frame its (extent, format, usage) appears, then recycles. No aliasing
    // within a frame yet (ADR-0019: measure first); no eviction yet (grows to the high-water
    // mark of distinct descs — revisit when a real workload varies).
    struct CachedTexture {
        rhi::Extent2D extent{};
        rhi::Format format = rhi::Format::Undefined;
        rhi::TextureUsage usage = rhi::TextureUsage::None;
        rhi::TextureHandle handle{};
        bool in_use = false; // claimed by a resource this frame
    };

    void add_pass_common(std::string_view name, bool is_raster, ExecuteFn fn);
    void declare_access(std::uint32_t resource, rhi::ResourceState state, bool write);
    void compile();
    void assign_physicals();

    rhi::Device& device_;
    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
    std::vector<std::uint32_t> order_; // live passes, execution order (built by compile())
    std::vector<CachedTexture> cache_;
    std::uint32_t timed_passes_ = 0; // how many passes got a timestamp pair this frame
    bool timing_overflow_warned_ = false;
};

} // namespace rime::render
