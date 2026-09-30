// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The M18.5 lavapipe proof: streamed pages, drawn out of a real page pool, survive a teleport
// without holes. Structural, never golden — every assertion compares two renders of the same
// fixture (vg_streaming_fixture.hpp) or checks a counter:
//
//   * REFERENCE: instance B at full residency, drawn by the pre-M18.5 byte-copy path (no pool).
//   * TELEPORT: the camera has been looking at instance A long enough for A's fine pages to fill
//     the pool; it jumps to B, whose wanted cut is entirely non-resident.
//       - the first frame after the jump covers every pixel the reference covers (the coarse cut,
//         drawn from the pool's permanent slot — a fallback, and counted);
//       - every frame on the way covers them too, and only ever shows clusters of its own cut;
//       - within N frames the cut converges and the visibility IDs equal the reference exactly,
//         which also proves the pool addressing: a vertex fetched from the wrong slot offset would
//         put a different triangle, or none, on some pixel;
//       - no frame's uploads exceed the budget.
//
// Frames are genuinely pipelined: two are kept in flight, each with its own graph, pass and
// readback, and the pool learns retirement only from fence polls. So A's pages can be evicted only
// once the fences of the frames that drew A have signalled, exactly as in a real loop.

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/render/passes.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/virtual_geometry_page_pool.hpp"
#include "rime/render/virtual_geometry_residency.hpp"
#include "rime/render/virtual_geometry_selection.hpp"
#include "rime/render/virtual_geometry_visibility_id.hpp"
#include "rime/render/virtual_geometry_visibility_pass.hpp"
#include "rime/rhi/device.hpp"
#include "vg_streaming_fixture.hpp"

namespace {

using namespace rime;
using namespace rime::render;
namespace fx = rime::test::vg_streaming;

constexpr std::uint32_t kSize = 64;
constexpr assets::AssetId kA{201};
constexpr assets::AssetId kB{202};
constexpr std::uint32_t kGeneration = 1;
constexpr VirtualGeometryPageCacheConfig kConfig{8, 192, 2 * 192, 3};
const std::vector<std::uint32_t> kLeaves{3, 4, 5, 6};

using Ids = std::vector<VirtualGeometryVisibilityWords>;

// Cluster slot == cluster index and one fixed generation, so the pool path and the byte-copy
// reference write identical IDs for identical coverage.
std::vector<VirtualGeometryClusterDraw> draws_for(const assets::VirtualGeometryAsset& asset,
                                                  const VirtualGeometrySelection& selection) {
    std::vector<VirtualGeometryClusterDraw> draws;
    for (const std::uint32_t g : selection.groups) {
        const assets::VirtualGeometryGroup& group = asset.groups[g];
        for (std::uint32_t c = 0; c < group.cluster_count; ++c) {
            draws.push_back({group.first_cluster + c, group.first_cluster + c, kGeneration});
        }
    }
    return draws;
}

// One frame's worth of GPU objects. Two of these rotate, so a frame's graph (its transients), its
// pass (its per-declare buffers) and its readback outlive the next frame's recording.
struct FrameContext {
    explicit FrameContext(rhi::Device& d)
        : device(d), pass(d), graph(std::make_unique<RenderGraph>(d)) {
        rhi::BufferDesc bd{};
        bd.size = kSize * kSize * sizeof(VirtualGeometryVisibilityWords);
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryUsage::GpuToCpu;
        readback = device.create_buffer(bd);
    }

    ~FrameContext() { device.destroy(readback); }

    FrameContext(const FrameContext&) = delete;
    FrameContext& operator=(const FrameContext&) = delete;

    rhi::Device& device;
    VirtualGeometryVisibilityPass pass;
    std::unique_ptr<RenderGraph> graph;
    rhi::BufferHandle readback;
    rhi::SubmitTicket ticket{};
    // What this frame drew, for checking once its pixels are back.
    assets::AssetId asset{};
    VirtualGeometrySelection selection;
    std::uint64_t frame = 0;
};

// Declare the visibility pass into `graph`, execute it into `cmd`, and copy the IDs out.
void record_visibility(RenderGraph& graph,
                       VirtualGeometryVisibilityPass& pass,
                       const VirtualGeometryVisibilityRequest& request,
                       rhi::CommandBuffer& cmd,
                       rhi::BufferHandle readback) {
    const RGTexture ids = graph.create_texture({{kSize, kSize}, rhi::Format::RG32Uint, "vg-ids"});
    const RGTexture depth_bits =
        graph.create_texture({{kSize, kSize}, rhi::Format::R32Uint, "vg-depth-bits"});
    const RGTexture depth = graph.create_texture({{kSize, kSize}, kDepthFormat, "vg-depth"});
    graph.export_texture(ids);
    (void)pass.declare(graph, ids, depth_bits, depth, request);
    graph.execute(cmd);
    cmd.copy_texture_to_buffer(graph.physical(ids), readback);
}

Ids read_ids(rhi::Device& device, rhi::BufferHandle readback) {
    Ids ids(kSize * kSize);
    device.read_buffer(readback, ids.data(), ids.size() * sizeof(VirtualGeometryVisibilityWords));
    return ids;
}

std::uint32_t covered(const Ids& ids) {
    std::uint32_t n = 0;
    for (const auto& id : ids) {
        n += id == kInvalidVirtualGeometryVisibilityWords ? 0 : 1;
    }
    return n;
}

} // namespace

TEST_CASE("vg streaming: a teleport renders hole-free on the coarse cut, then converges (M18.5)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (std::getenv("RIME_REQUIRE_VULKAN") != nullptr) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the streaming teleport proof");
        return;
    }
    if (!device->adapter().gpu_driven_draw) {
        MESSAGE("device lacks gpu_driven_draw — the visibility pass cannot draw; skipping");
        return;
    }

    const assets::VirtualGeometryAsset asset = fx::tree();
    REQUIRE(assets::validate_virtual_geometry(asset) == assets::VirtualGeometryError::None);

    // ── Reference: B at full residency through the byte-copy path ──────────────────────────
    Ids reference;
    {
        VirtualGeometryResidency full;
        REQUIRE(full.register_asset(kB, asset));
        for (std::uint32_t page = 1; page < asset.pages.size(); ++page) {
            REQUIRE(full.request_page(kB, page));
            REQUIRE(full.complete_page(kB, page));
        }
        const VirtualGeometrySelection selection =
            select_virtual_geometry(asset, {fx::kNear, 1.0f, full.page_residency_bytes(kB)});
        REQUIRE(selection.groups == kLeaves);
        const std::vector<VirtualGeometryClusterDraw> draws = draws_for(asset, selection);
        VirtualGeometryVisibilityRequest request{};
        request.asset = &asset;
        request.asset_id = kB;
        request.residency = &full;
        request.selection = &selection;
        request.clusters = draws;
        request.clip_from_object = core::identity();
        FrameContext ctx(*device);
        auto cmd = device->begin_commands();
        record_visibility(*ctx.graph, ctx.pass, request, *cmd, ctx.readback);
        device->submit_blocking(*cmd);
        REQUIRE(ctx.pass.stats().drawn == 4);
        reference = read_ids(*device, ctx.readback);
    }
    const std::uint32_t reference_covered = covered(reference);
    REQUIRE(reference_covered >= 30 * 30); // [-0.5, 0.5]^2 on 64^2 is 32^2 pixels

    // ── The streamed run ────────────────────────────────────────────────────────────────────
    VirtualGeometryPagePool pool(*device, kConfig);
    REQUIRE(pool.is_valid());
    REQUIRE(pool.register_asset(kA, asset));
    REQUIRE(pool.register_asset(kB, asset));
    CHECK(pool.stats().permanent_bytes == 2 * fx::kPageBytes);

    std::array<std::unique_ptr<FrameContext>, 2> ring{std::make_unique<FrameContext>(*device),
                                                      std::make_unique<FrameContext>(*device)};
    std::uint64_t teleport_frame = 0;
    std::optional<std::uint64_t> converged_frame;
    std::uint32_t checked_after_teleport = 0;

    // Check a frame whose GPU work is done.
    const auto check = [&](FrameContext& ctx) {
        const Ids ids = read_ids(*device, ctx.readback);
        // Every visible pixel names a cluster of this frame's cut (cluster slot == cluster index).
        for (const auto& id : ids) {
            if (id == kInvalidVirtualGeometryVisibilityWords) {
                continue;
            }
            const auto unpacked = unpack_virtual_geometry_visibility_id64(id);
            REQUIRE(unpacked.has_value());
            bool on_cut = false;
            for (const std::uint32_t g : ctx.selection.groups) {
                on_cut = on_cut || unpacked->cluster == asset.groups[g].first_cluster;
            }
            CHECK(on_cut);
        }
        if (ctx.asset != kB) {
            return;
        }
        ++checked_after_teleport;
        // Hole-free: every pixel the full-residency render covers is covered in THIS frame.
        std::uint32_t holes = 0;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            const bool wanted = !(reference[i] == kInvalidVirtualGeometryVisibilityWords);
            holes += (wanted && ids[i] == kInvalidVirtualGeometryVisibilityWords) ? 1 : 0;
        }
        CHECK(holes == 0);
        if (ctx.frame == teleport_frame) {
            // The first frame after the jump can only have drawn the permanent coarse cut.
            CHECK(ctx.selection.groups == std::vector<std::uint32_t>{0});
            CHECK(covered(ids) >= reference_covered);
        }
        if (ctx.selection.groups == kLeaves && !converged_frame) {
            converged_frame = ctx.frame;
            CHECK(ids == reference);
        }
    };

    const auto run_frame = [&](assets::AssetId id) {
        FrameContext& ctx = *ring[pool.frame() % ring.size()];
        if (ctx.ticket.is_valid()) {
            device->wait(ctx.ticket); // this slot's previous frame; the other stays in flight
            check(ctx);
            ctx.graph = std::make_unique<RenderGraph>(*device);
        }
        pool.begin_frame();
        ctx.frame = pool.frame();
        ctx.asset = id;
        ctx.selection = stream_virtual_geometry(pool.cache(), id, asset, fx::kNear, 1.0f);
        CHECK_FALSE(ctx.selection.groups.empty()); // CHECK: let the pixel proof see it too
        const std::vector<VirtualGeometryClusterDraw> draws = draws_for(asset, ctx.selection);

        VirtualGeometryVisibilityRequest request{};
        request.asset = &asset;
        request.asset_id = id;
        request.selection = &ctx.selection;
        request.clusters = draws;
        request.clip_from_object = core::identity();
        request.page_pool = &pool;

        auto cmd = device->begin_commands();
        pool.record_uploads(*cmd); // before any pass that reads the pool
        CHECK(pool.cache().stats().frame_upload_bytes <= kConfig.upload_budget_bytes);
        const std::uint32_t skipped_before = ctx.pass.stats().skipped_not_resident;
        record_visibility(*ctx.graph, ctx.pass, request, *cmd, ctx.readback);
        // The cut is selected from the pool's confirmed residency, so nothing it names is refused.
        CHECK(ctx.pass.stats().skipped_not_resident == skipped_before);
        ctx.ticket = device->submit(std::move(cmd));
        REQUIRE(ctx.ticket.is_valid());
        pool.end_frame(ctx.ticket);
    };

    // Look at A until its leaves are resident (the pool is then full of A's pages).
    for (int i = 0; i < 40 && !(pool.cache().is_resident(kA, 3) && pool.cache().is_resident(kA, 6));
         ++i) {
        run_frame(kA);
    }
    run_frame(kA);
    REQUIRE(pool.cache().free_slots() == 0);
    const VirtualGeometryPageCacheStats before = pool.cache().stats();

    // Teleport.
    teleport_frame = pool.frame() + 1;
    for (int i = 0; i < 40 && !converged_frame; ++i) {
        run_frame(kB);
    }
    for (auto& ctx : ring) { // drain
        if (ctx->ticket.is_valid()) {
            device->wait(ctx->ticket);
            check(*ctx);
            ctx->ticket = {};
        }
    }
    device->wait_idle();

    REQUIRE(converged_frame.has_value());
    CHECK(checked_after_teleport >= 2);
    const VirtualGeometryPageCacheStats& after = pool.cache().stats();
    CHECK(after.fallbacks > before.fallbacks);
    CHECK(after.evictions - before.evictions == 6); // all of A's streamed pages, and no more
    CHECK(after.deferred_budget > before.deferred_budget);
    CHECK(after.max_frame_upload_bytes <= kConfig.upload_budget_bytes);
    CHECK(after.use_of_nonresident == 0);
    CHECK(pool.stats().frames_failed == 0);
    CHECK(pool.stats().frames_retired > 0);
    MESSAGE("teleport at frame " << teleport_frame << ", converged at frame " << *converged_frame
                                 << " (" << (*converged_frame - teleport_frame + 1)
                                 << " frames); fallbacks " << after.fallbacks - before.fallbacks
                                 << ", deferred no-slot "
                                 << after.deferred_no_slot - before.deferred_no_slot << ", budget "
                                 << after.deferred_budget - before.deferred_budget);
}
