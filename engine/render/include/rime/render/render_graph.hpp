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

// A VIRTUAL storage BUFFER (m10.3), the exact analogue of RGTexture for bulk shader data: an index
// into this frame's resource table, physical only after execute(). Buffers earn a place in the
// graph for the same reason textures did — a compute pass that fills one and a later pass that
// reads it are a producer/consumer pair, and the graph is what turns that pair into an execution
// edge, a liveness fact ("this dispatch is not dead"), and the memory barrier between them.
// Clustered forward (m10.3) is the forcing case: without a declared buffer the light-culling
// dispatch writes nothing the graph can see, and culling would delete it.
struct RGBuffer {
    std::uint32_t index = kInvalidIndex;

    [[nodiscard]] bool is_valid() const noexcept { return index != kInvalidIndex; }
};

// What create_buffer needs. Like RGTextureDesc there is no usage field — a transient buffer is a
// storage buffer by construction (that is the only thing a pass can declare it as).
struct RGBufferDesc {
    std::uint64_t size_bytes = 0;
    std::string_view debug_name = {}; // copied; stamped onto the physical for captures
};

// What create_texture needs: extent + format. Usage flags are deliberately ABSENT — the graph
// accumulates them from how passes actually declare the texture (a Sampled read adds Sampled, a
// color attachment adds ColorAttachment, …), so a declaration can never disagree with usage.
struct RGTextureDesc {
    rhi::Extent2D extent{};
    rhi::Format format = rhi::Format::RGBA8Unorm;
    std::string_view debug_name = {}; // copied; also stamped onto the physical for captures
    // Array layers (m10.1): >1 makes a layered transient — a cascaded shadow map's N-cascade depth
    // array, rendered one layer per pass (RGDepthAttachment::layer). 1 (default) is an ordinary 2-D
    // texture. Part of the transient-cache key, so a layered and a flat texture never alias. Placed
    // last so the common positional `{extent, format, name}` call sites are unaffected.
    std::uint32_t array_layers = 1;
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
    // Which array layer of a layered depth target to render into (m10.1) — one cascade of a CSM.
    // 0 (default) for an ordinary depth texture. Requires `texture` created with array_layers >
    // layer.
    std::uint32_t layer = 0;
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
    // persistent history buffer — or m10.2's cached shadow array) so passes can declare against it.
    // `state` is what the texture is in RIGHT NOW (ResourceState::Undefined when freshly created /
    // contents irrelevant). Pass `extent`/`format`/`array_layers` when a pass will RENDER INTO the
    // imported texture — the graph derives the pass viewport + layer target from them (they may
    // stay default for a sample-only import, which never becomes an attachment). Imported textures
    // are never culled roots' victims: writes to them always survive culling — the outside world
    // sees them.
    [[nodiscard]] RGTexture import_texture(rhi::TextureHandle handle,
                                           rhi::ResourceState state,
                                           rhi::Extent2D extent = {},
                                           rhi::Format format = rhi::Format::Undefined,
                                           std::uint32_t array_layers = 1);

    // Declare a transient storage buffer (m10.3). No GPU memory yet — see RGBuffer. Satisfied at
    // execute() from a size-keyed cache, so a per-frame list buffer allocates once and recycles.
    [[nodiscard]] RGBuffer create_buffer(const RGBufferDesc& desc);

    // Wrap an externally owned buffer so passes can declare against it — the buffer twin of
    // import_texture. The clustered light list is the motivating case: the CPU packs it every
    // frame into a persistently-owned buffer, and the cull dispatch must still be ordered against
    // that. `state` is what it is in right now (ShaderRead for a buffer the host just wrote:
    // Vulkan's submission guarantee already makes host writes visible to the submitted work).
    [[nodiscard]] RGBuffer import_buffer(rhi::BufferHandle handle, rhi::ResourceState state);

    // Mark a created texture as a frame OUTPUT: its producer chain survives culling and its
    // physical handle is queryable after execute() (for a readback copy, a streamer tap, …).
    void export_texture(RGTexture texture);

    // The buffer twin: keep this buffer's producers alive and its contents readable after
    // execute() (a test reading back what a dispatch computed).
    void export_buffer(RGBuffer buffer);

    // The physical rhi handle behind a virtual texture. Valid after execute() for exported and
    // imported textures (transients may be recycled the moment the next frame compiles).
    [[nodiscard]] rhi::TextureHandle physical(RGTexture texture) const;

    // The physical rhi handle behind a virtual buffer (same validity rules as physical()).
    [[nodiscard]] rhi::BufferHandle physical_buffer(RGBuffer buffer) const;

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
        // Storage buffers this pass READS (m10.3) — the clustered forward pass reading the light
        // lists a dispatch filled. Read-only on purpose: no raster pass in the engine writes a
        // buffer yet, and leaving the write set out keeps the "who produced this?" question
        // answerable by looking only at compute passes.
        std::span<const RGBuffer> buffer_reads = {};
        bool fold_repeats = false; // see ComputePassDesc::fold_repeats
    };

    void add_raster_pass(std::string_view name, const RasterPassDesc& desc, ExecuteFn fn);

    struct ComputePassDesc {
        std::span<const RGTexture> sampled = {};       // read via bind_texture
        std::span<const RGTexture> storage_read = {};  // imageLoad only
        std::span<const RGTexture> storage_write = {}; // imageStore (or both) — the write set
        std::span<const RGBuffer> buffer_reads = {};   // storage buffers read (m10.3)
        std::span<const RGBuffer> buffer_writes = {};  // storage buffers written — the write set

        // THE ONE DECLARED EXCEPTION TO "A PASS NAME IS AN IDENTITY" (m17.3c).
        //
        // m17.3a's rule is that two passes in one frame may not share a name, and the graph
        // uniquifies any collision to `name#1`. That rule assumes the repeats are DISTINGUISHABLE —
        // cascade 2 is a different thing from cascade 3, so each gets a name. Some repeats are not:
        // the SDF clipmap stamps every dirty instance into a level with the same shader, the same
        // pipeline and the same target, and how many arrive depends on what the player just broke.
        // There, the k-th stamp is not a stable identity across frames, so `#1` would key a
        // distribution on nothing more than queue position.
        //
        // Setting this says "this name is declared several times per frame ON PURPOSE, and the
        // honest measurement is their SUM". The graph then leaves the name alone and
        // `resolve_timings` reports one entry carrying the total. It is opt-in because the default
        // must stay the strict rule: a repeat nobody declared is a bug, and folding by default is
        // exactly the silent merge the 2026-08-30 baseline's four `depth-prepass` keys were.
        bool fold_repeats = false;
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
    //
    // NAMES ARE UNIQUE in the returned list: the graph uniquified collisions at declaration, and
    // passes that declared `fold_repeats` are summed into one entry here. A consumer may therefore
    // treat a name as a key — which `PerfReport` and the committed JSON both do.
    struct PassTiming {
        // OWNED (m17.5), where it used to be a `string_view` into the graph with a "consume before
        // reset()" caveat attached. A pipelined frame's timestamps are read two frames after the
        // graph moved on, so a view into the graph is a dangling read by construction — and the
        // three samples that consume this all copied to a string immediately anyway.
        std::string name;
        double gpu_ms = 0.0;
    };

    [[nodiscard]] std::vector<PassTiming> resolve_timings(rhi::CommandBuffer& cmd) const;

    // A frame's timing SHAPE, taken at submit and resolved after the GPU catches up (m17.5).
    //
    // The pipelined loop's answer to a problem the blocking one never had: by the time frame N's
    // fence signals, the graph holds frame N+2's passes, so "which pass is timing slot 3" is a
    // question only frame N can answer and only while it is still the current frame. Snapshot the
    // answer at submit; pair it with the timestamps later.
    struct TimingPlan {
        std::vector<std::string> names; // one per timed pass, in timing-slot order
        std::vector<char> fold;         // did that pass declare fold_repeats
        std::uint32_t timed = 0;
    };

    [[nodiscard]] TimingPlan timing_plan() const;

    // Resolve a plan against the command buffer that produced it — which the caller must have
    // borrowed (`Device::wait_and_borrow`), since a reclaimed submission has already freed the
    // query pool these numbers live in.
    [[nodiscard]] static std::vector<PassTiming> resolve_timings(const TimingPlan& plan,
                                                                 rhi::CommandBuffer& cmd);

    // ── Per-frame CPU→GPU scratch (m17.4) ─────────────────────────────────────────────────
    //
    // THE ONE HAZARD THAT PIPELINING CANNOT ORDER FOR YOU. Submissions on the graphics queue are
    // GPU work, and GPU work is ordered against GPU work by barriers and submission order. A CPU
    // write to host-visible memory is not on the queue at all: when the loop stops waiting for the
    // GPU, frame N+1's `write_buffer` into a system's own uniform buffer lands while frame N is
    // still reading it, and the frame renders with next frame's numbers. `submit_blocking` hid
    // this completely — it is the reason all seven lighting systems could own fourteen unringed
    // host-visible buffers for a milestone and nothing ever looked wrong.
    //
    // The fix is a ring, and it lives HERE rather than fourteen times over in the systems. m16.1
    // gave `SceneRenderer` its own ring and the invariant behind it is subtle enough — "grow only
    // the slot the ring has come back round to, because its previous submission has been waited
    // on" — that copying it into every system is how one copy ends up subtly wrong. The graph is
    // already the owner of per-frame resource knowledge (it owns the transient caches and it owns
    // `reset()`, which IS the frame boundary), so a per-frame allocator belongs beside them.
    //
    // Transients deliberately do NOT need this: they are GPU-only, so the barriers the graph
    // already emits and the queue's own ordering cover them. Only the CPU-written ones are exposed.
    struct FrameSlice {
        rhi::BufferHandle buffer;
        std::uint32_t offset = 0;
        // Carried so a consumer can bind the slice without knowing the producing system's struct.
        // It also matters for correctness: binding with size 0 means "to the end of the buffer",
        // which on a shared block is both wrong in intent and capable of exceeding a device's
        // maxUniformBufferRange.
        std::uint32_t size = 0;
    };

    // Ring depth, as the swapchain's rule: one more slot than the frames the backend keeps in
    // flight, so the slot being written is never one the GPU can still be reading. Call once,
    // before the first frame — it destroys and rebuilds the ring, which is only legal while
    // nothing is in flight. The default (3 slots) is safe for every path the engine has today,
    // including a blocking one that needs only 1.
    void set_frames_in_flight(std::uint32_t frames);

    [[nodiscard]] std::uint32_t frame_slot_count() const noexcept {
        return static_cast<std::uint32_t>(frame_slots_.size());
    }

    // Copy `bytes` of `data` into this frame's scratch and say where it landed. The slice is valid
    // until this frame's GPU work completes, and its memory is not handed out again until the ring
    // returns to this slot. Offsets are 256-byte aligned — the universal uniform-offset alignment
    // this engine already assumes everywhere it strides a uniform array — so a slice is always a
    // legal `bind_uniform_buffer` offset.
    //
    // Never invalidates a slice already handed out this frame: a push that does not fit takes a
    // fresh block rather than growing the current one, because growing means destroying a buffer
    // that earlier passes in this same frame are still pointing at.
    [[nodiscard]] FrameSlice push_frame_data(const void* data, std::size_t bytes);

    // The same ring, handing out a WHOLE buffer rather than a slice of a shared one. Needed
    // because a buffer that enters the graph as an imported `RGBuffer` — the clustered light array
    // is the one — is addressed by handle alone: `import_buffer` carries no offset, so a slice
    // cannot be imported. Rather than let that one system grow its own ring and its own copy of
    // the recycling invariant, the ring serves both shapes.
    [[nodiscard]] rhi::BufferHandle push_frame_buffer(const void* data, std::size_t bytes);

    // How many bytes this frame has pushed so far — the number a caller would print if it wanted
    // to size the block, and what the test asserts the ring is actually recycling.
    [[nodiscard]] std::uint64_t frame_bytes_pushed() const noexcept { return frame_pushed_; }

    // How many passes a frame can time before the timestamp pool runs out — two slots per pass, so
    // half of `rhi::kMaxTimestamps`. Exposed (m17.3b) because a caller that silently reports fewer
    // passes than the frame declared is reporting UNATTRIBUTED GPU time as if it were absent, and
    // `execute()`'s log warning is not in the committed artifact anyone later reads.
    [[nodiscard]] static constexpr std::uint32_t max_timed_passes() noexcept {
        return rhi::kMaxTimestamps / 2;
    }

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
        bool fold_repeats = false; // this name is declared several times per frame on purpose
    };

    // Textures and buffers share ONE resource table (and therefore one index space, one versioning
    // walk, one liveness flood, one barrier walk): every question the compiler asks — who wrote
    // this last, does anything observable depend on it — is identical for both kinds, and only the
    // last step (allocate it / transition it) differs. RGTexture and RGBuffer are distinct C++
    // types so a mix-up is a compile error, not a runtime surprise.
    enum class ResourceKind : std::uint8_t { Texture, Buffer };

    struct Resource {
        ResourceKind kind = ResourceKind::Texture;
        rhi::Extent2D extent{};
        rhi::Format format = rhi::Format::Undefined;
        std::uint32_t array_layers = 1; // >1 → a layered transient (m10.1 CSM cascade array)
        std::uint64_t size_bytes = 0;   // buffers only
        std::string debug_name;
        bool imported = false;
        bool exported = false;
        rhi::TextureUsage usage = rhi::TextureUsage::None;        // accumulated from accesses
        rhi::ResourceState state = rhi::ResourceState::Undefined; // tracked while recording
        rhi::TextureHandle physical{}; // imported handle, or cache-assigned at execute()
        rhi::BufferHandle buffer{};    // the buffer twin of `physical` (kind == Buffer)
    };

    // The cross-frame physical cache: the one thing reset() keeps. A transient costs a real
    // allocation the first frame its (extent, format, usage) appears, then recycles. No aliasing
    // within a frame yet (ADR-0019: measure first); no eviction yet (grows to the high-water
    // mark of distinct descs — revisit when a real workload varies).
    struct CachedTexture {
        rhi::Extent2D extent{};
        rhi::Format format = rhi::Format::Undefined;
        rhi::TextureUsage usage = rhi::TextureUsage::None;
        std::uint32_t array_layers =
            1; // part of the key (m10.1): a layered target never aliases a flat one
        rhi::TextureHandle handle{};
        bool in_use = false; // claimed by a resource this frame
    };

    // The same cache for transient buffers, keyed by size alone (usage is always Storage —
    // TransferSrc is added for exported ones, so that rides in the key too).
    struct CachedBuffer {
        std::uint64_t size_bytes = 0;
        rhi::BufferUsage usage = rhi::BufferUsage::None;
        rhi::BufferHandle handle{};
        bool in_use = false;
    };

    void ensure_frame_ring();
    void add_pass_common(std::string_view name, bool is_raster, bool fold_repeats, ExecuteFn fn);
    void declare_access(std::uint32_t resource, rhi::ResourceState state, bool write);
    void compile();
    void assign_physicals();

    rhi::Device& device_;
    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
    std::vector<std::uint32_t> order_; // live passes, execution order (built by compile())

    // One ring slot: a chain of host-visible blocks, filled linearly. A chain rather than one
    // grown buffer because growth destroys, and destroying mid-frame would pull the memory out
    // from under slices already handed to earlier passes.
    struct FrameBlock {
        rhi::BufferHandle handle{};
        std::uint64_t size = 0;
    };

    struct FrameSlot {
        std::vector<FrameBlock> blocks;
        std::size_t block = 0;    // which block the next push lands in
        std::uint64_t offset = 0; // where in that block
    };

    // Whole-buffer pool, one entry per push per slot. Recreated only when a lap's push needs more
    // than the entry holds — safe for the same reason the blocks are: the ring has come back
    // round, so nothing this frame is pointing at it yet.
    struct FrameBufferPool {
        std::vector<FrameBlock> buffers;
        std::size_t next = 0;
    };

    std::vector<FrameSlot> frame_slots_;
    std::vector<FrameBufferPool> frame_buffers_;
    std::uint32_t frame_slot_ = 0;
    std::uint64_t frame_pushed_ = 0;

    std::vector<CachedTexture> cache_;
    std::vector<CachedBuffer> buffer_cache_;
    std::uint32_t timed_passes_ = 0; // how many passes got a timestamp pair this frame
    bool timing_overflow_warned_ = false;
};

} // namespace rime::render
