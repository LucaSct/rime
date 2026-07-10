// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/render/gpu_asset_bridge.hpp"

#include <span>
#include <vector>

#include "rime/assets/texture_asset.hpp"
#include "rime/rhi/device.hpp"
#include "rime/rhi/resources.hpp"

namespace rime::render {
namespace {

rhi::Format to_rhi_format(assets::TextureFormat format) noexcept {
    // The cook tags each texture sRGB or linear by *semantic* (base-color/emissive vs
    // normal/metallic-roughness/occlusion); the RHI format must match so the GPU sampler
    // sRGB-decodes colour but not data (the M6.3 colour-space rule, now enforced at upload).
    switch (format) {
        case assets::TextureFormat::Rgba8Srgb:
            return rhi::Format::RGBA8Srgb;
        case assets::TextureFormat::Rgba8Unorm:
        default:
            return rhi::Format::RGBA8Unorm;
    }
}

} // namespace

// placeholder_ is uploaded in the initializer list: device_ and server_ (declared before it) are
// already bound, so upload() may use them. The magenta placeholder is a valid 2×2 TextureAsset with
// a full mip chain, so it flows through the same upload path as any cooked texture.
GpuAssetBridge::GpuAssetBridge(rhi::Device& device, assets::AssetServer& server)
    : device_(device), server_(server), placeholder_(upload(server.placeholder_texture())) {}

assets::TextureAssetHandle GpuAssetBridge::request_texture(const std::filesystem::path& path) {
    const assets::TextureAssetHandle handle = server_.request_texture(path);
    tracked_.insert(handle.index); // a set, so a coalesced repeat request adds no duplicate work
    return handle;
}

std::size_t GpuAssetBridge::drain() {
    std::size_t newly_uploaded = 0;
    for (const std::uint32_t index : tracked_) {
        if (uploaded_.count(index) != 0) {
            continue; // already resident on the GPU
        }
        // get() returns the asset ONLY once it is Ready (nullptr while Loading, or if it Failed),
        // so this both gates on readiness and hands us the CPU bytes to upload. A failed load
        // simply never uploads, and texture_or_placeholder() keeps returning magenta for it — the
        // honest "this texture is missing" signal.
        if (const assets::TextureAsset* texture = server_.get(assets::TextureAssetHandle{index})) {
            uploaded_.emplace(index, upload(*texture));
            ++newly_uploaded;
        }
    }
    return newly_uploaded;
}

rhi::TextureHandle GpuAssetBridge::texture_or_placeholder(assets::TextureAssetHandle handle) const {
    const auto it = uploaded_.find(handle.index);
    return it != uploaded_.end() ? it->second : placeholder_;
}

rhi::TextureHandle GpuAssetBridge::upload(const assets::TextureAsset& texture) {
    rhi::TextureDesc desc{};
    desc.extent = {texture.width, texture.height};
    desc.mip_levels = static_cast<std::uint32_t>(texture.mips.size());
    desc.format = to_rhi_format(texture.format);
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    desc.debug_name = "cooked-texture";
    const rhi::TextureHandle handle = device_.create_texture(desc);

    // The cook laid the chain out level-0-first, each mip a byte slice of `pixels` — exactly what
    // write_texture_mips consumes, so upload is one memcpy per level, no transform.
    std::vector<rhi::MipData> levels;
    levels.reserve(texture.mips.size());
    for (const assets::TextureMip& mip : texture.mips) {
        levels.push_back(
            rhi::MipData{std::span<const std::byte>(texture.pixels.data() + mip.offset, mip.size)});
    }
    device_.write_texture_mips(handle, levels);
    return handle;
}

} // namespace rime::render
