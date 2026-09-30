// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <deque>

#include "rime/render/virtual_geometry_page_cache.hpp"
#include "rime/rhi/rhi.hpp"

// M18.5: the GPU half of virtual-geometry page streaming — real memory behind the page cache's
// decisions, through the RHI only.
//
//   * The POOL is one device-local storage buffer of `slot_count * slot_bytes`. A page's cooked
//     bytes (vertices, then u32 indices) are copied verbatim into its slot, so the visibility and
//     resolve shaders pull a cluster straight out of the pool: its first vertex is
//     `(slot * slot_bytes + vertex_offset) / stride` and its first index word is
//     `(slot * slot_bytes + index_offset) / 4 + first_index`. That is why the cache insists a slot
//     be a whole number of vertices and of u32 words.
//   * The STAGING RING is one host-visible buffer of `staging_segments` segments, each the size of
//     the per-frame upload budget. Frame F's uploads are host-written into segment F % segments
//     and copied into the pool by ONE copy_buffer_regions call recorded into frame F's command
//     buffer.
//   * RETIREMENT is read from fences, not inferred. Each frame's SubmitTicket is kept in order and
//     polled with Device::is_complete; the newest frame with every predecessor complete is the
//     watermark the cache's two rules run on. A frame submitted with submit_blocking has retired
//     when the call returns, which is the one case where "we submitted it" IS confirmation.
//
// Pages come from the asset's already-loaded CPU blob (VirtualGeometryAsset::page_bytes): this
// brick streams from RAM to VRAM under a budget. Disk I/O — reading a page off storage on demand —
// is a separate, later seam; nothing here assumes the bytes arrive synchronously except that
// record_uploads() reads them when it runs.
namespace rime::render {

struct VirtualGeometryPagePoolStats {
    std::uint64_t frames_begun = 0;
    std::uint64_t frames_retired = 0;   // frames whose fence the pool has seen signal
    std::uint64_t frames_failed = 0;    // end_frame() with an invalid ticket: uploads abandoned
    std::uint64_t regions_recorded = 0; // staging -> pool copies recorded
    std::uint64_t bytes_recorded = 0;
    std::uint64_t permanent_bytes = 0;  // uploaded, blocking, at registration
    std::uint32_t frames_in_flight = 0; // submitted, fence not yet seen (gauge)
};

class VirtualGeometryPagePool {
public:
    VirtualGeometryPagePool(rhi::Device& device, const VirtualGeometryPageCacheConfig& config);
    // Destroys the pool and staging buffers. The caller must have waited for every frame that used
    // them (wait_idle), as with any RHI resource.
    ~VirtualGeometryPagePool();

    VirtualGeometryPagePool(const VirtualGeometryPagePool&) = delete;
    VirtualGeometryPagePool& operator=(const VirtualGeometryPagePool&) = delete;

    [[nodiscard]] bool is_valid() const noexcept;

    // Register with the cache and upload the permanent coarse cut, BLOCKING, through a one-shot
    // staging buffer: when this returns true the coarse pages are on the device, so the cache's
    // "permanent = resident" is true by construction rather than by hope.
    [[nodiscard]] bool register_asset(assets::AssetId id,
                                      const assets::VirtualGeometryAsset& asset);

    // Open the next frame (frames are numbered by the pool, from 1): poll the fences of submitted
    // frames and begin the cache frame with the confirmed watermark. Then run the streaming driver
    // (stream_virtual_geometry) for each instance, then record_uploads, then the passes.
    void begin_frame();

    // Plan this frame's uploads, write them into the staging segment, and record the staging ->
    // pool copies into `cmd` (which must become this frame's submission). Record it BEFORE any pass
    // that reads the pool; the copy's own barrier makes the bytes visible to every later stage.
    // Returns the number of pages recorded.
    std::uint32_t record_uploads(rhi::CommandBuffer& cmd);

    // Close the frame with its submission's ticket. An invalid ticket means the submission did not
    // happen: the frame's uploads are abandoned (they never ran), never treated as retired.
    void end_frame(rhi::SubmitTicket ticket);
    // Close the frame after submit_blocking returned: it has retired.
    void end_frame_blocking();

    [[nodiscard]] VirtualGeometryPageCache& cache() noexcept { return cache_; }

    [[nodiscard]] const VirtualGeometryPageCache& cache() const noexcept { return cache_; }

    [[nodiscard]] rhi::BufferHandle buffer() const noexcept { return pool_; }

    [[nodiscard]] std::uint64_t pool_bytes() const noexcept { return cache_.config().pool_bytes(); }

    [[nodiscard]] VirtualGeometryFrame frame() const noexcept { return frame_; }

    [[nodiscard]] const VirtualGeometryPagePoolStats& stats() const noexcept { return stats_; }

private:
    struct Submitted {
        VirtualGeometryFrame frame = 0;
        rhi::SubmitTicket ticket{};
        bool retired = false; // blocking or failed: nothing left in flight
    };

    rhi::Device& device_;
    VirtualGeometryPageCache cache_;
    rhi::BufferHandle pool_;
    rhi::BufferHandle staging_;
    VirtualGeometryFrame frame_ = 0;
    VirtualGeometryFrame retired_ = 0;
    bool frame_open_ = false;
    std::deque<Submitted> submitted_;
    VirtualGeometryPagePoolStats stats_;
};

} // namespace rime::render
