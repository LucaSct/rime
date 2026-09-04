// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proof for the ASYNCHRONOUS SUBMISSION seam (ADR-0030 s1.1) — `submit()` / `is_complete()` /
// `wait()`, the non-blocking counterpart to `submit_blocking`.
//
// This file exists because the seam had no direct test at all. Two shipping subsystems already ride
// it — `FrameStreamer` keeps a ring of readback slots keyed on ticket completion, and `ScenePicker`
// hides its glass-to-CPU stall behind it — but both prove their own behaviour, not the seam's, and
// `is_complete` appeared nowhere in `tests/` before this. That was survivable while the seam was a
// side path. It stops being survivable the moment the FRAME rides it: the pipelined loop M17 needs
// (CPU of frame N+1 overlapping GPU of frame N) has this contract underneath every frame it draws.
//
// What makes the contract subtle, and therefore worth pinning:
//
//   * `submit()` takes OWNERSHIP of the command buffer, unlike `submit_blocking`, because the
//     backend must keep it — and the transient descriptor pools it baked — alive past the caller's
//     scope. Whoever first observes completion reclaims both.
//   * Ticket ids are monotonic so a stale ticket can never alias a live submission
//     (`vulkan_backend.hpp:228`). Reuse an id and a caller polling an old ticket would be told
//     "done" about someone else's work — or worse, reclaim it.
//   * An invalid, unknown, or already-reclaimed ticket answers `is_complete() == true`: there is
//     nothing left in flight. That is a deliberate answer, not a fallthrough, and a caller that
//     loops until complete depends on it to terminate.
//
// The central case keeps several submissions in flight AT ONCE and then checks that each kept its
// own work — which is the pipelining property itself, stated at the level where it is cheap to
// falsify. Distinguishing them needs no new shader: submission k dispatches k+1 workgroups, so the
// length of the written prefix in its output buffer names which submission wrote it. If the
// backend serialized the queue, aliased a ticket, or handed a command buffer to the wrong fence,
// the prefixes come back wrong or the run deadlocks — none of which a "did it complete" check
// alone would notice.

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "fill.comp.spv.h"
#include "rime/rhi/rhi.hpp"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// Must equal fill.comp's local_size_x. One workgroup writes exactly this many uints.
constexpr std::uint32_t kGroupWidth = 64;

} // namespace

TEST_CASE("rhi: submissions run in flight together, and each keeps its own work (ADR-0030 s1.1)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the async-submission proof");
        return;
    }

    constexpr std::uint32_t kInFlight = 4;
    constexpr std::uint32_t kCount = kInFlight * kGroupWidth; // every buffer is full-sized

    ShaderDesc csd{};
    csd.stage = ShaderStage::Compute;
    csd.spirv = fill_comp_spv;
    csd.spirv_size_bytes = sizeof(fill_comp_spv);
    csd.debug_name = "fill.comp";
    const ShaderHandle csh = device->create_shader(csd);

    const BindingDesc bindings[] = {{0, BindingType::StorageBuffer, StageMask::Compute}};
    ComputePipelineDesc pd{};
    pd.shader = csh;
    pd.bindings = bindings;
    pd.debug_name = "async-fill-pipeline";
    const PipelineHandle pipe = device->create_compute_pipeline(pd);

    // One output buffer per in-flight submission, zeroed first so "not written by this dispatch"
    // is a value the CPU can tell apart from "written" rather than whatever the allocator left.
    std::array<BufferHandle, kInFlight> buffers{};
    const std::vector<std::uint32_t> zeros(kCount, 0u);
    for (std::uint32_t k = 0; k < kInFlight; ++k) {
        BufferDesc bd{};
        bd.size = kCount * sizeof(std::uint32_t);
        bd.usage = BufferUsage::Storage;
        bd.memory = MemoryUsage::GpuToCpu; // host-readable, so every element is verifiable
        bd.debug_name = "async-fill-output";
        buffers[k] = device->create_buffer(bd);
        device->write_buffer(buffers[k], zeros.data(), zeros.size() * sizeof(std::uint32_t));
    }

    // Record and submit all of them WITHOUT waiting in between. This is the shape a pipelined
    // frame loop has, and the shape `submit_blocking` cannot express.
    std::array<SubmitTicket, kInFlight> tickets{};
    for (std::uint32_t k = 0; k < kInFlight; ++k) {
        auto cmd = device->begin_commands();
        cmd->bind_compute_pipeline(pipe);
        cmd->bind_storage_buffer(0, buffers[k]);
        cmd->dispatch(k + 1, 1, 1); // k+1 groups => a written prefix that names this submission
        tickets[k] = device->submit(std::move(cmd));
    }

    SUBCASE("every ticket is valid and distinct, so a stale one cannot alias a live one") {
        for (std::uint32_t k = 0; k < kInFlight; ++k)
            CHECK(tickets[k].is_valid());
        for (std::uint32_t a = 0; a < kInFlight; ++a) {
            for (std::uint32_t b = a + 1; b < kInFlight; ++b)
                CHECK(tickets[a].id != tickets[b].id);
        }
    }

    // Drain. `wait` is the blocking half of the contract; the poll half is exercised below.
    for (std::uint32_t k = 0; k < kInFlight; ++k)
        device->wait(tickets[k]);

    SUBCASE("each submission's own work landed in its own buffer — the vacuity guard") {
        // Completion is not the claim. A seam that reported "done" while dropping the work, or
        // while running submission 2's commands against submission 0's buffer, would pass every
        // is_complete() check ever written. So verify the WORK, element for element.
        std::size_t buffers_wrong = 0;
        for (std::uint32_t k = 0; k < kInFlight; ++k) {
            std::vector<std::uint32_t> data(kCount);
            device->read_buffer(buffers[k], data.data(), data.size() * sizeof(std::uint32_t), 0);
            const std::uint32_t written = (k + 1) * kGroupWidth;
            std::size_t mismatches = 0;
            for (std::uint32_t i = 0; i < kCount; ++i) {
                const std::uint32_t want = i < written ? i * 7u + 3u : 0u;
                if (data[i] != want)
                    ++mismatches;
            }
            if (mismatches != 0)
                ++buffers_wrong;
        }
        CHECK(buffers_wrong == 0);
    }

    SUBCASE("a completed ticket stays complete, and reclaiming twice is not an error") {
        for (std::uint32_t k = 0; k < kInFlight; ++k) {
            CHECK(device->is_complete(tickets[k]));
            CHECK(device->is_complete(tickets[k])); // already reclaimed => still true
            device->wait(tickets[k]);               // no-op on an already-reclaimed ticket
        }
    }

    SUBCASE("an invalid or unknown ticket reports complete rather than hanging a poll loop") {
        // A caller draining "while (!is_complete(t))" must terminate on a ticket that names
        // nothing — the default-constructed one it started with, or one whose submission was
        // reclaimed by someone else. Returning false there is an infinite loop, not a safe default.
        CHECK(device->is_complete(SubmitTicket{}));
        CHECK(device->is_complete(SubmitTicket{~std::uint64_t{0}}));
        device->wait(SubmitTicket{}); // must not block
    }

    SUBCASE("a fresh submission after a drain is independent of the drained tickets") {
        // The monotonic-id claim, from the caller's side: submitting again must not resurrect a
        // reclaimed ticket, and the old ticket must not start answering for the new work.
        auto cmd = device->begin_commands();
        cmd->bind_compute_pipeline(pipe);
        cmd->bind_storage_buffer(0, buffers[0]);
        cmd->dispatch(kInFlight, 1, 1); // fill buffer 0 completely, unlike its first pass
        const SubmitTicket fresh = device->submit(std::move(cmd));
        REQUIRE(fresh.is_valid());
        for (std::uint32_t k = 0; k < kInFlight; ++k)
            CHECK(fresh.id != tickets[k].id);

        // Poll rather than wait, so the non-blocking half of the contract is exercised too. It
        // must go true on its own; a seam that only ever completes under wait() would pass every
        // other case in this file.
        int spins = 0;
        while (!device->is_complete(fresh) && spins < 1'000'000)
            ++spins;
        CHECK(device->is_complete(fresh));

        std::vector<std::uint32_t> data(kCount);
        device->read_buffer(buffers[0], data.data(), data.size() * sizeof(std::uint32_t), 0);
        std::size_t mismatches = 0;
        for (std::uint32_t i = 0; i < kCount; ++i) {
            if (data[i] != i * 7u + 3u)
                ++mismatches;
        }
        CHECK(mismatches == 0);
    }

    for (std::uint32_t k = 0; k < kInFlight; ++k)
        device->destroy(buffers[k]);
    device->destroy(pipe);
    device->destroy(csh);
}
