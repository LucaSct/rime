// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "rime/assets/asset_id.hpp"
#include "rime/assets/asset_server.hpp"
#include "rime/render/terrain_pass.hpp"

// THE TERRAIN BUILDER (m19.8a, ADR-0069 §4): a heightfield's palette of AssetIds in, the
// `TerrainPalette` `TerrainPass::upload` takes out.
//
// `TerrainPass` deliberately never touches the asset system (terrain_pass.hpp: "the caller looks
// each `HeightfieldAsset::layers[k]` up and hands over the colour"), and until now the only caller
// was a test building palettes by hand. ADR-0066 §2 decided HOW a slot is resolved and left the
// code to this brick: a v2 heightfield's palette slot names either a `Material` (every v2 terrain
// cooked before m19.7a) or a `TerrainLayer`, and the builder DISPATCHES ON THE KIND —
//
//   Material      → base colour, metallic and roughness; no texture, uv_scale 1, contrast 0.
//                   Exactly the m19.4/m19.5 path, so an existing terrain draws as it always has.
//   TerrainLayer  → its referenced Material's scalars, PLUS its packed albedo+height texture,
//                   its uv_scale and its height contrast (the m19.7b/m19.7c inputs).
//
// The kind comes from the asset MANIFEST entry the id resolves through (`Manifest::find_by_id`),
// which `rime cook` writes from the very header ADR-0066 §2 says to read. Reading the manifest
// instead of opening the file costs no IO on the frame thread; the typed request then re-reads
// the header, so a manifest that lies about a kind still fails the load (`WrongKind`) and is
// counted, rather than decoding one kind as another.
//
// ── SHARED TEXTURES ARE REFERENCE-COUNTED ───────────────────────────────────────────────────────
//
// Layers are shared: the same grass covers a hundred tiles. Each layer texture is uploaded ONCE
// and counted per palette that uses it; it is destroyed when the last palette using it is released
// — and not before, because a tile still on screen is still sampling it. A palette holds one
// reference per SLOT that names the texture, taken when that slot's layer record resolves, and
// `release` returns exactly the references its slots took — never more, so one palette's release
// cannot spend another's.
//
// The GPU fence is the CALLER's: `release` destroys at once (the RHI does), so it must only be
// called once every frame that drew a tile with this palette has retired. TerrainResidency calls it
// from slot reclamation, which is exactly that moment (terrain_residency.hpp).
//
// What is NOT reference-counted: the CPU copy. Textures and materials are AssetServer's RETAINED
// kinds (ADR-0067 §5): the decoded texture stays in CPU memory until the server dies, so
// re-entering an area re-uploads from RAM without re-reading the file. Moving them to the streamed
// path is deferred (ADR-0067's "dependent loads"). The TerrainLayer record itself IS streamed and
// is released as soon as its four fields are copied.
namespace rime::assets {
class Manifest;
}

namespace rime::render {

using TerrainPaletteHandle = std::uint32_t;
inline constexpr TerrainPaletteHandle kInvalidTerrainPalette = 0xFFFFFFFFu;

enum class TerrainPaletteState : std::uint8_t { Pending, Ready, Failed };

// Guardrail 5: every way a palette can fail to resolve has its own counter, and so does every
// texture created and destroyed — "released exactly once" is an equality between two of them.
struct TerrainBuilderCounters {
    std::uint64_t palettes_requested = 0;
    std::uint64_t palettes_ready = 0;  // reached Ready (once per palette)
    std::uint64_t palettes_failed = 0; // reached Failed (once per palette)
    std::uint64_t palettes_released = 0;
    std::uint64_t unresolved_ids = 0;       // a slot / layer reference the manifest does not list
    std::uint64_t wrong_kind = 0;           // listed, but not a kind that position may name
    std::uint64_t failed_loads = 0;         // a material, layer or texture whose load failed
    std::uint64_t unsupported_textures = 0; // not RGBA8_SRGB, or the GPU allocation failed
    std::uint64_t textures_uploaded = 0;
    std::uint64_t textures_destroyed = 0;
};

class TerrainLayerBuilder {
public:
    // Borrows all four. `cooked_dir` is where the manifest's `cooked-file` column is resolved.
    TerrainLayerBuilder(rhi::Device& device,
                        assets::AssetServer& server,
                        const assets::Manifest& manifest,
                        std::filesystem::path cooked_dir);
    // Destroys any texture still referenced (a palette never released is the caller's leak, warned
    // once). The caller must have retired every frame that sampled them.
    ~TerrainLayerBuilder();

    TerrainLayerBuilder(const TerrainLayerBuilder&) = delete;
    TerrainLayerBuilder& operator=(const TerrainLayerBuilder&) = delete;

    // Start resolving a palette (slot k = `HeightfieldAsset::layers[k]`; a zero id is an unused
    // slot). Issues the loads it can at once. Every call must be balanced by one `release`.
    [[nodiscard]] TerrainPaletteHandle request(const std::array<assets::AssetId, 4>& layers);

    // Advance a palette's resolution — main thread, after `AssetServer::pump()`. Once Ready or
    // Failed it stays so. Unknown handle → Failed.
    TerrainPaletteState update(TerrainPaletteHandle handle);

    // The resolved palette: non-null only when Ready. Its texture handles stay valid until this
    // palette's `release`.
    [[nodiscard]] const TerrainPalette* palette(TerrainPaletteHandle handle) const;

    // Give the palette back: drop its texture references (destroying a texture whose count reaches
    // zero) and any streamed layer it still owns. GPU-fence-unsafe by design — see the header.
    // Returns false for an unknown or already-released handle.
    bool release(TerrainPaletteHandle handle);

    [[nodiscard]] const TerrainBuilderCounters& counters() const noexcept { return counters_; }

    // Layer textures alive on the GPU right now, the references held on them, and their bytes.
    [[nodiscard]] std::size_t live_textures() const noexcept { return textures_.size(); }

    [[nodiscard]] std::uint64_t texture_references() const noexcept;
    [[nodiscard]] std::uint64_t texture_bytes() const noexcept;
    [[nodiscard]] std::size_t live_palettes() const noexcept;

private:
    // One palette slot's resolution, a small state machine driven by update().
    struct Slot {
        enum class Step : std::uint8_t {
            Done,         // resolved into `layer` (or unused)
            WaitMaterial, // a Material slot, or a TerrainLayer's material
            WaitLayer,    // a TerrainLayer record loading
            Failed,
        };
        Step step = Step::Done;
        bool is_terrain_layer = false;
        assets::MaterialAssetHandle material{};
        assets::TerrainLayerAssetHandle layer_record{}; // owned while WaitLayer
        assets::AssetId texture{};                      // a reference held in textures_ when valid
        TerrainLayer layer{};
    };

    struct Palette {
        bool live = false;
        TerrainPaletteState state = TerrainPaletteState::Pending;
        std::array<Slot, 4> slots{};
        TerrainPalette resolved{};
    };

    struct Texture {
        assets::TextureAssetHandle cpu{};
        rhi::TextureHandle gpu{};
        std::uint64_t bytes = 0;
        std::uint32_t references = 0;
        bool failed = false;
    };

    void begin_slot(Slot& slot, assets::AssetId id);
    void begin_layer_dependencies(Slot& slot, const assets::TerrainLayerAsset& record);
    void advance_slot(Slot& slot);
    [[nodiscard]] bool texture_ready(assets::AssetId id);
    void release_texture(assets::AssetId id);
    void fail(Slot& slot);

    rhi::Device& device_;
    assets::AssetServer& server_;
    const assets::Manifest& manifest_;
    std::filesystem::path cooked_dir_;
    std::vector<Palette> palettes_;
    std::vector<TerrainPaletteHandle> free_;
    std::unordered_map<std::uint64_t, Texture> textures_; // by texture AssetId
    TerrainBuilderCounters counters_{};
};

} // namespace rime::render
