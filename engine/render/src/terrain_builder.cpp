// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#include "rime/render/terrain_builder.hpp"

#include <utility>

#include "rime/assets/manifest.hpp"
#include "rime/assets/material_asset.hpp"
#include "rime/assets/terrain_layer_asset.hpp"
#include "rime/assets/texture_asset.hpp"
#include "rime/core/diagnostics/log.hpp"
#include "rime/rhi/device.hpp"

namespace rime::render {

TerrainLayerBuilder::TerrainLayerBuilder(rhi::Device& device,
                                         assets::AssetServer& server,
                                         const assets::Manifest& manifest,
                                         std::filesystem::path cooked_dir)
    : device_(device), server_(server), manifest_(manifest), cooked_dir_(std::move(cooked_dir)) {}

TerrainLayerBuilder::~TerrainLayerBuilder() {
    std::size_t leaked = 0;
    for (std::size_t i = 0; i < palettes_.size(); ++i) {
        if (palettes_[i].live) {
            ++leaked;
            release(static_cast<TerrainPaletteHandle>(i));
        }
    }
    if (leaked > 0) {
        RIME_WARN("terrain builder: {} palette(s) were never released", leaked);
    }
    for (auto& [id, t] : textures_) {
        device_.destroy(t.gpu);
    }
}

TerrainPaletteHandle TerrainLayerBuilder::request(const std::array<assets::AssetId, 4>& layers) {
    TerrainPaletteHandle h = kInvalidTerrainPalette;
    if (!free_.empty()) {
        h = free_.back();
        free_.pop_back();
    } else {
        h = static_cast<TerrainPaletteHandle>(palettes_.size());
        palettes_.emplace_back();
    }
    Palette& p = palettes_[h];
    p = Palette{};
    p.live = true;
    ++counters_.palettes_requested;
    for (std::size_t k = 0; k < 4; ++k) {
        begin_slot(p.slots[k], layers[k]);
    }
    return h;
}

void TerrainLayerBuilder::begin_slot(Slot& slot, assets::AssetId id) {
    if (!id.is_valid()) {
        slot.step = Slot::Step::Done; // unused: the default layer, which upload() never reads
        return;
    }
    const assets::ManifestEntry* entry = manifest_.find_by_id(id);
    if (entry == nullptr) {
        ++counters_.unresolved_ids;
        fail(slot);
        return;
    }
    // THE KIND DISPATCH (ADR-0066 §2). Anything other than the two kinds a palette slot may name
    // is refused — a texture or a mesh id in a palette is a cook bug, not a colour to guess.
    switch (entry->kind) {
        case assets::AssetKind::Material:
            slot.material = server_.request_material(cooked_dir_ / entry->cooked_file);
            slot.step = Slot::Step::WaitMaterial;
            return;
        case assets::AssetKind::TerrainLayer:
            slot.is_terrain_layer = true;
            slot.layer_record = server_.request_terrain_layer(cooked_dir_ / entry->cooked_file);
            slot.step = Slot::Step::WaitLayer;
            return;
        default:
            ++counters_.wrong_kind;
            fail(slot);
            return;
    }
}

void TerrainLayerBuilder::begin_layer_dependencies(Slot& slot,
                                                   const assets::TerrainLayerAsset& record) {
    // Copy what the pass needs out of the record, then hand the streamed record back: the four
    // fields are all a palette keeps of it.
    slot.layer.uv_scale[0] = record.uv_scale[0];
    slot.layer.uv_scale[1] = record.uv_scale[1];
    slot.layer.height_contrast = record.height_contrast;
    const assets::AssetId material_id = record.material;
    const assets::AssetId texture_id = record.albedo_height;
    server_.release(slot.layer_record);
    slot.layer_record = {};

    const assets::ManifestEntry* m = manifest_.find_by_id(material_id);
    const assets::ManifestEntry* t = manifest_.find_by_id(texture_id);
    if (m == nullptr || t == nullptr) {
        ++counters_.unresolved_ids;
        fail(slot);
        return;
    }
    if (m->kind != assets::AssetKind::Material || t->kind != assets::AssetKind::Texture) {
        ++counters_.wrong_kind;
        fail(slot);
        return;
    }
    slot.material = server_.request_material(cooked_dir_ / m->cooked_file);
    // The reference is taken NOW, while the texture may still be loading, so two palettes that
    // share it share one load and one upload rather than racing to make two.
    auto [it, inserted] = textures_.try_emplace(texture_id.value);
    if (inserted) {
        it->second.cpu = server_.request_texture(cooked_dir_ / t->cooked_file);
    }
    ++it->second.references;
    slot.texture = texture_id;
    slot.step = Slot::Step::WaitMaterial;
}

bool TerrainLayerBuilder::texture_ready(assets::AssetId id) {
    Texture& t = textures_.at(id.value);
    if (t.gpu.is_valid()) {
        return true;
    }
    if (t.failed) {
        return false;
    }
    const assets::AssetState s = server_.state(t.cpu);
    if (s == assets::AssetState::Loading) {
        return false;
    }
    const assets::TextureAsset* cpu = server_.get(t.cpu);
    if (s != assets::AssetState::Ready || cpu == nullptr) {
        t.failed = true;
        ++counters_.failed_loads;
        return false;
    }
    // ADR-0066 §3: a terrain layer texture is cooked RGBA8_SRGB — colour decoded, height (A)
    // passed through linear. Any other format would change what the shader's A means (a BC7
    // block would at least survive; a UNORM texture would hand back gamma-encoded colour), so it
    // is refused and counted rather than uploaded as something it is not.
    if (cpu->format != assets::TextureFormat::Rgba8Srgb || cpu->mips.empty()) {
        t.failed = true;
        ++counters_.unsupported_textures;
        return false;
    }
    rhi::TextureDesc desc{};
    desc.extent = {cpu->width, cpu->height};
    desc.mip_levels = static_cast<std::uint32_t>(cpu->mips.size());
    desc.format = rhi::Format::RGBA8Srgb;
    desc.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    desc.debug_name = "terrain-layer-texture";
    t.gpu = device_.create_texture(desc);
    if (!t.gpu.is_valid()) {
        t.failed = true;
        ++counters_.unsupported_textures;
        return false;
    }
    // The whole cooked chain, verbatim — GpuAssetBridge::upload's walk (gpu_asset_bridge.cpp).
    std::vector<rhi::MipData> levels;
    levels.reserve(cpu->mips.size());
    for (const assets::TextureMip& mip : cpu->mips) {
        levels.push_back(
            rhi::MipData{std::span<const std::byte>(cpu->pixels.data() + mip.offset, mip.size)});
    }
    device_.write_texture_mips(t.gpu, levels);
    t.bytes = cpu->pixels.size();
    ++counters_.textures_uploaded;
    return true;
}

void TerrainLayerBuilder::release_texture(assets::AssetId id) {
    const auto it = textures_.find(id.value);
    if (it == textures_.end()) {
        return;
    }
    if (--it->second.references == 0) {
        // The last palette using it is gone, and (the caller's contract) so is every frame that
        // sampled it. A CPU copy stays with the AssetServer (a retained kind).
        if (it->second.gpu.is_valid()) {
            device_.destroy(it->second.gpu);
            ++counters_.textures_destroyed;
        }
        textures_.erase(it);
    }
}

void TerrainLayerBuilder::fail(Slot& slot) {
    if (slot.layer_record.is_valid()) {
        server_.release(slot.layer_record);
        slot.layer_record = {};
    }
    slot.step = Slot::Step::Failed;
}

void TerrainLayerBuilder::advance_slot(Slot& slot) {
    if (slot.step == Slot::Step::WaitLayer) {
        const assets::AssetState s = server_.state(slot.layer_record);
        if (s == assets::AssetState::Loading) {
            return;
        }
        const assets::TerrainLayerAsset* record = server_.get(slot.layer_record);
        if (s != assets::AssetState::Ready || record == nullptr) {
            ++counters_.failed_loads;
            fail(slot);
            return;
        }
        begin_layer_dependencies(slot, *record);
    }
    if (slot.step != Slot::Step::WaitMaterial) {
        return;
    }
    const assets::AssetState ms = server_.state(slot.material);
    if (ms == assets::AssetState::Loading) {
        return;
    }
    const assets::MaterialAsset* m = server_.get(slot.material);
    if (ms != assets::AssetState::Ready || m == nullptr) {
        ++counters_.failed_loads;
        fail(slot);
        return;
    }
    if (slot.texture.is_valid()) {
        if (!texture_ready(slot.texture)) {
            if (textures_.at(slot.texture.value).failed) {
                fail(slot);
            }
            return;
        }
        slot.layer.albedo_height = textures_.at(slot.texture.value).gpu;
    }
    // The scalars every material path shares — the m19.4/m19.5 fields, and nothing a TerrainLayer
    // could override (ADR-0066 §1: the layer REFERENCES a material for what the two share).
    slot.layer.base_color = {m->base_color[0], m->base_color[1], m->base_color[2]};
    slot.layer.metallic = m->metallic;
    slot.layer.roughness = m->roughness;
    slot.step = Slot::Step::Done;
}

TerrainPaletteState TerrainLayerBuilder::update(TerrainPaletteHandle handle) {
    if (handle >= palettes_.size() || !palettes_[handle].live) {
        return TerrainPaletteState::Failed;
    }
    Palette& p = palettes_[handle];
    if (p.state != TerrainPaletteState::Pending) {
        return p.state;
    }
    bool done = true;
    bool failed = false;
    for (Slot& s : p.slots) {
        advance_slot(s);
        done = done && s.step == Slot::Step::Done;
        failed = failed || s.step == Slot::Step::Failed;
    }
    // One failed slot fails the palette: a tile drawn with a missing layer would be a silently
    // wrong picture (the pass's own rule — terrain_pass.hpp refuses a splat tile with no palette).
    if (failed) {
        p.state = TerrainPaletteState::Failed;
        ++counters_.palettes_failed;
    } else if (done) {
        for (std::size_t k = 0; k < 4; ++k) {
            p.resolved[k] = p.slots[k].layer;
        }
        p.state = TerrainPaletteState::Ready;
        ++counters_.palettes_ready;
    }
    return p.state;
}

const TerrainPalette* TerrainLayerBuilder::palette(TerrainPaletteHandle handle) const {
    if (handle >= palettes_.size() || !palettes_[handle].live ||
        palettes_[handle].state != TerrainPaletteState::Ready) {
        return nullptr;
    }
    return &palettes_[handle].resolved;
}

bool TerrainLayerBuilder::release(TerrainPaletteHandle handle) {
    if (handle >= palettes_.size() || !palettes_[handle].live) {
        return false;
    }
    Palette& p = palettes_[handle];
    // ONE reference per distinct texture, mirroring how they were taken: each TerrainLayer slot
    // took its own, so each gives its own back. Two slots naming one texture took two and return
    // two.
    for (Slot& s : p.slots) {
        if (s.layer_record.is_valid()) {
            server_.release(s.layer_record);
            s.layer_record = {};
        }
        if (s.texture.is_valid()) {
            release_texture(s.texture);
            s.texture = {};
        }
    }
    p.live = false;
    free_.push_back(handle);
    ++counters_.palettes_released;
    return true;
}

std::uint64_t TerrainLayerBuilder::texture_references() const noexcept {
    std::uint64_t n = 0;
    for (const auto& [id, t] : textures_) {
        n += t.references;
    }
    return n;
}

std::uint64_t TerrainLayerBuilder::texture_bytes() const noexcept {
    std::uint64_t n = 0;
    for (const auto& [id, t] : textures_) {
        n += t.gpu.is_valid() ? t.bytes : 0;
    }
    return n;
}

std::size_t TerrainLayerBuilder::live_palettes() const noexcept {
    std::size_t n = 0;
    for (const Palette& p : palettes_) {
        n += p.live ? 1 : 0;
    }
    return n;
}

} // namespace rime::render
