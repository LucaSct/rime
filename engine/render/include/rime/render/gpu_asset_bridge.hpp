// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#include "rime/assets/asset_server.hpp"
#include "rime/rhi/types.hpp"

// The GPU asset bridge: turns CPU-resident cooked assets (engine/assets) into live GPU resources on
// the frame thread. engine/assets links neither rhi nor render, so a "Ready" asset is validated CPU
// bytes, not an uploaded texture — the render layer owns GPU residency. This is the single allowed
// assets↔render edge: render *consumes* cooked assets; assets never depends on render (ADR-0025).
//
// Each frame, after AssetServer::pump() readies CPU loads, drain() uploads any newly-ready texture
// through the RHI and caches its handle, so a material's borrowed placeholder can be swapped for
// the real texture. This is the FIRST consumer of the RHI's per-mip upload path
// (Device::write_texture_mips, built in M6.3): the cook generated the whole gamma-correct mip chain
// offline, so the bridge uploads each level verbatim rather than regenerating on the GPU.
//
// v1 scope: textures. Mesh upload and the material pipeline (resolving a material's texture
// AssetIds into a PbrMaterialDesc) are the next bricks; the drain() seam and the placeholder swap
// generalize to them unchanged.
namespace rime::rhi {
class Device; // used only by reference here — forward-declared to keep this header light
}

namespace rime::render {

class GpuAssetBridge {
public:
    // Uploads the AssetServer's magenta placeholder texture once, so texture_or_placeholder() can
    // return a valid handle for a still-loading texture without a per-call branch.
    GpuAssetBridge(rhi::Device& device, assets::AssetServer& server);

    GpuAssetBridge(const GpuAssetBridge&) = delete;
    GpuAssetBridge& operator=(const GpuAssetBridge&) = delete;

    // Request a texture and track it for GPU upload. Forwards to the AssetServer (so
    // path-coalescing, the async job, and the CPU placeholder all apply) and remembers the handle
    // so drain() uploads it once the load completes. Repeat requests for the same path coalesce to
    // one handle and one upload.
    [[nodiscard]] assets::TextureAssetHandle request_texture(const std::filesystem::path& path);

    // Frame thread, once per frame right after AssetServer::pump(): upload every tracked texture
    // that has become Ready and is not yet on the GPU. Returns how many were newly uploaded.
    // Idempotent — an already-uploaded or still-loading texture is skipped.
    std::size_t drain();

    // The GPU handle for a requested texture: the uploaded texture once drained, otherwise the
    // magenta placeholder. Never invalid, so material binding never branches on "is it loaded
    // yet?".
    [[nodiscard]] rhi::TextureHandle
    texture_or_placeholder(assets::TextureAssetHandle handle) const;

    [[nodiscard]] rhi::TextureHandle placeholder_texture() const noexcept { return placeholder_; }

    // How many textures the bridge has uploaded to the GPU (the upload-once counter a proof
    // asserts).
    [[nodiscard]] std::size_t uploaded_count() const noexcept { return uploaded_.size(); }

private:
    // Create an RHI texture from a cooked TextureAsset and upload its whole mip chain verbatim.
    [[nodiscard]] rhi::TextureHandle upload(const assets::TextureAsset& texture);

    rhi::Device& device_;
    assets::AssetServer& server_;
    rhi::TextureHandle placeholder_{};          // magenta, uploaded once at ctor
    std::unordered_set<std::uint32_t> tracked_; // requested texture-handle indices
    std::unordered_map<std::uint32_t, rhi::TextureHandle> uploaded_; // handle index → GPU texture
};

} // namespace rime::render
