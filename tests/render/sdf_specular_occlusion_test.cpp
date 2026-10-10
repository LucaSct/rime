// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// ADR-0078 section 2, SDF cone-traced sky specular occlusion: the two limits the first proof named
// about itself -- "the proof is flat and pure sky fallback, so it does not prove curved surfaces
// or mixed SSR hits". The feature is OFF by default; every case switches it on explicitly and
// proves the enabled path. No golden images: each claim is a structural property with a margin.
//
//   1. CURVED. A sphere replaces the flat plane in the enclosed/outdoor pair. A plane has ONE
//      normal, so one cone width; a sphere's normal changes per pixel, so the reflection direction
//      and the cone (2*alpha, from the widened alpha) both vary across the surface.
//   2. MIXED SSR HITS. A smooth metal floor with an emissive box standing on it, SSR on, under a
//      canopy that the camera cannot see. Over part of the floor the reflection ray finds the box
//      on screen (a real SSR hit); over the rest it leaves the screen into the sky, and there the
//      canopy blocks it for some pixels and not for others. ssr_resolve.frag multiplies ONLY the
//      sky fallback, so toggling the occlusion must leave the hit pixels alone, darken the missed-
//      and-blocked pixels and leave the missed-and-open ones alone. The partition is DERIVED from
//      rendered output (an object-free twin render says which pixels the box touches; an on/off
//      diff says which the occlusion touches), then cross-checked against the pinhole geometry.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
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
#include "rime/render/lighting/sky.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

using namespace rime;
using namespace rime::render;
using rime::render::test::decode_hdr;
using rime::render::test::HdrImage;
using rime::render::test::read_texture;
using rime::render::test::vulkan_required;

namespace {

constexpr std::uint32_t kSize = 96;
constexpr float kFov = 1.1f;
// The outdoor twin must be beyond the 8 m cone reach of EVERYTHING in the cavity, not just of the
// cavity centre: at x = 12 the sphere's -x limb is 7.7 m from the cavity's east wall and the
// occlusion rightly darkened it (measured on the first run). 20 m leaves 15.7 m.
constexpr float kOutdoorX = 20.0f;

[[nodiscard]] std::unique_ptr<rhi::Device> device_or_skip(const char* what) {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available -- skipping " << what);
    }
    return device;
}

double rel(double a, double b) {
    return std::abs(a - b) / std::max(std::abs(b), 1e-9);
}

SkyParams clear_sky() {
    SkyParams sp{};
    sp.enabled = true;
    sp.clouds_enabled = false;
    sp.use_scene_sun = false;
    sp.sun_direction[0] = 0.0f;
    sp.sun_direction[1] = 0.8f;
    sp.sun_direction[2] = -0.6f;
    return sp;
}

// Analytic box field sampled at texel centres (the renderer's extraction/compose still runs).
float box_distance(core::Vec3 p, core::Vec3 h) {
    const core::Vec3 q{std::fabs(p.x) - h.x, std::fabs(p.y) - h.y, std::fabs(p.z) - h.z};
    const core::Vec3 outside{std::max(q.x, 0.0f), std::max(q.y, 0.0f), std::max(q.z, 0.0f)};
    const float outside_len =
        std::sqrt(outside.x * outside.x + outside.y * outside.y + outside.z * outside.z);
    const float inside = std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
    return outside_len + inside;
}

assets::MeshSdfAsset box_sdf(core::Vec3 half_extents, std::uint32_t target_resolution) {
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
    for (std::uint32_t kz = 0; kz < res[2]; ++kz)
        for (std::uint32_t jy = 0; jy < res[1]; ++jy)
            for (std::uint32_t ix = 0; ix < res[0]; ++ix) {
                const core::Vec3 p{sdf.grid_origin.x + (static_cast<float>(ix) + 0.5f) * voxel_size,
                                   sdf.grid_origin.y + (static_cast<float>(jy) + 0.5f) * voxel_size,
                                   sdf.grid_origin.z +
                                       (static_cast<float>(kz) + 0.5f) * voxel_size};
                const float d = box_distance(p, half_extents);
                sdf.distances[sdf.index(ix, jy, kz)] = d;
                max_abs = std::max(max_abs, std::fabs(d));
            }
    sdf.max_abs_distance = max_abs;
    return sdf;
}

// A black, SDF-carrying slab: invisible to the sky proofs except as an occluder.
void place_slab(SceneRenderer& renderer,
                MeshRegistry& meshes,
                MaterialRegistry& materials,
                ecs::World& world,
                core::Vec3 half,
                core::Vec3 at) {
    PbrMaterialDesc wall{};
    wall.base_color[0] = wall.base_color[1] = wall.base_color[2] = 0.0f;
    const MaterialId wall_mat = materials.add(wall);
    const SdfSourceId sdf = renderer.register_sdf_source(box_sdf(half, 64));
    const MeshId mesh = meshes.add(make_box(half), "sdf-occlusion-slab");
    core::Transform t{};
    t.translation = at;
    (void)world.spawn_with(
        ecs::WorldTransform{t}, MeshRef{mesh}, MaterialRef{wall_mat}, SdfRef{sdf});
}

HdrImage render_hdr(rhi::Device& device, SceneRenderer& renderer, ecs::World& world) {
    RenderGraph graph(device);
    graph.reset();
    const auto out = renderer.render(graph, world, {kSize, kSize});
    REQUIRE(out.hdr.is_valid());
    graph.export_texture(out.hdr);
    auto cmd = device.begin_commands();
    graph.execute(*cmd);
    device.submit_blocking(*cmd);
    return decode_hdr(read_texture(device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
}

// ── Scene 1: a sphere in the enclosed / outdoor pair
// ──────────────────────────────────────────────
struct SphereScene {
    ecs::World world;
    ecs::Entity camera;

    SphereScene(SceneRenderer& renderer, MeshRegistry& meshes, MaterialRegistry& materials) {
        register_render_components(world);
        PbrMaterialDesc metal{};
        metal.base_color[0] = metal.base_color[1] = metal.base_color[2] = 0.9f;
        metal.metallic = 1.0f;
        metal.roughness = 0.6f;
        const MaterialId mat = materials.add(metal);
        const MeshId sphere = meshes.add(make_uv_sphere(0.8f, 32, 64), "sdf-specular-sphere");
        for (const float x : {0.0f, kOutdoorX}) {
            core::Transform t{};
            t.translation = {x, 1.0f, -2.5f};
            (void)world.spawn_with(ecs::WorldTransform{t}, MeshRef{sphere}, MaterialRef{mat});
        }
        // The same sealed cavity as the flat proof (x/z = [-3, 3], y = [-0.125, 3]) and an
        // identical solid floor under the outdoor sphere, which catches a self-occlusion defect.
        const auto s = [&](core::Vec3 h, core::Vec3 at) {
            place_slab(renderer, meshes, materials, world, h, at);
        };
        s({3.5f, 0.25f, 3.5f}, {0.0f, -0.375f, 0.0f});
        s({3.5f, 0.25f, 3.5f}, {0.0f, 3.25f, 0.0f});
        s({0.25f, 2.0f, 3.5f}, {-3.25f, 1.5f, 0.0f});
        s({0.25f, 2.0f, 3.5f}, {3.25f, 1.5f, 0.0f});
        s({3.5f, 2.0f, 0.25f}, {0.0f, 1.5f, -3.25f});
        s({3.5f, 2.0f, 0.25f}, {0.0f, 1.5f, 3.25f});
        s({3.5f, 0.25f, 3.5f}, {kOutdoorX, -0.375f, 0.0f});
        core::Transform cam{};
        cam.translation = {0.0f, 1.0f, 0.0f};
        camera = world.spawn_with(ecs::WorldTransform{cam}, Camera{kFov, 0.1f, 100.0f, true});
    }

    void view(float x) {
        core::Transform cam{};
        cam.translation = {x, 1.0f, 0.0f};
        *world.get<ecs::WorldTransform>(camera) = ecs::WorldTransform{cam};
        world.mark_changed<ecs::WorldTransform>(camera);
    }
};

// The pixels that are sphere AND whose mirror ray points at or above the horizon, from the pinhole
// geometry (focal length in pixels = (size/2)/tan(fov/2); sphere r = 0.8 centred 2.5 m dead ahead
// and level with the camera). Why only those: a sphere's lower half reflects DOWN into the solid
// floor that sits under it, and that floor is a genuine occluder -- the occlusion is RIGHT to
// darken it, outdoors as well as indoors (measured on this proof's first run: it did, to 0). The
// "outdoor keeps its sky" claim is therefore made where sky is what the ray actually sees. Such a
// ray starts at y >= 1 m and never descends, so the floor (4 voxels = 0.5 m narrow band, 1.125 m
// below) cannot reach it. The mask is analytic, so it also fixes the pixel set across all runs.
bool sphere_reflects_upward(std::uint32_t px, std::uint32_t py) {
    const double f = 48.0 / std::tan(0.5 * static_cast<double>(kFov));
    double dx = (px + 0.5 - 48.0) / f, dy = -(py + 0.5 - 48.0) / f, dz = -1.0;
    const double inv = 1.0 / std::sqrt(dx * dx + dy * dy + dz * dz);
    dx *= inv;
    dy *= inv;
    dz *= inv;
    // Ray-sphere: centre c = (0, 0, -2.5) relative to the camera, radius 0.8.
    const double cz = -2.5, r = 0.8;
    const double b = dz * cz;
    const double disc = b * b - (cz * cz - r * r);
    if (disc <= 0.0)
        return false;
    const double t = b - std::sqrt(disc);
    const double nx = (dx * t) / r, ny = (dy * t) / r, nz = (dz * t - cz) / r;
    const double vn = dx * nx + dy * ny + dz * nz;
    return dy - 2.0 * vn * ny >= 0.0; // reflected ray's y component
}

} // namespace

TEST_CASE("SDF specular occlusion: an enclosed SPHERE loses sky and the identical outdoor sphere "
          "keeps it") {
    auto device = device_or_skip("the curved SDF specular occlusion proof");
    if (!device)
        return;
    for (const bool ssr : {false, true}) {
        for (const bool chain : {false, true}) {
            CAPTURE(ssr);
            CAPTURE(chain);
            MeshRegistry meshes(*device);
            MaterialRegistry materials;
            SceneRenderer renderer(*device, meshes, materials);
            render::test::disable_temporal_aa(renderer);
            CHECK_FALSE(renderer.sdf_specular_occlusion_enabled());
            renderer.set_sky(clear_sky());
            renderer.set_sky_specular_prefilter_enabled(chain);
            LightingSettings ls{};
            ls.sdf_clipmap_enabled = true;
            ls.ssr_enabled = ssr;
            renderer.set_lighting(ls);
            SphereScene scene(renderer, meshes, materials);
            for (const float x : {0.0f, kOutdoorX}) {
                CAPTURE(x);
                scene.view(x);
                renderer.set_sdf_specular_occlusion_enabled(false);
                (void)render_hdr(*device, renderer, scene.world); // warm bakes / recentre
                const HdrImage off = render_hdr(*device, renderer, scene.world);
                renderer.set_sdf_specular_occlusion_enabled(true);
                const HdrImage on = render_hdr(*device, renderer, scene.world);
                double off_sum = 0.0, on_sum = 0.0, lo = 1e30, hi = 0.0;
                double worst_ratio = 0.0;  // largest per-pixel on/off over the disc
                double worst_change = 0.0; // largest per-pixel relative change over the disc
                int n = 0;
                for (std::uint32_t y = 0; y < kSize; ++y)
                    for (std::uint32_t px = 0; px < kSize; ++px) {
                        if (!sphere_reflects_upward(px, y))
                            continue;
                        const double a = off.luminance(px, y);
                        const double b = on.luminance(px, y);
                        off_sum += a;
                        on_sum += b;
                        lo = std::min(lo, a);
                        hi = std::max(hi, a);
                        worst_ratio = std::max(worst_ratio, b / std::max(a, 1e-9));
                        worst_change = std::max(worst_change, rel(b, a));
                        ++n;
                    }
                const double off_mean = off_sum / n;
                const double on_mean = on_sum / n;
                REQUIRE_MESSAGE(off_mean > 1e-4, "control sphere must reflect a nonzero sky");
                MESSAGE("SDF sphere: mask px=" << n << " SSR=" << ssr << " chain=" << chain
                                               << " camera x=" << x << " off mean=" << off_mean
                                               << " [" << lo << ", " << hi << "] on mean="
                                               << on_mean << " ratio=" << on_mean / off_mean
                                               << " worst px ratio=" << worst_ratio
                                               << " worst px change=" << worst_change);
                // The disc is wholly sphere. Its normal -- hence its reflection direction and its
                // cone width -- changes from pixel to pixel, so the unoccluded image must NOT be
                // flat; that variation is what makes this a curved-surface proof.
                CHECK_MESSAGE((hi - lo) > 0.05 * off_mean,
                              "the sphere's sky specular must vary across the disc");
                if (x == 0.0f) {
                    CHECK_MESSAGE(on_mean < off_mean * 0.1,
                                  "enclosed sphere sky must drop by at least 10x");
                } else {
                    CHECK_MESSAGE(rel(on_mean, off_mean) < 0.02,
                                  "outdoor sphere sky must change by less than 2%");
                    CHECK_MESSAGE(worst_change < 0.02,
                                  "no outdoor sphere pixel may lose its sky specular");
                }
            }
            CHECK(renderer.sdf_specular_occlusion_stats().enabled_frames == 2);
            CHECK(renderer.sdf_specular_occlusion_stats().disabled_frames == 4);
            CHECK(renderer.sdf_specular_occlusion_stats().unavailable_frames == 0);
        }
    }
}

// ── Scene 2: a smooth metal floor, an emissive box on it, a canopy the camera cannot see ─────────
namespace {

constexpr float kPitch = -0.5404f; // look down at the floor, as ssr_test.cpp does
constexpr core::Vec3 kCam{0.0f, 1.5f, 1.0f};
constexpr core::Vec3 kBoxHalf{0.35f, 0.35f, 0.35f};
constexpr core::Vec3 kBoxAt{0.0f, 0.35f, -1.8f}; // stands on the floor (y = 0)

core::Mat4 mixed_view_proj() {
    core::Transform cam{};
    cam.translation = kCam;
    cam.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, kPitch);
    return core::perspective(kFov, 1.0f, 0.1f, 40.0f) * core::inverse(core::to_matrix(cam));
}

// The canopy (y = [3, 3.5], x = +-1.2, z = [-12, 3]) is 1.5 m above the camera. The view's top edge
// is +0.5 degrees above the horizon, so the nearest canopy edge that is ahead of the camera
// (z = -12, 1.5 m up) is 6.6 degrees up: off-screen. It exists only as an SDF occluder.
// A floor point's mirror ray from this camera rises 1.5/(1-z0) per unit of -z and, at the canopy's
// height, has travelled 2*(1-z0) further and tripled its x offset: so a floor point with
// |x0| < 0.4 flies into the canopy and one with |x0| > 0.4 passes beside it.
struct MixedScene {
    ecs::World world;

    MixedScene(SceneRenderer& renderer,
               MeshRegistry& meshes,
               MaterialRegistry& materials,
               bool with_object) {
        register_render_components(world);
        PbrMaterialDesc floor_mat{};
        floor_mat.base_color[0] = floor_mat.base_color[1] = floor_mat.base_color[2] = 0.9f;
        floor_mat.metallic = 1.0f;
        floor_mat.roughness = 0.2f; // resolve cone = smoothstep(0.25, 0.55, r) = 0: a mirror
        const MaterialId floor = materials.add(floor_mat);
        const MeshId plane = meshes.add(make_plane(6.0f), "sdf-mixed-floor");
        (void)world.spawn_with(ecs::WorldTransform{}, MeshRef{plane}, MaterialRef{floor});
        place_slab(renderer, meshes, materials, world, {7.0f, 0.25f, 7.0f}, {0.0f, -0.375f, 0.0f});
        place_slab(renderer, meshes, materials, world, {1.2f, 0.25f, 7.5f}, {0.0f, 3.25f, -4.5f});
        if (with_object) {
            PbrMaterialDesc glow{};
            glow.base_color[0] = glow.base_color[1] = glow.base_color[2] = 0.0f;
            // A metal with a black base colour has F0 = 0 and no diffuse: it reflects NOTHING, so
            // the box is pure emission. Otherwise the forward pass would add the box's own sky
            // specular, which the SDF occlusion rightly darkens under the canopy, and a screen hit
            // on the box would change because the box it hit changed, not because the resolve
            // occluded a hit.
            glow.metallic = 1.0f;
            glow.roughness = 1.0f;
            glow.emissive[0] = 4.0f;
            glow.emissive[1] = 1.0f;
            glow.emissive[2] = 0.5f;
            const MaterialId gm = materials.add(glow);
            const MeshId box = meshes.add(make_box(kBoxHalf), "sdf-mixed-box");
            core::Transform t{};
            t.translation = kBoxAt;
            (void)world.spawn_with(ecs::WorldTransform{t}, MeshRef{box}, MaterialRef{gm});
        }
        core::Transform cam{};
        cam.translation = kCam;
        cam.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, kPitch);
        (void)world.spawn_with(ecs::WorldTransform{cam}, Camera{kFov, 0.1f, 40.0f, true});
    }
};

struct MixedFrames {
    HdrImage off;
    HdrImage on;
};

MixedFrames render_mixed(rhi::Device& device, bool with_object, bool chain) {
    MeshRegistry meshes(device);
    MaterialRegistry materials;
    SceneRenderer renderer(device, meshes, materials);
    render::test::disable_temporal_aa(renderer);
    renderer.set_sky(clear_sky());
    renderer.set_sky_specular_prefilter_enabled(chain);
    LightingSettings ls{};
    ls.sdf_clipmap_enabled = true;
    ls.ssr_enabled = true;
    renderer.set_lighting(ls);
    MixedScene scene(renderer, meshes, materials, with_object);
    renderer.set_sdf_specular_occlusion_enabled(false);
    (void)render_hdr(device, renderer, scene.world); // warm bakes / recentre
    MixedFrames f;
    f.off = render_hdr(device, renderer, scene.world);
    renderer.set_sdf_specular_occlusion_enabled(true);
    f.on = render_hdr(device, renderer, scene.world);
    CHECK(renderer.sdf_specular_occlusion_stats().enabled_frames == 1);
    CHECK(renderer.sdf_specular_occlusion_stats().unavailable_frames == 0);
    return f;
}

struct Rect {
    int x0 = 1 << 20, y0 = 1 << 20, x1 = -(1 << 20), y1 = -(1 << 20);

    void add(render::test::Pixel p) {
        x0 = std::min(x0, static_cast<int>(std::floor(p.x)));
        x1 = std::max(x1, static_cast<int>(std::ceil(p.x)));
        y0 = std::min(y0, static_cast<int>(std::floor(p.y)));
        y1 = std::max(y1, static_cast<int>(std::ceil(p.y)));
    }

    [[nodiscard]] bool contains(int x, int y, int margin) const {
        return x >= x0 - margin && x <= x1 + margin && y >= y0 - margin && y <= y1 + margin;
    }
};

} // namespace

TEST_CASE("SDF specular occlusion: only the sky fallback is occluded, never a real SSR hit") {
    auto device = device_or_skip("the mixed SSR / SDF occlusion proof");
    if (!device)
        return;
    const core::Mat4 vp = mixed_view_proj();
    // The box and its mirror image in the floor, as pinhole rectangles: where a direct view of the
    // box and a mirror ray that hits it can appear.
    Rect direct, mirror;
    for (const float sx : {-1.0f, 1.0f})
        for (const float sy : {-1.0f, 1.0f})
            for (const float sz : {-1.0f, 1.0f}) {
                const core::Vec3 c{kBoxAt.x + sx * kBoxHalf.x,
                                   kBoxAt.y + sy * kBoxHalf.y,
                                   kBoxAt.z + sz * kBoxHalf.z};
                direct.add(render::test::project(vp, c, kSize));
                mirror.add(render::test::project(vp, {c.x, -c.y, c.z}, kSize));
            }
    CAPTURE(direct.x0);
    CAPTURE(direct.x1);
    CAPTURE(mirror.y0);
    CAPTURE(mirror.y1);

    for (const bool chain : {false, true}) {
        CAPTURE(chain);
        const MixedFrames with = render_mixed(*device, true, chain);
        const MixedFrames without = render_mixed(*device, false, chain);

        // The box's front-bottom edge touches the floor on this row; every floor reflection of the
        // box lies strictly below it, every direct view of the box on or above it.
        const float contact_row =
            render::test::project(vp, {0.0f, 0.0f, kBoxAt.z + kBoxHalf.z}, kSize).y;
        // The object-free twin IS the "ray missed" oracle: a floor pixel whose mirror ray hit the
        // box changes when the box is removed; one whose ray flew into the sky does not. The box's
        // OWN pixels also differ between the twins but are a different class -- they are a surface
        // the resolve shades with a rough probe of its own, so they are counted apart.
        int hit = 0, own = 0, own_darker = 0, dark = 0, open = 0, hit_outside_mirror = 0;
        double worst_hit_change = 0.0, worst_dark_ratio = 0.0, best_dark_ratio = 1e30;
        double worst_open_change = 0.0;
        int brighter = 0;
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const int ix = static_cast<int>(x), iy = static_cast<int>(y);
                const double w_off = with.off.luminance(x, y);
                const double w_on = with.on.luminance(x, y);
                const double wo_off = without.off.luminance(x, y);
                const bool object_touches = rel(w_off, wo_off) > 0.05;
                const double change = rel(w_on, w_off);
                const bool sdf_touches = change > 0.05;
                if (w_on > w_off * 1.0001)
                    ++brighter;
                if (object_touches && static_cast<float>(iy) <= contact_row + 1.5f) {
                    // At or above the box's floor contact line: the box itself, never a reflection.
                    ++own;
                    if (w_on < w_off)
                        ++own_darker;
                } else if (object_touches) {
                    ++hit;
                    worst_hit_change = std::max(worst_hit_change, change);
                    if (!mirror.contains(ix, iy, 3))
                        ++hit_outside_mirror;
                } else if (sdf_touches) {
                    ++dark;
                    const double ratio = w_on / std::max(w_off, 1e-9);
                    worst_dark_ratio = std::max(worst_dark_ratio, ratio);
                    best_dark_ratio = std::min(best_dark_ratio, ratio);
                } else {
                    ++open;
                    worst_open_change = std::max(worst_open_change, change);
                }
            }
        MESSAGE("SDF mixed: chain=" << chain << " direct rect x[" << direct.x0 << "," << direct.x1
                                    << "] y[" << direct.y0 << "," << direct.y1 << "] mirror rect x["
                                    << mirror.x0 << "," << mirror.x1 << "] y[" << mirror.y0 << ","
                                    << mirror.y1 << "]");
        MESSAGE("SDF mixed: chain="
                << chain << " SSR-hit floor px=" << hit
                << " (outside mirror rect +3px: " << hit_outside_mirror
                << ") worst on/off change=" << worst_hit_change << " | box own px=" << own
                << " (darker " << own_darker << ") | missed+blocked px=" << dark << " on/off in ["
                << best_dark_ratio << ", " << worst_dark_ratio << "] | unclassified px=" << open
                << " worst change=" << worst_open_change << " | px that got brighter=" << brighter);

        // The partition must be non-degenerate or the assertions below prove nothing.
        REQUIRE_MESSAGE(hit >= 20, "some floor rays must genuinely hit the box on screen");
        REQUIRE_MESSAGE(dark >= 20, "some floor rays must miss into a blocked sky direction");
        // 1. A floor pixel that found a real screen hit is unchanged by the occlusion.
        CHECK_MESSAGE(worst_hit_change < 0.02, "an SSR hit must not be occluded");
        // 2. Every pixel whose ray missed into a blocked direction got darker, not just different;
        //    and nothing anywhere got brighter.
        CHECK_MESSAGE(worst_dark_ratio < 0.95, "occluded sky fallback must darken");
        CHECK(brighter == 0);
        // 3. The darkening is LOCAL, not a global dimming: most of the frame is not in the
        //    darkened set. Note what this class is and is not -- it is every floor pixel that fell
        //    into none of the three named sets, so it includes the penumbra at the canopy's edge,
        //    where a cone partly intersects the blocker. Its worst change is therefore NOT zero
        //    (reported above, a few percent) and is deliberately not asserted to be: a cone filter
        //    with a soft edge is supposed to have a gradient there. What is asserted is only that
        //    this class outnumbers the darkened one, which is what "local" means.
        CHECK_MESSAGE(open > dark, "most pixels must not be in the darkened set");
        // 4. Geometry cross-check of the DERIVED hit set: it lies in the box's mirror image in the
        //    floor, with a 3 px margin for the hit march's step and edge fade.
        CHECK_MESSAGE(hit_outside_mirror == 0,
                      "derived SSR-hit pixels must lie in the box's mirror image");
        CHECK(own > 0);

        // 5. Hand-picked floor points whose mirror rays are known from the geometry above, as an
        //    independent check of the derived partition: x0=0 flies into the canopy (blocked),
        //    x0=1 passes beside it (open); neither can hit the box (it is ahead, z < -1.45).
        const auto pix = [&](float x0, float z0) {
            const render::test::Pixel p = render::test::project(vp, {x0, 0.0f, z0}, kSize);
            REQUIRE(p.x >= 0.0f);
            REQUIRE(p.x < static_cast<float>(kSize));
            REQUIRE(p.y >= 0.0f);
            REQUIRE(p.y < static_cast<float>(kSize));
            return std::pair<std::uint32_t, std::uint32_t>{static_cast<std::uint32_t>(p.x),
                                                           static_cast<std::uint32_t>(p.y)};
        };
        const auto [bx, by] = pix(0.0f, 0.0f);
        const auto [ox, oy] = pix(1.0f, -0.5f);
        const double blocked_off = with.off.luminance(bx, by);
        const double blocked_on = with.on.luminance(bx, by);
        CHECK_MESSAGE(rel(without.off.luminance(bx, by), blocked_off) < 0.02,
                      "the blocked point's ray must miss the box (twin render agrees)");
        const double open_off = with.off.luminance(ox, oy);
        const double open_on = with.on.luminance(ox, oy);
        CHECK_MESSAGE(rel(without.off.luminance(ox, oy), open_off) < 0.02,
                      "the open point's ray must miss the box (twin render agrees)");
        MESSAGE("SDF mixed: blocked floor point off=" << blocked_off << " on=" << blocked_on
                                                      << " | open floor point off=" << open_off
                                                      << " on=" << open_on);
        CHECK(blocked_off > 1e-4);
        CHECK_MESSAGE(blocked_on < blocked_off * 0.5, "a floor ray into the canopy must darken");
        CHECK(open_off > 1e-4);
        CHECK_MESSAGE(rel(open_on, open_off) < 0.02, "a floor ray beside the canopy must not");
    }
}
