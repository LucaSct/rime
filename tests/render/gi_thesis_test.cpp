// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// THE M10 THESIS PROOF (m10.5b, ADR-0032). Every prior M10 brick built toward one sentence in
// docs/adr/0032-lighting-v2.md: break a wall between sun and floor and NEXT FRAME the shadow moves
// (m10.1/m10.2) AND the bounced light updates (m10.4b's clipmap + m10.5a's probes + THIS brick's
// consumption + destruction-reactive hysteresis). This file is the executable version of that
// sentence, driven end to end through SceneRenderer (not the raw compute passes ddgi_test.cpp /
// sdf_clipmap_test.cpp drive) because the claim is about a RENDERED PIXEL, not an atlas texel.
//
// No golden images — every claim is a stated, measured margin (ADR-0032 §11):
//
//  (1) THE THESIS. A floor patch sits in the shadow of a suspended slab (a real cast shadow,
//      CSM-verified) — its only light is indirect. Since m10.6 the two configs are two whole
//      renderers, not a term toggled on top of one: DDGI OFF is M5.6's flat ambient constant;
//      DDGI ON REPLACES that constant with the traced field (m10.6 retires the placeholder rather
//      than double-counting it — see the shader, and pbr.md's indirect section). With the wall
//      present those two AGREE at this patch to within a quantization step, because the patch sees
//      mostly open SKY and DDGI's escaped rays return exactly that same ambient as their sky term
//      (ddgi_trace.comp) — in the open-sky limit the field MUST reduce to the constant it replaced.
//      Then the slab is REMOVED (SdfClipmap::remove_instance + DdgiProbes::invalidate, the
//      fast-tracked hysteresis this brick adds), a small STATED number of updates run, and two
//      things are asserted about the SAME patch: it is materially brighter (the shadow moved —
//      direct sun reaches it, a CSM effect present with DDGI on OR off), AND the isolated indirect
//      term (on − off) has risen from ~0 to a clearly-resolved positive value — the newly-open
//      sunlit floor's bounce, which the flat constant can never express. The second half is the
//      one only GI can produce; the constant-ambient renderer's indirect term is nailed in place.
//
//  (2) THE LEAK GUARD. A probe on the lit side of a standing wall must not brighten a fragment on
//      the sealed, dark side, even though that bright probe sits in the fragment's own 8-probe
//      interpolation cage — the Chebyshev visibility weight (this brick's other shader addition)
//      is what stops the naive trilinear blend from leaking through solid geometry. Proven
//      geometrically (docs/math/ddgi.md §12's own framing): read the bright probe's OWN stored
//      irradiance back directly (so "a naive blend would have leaked" isn't just asserted, it's
//      measured), then confirm the actual RENDERED fragment right next to it, across the wall,
//      still reads dark.
//
// The destruction-reactive hysteresis MATH itself (does invalidate() actually shorten convergence
// the way docs/math/ddgi.md §8 predicts) is proven analytically in tests/render/ddgi_test.cpp,
// reusing m10.5a's own clean "turn the light off and watch it decay" instrument — this file does
// not re-derive that; it exercises the mechanism in context, as one step of the bigger claim.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "render_test_support.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/ddgi.hpp"
#include "rime/render/lighting/local_shadows.hpp"
#include "rime/render/lighting/sdf_clipmap.hpp"
#include "rime/render/lighting/settings.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::decode_hdr;
using rime::render::test::HdrImage;
using rime::render::test::project;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

// ── Hand-built, analytically-exact box SDFs — the identical shape
// ddgi_test.cpp/sdf_clipmap_test.cpp build (each GPU test TU keeps its own copy; see
// ddgi_test.cpp's own header for why). ──────────────
float analytic_box_distance(core::Vec3 p, core::Vec3 h) {
    const core::Vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
    const core::Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    const float outside_len =
        std::sqrt(outside.x * outside.x + outside.y * outside.y + outside.z * outside.z);
    const float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
    return outside_len + inside;
}

assets::MeshSdfAsset build_box_sdf(core::Vec3 half_extents, std::uint32_t target_resolution = 24) {
    const float longest = std::max({half_extents.x, half_extents.y, half_extents.z}) * 2.0f;
    const float voxel_size = longest / static_cast<float>(target_resolution);
    const float pad = 2.0f * voxel_size;
    std::uint32_t res[3] = {0, 0, 0};
    float origin[3] = {0.0f, 0.0f, 0.0f};
    const float half[3] = {half_extents.x, half_extents.y, half_extents.z};
    for (int a = 0; a < 3; ++a) {
        const float padded_extent = 2.0f * half[a] + 2.0f * pad;
        res[a] = std::max<std::uint32_t>(
            static_cast<std::uint32_t>(std::ceil(padded_extent / voxel_size)), 4u);
        origin[a] = -0.5f * static_cast<float>(res[a]) * voxel_size;
    }
    assets::MeshSdfAsset sdf;
    sdf.grid_origin = {origin[0], origin[1], origin[2]};
    sdf.voxel_size = voxel_size;
    sdf.resolution = {res[0], res[1], res[2]};
    sdf.local_bounds =
        assets::Aabb{core::Vec3{-half_extents.x, -half_extents.y, -half_extents.z}, half_extents};
    sdf.distances.resize(sdf.voxel_count());
    float max_abs = 0.0f;
    for (std::uint32_t kz = 0; kz < res[2]; ++kz) {
        for (std::uint32_t jy = 0; jy < res[1]; ++jy) {
            for (std::uint32_t ix = 0; ix < res[0]; ++ix) {
                const core::Vec3 p{sdf.grid_origin.x + (static_cast<float>(ix) + 0.5f) * voxel_size,
                                   sdf.grid_origin.y + (static_cast<float>(jy) + 0.5f) * voxel_size,
                                   sdf.grid_origin.z +
                                       (static_cast<float>(kz) + 0.5f) * voxel_size};
                const float d = analytic_box_distance(p, half_extents);
                sdf.distances[sdf.index(ix, jy, kz)] = d;
                max_abs = std::max(max_abs, std::fabs(d));
            }
        }
    }
    sdf.max_abs_distance = max_abs;
    return sdf;
}

// Spawn a visual box entity: a unit cube (make_cube(1)) non-uniformly scaled to `half_extents` and
// translated to `center`. The SDF twin (registered separately, by hand, via SdfClipmap) is what
// DDGI actually traces — a non-uniform WorldTransform is fine here because THIS entity never goes
// through update_instance (SdfClipmap's own "uniform scale only" rule, sdf_clipmap.hpp, is about
// the transform passed to IT, not about rendering).
ecs::Entity spawn_box(ecs::World& world,
                      MeshId cube_mesh,
                      MaterialId mat,
                      core::Vec3 center,
                      core::Vec3 half_extents) {
    core::Transform tf{};
    tf.translation = center;
    tf.scale = half_extents;
    return world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube_mesh}, MaterialRef{mat});
}

// A quick doctest CHECK helper's own gotcha (the brief's own warning): a bare `CHECK(a > b * c)`
// is fine (no leading `*`), but a MESSAGE/CHECK expression that itself STARTS with a product
// mis-parses through doctest's `mb * expr` streaming machinery. Every comparison below either
// leads with a named local or is parenthesized — this comment is the reminder, not a workaround
// needed anywhere below (checked while writing them).

} // namespace

TEST_CASE("gi thesis: breaking a wall relights its shadow AND the indirect field updates "
          "(m10.5b, ADR-0032's headline sentence)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the GI thesis proof");
        return;
    }

    constexpr std::uint32_t kSize = 192;

    MeshRegistry meshes(*device);
    const MeshId floor_mesh = meshes.add(make_plane(6.0f), "gi-thesis-floor");
    const MeshId cube_mesh = meshes.add(make_cube(1.0f), "gi-thesis-cube");

    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.8f;
    md.metallic = 0.0f;
    md.roughness = 1.0f;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.02f, 0.02f, 0.02f);

    // ── The scene (docs/math/ddgi.md §12 walks the geometry and the two occlusion mechanisms
    // (camera vs. slab, sun vs. slab) it has to satisfy simultaneously) ──────────────────────
    //
    //   * Floor: y = 0, half-extent 6 m, lit by a sun.
    //   * A SUSPENDED slab ("the wall"): 3x1x1 m, floating from y=0.6 to y=1.6 (NOT touching the
    //     floor — the gap is what lets an elevated, oblique camera see the floor behind it at all;
    //     a wall standing ON the floor would put the shadowed patch in the camera's own blind
    //     spot from any angle shallow enough to also see past the slab).
    //   * Sun travels (0,-1,-1)/sqrt(2) — a 45 degree sun. A box's shadow on flat ground, for a
    //     light this steep, spans from its own near (sunward) face's ground projection to its far
    //     face's ground projection RAISED by its height (the standard box-umbra construction): near
    //     edge at z = 0.5 - 0.6*1 = -0.1, far edge at z = -0.5 - 1.6*1 = -2.1. The test patch sits
    //     at z = -1.1, centred in that band with a full metre of margin on each side.
    //   * The wall's TOP face (y=1.6, sunlit, NdotL = cos(45) for this sun) is what a probe just
    //     inside the shadow band can see by looking up-and-over its own near edge — the physical
    //     bounce path this test measures. Nothing multi-bounces (m10.5a's honest limit): the ONLY
    //     source DDGI can report at the shadowed patch is direct sunlight reflected off something
    //     that itself has an unobstructed view of the sun — the slab's top, and the open floor.
    const core::Vec3 slab_half{1.5f, 0.5f, 0.5f};
    const core::Vec3 slab_center{0.0f, 1.1f, 0.0f};
    const std::uint64_t kFloorSdfId = 1;
    const std::uint64_t kSlabSdfId = 2;

    renderer.sdf_clipmap().update_instance(kFloorSdfId,
                                           build_box_sdf({6.0f, 0.2f, 6.0f}),
                                           core::mat4_translation({0.0f, -0.2f, 0.0f}));
    renderer.sdf_clipmap().update_instance(
        kSlabSdfId, build_box_sdf(slab_half), core::mat4_translation(slab_center));

    // A directional light shines along its entity's local -z (DirectionalLight's own convention,
    // components.hpp); rotating -45 degrees about world X turns local -z, (0,0,-1), into
    // (0,-sin45,-cos45) = (0,-0.7071,-0.7071) — the 45-degree sun the shadow-band derivation above
    // assumes (the same single-axis-pitch construction shadow_test.cpp uses for its own straight-
    // down sun, generalized off the pole).
    core::Transform light_tf{};
    light_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.7853982f);

    // The camera sits at the SAME z as both measurement points (z=-1.1) so its straight-down sight
    // line never enters the slab's OWN z-span [-0.5, 0.5] at all — it cannot be occluded by the
    // very thing it needs to see past, at ANY height or x offset (docs/math/ddgi.md §12). Height
    // 2.0 (not, say, 1.8) is deliberate: DdgiProbes snaps its lattice's Y origin to a whole
    // multiple of the probe spacing (0.4) centred on THIS position, and a camera chosen so the
    // ideal (pre-snap) origin lands close to a spacing BOUNDARY leaves the snap a coin flip on
    // floating-point rounding — this test's own first draft picked 1.8 (ideal Y origin exactly
    // 0.4, a whole multiple) and the rounding happened to go the wrong way, silently snapping the
    // LOWEST probe layer to y=0.0 — exactly the floor's own surface, where every sphere-traced ray
    // self-intersects at t~0 and both atlases end up storing near-zero garbage. 2.0 centres the
    // ideal origin at 0.6, comfortably mid-cell between the 0.4 and 0.8 grid lines, so the snap is
    // unambiguous and the lowest layer sits a full 0.4 m clear of the floor.
    core::Transform cam_tf{};
    cam_tf.translation = {1.0f, 2.0f, -1.1f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f); // straight down

    const float aspect = 1.0f;
    const float fov_y = 1.2f;
    const core::Mat4 view_proj =
        core::perspective(fov_y, aspect, 0.1f, 20.0f) * core::inverse(core::to_matrix(cam_tf));

    // The shadowed patch (DDGI-only light) and a sunlit control (sanity: the pipeline lights
    // SOMETHING) — both share the camera's own z, so both sightlines share its occlusion-free
    // property above.
    const test::Pixel shadow_px = project(view_proj, {0.0f, 0.0f, -1.1f}, kSize);
    const test::Pixel lit_px = project(view_proj, {2.0f, 0.0f, -1.1f}, kSize);
    REQUIRE(shadow_px.x >= 0.0f);
    REQUIRE(shadow_px.x < static_cast<float>(kSize));
    REQUIRE(shadow_px.y >= 0.0f);
    REQUIRE(shadow_px.y < static_cast<float>(kSize));
    REQUIRE(lit_px.x >= 0.0f);
    REQUIRE(lit_px.x < static_cast<float>(kSize));

    LightingSettings ls;
    ls.shadows_enabled = true;
    ls.cascade_count = 2;
    ls.shadow_map_resolution = 1024;
    ls.sdf_clipmap_enabled = true;
    ls.ddgi_enabled = true;
    ls.ddgi_probe_count_x = 8;
    ls.ddgi_probe_count_y = 8;
    ls.ddgi_probe_count_z = 4;
    ls.ddgi_probe_spacing = 0.4f;
    ls.ddgi_rays_per_probe = 64;
    ls.ddgi_max_trace_distance = 10.0f;
    ls.ddgi_hysteresis = 0.9f;

    // Populates `world` with the floor + sun + camera, plus the wall's VISUAL mesh when
    // `with_wall` — ecs::World is neither copyable nor movable (its own RAII-of-archetypes
    // design), so this fills a World the caller already owns rather than returning one. `ddgi_on`
    // picks the shader-level toggle. The SDF instances and the DdgiProbes/SdfClipmap objects are
    // on `renderer` itself and persist across every call — only the ECS world and the flag vary,
    // exactly the shadow_test.cpp / clustered_test.cpp pattern.
    const auto populate_world = [&](ecs::World& world, bool with_wall) {
        register_render_components(world);
        (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor_mesh}, MaterialRef{mat});
        if (with_wall) {
            (void)spawn_box(world, cube_mesh, mat, slab_center, slab_half);
        }
        (void)world.spawn_with(ecs::WorldTransform{light_tf},
                               DirectionalLight{1.0f, 1.0f, 1.0f, 4.0f});
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{fov_y, 0.1f, 20.0f, true});
    };

    const auto step = [&](ecs::World& world, bool ddgi_on) {
        ls.ddgi_enabled = ddgi_on;
        renderer.set_lighting(ls);
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.hdr.is_valid());
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
    };

    const auto lum = [](const HdrImage& img, const test::Pixel& px) {
        return img.luminance(static_cast<std::uint32_t>(px.x), static_cast<std::uint32_t>(px.y));
    };

    // ── Warm up with the wall present: enough updates that the indirect field settles near its
    // steady state (0.9^20 ~= 0.12 of the FIRST sample's own influence survives — "materially
    // converged", not merely "started to move"; see docs/math/ddgi.md §8 for the closed form this
    // is checked against directly in ddgi_test.cpp). ────────────────────────────────────────────
    ecs::World world_with_wall;
    populate_world(world_with_wall, /*with_wall=*/true);
    constexpr int kWarmupUpdates = 20;
    for (int i = 0; i < kWarmupUpdates; ++i) {
        (void)step(world_with_wall, /*ddgi_on=*/true);
    }

    const HdrImage before_on_img = step(world_with_wall, /*ddgi_on=*/true);
    const HdrImage before_off_img = step(world_with_wall, /*ddgi_on=*/false);
    const float before_on = lum(before_on_img, shadow_px);
    const float before_off = lum(before_off_img, shadow_px);
    const float control_lit = lum(before_on_img, lit_px);

    MESSAGE("BEFORE (wall present): shadowed patch ddgi-on="
            << before_on << " ddgi-off=" << before_off << "; sunlit control=" << control_lit);

    REQUIRE(control_lit > 0.05f); // sanity: the direct pipeline really is lighting something

    // (1a) In the shadow (no direct sun), DDGI-on and DDGI-off AGREE to within a quantization step:
    // the patch sees mostly open sky, and DDGI's escaped rays hand back precisely SceneRenderer's
    // ambient as their sky term (ddgi_trace.comp), so post-m10.6 the traced field reduces to the
    // exact constant it replaced. Measured |on - off| ~= 1.5e-5 (a single 16-bit LSB). This is a
    // CORRECTNESS property, not a coincidence — and it is the baseline the wall-break must move
    // away from below, which is what makes the divergence there attributable to the new geometry
    // alone.
    CHECK(std::fabs(before_on - before_off) < 0.25f * before_off);
    // And the patch is genuinely DIM relative to the sunlit control, not a shading bug that lit it
    // fully — this is still the shadow.
    CHECK(before_on < control_lit * 0.5f);

    // ── Break the wall: remove BOTH representations (the visual mesh — a different World — and
    // the SDF instance), then invalidate the DDGI region so its probes fast-track instead of riding
    // out the default ~30-frame hysteresis (docs/math/ddgi.md §8/§11). ─────────────────────────
    renderer.sdf_clipmap().remove_instance(kSlabSdfId);
    const WorldAabb slab_region{slab_center - slab_half, slab_center + slab_half};
    renderer.invalidate_ddgi_region(slab_region);

    ecs::World world_no_wall;
    populate_world(world_no_wall, /*with_wall=*/false);

    // N = kFastTrackUpdates (5, ddgi.cpp) + 1 margin update. This is the number this brick's own
    // thesis rests on: with the DEFAULT hysteresis (0.97) alone, reaching a comparable fraction of
    // the way to a new steady state would take on the order of 1/(1-0.97) ~= 33 updates — the
    // fast-track's whole point, proven directly (against the analytic prediction, not just "it
    // moved") in ddgi_test.cpp's own invalidate() test.
    constexpr int kPostBreakUpdates = 6;
    HdrImage after_on_img{};
    for (int i = 0; i < kPostBreakUpdates; ++i) {
        after_on_img = step(world_no_wall, /*ddgi_on=*/true);
    }
    const HdrImage after_off_img = step(world_no_wall, /*ddgi_on=*/false);
    const float after_on = lum(after_on_img, shadow_px);
    const float after_off = lum(after_off_img, shadow_px);

    MESSAGE("AFTER (" << kPostBreakUpdates << " updates post-break): shadowed patch ddgi-on="
                      << after_on << " ddgi-off=" << after_off);

    // (2) THE PAYOFF, both halves.
    //
    // (a) The shadow moved: with the slab gone its hard CSM shadow lifts and direct sun floods the
    // patch. BOTH configs jump well past their "before" values — the shadow map is
    // DDGI-independent, so this half shows up with DDGI on OR off — a wide 3x margin, because the
    // returning direct term dwarfs everything.
    CHECK(after_on > before_on * 3.0f);
    CHECK(after_off > before_off * 3.0f);

    // (b) The bounced light updated — the half ONLY DDGI can express. Isolate the indirect term as
    // (on - off): the two configs differ solely in whether the flat ambient is replaced by the
    // traced field (their direct sun + CSM paths are byte-identical), so this difference is exactly
    // [traced indirect] - [flat ambient]. With the wall present it was ~0 (1a: they agreed). With
    // the wall gone the newly-unoccluded sunlit floor bounces real light the constant never
    // modeled, and the difference climbs to a clearly-resolved positive value. Measured: ~0 ->
    // ~+0.0029 (about 190 LSB at 16-bit readback). The constant-ambient renderer CANNOT produce
    // this — its indirect term is nailed to the same number before and after; that is the entire
    // reason GI exists. This rise, appearing exactly when the geometry opened, IS "the bounced
    // light updates when a wall falls."
    const float gi_before = before_on - before_off; // ~0: the field == the ambient it replaced
    const float gi_after = after_on - after_off;    // >0: the field now exceeds it (real bounce)
    MESSAGE("isolated GI term (on-off): before=" << gi_before << " after=" << gi_after);
    // The run is fully deterministic (DdgiProbes' ray-rotation RNG is fixed-seed splitmix64 and
    // this test builds a fresh SceneRenderer each run — bit-identical across repeats), so these are
    // stable properties, not shot noise. Before: the field matched the ambient (within a few 16-bit
    // LSBs).
    CHECK(std::fabs(gi_before) < 0.0003f);
    // After: it rose a clearly-resolved amount ABOVE that ambient floor — bounce ADDED, not
    // occlusion removed. 0.0015 is ~half the measured +0.0029, far above the +-1 LSB before-noise.
    CHECK(gi_after > 0.0015f);
}

TEST_CASE("gi thesis: Chebyshev visibility stops a lit probe leaking through a wall onto a dark "
          "fragment right next to it (m10.5b)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the DDGI leak-guard proof");
        return;
    }

    constexpr std::uint32_t kSize = 128;

    MeshRegistry meshes(*device);
    const MeshId floor_mesh = meshes.add(make_plane(4.0f), "leak-floor");
    const MeshId cube_mesh = meshes.add(make_cube(1.0f), "leak-cube");

    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.8f;
    md.metallic = 0.0f;
    md.roughness = 1.0f;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.02f, 0.02f, 0.02f);

    // ── The scene: a small sealed ROOM (floor + ceiling only — a purely VERTICAL sun means only
    // an overhead ceiling can shadow anything; no side walls are needed to keep the interior dark,
    // docs/math/ddgi.md §12) standing right next to a completely open, sunlit floor, separated by
    // one standing dividing WALL. A probe just inside the room reads dark (the room's own ceiling
    // self-shadows it, the m10.5a "sealed box" mechanism); a probe just past the wall, in the open,
    // reads bright (the m10.5a "open floor" mechanism) — and because the probe spacing (0.5 m) puts
    // both in the SAME 8-probe interpolation cage as a fragment near the wall, this is precisely
    // the geometry a naive (Chebyshev-free) trilinear blend would leak light through.
    const core::Vec3 floor_half{3.0f, 0.15f, 2.0f};
    const core::Vec3 floor_center{1.5f, -0.15f, 0.0f}; // top at y=0, spans x in [-1.5, 4.5]
    const core::Vec3 ceiling_half{1.4f, 0.15f, 2.0f};
    const core::Vec3 ceiling_center{-0.2f, 2.15f, 0.0f}; // bottom at y=2.0, spans x in [-1.6, 1.2]
    const core::Vec3 wall_half{0.15f, 1.3f, 1.15f};
    const core::Vec3 wall_center{1.25f, 1.0f, 0.0f}; // spans x in [1.1, 1.4] — the room's only wall

    renderer.sdf_clipmap().update_instance(
        1, build_box_sdf(floor_half), core::mat4_translation(floor_center));
    renderer.sdf_clipmap().update_instance(
        2, build_box_sdf(ceiling_half), core::mat4_translation(ceiling_center));
    renderer.sdf_clipmap().update_instance(
        3, build_box_sdf(wall_half), core::mat4_translation(wall_center));

    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor_mesh}, MaterialRef{mat});
    (void)spawn_box(world, cube_mesh, mat, ceiling_center, ceiling_half);
    // The dividing wall gets NO visual mesh: the camera never looks at it (see below) and a purely
    // vertical sun does not need it to make the room's interior shadowed (the ceiling alone does
    // that) — only its SDF twin matters, for DDGI's own sphere-traced occlusion.
    core::Transform light_tf{};
    light_tf.rotation =
        core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f); // straight down
    (void)world.spawn_with(ecs::WorldTransform{light_tf}, DirectionalLight{1.0f, 1.0f, 1.0f, 3.0f});

    // Camera INSIDE the room, looking straight down at a floor fragment deep in the room but right
    // up against the dividing wall's inner face (x=1.1) — exactly where a leak would show most.
    const float frag_x = 1.05f;
    core::Transform cam_tf{};
    cam_tf.translation = {frag_x, 1.9f, 0.0f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
    (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{0.9f, 0.1f, 10.0f, true});

    const core::Mat4 view_proj =
        core::perspective(0.9f, 1.0f, 0.1f, 10.0f) * core::inverse(core::to_matrix(cam_tf));
    const test::Pixel frag_px = project(view_proj, {frag_x, 0.0f, 0.0f}, kSize);
    REQUIRE(frag_px.x >= 0.0f);
    REQUIRE(frag_px.x < static_cast<float>(kSize));
    REQUIRE(frag_px.y >= 0.0f);
    REQUIRE(frag_px.y < static_cast<float>(kSize));

    LightingSettings ls;
    ls.shadows_enabled = true;
    ls.cascade_count = 2;
    ls.shadow_map_resolution = 512;
    ls.sdf_clipmap_enabled = true;
    ls.ddgi_enabled = true;
    ls.ddgi_probe_count_x = 5;
    ls.ddgi_probe_count_y = 1;
    ls.ddgi_probe_count_z = 1;
    ls.ddgi_probe_spacing = 0.5f;
    ls.ddgi_rays_per_probe = 64;
    ls.ddgi_hysteresis = 0.7f;

    const auto step = [&](bool ddgi_on) {
        ls.ddgi_enabled = ddgi_on;
        renderer.set_lighting(ls);
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.hdr.is_valid());
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
    };

    for (int i = 0; i < 10; ++i) {
        (void)step(/*ddgi_on=*/true);
    }
    const HdrImage on_img = step(/*ddgi_on=*/true);
    const HdrImage off_img = step(/*ddgi_on=*/false);
    const float frag_on = on_img.luminance(static_cast<std::uint32_t>(frag_px.x),
                                           static_cast<std::uint32_t>(frag_px.y));
    const float frag_off = off_img.luminance(static_cast<std::uint32_t>(frag_px.x),
                                             static_cast<std::uint32_t>(frag_px.y));
    MESSAGE("dark fragment (x=" << frag_x << "): ddgi-on=" << frag_on << " ddgi-off=" << frag_off);

    // ── The geometric evidence: read the atlas DIRECTLY (bypassing the shader entirely) for the
    // two probes that bracket `frag_x` — probe 2 (world x=1.0, inside the sealed room) and probe 3
    // (world x=1.5, in the open) — the same peak-luminance scan m10.5a's own tests use. This is
    // what proves "a naive blend WOULD have leaked": a concretely bright neighbour really is one
    // interpolation step away, not a hypothetical.
    const rhi::Extent2D iext = renderer.ddgi().irradiance_atlas_extent();
    const std::vector<std::uint8_t> ibytes =
        test::read_texture(*device, renderer.ddgi().irradiance_atlas(), iext.width, iext.height, 8);
    const HdrImage iatlas = decode_hdr(ibytes, iext.width, iext.height);
    const std::uint32_t probes_per_row = ls.ddgi_probe_count_x * ls.ddgi_probe_count_y;
    const auto tile_peak = [&](std::uint32_t global_index) {
        const std::uint32_t col = global_index % probes_per_row;
        const std::uint32_t row = global_index / probes_per_row;
        const std::uint32_t phys = kDdgiIrradianceTileSize;
        float peak = 0.0f;
        for (std::uint32_t ty = 0; ty < phys; ++ty)
            for (std::uint32_t tx = 0; tx < phys; ++tx)
                peak = std::max(peak, iatlas.luminance(col * phys + tx, row * phys + ty));
        return peak;
    };
    const float inside_peak = tile_peak(2);  // world x = 1.0, inside the sealed room
    const float outside_peak = tile_peak(3); // world x = 1.5, in the open

    MESSAGE("probe 2 (inside, x=1.0) peak=" << inside_peak
                                            << "; probe 3 (outside, x=1.5) peak=" << outside_peak);

    // The two probes really are as different as the scene intends: inside stays dark (the sealed-
    // room mechanism, m10.5a's own proven property), outside is clearly lit (the open-floor
    // mechanism) — without this contrast the rest of the test would not mean anything.
    CHECK(inside_peak < 0.05f);
    CHECK(outside_peak > 0.3f);

    // frag_x = 1.05 sits at rel = (1.05 - 0.0) / 0.5 = 2.1 within the probe lattice, i.e. 90% probe
    // 2 (inside, dark) / 10% probe 3 (outside, bright) — so a NAIVE trilinear-only blend (no
    // Chebyshev weight at all) would add roughly 0.1 * outside_peak of leaked light on top of the
    // ambient floor. The Chebyshev test's whole job is recognizing that probe 3's own rays toward
    // this fragment's direction stop at the dividing wall (a few tens of centimetres away, not the
    // ~1 m to the fragment) and suppressing its contribution accordingly.
    const float naive_leak_estimate = 0.1f * outside_peak;
    const float indirect_delta = frag_on - frag_off;
    MESSAGE("indirect delta at dark fragment="
            << indirect_delta << "; naive (unweighted) leak estimate=~" << naive_leak_estimate);

    // The measured indirect contribution at the dark fragment is a small fraction of what an
    // unweighted blend would have leaked — comfortably below it (not just "a bit less"), while 0.4
    // is still generous slack above zero for the room's own (legitimate, small) sky-facing
    // contribution and ray-sampling noise.
    CHECK(indirect_delta < naive_leak_estimate * 0.4f);
    // And restated in absolute terms against the room's own ambient floor, so this check does not
    // depend on naive_leak_estimate alone being well-calibrated: the fragment stays close to the
    // flat ambient baseline, not measurably lit.
    CHECK(frag_on < frag_off * 1.6f);
}

TEST_CASE(
    "gi thesis II: a wall falls between a sunlit room and a covered dark one — the dark "
    "room's floor lights up from BOUNCE ALONE, no direct light reaching it (m10.6, ADR-0032)") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping the two-room walls-fall proof");
        return;
    }

    constexpr std::uint32_t kSize = 128;

    MeshRegistry meshes(*device);
    const MeshId floor_mesh = meshes.add(make_plane(4.0f), "two-room-floor");
    const MeshId cube_mesh = meshes.add(make_cube(1.0f), "two-room-cube");

    MaterialRegistry materials;
    PbrMaterialDesc md{};
    md.base_color[0] = md.base_color[1] = md.base_color[2] = 0.8f;
    md.metallic = 0.0f;
    md.roughness = 1.0f;
    const MaterialId mat = materials.add(md);

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.02f, 0.02f, 0.02f);

    // ── The scene (a walls-fall variant of the leak-guard rig above; the ambient retirement it
    // exercises is derived in docs/math/pbr.md §6.1). A
    // covered "dark room" — a ceiling over it blocks the purely VERTICAL sun, the same sealed-box
    // mechanism m10.5a proved — sits beside an open, sunlit floor, the two sealed apart by a
    // dividing WALL. The crucial property: the dark room gets NO direct light in EITHER state,
    // because the ceiling blocks the sun whether or not the side wall stands. So when the wall
    // falls the only thing that can brighten the dark room's floor is bounce from the sunlit floor
    // next door. The slab thesis above had GI as a small correction to a direct-light-dominated
    // change (the CSM shadow lifting); here direct light contributes nothing to the change, so the
    // rise is GI, undiluted — and the DDGI-off control, the flat constant that GI replaced, must
    // stay put across the break. Geometry reused verbatim from the leak-guard test so its
    // probe-snap analysis (probe 2 lands at world x=1.0) carries over unchanged.
    // ────────────────────────────────────────────
    const core::Vec3 floor_half{3.0f, 0.15f, 2.0f};
    const core::Vec3 floor_center{1.5f, -0.15f, 0.0f}; // top at y=0, x in [-1.5, 4.5]
    const core::Vec3 ceiling_half{1.4f, 0.15f, 2.0f};
    const core::Vec3 ceiling_center{-0.2f, 2.15f, 0.0f}; // bottom y=2.0, covers x in [-1.6, 1.2]
    const core::Vec3 wall_half{0.15f, 1.3f, 1.15f};
    const core::Vec3 wall_center{1.25f, 1.0f, 0.0f}; // x in [1.1, 1.4] — the divider that falls
    const std::uint64_t kFloorSdf = 1, kCeilingSdf = 2, kWallSdf = 3;

    renderer.sdf_clipmap().update_instance(
        kFloorSdf, build_box_sdf(floor_half), core::mat4_translation(floor_center));
    renderer.sdf_clipmap().update_instance(
        kCeilingSdf, build_box_sdf(ceiling_half), core::mat4_translation(ceiling_center));
    renderer.sdf_clipmap().update_instance(
        kWallSdf, build_box_sdf(wall_half), core::mat4_translation(wall_center));

    core::Transform light_tf{};
    light_tf.rotation =
        core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f); // straight down

    // Camera inside the dark room, looking straight down at a floor fragment right by the divider —
    // the spot that receives the most bounce once it falls. x=1.0 is under the ceiling (x<1.2) and,
    // by the leak test's own snap analysis, is exactly where DDGI probe 2 lands, so the fragment
    // sits right on a probe.
    const float frag_x = 1.0f;
    core::Transform cam_tf{};
    cam_tf.translation = {frag_x, 1.9f, 0.0f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);

    const core::Mat4 view_proj =
        core::perspective(0.9f, 1.0f, 0.1f, 10.0f) * core::inverse(core::to_matrix(cam_tf));
    const test::Pixel frag_px = project(view_proj, {frag_x, 0.0f, 0.0f}, kSize);
    REQUIRE(frag_px.x >= 0.0f);
    REQUIRE(frag_px.x < static_cast<float>(kSize));
    REQUIRE(frag_px.y >= 0.0f);
    REQUIRE(frag_px.y < static_cast<float>(kSize));

    LightingSettings ls;
    ls.shadows_enabled = true;
    ls.cascade_count = 2;
    ls.shadow_map_resolution = 512;
    ls.sdf_clipmap_enabled = true;
    ls.ddgi_enabled = true;
    ls.ddgi_probe_count_x = 5;
    ls.ddgi_probe_count_y = 1;
    ls.ddgi_probe_count_z = 1;
    ls.ddgi_probe_spacing = 0.5f;
    ls.ddgi_rays_per_probe = 64;
    ls.ddgi_hysteresis = 0.7f;

    const auto populate = [&](ecs::World& world, bool with_wall) {
        register_render_components(world);
        (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{floor_mesh}, MaterialRef{mat});
        (void)spawn_box(world, cube_mesh, mat, ceiling_center, ceiling_half);
        if (with_wall) {
            (void)spawn_box(world, cube_mesh, mat, wall_center, wall_half);
        }
        (void)world.spawn_with(ecs::WorldTransform{light_tf},
                               DirectionalLight{1.0f, 1.0f, 1.0f, 3.0f});
        (void)world.spawn_with(ecs::WorldTransform{cam_tf}, Camera{0.9f, 0.1f, 10.0f, true});
    };

    const auto step = [&](ecs::World& world, bool ddgi_on) {
        ls.ddgi_enabled = ddgi_on;
        renderer.set_lighting(ls);
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        REQUIRE(out.hdr.is_valid());
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
    };
    const auto lum = [&](const HdrImage& img) {
        return img.luminance(static_cast<std::uint32_t>(frag_px.x),
                             static_cast<std::uint32_t>(frag_px.y));
    };

    // Converge the dark room with the wall up (20 updates ~ steady state at h=0.7).
    ecs::World walled;
    populate(walled, /*with_wall=*/true);
    for (int i = 0; i < 20; ++i) {
        (void)step(walled, /*ddgi_on=*/true);
    }
    const float before_on = lum(step(walled, /*ddgi_on=*/true));
    const float before_off = lum(step(walled, /*ddgi_on=*/false));
    MESSAGE("BEFORE (wall up): dark-room floor ddgi-on=" << before_on
                                                         << " ddgi-off=" << before_off);

    // The dark room really is dark with the wall up: its floor sits at (or below) the flat ambient
    // floor in both configs — the ceiling blocks the sun, the wall blocks the bounce.
    CHECK(before_on < 0.05f);

    // ── Break the wall: remove its SDF twin and its visual mesh, and invalidate the DDGI region so
    // the probes fast-track instead of riding out the full hysteresis. ───────────────────────────
    renderer.sdf_clipmap().remove_instance(kWallSdf);
    const WorldAabb wall_region{wall_center - wall_half, wall_center + wall_half};
    renderer.invalidate_ddgi_region(wall_region);
    ecs::World open;
    populate(open, /*with_wall=*/false);

    constexpr int kPostBreak = 6;
    float after_on = 0.0f;
    for (int i = 0; i < kPostBreak; ++i) {
        after_on = lum(step(open, /*ddgi_on=*/true));
    }
    const float after_off = lum(step(open, /*ddgi_on=*/false));
    MESSAGE("AFTER (" << kPostBreak << " updates, wall gone): dark-room floor ddgi-on=" << after_on
                      << " ddgi-off=" << after_off);

    // (a) The dark room's floor LIT UP under GI: with the divider gone, the sunlit floor next door
    // bounces real light onto it. Measured: 0.00068 -> 0.0059, an ~8.6x rise (0.0052 absolute,
    // about 340 LSB at 16-bit — a resolved, deterministic signal). The margins sit well inside
    // that.
    const float on_rise = after_on - before_on;
    MESSAGE("GI rise on the dark floor: " << on_rise << " (from " << before_on << ")");
    CHECK(after_on > before_on * 3.0f);
    CHECK(on_rise > 0.003f);

    // (b) And it was BOUNCE, not direct light. The DDGI-off control — the same scene lit by the
    // flat ambient constant GI replaced — is BIT-IDENTICAL across the break (0.0159912 both times,
    // exactly 0.8 * the 0.02 ambient), because the ceiling blocks the vertical sun whether or not
    // the side wall stands: the direct term at this fragment is zero throughout. The
    // constant-ambient renderer is blind to the fallen wall; only the traced field sees it. This is
    // the milestone sentence with GI as the ENTIRE signal, not a correction to a
    // direct-light-dominated change like the slab test.
    CHECK(std::fabs(after_off - before_off) <
          1.0e-4f);            // control bit-flat: direct light never moved
    CHECK(before_off < 0.02f); // and is pure ambient — zero direct light here
    // A closing honesty note, the leak guard's point restated dynamically: even lit by bounce the
    // enclosed room stays DIMMER than the flat constant would have painted it (0.0059 < 0.016). GI
    // does not invent the fill the constant assumed — it computes the real, smaller amount that
    // physically arrives. That is why retiring the constant (m10.6) is a correctness change, and
    // why this test measures the rise against the room's OWN sealed self, not against the ambient
    // floor.
    CHECK(after_on < after_off);
}
