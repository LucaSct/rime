// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.

#include "rime/blockkit/palette.hpp"

#include <utility>
#include <vector>

#include "rime/ecs/query.hpp"
#include "rime/render/components.hpp"

namespace rime::blockkit {
namespace {

[[nodiscard]] render::PbrMaterialDesc
opaque(float r, float g, float b, float roughness, float metallic = 0.0f) noexcept {
    render::PbrMaterialDesc d{};
    d.base_color[0] = r;
    d.base_color[1] = g;
    d.base_color[2] = b;
    d.metallic = metallic;
    d.roughness = roughness;
    return d;
}

} // namespace

BlockPalette build_palette(render::MaterialRegistry& materials) {
    BlockPalette p;
    const std::size_t before = materials.size();

    for (std::size_t t = 0; t < kTintCount; ++t) {
        const BuildingTint& tint = kBuildingTints[t];
        p.facade[t] = materials.add(
            opaque(tint.base_color[0], tint.base_color[1], tint.base_color[2], tint.roughness));
        // The ground-storey band: the same colour taken down and roughened. One multiply rather
        // than four hand-picked colours, so adding a fifth tint never needs a matching band picked
        // by eye.
        p.band[t] = materials.add(opaque(tint.base_color[0] * kBandScale,
                                         tint.base_color[1] * kBandScale,
                                         tint.base_color[2] * kBandScale,
                                         kBandRoughness));
    }

    p.roof = materials.add(opaque(0.62f, 0.60f, 0.57f, 0.85f));
    // Dark and comparatively smooth: at dusk the road is what carries the lamp highlights, and a
    // matte road at this light level is a black hole with buildings floating on it.
    p.street = materials.add(opaque(0.10f, 0.10f, 0.11f, kStreetRoughness));
    p.kerb = materials.add(opaque(0.30f, 0.30f, 0.32f, 0.80f));
    p.barrier = materials.add(opaque(0.35f, 0.33f, 0.20f, 0.85f));
    p.lamp_mast = materials.add(opaque(0.18f, 0.18f, 0.20f, 0.35f, /*metallic=*/0.8f));
    p.crate = materials.add(opaque(0.42f, 0.30f, 0.18f, 0.90f));

    // The fixture glows so the lamp reads as the source of its own light. There is no bloom pass,
    // so this is a bright quad rather than a halo — modest values on purpose.
    render::PbrMaterialDesc head = opaque(0.05f, 0.05f, 0.06f, 0.4f);
    head.emissive[0] = kLampEmissive[0];
    head.emissive[1] = kLampEmissive[1];
    head.emissive[2] = kLampEmissive[2];
    p.lamp_head = materials.add(head);

    p.material_count = materials.size() - before;
    return p;
}

void upload_prop_meshes(BlockPalette& palette, render::MeshRegistry& meshes) {
    // UNIT primitives, sized by each prop's own LocalTransform scale. The alternative — baking a
    // mesh per prop size — would put the street's length into the palette, which is exactly the
    // dimension coupling role.hpp exists to avoid. `pbr_forward.vert` builds a real inverse-
    // transpose normal matrix, so non-uniform scale shades correctly.
    palette.unit_cube = meshes.add(render::make_cube(0.5f), "blockkit_unit_cube");
    palette.street_plane = meshes.add(render::make_plane(0.5f, 24.0f), "blockkit_street");
}

PaletteStats apply_palette(ecs::World& world, const BlockPalette& palette) {
    render::register_render_components(world);

    // Collect first, then stamp: adding a component relocates the entity between archetypes, and
    // the query contract forbids structural change during iteration.
    std::vector<std::pair<ecs::Entity, SlabRole>> targets;
    world.query<SlabRole>().for_each(
        [&](ecs::Entity e, SlabRole& role) { targets.emplace_back(e, role); });

    PaletteStats stats;
    for (const auto& [entity, role] : targets) {
        render::MaterialId material = render::kInvalidMaterialId;
        render::MeshId mesh = render::kInvalidMeshId;
        const std::size_t tint = role.tint % kTintCount;

        switch (role.kind) {
            case slab_kind::kWall:
            case slab_kind::kSideWall:
            case slab_kind::kHalfWall:
                // The ground storey wears the darker band — the shopfront rule that gives an
                // untextured facade some vertical structure under a low sun.
                material = role.storey == 0 ? palette.band[tint] : palette.facade[tint];
                break;
            case slab_kind::kFloor:
            case slab_kind::kRoof:
                material = palette.roof;
                break;
            case slab_kind::kCrate:
                material = palette.crate;
                break;
            case slab_kind::kStreet:
                material = palette.street;
                mesh = palette.street_plane;
                break;
            case slab_kind::kKerb:
                material = palette.kerb;
                mesh = palette.unit_cube;
                break;
            case slab_kind::kBarrier:
                material = palette.barrier;
                mesh = palette.unit_cube;
                break;
            case slab_kind::kLampMast:
                material = palette.lamp_mast;
                mesh = palette.unit_cube;
                break;
            case slab_kind::kLampHead:
                material = palette.lamp_head;
                mesh = palette.unit_cube;
                break;
            case slab_kind::kLight:
                continue; // a light or the camera — no geometry, and not a gap in the palette
            default:
                // A role the palette has no answer for. Counted rather than ignored: an unhandled
                // kind is otherwise an invisible prop, which is the kind of failure that reads as
                // success in a headless proof and as "something is missing" in a screenshot nobody
                // takes.
                ++stats.missing;
                continue;
        }

        (void)world.add_component(entity, render::MaterialRef{material});
        ++stats.materialed;
        if (mesh != render::kInvalidMeshId) {
            (void)world.add_component(entity, render::MeshRef{mesh});
            ++stats.meshed;
        }
    }
    return stats;
}

} // namespace rime::blockkit
