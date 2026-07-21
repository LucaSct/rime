// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// The 3-D storage-image spike (ADR-0032 §10, m10.4): a "verify, don't assume" brick. Two RHI
// primitives already exist independently — a texture with `depth > 1` (a 3-D/volume image,
// ADR-0013, proven by tests/rhi/volume_texture_test.cpp for *sampling*) and `TextureUsage::Storage`
// + compute `imageStore` (M5.2, proven by tests/rhi/compute_test.cpp for a 2-D image) — but the
// *combination*, a compute shader imageStore()ing into a 3-D storage image, had never been
// exercised, and m10.4b's SDF clipmap needs exactly that combination for its compose pass. This
// test is the spike: dispatch a compute shader that writes every voxel of a 3-D storage image with
// a value encoding its own coordinate, read the whole volume back, and check every voxel exactly.
//
// Running it found a real gap, now fixed: `CommandBuffer::copy_texture_to_buffer` hardcoded the
// copy region's depth to 1 (engine/rhi/src/vulkan/command_buffer_vulkan.cpp), silently truncating
// a volume readback to its z=0 slice — invisible until something actually tried to read back more
// than one slice, which nothing had before this brick. This test is what keeps that combination
// covered from here on; per ADR-0032 §10, "if the combination has a gap, fix the gap; if it just
// works, the test is the proof it keeps working."

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/rhi/rhi.hpp"
#include "volume_stamp.comp.spv.h"

namespace {
bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}
} // namespace

TEST_CASE("rhi: a compute dispatch imageStores into a 3-D storage image and reads back exactly "
          "(ADR-0032 §10 spike)") {
    using namespace rime::rhi;

    auto device = create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the 3-D storage-image spike");
        return;
    }

    // 8x8x6: width/height a clean multiple of the 4x4x4 workgroup, depth deliberately NOT (6 needs
    // 2 groups of 4 = 8 invocations covering only 6 valid voxels) — so the shader's own
    // imageSize()-bounds check is exercised on the extra dimension a volume adds, not just x/y.
    constexpr std::uint32_t kW = 8, kH = 8, kD = 6;

    ShaderDesc csd{};
    csd.stage = ShaderStage::Compute;
    csd.spirv = volume_stamp_comp_spv;
    csd.spirv_size_bytes = sizeof(volume_stamp_comp_spv);
    csd.debug_name = "volume_stamp.comp";
    const ShaderHandle csh = device->create_shader(csd);

    const BindingDesc bindings[] = {{0, BindingType::StorageImage, StageMask::Compute}};
    ComputePipelineDesc pd{};
    pd.shader = csh;
    pd.bindings = bindings;
    pd.debug_name = "volume-stamp-pipeline";
    const PipelineHandle pipe = device->create_compute_pipeline(pd);

    TextureDesc td{};
    td.extent = {kW, kH};
    td.depth = kD; // > 1: a 3-D (volume) image, not an ordinary 2-D one
    td.format = Format::RGBA8Unorm;
    td.usage = TextureUsage::Storage | TextureUsage::TransferSrc; // written by CS, read back raw
    td.debug_name = "volume-storage-spike";
    const TextureHandle vol = device->create_texture(td);

    const std::uint64_t bytes = static_cast<std::uint64_t>(kW) * kH * kD * 4;
    BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = BufferUsage::TransferDst;
    rbd.memory = MemoryUsage::GpuToCpu;
    rbd.debug_name = "volume-storage-readback";
    const BufferHandle readback = device->create_buffer(rbd);

    auto cmd = device->begin_commands();
    cmd->bind_compute_pipeline(pipe);
    cmd->bind_storage_image(0, vol); // transitions the 3-D image to GENERAL, same as the 2-D path
    cmd->dispatch((kW + 3) / 4, (kH + 3) / 4, (kD + 3) / 4);
    cmd->copy_texture_to_buffer(vol, readback); // the fixed depth-aware readback
    device->submit_blocking(*cmd);

    std::vector<std::uint8_t> px(bytes);
    device->read_buffer(readback, px.data(), px.size(), 0);

    // Every voxel, not a sample: x/y/z < 256 so n/255.0 round-trips through UNORM8 exactly, and the
    // volume is small enough on lavapipe that an exhaustive check is cheap and gives a much sharper
    // proof than spot-checking a few slices (the earlier depth bug affected everything past z=0).
    std::size_t mismatches = 0;
    for (std::uint32_t z = 0; z < kD; ++z) {
        for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                const std::size_t i =
                    (static_cast<std::size_t>(z) * kH * kW + static_cast<std::size_t>(y) * kW + x) *
                    4;
                if (px[i + 0] != x || px[i + 1] != y || px[i + 2] != z || px[i + 3] != 255) {
                    ++mismatches;
                }
            }
        }
    }
    CHECK(mismatches == 0);

    device->destroy(readback);
    device->destroy(vol);
    device->destroy(pipe);
    device->destroy(csh);
}
