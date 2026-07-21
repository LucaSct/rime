// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The render graph compiler + executor (M5.4, ADR-0019). The interesting work is compile():
// declared accesses become dependency edges by RESOURCE VERSIONING, the edges become an
// execution order by a topological sort with a declared-order tiebreak, liveness flows backwards
// from the frame's outputs to cull dead passes, and the per-resource state walk emits exactly
// the barriers the order requires. Everything here runs per frame, on purpose — a frame is data,
// rebuilt as cheaply as it is described (the measured cost lives in the M5.4 proof).

#include "rime/render/render_graph.hpp"

#include <algorithm>

#include "rime/core/diagnostics/assert.hpp"
#include "rime/core/diagnostics/log.hpp"

namespace rime::render {

RenderGraph::~RenderGraph() {
    for (const CachedTexture& c : cache_) {
        device_.destroy(c.handle);
    }
}

void RenderGraph::reset() {
    passes_.clear();
    resources_.clear();
    order_.clear();
    timed_passes_ = 0;
    for (CachedTexture& c : cache_) {
        c.in_use = false; // last frame's transients become this frame's free list
    }
}

RGTexture RenderGraph::create_texture(const RGTextureDesc& desc) {
    Resource r;
    r.extent = desc.extent;
    r.format = desc.format;
    r.array_layers = desc.array_layers == 0 ? 1u : desc.array_layers;
    r.debug_name.assign(desc.debug_name);
    resources_.push_back(std::move(r));
    return RGTexture{static_cast<std::uint32_t>(resources_.size() - 1)};
}

RGTexture RenderGraph::import_texture(rhi::TextureHandle handle, rhi::ResourceState state) {
    Resource r;
    r.imported = true;
    r.physical = handle;
    r.state = state;
    resources_.push_back(std::move(r));
    return RGTexture{static_cast<std::uint32_t>(resources_.size() - 1)};
}

void RenderGraph::export_texture(RGTexture texture) {
    if (!texture.is_valid() || texture.index >= resources_.size()) {
        RIME_ERROR("render: export_texture with an invalid RGTexture");
        return;
    }
    resources_[texture.index].exported = true;
}

rhi::TextureHandle RenderGraph::physical(RGTexture texture) const {
    if (!texture.is_valid() || texture.index >= resources_.size()) {
        RIME_ERROR("render: physical() with an invalid RGTexture");
        return {};
    }
    return resources_[texture.index].physical;
}

void RenderGraph::declare_access(std::uint32_t resource, rhi::ResourceState state, bool write) {
    if (resource >= resources_.size()) {
        RIME_ERROR("render: pass '{}' declares an invalid RGTexture", passes_.back().name);
        return;
    }
    passes_.back().accesses.push_back({resource, state, write});

    // Usage accumulates from declarations, so a created texture's flags can never disagree with
    // how it is actually used (RGTextureDesc deliberately has no usage field).
    Resource& r = resources_[resource];
    switch (state) {
        case rhi::ResourceState::ColorTarget:
            r.usage |= rhi::TextureUsage::ColorAttachment;
            break;
        case rhi::ResourceState::DepthTarget:
            r.usage |= rhi::TextureUsage::DepthStencil;
            break;
        case rhi::ResourceState::ShaderRead:
            r.usage |= rhi::TextureUsage::Sampled;
            break;
        case rhi::ResourceState::StorageReadWrite:
            r.usage |= rhi::TextureUsage::Storage;
            break;
        case rhi::ResourceState::TransferSrc:
            r.usage |= rhi::TextureUsage::TransferSrc;
            break;
        case rhi::ResourceState::TransferDst:
            r.usage |= rhi::TextureUsage::TransferDst;
            break;
        default:
            break;
    }
}

void RenderGraph::add_pass_common(std::string_view name, bool is_raster, ExecuteFn fn) {
    Pass p;
    p.name.assign(name);
    p.is_raster = is_raster;
    p.fn = std::move(fn);
    passes_.push_back(std::move(p));
}

void RenderGraph::add_raster_pass(std::string_view name, const RasterPassDesc& desc, ExecuteFn fn) {
    if (desc.colors.empty() && desc.depth == nullptr) {
        RIME_ERROR("render: raster pass '{}' declares no attachments", name);
        return;
    }
    if (desc.colors.size() > rhi::kMaxColorAttachments) {
        RIME_ERROR("render: raster pass '{}' declares {} color targets (max {})",
                   name,
                   desc.colors.size(),
                   rhi::kMaxColorAttachments);
        return;
    }
    add_pass_common(name, true, std::move(fn));
    Pass& p = passes_.back();
    p.colors.assign(desc.colors.begin(), desc.colors.end());
    if (desc.depth != nullptr) {
        p.has_depth = true;
        p.depth = *desc.depth;
    }

    // Attachments are writes (LoadOp::Load makes them read-modify-write — same edge direction);
    // a read_only depth attachment is genuinely a read, which the dependency builder rewards
    // with weaker ordering (read-after-write instead of write-after-write).
    for (const RGColorAttachment& c : p.colors) {
        declare_access(c.texture.index, rhi::ResourceState::ColorTarget, true);
    }
    if (p.has_depth) {
        declare_access(p.depth.texture.index, rhi::ResourceState::DepthTarget, !p.depth.read_only);
    }
    for (const RGTexture& t : desc.sampled) {
        declare_access(t.index, rhi::ResourceState::ShaderRead, false);
    }
    for (const RGTexture& t : desc.storage) {
        declare_access(t.index, rhi::ResourceState::StorageReadWrite, true);
    }
}

void RenderGraph::add_compute_pass(std::string_view name,
                                   const ComputePassDesc& desc,
                                   ExecuteFn fn) {
    add_pass_common(name, false, std::move(fn));
    for (const RGTexture& t : desc.sampled) {
        declare_access(t.index, rhi::ResourceState::ShaderRead, false);
    }
    for (const RGTexture& t : desc.storage_read) {
        declare_access(t.index, rhi::ResourceState::StorageReadWrite, false);
    }
    for (const RGTexture& t : desc.storage_write) {
        declare_access(t.index, rhi::ResourceState::StorageReadWrite, true);
    }
}

void RenderGraph::compile() {
    const std::size_t n = passes_.size();

    // ── 1. Dependency edges by resource versioning ────────────────────────────────────────
    // Walk passes in declared order tracking, per resource, who wrote it last and who has read
    // that version. A read depends on the last write (RAW). A write depends on the last write
    // (WAW) *and* on every read of the old version (WAR — the write must not clobber data a
    // reader still needs). This is SSA thinking applied to GPU resources: each write creates a
    // new version, and edges connect versions to their consumers.
    struct ResourceLife {
        std::uint32_t last_writer = kInvalidIndex;
        std::vector<std::uint32_t> readers;
    };

    std::vector<ResourceLife> life(resources_.size());
    std::vector<std::vector<std::uint32_t>> edges(n); // edges[a] = passes that must run after a
    std::vector<std::uint32_t> indegree(n, 0);

    const auto add_edge = [&](std::uint32_t from, std::uint32_t to) {
        if (from == kInvalidIndex || from == to)
            return;
        // Duplicate edges only skew indegree bookkeeping if we count them twice.
        if (std::find(edges[from].begin(), edges[from].end(), to) != edges[from].end())
            return;
        edges[from].push_back(to);
        ++indegree[to];
    };

    for (std::uint32_t pi = 0; pi < n; ++pi) {
        for (const Access& a : passes_[pi].accesses) {
            ResourceLife& rl = life[a.resource];
            if (a.write) {
                add_edge(rl.last_writer, pi);
                for (std::uint32_t reader : rl.readers)
                    add_edge(reader, pi);
                rl.last_writer = pi;
                rl.readers.clear();
            } else {
                add_edge(rl.last_writer, pi);
                rl.readers.push_back(pi);
            }
        }
    }

    // ── 2. Liveness: cull passes that feed no output ──────────────────────────────────────
    // A pass is live iff something the outside world can observe depends on it: it writes an
    // imported or exported resource, or a live pass reads something it writes. Seed with the
    // observable writers, then let liveness flow backwards along the edges.
    std::vector<bool> live(n, false);
    std::vector<std::uint32_t> worklist;
    for (std::uint32_t pi = 0; pi < n; ++pi) {
        for (const Access& a : passes_[pi].accesses) {
            const Resource& r = resources_[a.resource];
            if (a.write && (r.imported || r.exported)) {
                live[pi] = true;
                worklist.push_back(pi);
                break;
            }
        }
    }
    // Reverse edges once so "who does pass X depend on" is a lookup.
    std::vector<std::vector<std::uint32_t>> redges(n);
    for (std::uint32_t a = 0; a < n; ++a) {
        for (std::uint32_t b : edges[a])
            redges[b].push_back(a);
    }
    while (!worklist.empty()) {
        const std::uint32_t pi = worklist.back();
        worklist.pop_back();
        for (std::uint32_t dep : redges[pi]) {
            if (!live[dep]) {
                live[dep] = true;
                worklist.push_back(dep);
            }
        }
    }
    for (std::uint32_t pi = 0; pi < n; ++pi)
        passes_[pi].culled = !live[pi];

    // ── 3. Topological order, declared order breaking ties ───────────────────────────────
    // Kahn's algorithm, always taking the READY pass with the lowest declared index. The
    // tiebreak buys two things: determinism (the same declarations always schedule identically —
    // a property tests and humans both lean on) and least-surprise (independent passes run in
    // the order the code declared them). O(n²) ready-scans are irrelevant at dozens of passes.
    order_.clear();
    order_.reserve(n);
    std::vector<bool> emitted(n, false);
    std::vector<std::uint32_t> remaining_indegree = indegree;
    for (std::size_t emitted_count = 0; emitted_count < n;) {
        std::uint32_t pick = kInvalidIndex;
        for (std::uint32_t pi = 0; pi < n; ++pi) {
            if (!emitted[pi] && remaining_indegree[pi] == 0) {
                pick = pi;
                break;
            }
        }
        RIME_ASSERT_MSG(pick != kInvalidIndex,
                        "render graph has a dependency cycle — impossible from spans of "
                        "forward-declared accesses unless a resource is re-written after a read "
                        "of its own output in one pass");
        emitted[pick] = true;
        ++emitted_count;
        for (std::uint32_t next : edges[pick])
            --remaining_indegree[next];
        if (live[pick])
            order_.push_back(pick); // culled passes take part in ordering math, not execution
    }
}

void RenderGraph::assign_physicals() {
    for (Resource& r : resources_) {
        if (r.imported || r.physical.is_valid())
            continue;
        bool used = false; // untouched virtuals (declared, never accessed) get no memory
        for (const std::uint32_t pi : order_) {
            for (const Access& a : passes_[pi].accesses) {
                if (&resources_[a.resource] == &r) {
                    used = true;
                    break;
                }
            }
            if (used)
                break;
        }
        if (!used)
            continue;

        // Exported textures exist to be consumed outside the graph — a readback copy or a
        // streamer tap — so they get TransferSrc whether or not a pass declared it.
        if (r.exported)
            r.usage |= rhi::TextureUsage::TransferSrc;

        // Cache lookup: first free physical whose (extent, format, usage) matches exactly.
        // Usage is part of the key on purpose — reusing a Sampled-only texture as a color target
        // would need recreation anyway.
        CachedTexture* found = nullptr;
        for (CachedTexture& c : cache_) {
            if (!c.in_use && c.extent.width == r.extent.width &&
                c.extent.height == r.extent.height && c.format == r.format && c.usage == r.usage &&
                c.array_layers == r.array_layers) {
                found = &c;
                break;
            }
        }
        if (found == nullptr) {
            rhi::TextureDesc td{};
            td.extent = r.extent;
            td.format = r.format;
            td.usage = r.usage;
            td.array_layers = r.array_layers; // m10.1: a layered transient (CSM cascade array)
            td.debug_name = r.debug_name;
            const rhi::TextureHandle handle = device_.create_texture(td);
            if (!handle.is_valid()) {
                RIME_ERROR("render: transient allocation failed for '{}'", r.debug_name);
                continue;
            }
            cache_.push_back({r.extent, r.format, r.usage, r.array_layers, handle, false});
            found = &cache_.back();
        }
        found->in_use = true;
        r.physical = found->handle;
        r.state = rhi::ResourceState::Undefined; // fresh (or recycled — contents are garbage)
    }
}

void RenderGraph::execute(rhi::CommandBuffer& cmd) {
    compile();
    assign_physicals();

    for (const std::uint32_t pi : order_) {
        Pass& pass = passes_[pi];
        cmd.begin_debug_label(pass.name);

        // Timestamps bracket every pass while slots last (64 slots = 32 timed passes; a bigger
        // frame times its first 32 and says so once).
        const bool timed = timed_passes_ < rhi::kMaxTimestamps / 2;
        if (timed) {
            cmd.write_timestamp(timed_passes_ * 2);
        } else if (!timing_overflow_warned_) {
            RIME_WARN("render: more than {} passes — timing only the first {}",
                      rhi::kMaxTimestamps / 2,
                      rhi::kMaxTimestamps / 2);
            timing_overflow_warned_ = true;
        }

        // ── Graph-owned barriers (ADR-0019) ───────────────────────────────────────────────
        // Bring every declared resource into the state this pass needs. Attachment states are
        // deliberately delegated: begin_rendering() already performs tracked, correct attachment
        // transitions — and since M5.6 its REUSED-attachment transitions order against all prior
        // writes, so it genuinely is the write-after-write / read-after-write barrier between
        // passes hitting the same target (the depth pre-pass feeding the forward pass rides
        // exactly that). The graph emits transitions for the cases the implicit path cannot
        // see — sampled/storage reads of textures some earlier pass wrote. One owner per kind of
        // knowledge: frame-global read hazards here, attachment mechanics in the backend.
        for (const Access& a : passes_[pi].accesses) {
            Resource& r = resources_[a.resource];
            const bool attachment = a.state == rhi::ResourceState::ColorTarget ||
                                    a.state == rhi::ResourceState::DepthTarget;
            if (attachment) {
                r.state = a.state; // begin_rendering will transition; keep our tracking honest
                continue;
            }
            if (r.state != a.state) {
                cmd.texture_barrier(r.physical, r.state, a.state);
                r.state = a.state;
            }
        }

        if (pass.is_raster) {
            // Build the real RenderingInfo from the baked declaration and open the pass scope —
            // the λ only binds and draws. Viewport/scissor default to the full first target so
            // the overwhelmingly common case writes zero boilerplate.
            std::vector<rhi::ColorAttachment> colors;
            colors.reserve(pass.colors.size());
            for (const RGColorAttachment& c : pass.colors) {
                colors.push_back({resources_[c.texture.index].physical, c.load, c.store, c.clear});
            }
            rhi::RenderingInfo ri{};
            if (colors.size() == 1) {
                ri.color = colors[0];
            } else if (!colors.empty()) {
                ri.colors = colors;
            }
            if (pass.has_depth) {
                rhi::DepthStencilAttachment da{};
                da.target = resources_[pass.depth.texture.index].physical;
                da.load_op = pass.depth.load;
                da.store_op = pass.depth.store;
                da.clear_depth = pass.depth.clear_depth;
                da.clear_stencil = pass.depth.clear_stencil;
                da.layer =
                    pass.depth.layer; // m10.1: render into one cascade of a layered depth target
                ri.depth_stencil = da;
            }
            cmd.begin_rendering(ri);

            const Resource& first = pass.colors.empty() ? resources_[pass.depth.texture.index]
                                                        : resources_[pass.colors[0].texture.index];
            rhi::Viewport vp{};
            vp.width = static_cast<float>(first.extent.width);
            vp.height = static_cast<float>(first.extent.height);
            vp.max_depth = 1.0f;
            cmd.set_viewport(vp);
            rhi::Rect2D sc{};
            sc.width = first.extent.width;
            sc.height = first.extent.height;
            cmd.set_scissor(sc);

            pass.fn(cmd);
            cmd.end_rendering();
        } else {
            pass.fn(cmd); // compute/copy body: bind + dispatch (outside any rendering scope)
        }

        if (timed) {
            cmd.write_timestamp(timed_passes_ * 2 + 1);
            ++timed_passes_;
        }
        cmd.end_debug_label();
    }
}

std::vector<RenderGraph::PassTiming> RenderGraph::resolve_timings(rhi::CommandBuffer& cmd) const {
    std::vector<PassTiming> out;
    if (timed_passes_ == 0)
        return out;
    std::vector<std::uint64_t> ns(static_cast<std::size_t>(timed_passes_) * 2);
    if (!cmd.read_timestamps(ns))
        return out; // device cannot timestamp — documented degrade
    out.reserve(timed_passes_);
    std::uint32_t timing_slot = 0;
    for (const std::uint32_t pi : order_) {
        if (timing_slot >= timed_passes_)
            break;
        const double ms = static_cast<double>(ns[timing_slot * 2 + 1] - ns[timing_slot * 2]) / 1e6;
        out.push_back({passes_[pi].name, ms});
        ++timing_slot;
    }
    return out;
}

} // namespace rime::render
