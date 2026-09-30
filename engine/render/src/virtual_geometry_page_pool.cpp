// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The page pool (M18.5): executes the cache's plan against real RHI buffers. See the header for
// the memory layout; the notes here are about the fence watermark.

#include "rime/render/virtual_geometry_page_pool.hpp"

#include <vector>

namespace rime::render {

VirtualGeometryPagePool::VirtualGeometryPagePool(rhi::Device& device,
                                                 const VirtualGeometryPageCacheConfig& config)
    : device_(device), cache_(config) {
    if (!cache_.is_valid()) {
        return;
    }
    rhi::BufferDesc pd{};
    pd.size = config.pool_bytes();
    // Storage: the shaders pull vertices and indices out of it. TransferDst: the uploads land in
    // it. Device-local: after the copy, only the GPU ever reads it.
    pd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferDst;
    pd.memory = rhi::MemoryUsage::GpuOnly;
    pd.debug_name = "vg-page-pool";
    pool_ = device_.create_buffer(pd);

    rhi::BufferDesc sd{};
    sd.size = config.staging_bytes();
    sd.usage = rhi::BufferUsage::TransferSrc;
    sd.memory = rhi::MemoryUsage::CpuToGpu;
    sd.debug_name = "vg-page-staging";
    staging_ = device_.create_buffer(sd);
}

VirtualGeometryPagePool::~VirtualGeometryPagePool() {
    device_.destroy(staging_);
    device_.destroy(pool_);
}

bool VirtualGeometryPagePool::is_valid() const noexcept {
    return cache_.is_valid() && pool_.is_valid() && staging_.is_valid();
}

bool VirtualGeometryPagePool::register_asset(assets::AssetId id,
                                             const assets::VirtualGeometryAsset& asset) {
    if (!is_valid()) {
        return false;
    }
    std::vector<VirtualGeometryPageUpload> permanent;
    if (!cache_.register_asset(id, asset, permanent)) {
        return false;
    }
    if (permanent.empty()) {
        return true; // already registered, or an asset whose coarse cut is somehow page-free
    }
    // A one-shot staging buffer rather than the ring: registration can happen mid-run, when every
    // ring segment may belong to a frame in flight. The permanent pages' slots were free, so no
    // in-flight frame reads them and the blocking copy races with nothing.
    std::vector<std::byte> bytes;
    std::vector<rhi::BufferCopyRegion> regions;
    for (const VirtualGeometryPageUpload& u : permanent) {
        const assets::VirtualGeometryPage& page = asset.pages[u.page];
        regions.push_back(
            {bytes.size(), std::uint64_t{u.slot} * cache_.config().slot_bytes, u.bytes});
        const auto* first = asset.page_bytes.data() + page.byte_offset;
        bytes.insert(bytes.end(), first, first + u.bytes);
    }
    rhi::BufferDesc sd{};
    sd.size = bytes.size();
    sd.usage = rhi::BufferUsage::TransferSrc;
    sd.memory = rhi::MemoryUsage::CpuToGpu;
    sd.initial_data = bytes.data();
    sd.debug_name = "vg-page-bootstrap";
    const rhi::BufferHandle bootstrap = device_.create_buffer(sd);
    if (!bootstrap.is_valid()) {
        return false;
    }
    auto cmd = device_.begin_commands();
    cmd->copy_buffer_regions(bootstrap, pool_, regions);
    device_.submit_blocking(*cmd);
    device_.destroy(bootstrap);
    stats_.permanent_bytes += bytes.size();
    return true;
}

void VirtualGeometryPagePool::begin_frame() {
    // Walk the submissions in order and stop at the first that is still running. Completion is
    // taken strictly in order even if a later fence happens to signal first: the watermark means
    // "every frame up to here is done", and that claim is what slot reuse leans on.
    while (!submitted_.empty()) {
        Submitted& front = submitted_.front();
        if (!front.retired && !device_.is_complete(front.ticket)) {
            break;
        }
        retired_ = front.frame;
        ++stats_.frames_retired;
        submitted_.pop_front();
    }
    stats_.frames_in_flight = static_cast<std::uint32_t>(submitted_.size());
    ++frame_;
    frame_open_ = true;
    ++stats_.frames_begun;
    cache_.begin_frame(frame_, retired_);
}

std::uint32_t VirtualGeometryPagePool::record_uploads(rhi::CommandBuffer& cmd) {
    if (!is_valid() || !frame_open_) {
        return 0;
    }
    const std::span<const VirtualGeometryPageUpload> plan = cache_.plan_uploads();
    if (plan.empty()) {
        return 0;
    }
    std::vector<rhi::BufferCopyRegion> regions;
    regions.reserve(plan.size());
    for (const VirtualGeometryPageUpload& u : plan) {
        const assets::VirtualGeometryAsset* asset = cache_.asset(u.asset);
        const assets::VirtualGeometryPage& page = asset->pages[u.page];
        // The host write goes into this frame's segment, which the cache handed out only after the
        // frame that last used it retired — so no GPU copy is still reading these bytes.
        device_.write_buffer(
            staging_, asset->page_bytes.data() + page.byte_offset, u.bytes, u.staging_offset);
        regions.push_back(
            {u.staging_offset, std::uint64_t{u.slot} * cache_.config().slot_bytes, u.bytes});
        stats_.bytes_recorded += u.bytes;
    }
    cmd.copy_buffer_regions(staging_, pool_, regions);
    stats_.regions_recorded += regions.size();
    return static_cast<std::uint32_t>(regions.size());
}

void VirtualGeometryPagePool::end_frame(rhi::SubmitTicket ticket) {
    if (!frame_open_) {
        return;
    }
    frame_open_ = false;
    if (!ticket.is_valid()) {
        // is_complete() answers true for an invalid ticket — right for "is anything in flight?",
        // wrong for "did the copies run?". Nothing ran, so the uploads are undone, not promoted.
        cache_.abandon_frame(frame_);
        ++stats_.frames_failed;
        submitted_.push_back({frame_, ticket, true});
        return;
    }
    submitted_.push_back({frame_, ticket, false});
}

void VirtualGeometryPagePool::end_frame_blocking() {
    if (!frame_open_) {
        return;
    }
    frame_open_ = false;
    submitted_.push_back({frame_, {}, true});
}

} // namespace rime::render
