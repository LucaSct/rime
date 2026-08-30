// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "rime/blockkit/role.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"

// The block's LOOK, in one file (m13.2c). Luca's rulings, 2026-08-29.
//
// Everything here is a handful of floats, and that is the point: `.rdest` carries no material and
// the `.rscene` carries no MaterialRef (role.hpp explains why), so the entire appearance of the
// vision demo is this one translation unit. Re-tinting the block is an edit here, not a regenerated
// scene, not a re-cook, and not a schema change.
//
// THE REGISTER: DUSK, SUN RAKING DOWN THE STREET. A low warm sun along the street axis, a cool sky
// fill, and interiors that are genuinely darker than outside. Chosen over overcast noon and over
// night-only for one reason each:
//
//   * Overcast noon makes the 36 local lights DECORATION — bright ambient drowns them, and
//     ADR-0035 §1's ">= 32 active local lights" floor becomes a count of things that do not matter.
//     It also flattens M10's whole thesis: a room already lit by ambient does not visibly relight
//     when you breach its wall.
//   * Night-only is the most dramatic and the most dishonest: with no sun there are no cascaded
//     directional shadows, so an entire M10 technique goes unshown in the demo built to show M10.
//
// Dusk keeps both. The sun casts long raking cascades down the street; the interiors are dark
// enough that their own lights do real work and a breach reads as a change.
//
// TWO ENGINE CONSTRAINTS THIS FORCES, both load-bearing rather than incidental:
//
//   1. `LightingSettings::clustered_enabled` MUST be on. The ADR-0022 uniform-block path caps at
//      `render::kMaxPointLights` == 16 and this block has 36 local lights. m10.3 built clustered
//      forward for exactly this; without the flag the renderer warns once and drops the excess,
//      which looks like "some rooms are unlit" rather than like a misconfiguration.
//   2. Only `kMaxLocalShadows` (8) of the 12 street lamps can cast. The rest render as unshadowed
//      cones — the honest degradation m10.2 documented. It is counted in the proof so it stays a
//      stated number rather than something discovered in a screenshot.
namespace rime::blockkit {

// ── Materials ────────────────────────────────────────────────────────────────────────────────────
// Four building tints, because no textures exist: base colour and roughness are the entire surface
// vocabulary, and eight buildings in one grey would read as a test scene rather than a street. Each
// building's ground storey additionally gets a darker, rougher band (`kBandScale`), which is the
// cheapest rule that gives a facade vertical structure — it reads as shopfronts under a low sun.
inline constexpr std::size_t kTintCount = 4;
inline constexpr float kBandScale = 0.55f; // ground-storey base colour multiplier
inline constexpr float kBandRoughness = 0.95f;

// Linear RGB + roughness per tint: weathered concrete, brick, pale render, grey-green panel.
struct BuildingTint {
    float base_color[3];
    float roughness;
};

inline constexpr std::array<BuildingTint, kTintCount> kBuildingTints{{
    {{0.62f, 0.60f, 0.57f}, 0.85f}, // concrete
    {{0.55f, 0.34f, 0.28f}, 0.90f}, // brick
    {{0.74f, 0.72f, 0.67f}, 0.80f}, // pale render
    {{0.44f, 0.48f, 0.45f}, 0.75f}, // grey-green panel
}};

// The street is deliberately smoother than anything else (roughness 0.45): at dusk it is what
// carries the lamp highlights and the SSR reflection of the lit facades, and a matte road at this
// light level is a black hole with buildings floating on it.
inline constexpr float kStreetRoughness = 0.45f;

// ── The lighting rig ─────────────────────────────────────────────────────────────────────────────
inline constexpr float kSunElevation = 0.14f; // radians (~8 deg) — a low, raking sun
inline constexpr float kSunColor[3] = {0.95f, 0.78f, 0.58f};
inline constexpr float kSunIntensity = 1.4f;

// The cool sky fill that keeps the shadow side readable without lighting the interiors.
inline constexpr float kAmbient[3] = {0.030f, 0.035f, 0.055f};

// One warm point per storey per building, hung near the ceiling. Radius 6 keeps a light inside the
// room it belongs to (the footprint is 8 m), which is what makes a breach visible: the light was
// always there, the wall was what stopped you seeing it.
inline constexpr float kInteriorColor[3] = {1.0f, 0.85f, 0.62f};
inline constexpr float kInteriorIntensity = 3.0f;
inline constexpr float kInteriorRadius = 6.0f;
inline constexpr float kInteriorCeilingDrop = 0.8f; // below the storey's ceiling

// Cool street lamps, aimed straight down. Spots rather than points because spots are M10's local
// shadow casters — a lamp that does not throw the rubble's shadow across the road is not doing the
// job the demo needs it for.
inline constexpr float kLampColor[3] = {0.85f, 0.88f, 1.0f};
inline constexpr float kLampIntensity = 6.0f;
inline constexpr float kLampRange = 12.0f;
inline constexpr float kLampInnerAngle = 0.436f; // 25 deg
inline constexpr float kLampOuterAngle = 0.698f; // 40 deg
inline constexpr float kLampHeight = 4.0f;

// The fixture itself glows, so the lamp reads as a light source and not as a box with light coming
// out of nowhere. There is no bloom pass, so the emissive is modest — it is a bright quad, not a
// halo.
inline constexpr float kLampEmissive[3] = {3.0f, 3.1f, 3.4f};

// ── The palette ──────────────────────────────────────────────────────────────────────────────────
// Built once into a caller's registries; every id here is a dense index into THOSE registries, so
// the palette must be built against the same pair the frame renders with. That coupling is the
// reason the ids are handed back in a struct rather than being written into the scene file.
struct BlockPalette {
    // Per-tint facade and its ground-storey band.
    std::array<render::MaterialId, kTintCount> facade{};
    std::array<render::MaterialId, kTintCount> band{};

    render::MaterialId roof{};
    render::MaterialId street{};
    render::MaterialId kerb{};
    render::MaterialId barrier{};
    render::MaterialId lamp_mast{};
    render::MaterialId lamp_head{};
    render::MaterialId crate{};

    // Prop geometry. Slabs and crates draw as their fractured PARTS (the m13.2d render bridge), so
    // they get no mesh here — only a material their parts inherit.
    //
    // These stay invalid until `upload_prop_meshes` runs, and that is a deliberate seam rather than
    // an oversight: `render::MeshRegistry` holds an `rhi::Device&` and uploads on add, so anything
    // that mints a MeshId needs a GPU. Materials are plain CPU data. Splitting the palette along
    // that line is what lets m13.2c's whole proof — assemble, save, load, bind, shade-by-role —
    // run headless on every CI OS and under the sanitizers, with only the drawing half needing a
    // device.
    render::MeshId unit_cube = render::kInvalidMeshId; // scaled per prop by its LocalTransform
    render::MeshId street_plane = render::kInvalidMeshId;

    // How many materials this palette added. The proof asserts the registry grew by exactly this,
    // which is what catches a palette that silently reused a caller's material id.
    std::size_t material_count = 0;
};

// Add the block's materials to `materials` and hand back their ids. GPU-free.
[[nodiscard]] BlockPalette build_palette(render::MaterialRegistry& materials);

// Upload the two prop primitives and fill in the palette's mesh ids. Needs a device (the registry
// owns one); callers without a GPU simply skip it and get a material-only palette.
void upload_prop_meshes(BlockPalette& palette, render::MeshRegistry& meshes);

// How applying the palette went. `missing` counts entities whose SlabRole named a kind the palette
// has no answer for — the counter that turns "a prop is invisible" from something you notice in a
// screenshot into a red number. It must be zero.
struct PaletteStats {
    std::size_t materialed = 0; // entities given a MaterialRef
    std::size_t meshed = 0;     // entities given a MeshRef (props only)
    std::size_t missing = 0;    // roles the palette did not cover
};

// Stamp MeshRef/MaterialRef onto every entity carrying a SlabRole, deriving both from the role.
// Registers the render components it writes. Idempotent: re-running restamps the same values, which
// is what makes "edit the palette, re-apply" a live operation rather than a reload.
PaletteStats apply_palette(ecs::World& world, const BlockPalette& palette);

} // namespace rime::blockkit
