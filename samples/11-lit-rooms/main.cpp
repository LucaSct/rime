// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// 11-lit-rooms — Milestone 10's "done when": the whole advanced-lighting stack on ONE scene, and
// opening a wall visibly changes the light in the room behind it. It closes M10 the way 08-gltf-zoo
// closed M6 and 10-destructible-wall closed M8 — the milestone ends in a runnable proof, not a
// compile — and it is the first place every M10 technique runs together in one frame:
//
//   • CSM directional shadows (m10.1)      — the sun, shadowing through cascades.
//   • local spot shadows + cache (m10.2)   — a lamp in the lit room, its shadow map cached and
//                                            re-rendered only when its region is invalidated.
//   • clustered forward (m10.3)            — the point lights, culled into froxels.
//   • SDF clipmap + DDGI probes (m10.4/5)  — the global-illumination field, sphere-traced through
//                                            the SDF the rooms register.
//   • SSR (m10.7)                          — a reflective floor, mirroring the lit room and falling
//                                            back to the probe field where the screen cannot see.
//
// THE BEAT (ADR-0032's headline): a dividing wall seals a dark room from a sunlit one. Break the
// wall and, in the same handful of frames, the dark room's floor lights up — direct sun through the
// new gap, the CSM shadow lifting, and the DDGI bounce arriving — and the reflective floor picks
// the change up. The wall "breaks" here by the honest destruction↔lighting seam M10 built (ADR-0032
// C2): its SDF twin and shadow-caster region are dropped and the lighting caches are invalidated,
// exactly the hooks a real M8 destruction event drives (10-destructible-wall wires the full physics
// version; this sample isolates the LIGHTING response, so the wall opens on a script/keypress).
//
// Run it:   build/dev/bin/lit_rooms --headless [--frames N] [--ppm out.ppm]
//           build/dev/bin/lit_rooms --serve [--host 0.0.0.0] [--port 9100] [--codec jpeg]
//
// The headless self-check is the CI-gated done-when: on a host with a Vulkan device (lavapipe on
// Linux CI) it renders the full stack, breaks the wall, and asserts the dark room's floor
// brightens; with no device it is an honest skip (exit 0) unless RIME_REQUIRE_VULKAN demands one.
// The rigorous, isolated GI mechanism proofs live in tests/render/gi_thesis_test.cpp; this sample
// is the everything-on-at-once integration and the lived demo.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "rime/app/application.hpp"
#include "rime/assets/sdf_asset.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/core/math/vec.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/platform/clock.hpp"
#include "rime/platform/socket.hpp"
#include "rime/render/components.hpp"
#include "rime/render/lighting/local_shadows.hpp" // WorldAabb
#include "rime/render/lighting/settings.hpp"
#include "rime/render/material.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"
#include "rime/rhi/rhi.hpp"
#include "rime/stream/frame_codec.hpp"
#include "rime/stream/frame_streamer.hpp"
#include "rime/stream/protocol.hpp"

namespace {

using namespace rime;
using ecs::WorldTransform;

constexpr std::uint32_t kWidth = 960;
constexpr std::uint32_t kHeight =
    540; // a DDGI-heavy frame on lavapipe; 540p keeps convergence brisk

// ── Analytically-exact box SDF (the identical construction gi_thesis_test.cpp / ddgi_test.cpp keep
// their own copy of — each GPU translation unit carries one). The rooms register these so the DDGI
// probes have a field to sphere-trace; the wall's is dropped when it breaks. ─────────────────────
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

// Spawn a visual box: a unit cube non-uniformly scaled to `half_extents` at `center`.
ecs::Entity spawn_box(ecs::World& world,
                      render::MeshId cube,
                      render::MaterialId mat,
                      core::Vec3 center,
                      core::Vec3 half_extents) {
    core::Transform tf{};
    tf.translation = center;
    tf.scale = half_extents;
    return world.spawn_with(WorldTransform{tf}, render::MeshRef{cube}, render::MaterialRef{mat});
}

// ── The scene geometry: a wall and its shadow ───────────────────────────────────────────────────
// A dividing WALL stands across the floor, perpendicular to the view. A 45°-ish sun rakes over it,
// so the wall casts a hard shadow across the floor strip in front of it (toward the camera), sealing
// that strip — "the dark room" — from the direct sun; the floor beyond is sunlit. Break the wall and
// the shadow lifts: direct sun floods the strip, its CSM shadow is gone, the DDGI bounce updates, and
// the reflective floor picks it all up. Unlike gi_thesis_test.cpp's ceiling-sealed room (whose
// relight is a small GI-only rise, deliberately measured in HDR), a lifted SUN shadow is a large,
// plainly-visible LDR change — the right thing for a lived demo. All coordinates in metres.
constexpr core::Vec3 kFloorCenter{0.0f, -0.15f, 0.0f}; // a slab floor, top at y=0
constexpr core::Vec3 kFloorHalf{3.5f, 0.15f, 3.5f};
constexpr core::Vec3 kWallCenter{0.0f, 1.2f, 0.0f}; // across z=0, x in [-2.5, 2.5], the divider
constexpr core::Vec3 kWallHalf{2.5f, 1.2f, 0.15f};
constexpr core::Vec3 kBackCenter{0.0f, 1.0f, -3.35f}; // a low back wall behind the sunlit room
constexpr core::Vec3 kBackHalf{3.5f, 1.0f, 0.15f};    // short, so its own sun shadow stays far back
constexpr core::Vec3 kPillarCenter{1.7f,
                                   0.5f,
                                   1.5f}; // a block in the foreground for the spot to cast
constexpr core::Vec3 kPillarHalf{0.22f, 0.5f, 0.22f};

constexpr std::uint64_t kFloorSdf = 1, kWallSdf = 2, kBackSdf = 3;

// A rendering app for the lit rooms: owns the registries + SceneRenderer, registers the SDF field,
// builds the world (all M10 lights + a reflective floor), and drives one render per frame.
// break_wall drops the divider through the destruction↔lighting seam.
struct LitRoomsApp {
    app::Application& app; // borrowed — the caller constructs it and CHECKS its device first
    render::MeshRegistry meshes;
    render::MaterialRegistry materials;
    render::SceneRenderer renderer;
    render::RGTexture last_ldr{};
    ecs::Entity wall_visual{};
    bool wall_broken = false;

    explicit LitRoomsApp(app::Application& application)
        : app(application), meshes(*app.device()), renderer(*app.device(), meshes, materials) {
        build();
        app.on_render([this](app::FrameContext& ctx) {
            last_ldr = renderer.render(*ctx.graph, ctx.world, ctx.extent, true).ldr;
        });
    }

    LitRoomsApp(const LitRoomsApp&) = delete;
    LitRoomsApp& operator=(const LitRoomsApp&) = delete;

    void build() {
        ecs::World& world = app.world();
        render::register_render_components(world);

        const render::MeshId cube = meshes.add(render::make_cube(1.0f), "cube");
        const render::MeshId plane = meshes.add(render::make_plane(6.0f), "floor");

        // A smooth, mid-grey REFLECTIVE floor: low roughness so SSR mirrors the lit room and falls
        // back to the probe field elsewhere; the diffuse albedo still shows the GI arriving.
        render::PbrMaterialDesc floor_md{};
        floor_md.base_color[0] = floor_md.base_color[1] = floor_md.base_color[2] = 0.35f;
        floor_md.metallic = 0.0f;
        floor_md.roughness = 0.22f;
        const render::MaterialId floor_mat = materials.add(floor_md);

        // Matte grey for the structure (walls/pillar) — a plausible plaster albedo, the grey-world
        // the DDGI bounce assumes anyway.
        render::PbrMaterialDesc wall_md{};
        wall_md.base_color[0] = wall_md.base_color[1] = wall_md.base_color[2] = 0.72f;
        wall_md.metallic = 0.0f;
        wall_md.roughness = 0.9f;
        const render::MaterialId wall_mat = materials.add(wall_md);

        // The floor plane sits at y=0 (its SDF twin is a thin slab just under it, so a probe just
        // above the floor sees a surface, not empty space). The divider stands across it; a back
        // wall encloses the sunlit room beyond and gives the DDGI something to bounce off.
        (void)world.spawn_with(
            WorldTransform{}, render::MeshRef{plane}, render::MaterialRef{floor_mat});
        (void)spawn_box(world, cube, wall_mat, kBackCenter, kBackHalf);
        (void)spawn_box(world, cube, wall_mat, kPillarCenter, kPillarHalf);
        wall_visual = spawn_box(world, cube, wall_mat, kWallCenter, kWallHalf);

        // The SDF field the DDGI probes trace: the floor slab, the divider, the back wall. (The
        // pillar is small — left out of the field to keep the trace cheap; it still casts a real
        // local shadow via the spot's shadow map.)
        renderer.sdf_clipmap().update_instance(
            kFloorSdf, build_box_sdf(kFloorHalf), core::mat4_translation(kFloorCenter));
        renderer.sdf_clipmap().update_instance(
            kWallSdf, build_box_sdf(kWallHalf), core::mat4_translation(kWallCenter));
        renderer.sdf_clipmap().update_instance(
            kBackSdf, build_box_sdf(kBackHalf), core::mat4_translation(kBackCenter));

        // The sun: a ~53° sun raking over the divider from behind it (travelling down and +z), so
        // the wall throws a hard shadow across the floor strip in FRONT of it (z in [0, ~1.8]) —
        // the strip the camera looks straight at, and the one the break relights. CSM shadows it.
        // Local −z rotated so it travels (0, −0.8, +0.6).
        core::Transform sun_tf{};
        sun_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -2.2142975f);
        (void)world.spawn_with(WorldTransform{sun_tf},
                               render::DirectionalLight{1.0f, 0.97f, 0.9f, 3.4f});

        // A warm SPOT lamp over the foreground pillar, aimed straight down so it casts a real local
        // shadow (m10.2's cached shadow map) — a pool of lamplight standing apart from the sun's,
        // off to the side of the strip the proof measures. Its −z is the cone axis, so pitch it
        // down.
        core::Transform spot_tf{};
        spot_tf.translation = {kPillarCenter.x, 2.7f, kPillarCenter.z};
        spot_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -1.5707963f);
        (void)world.spawn_with(WorldTransform{spot_tf},
                               render::SpotLight{1.0f, 0.72f, 0.4f, 22.0f, 6.0f, 0.5f, 0.72f});

        // A couple of POINT lights for the clustered path to cull (m10.3), tight-radius so — point
        // lights being unshadowed here (cube shadows are a deferred follow-up) — they don't wash
        // across the strip the proof measures. Fields are {r, g, b, intensity, radius}. One by the
        // pillar, one in the sunlit room beyond the divider (seen once it falls).
        (void)world.spawn_with(
            WorldTransform{poser({kPillarCenter.x + 0.1f, 1.2f, kPillarCenter.z})},
            render::PointLight{1.0f, 0.6f, 0.4f, 5.0f, 1.7f});
        (void)world.spawn_with(WorldTransform{poser({-0.4f, 1.2f, -2.0f})},
                               render::PointLight{0.45f, 0.6f, 1.0f, 4.0f, 1.8f});

        renderer.set_ambient(0.02f, 0.02f, 0.025f);

        // Every M10 gate on at once — the integration this sample exists to exercise.
        render::LightingSettings ls;
        ls.shadows_enabled = true;
        ls.cascade_count = 3;
        ls.shadow_map_resolution = 1024;
        ls.local_shadows_enabled = true;
        ls.local_shadow_resolution = 1024;
        ls.clustered_enabled = true;
        ls.sdf_clipmap_enabled = true;
        ls.ddgi_enabled = true;
        ls.ddgi_probe_count_x = 6;
        ls.ddgi_probe_count_y = 5;
        ls.ddgi_probe_count_z = 10; // long enough to reach from the eye across the shadowed strip
        ls.ddgi_probe_spacing = 0.5f;
        ls.ddgi_rays_per_probe = 64;
        ls.ddgi_hysteresis = 0.85f;
        ls.ssr_enabled = true;
        ls.ssr_max_distance = 8.0f;
        ls.ssr_thickness = 0.5f;
        renderer.set_lighting(ls);
    }

    static WorldTransform poser(core::Vec3 p) {
        core::Transform t{};
        t.translation = p;
        return WorldTransform{t};
    }

    // Break the divider: despawn its visual leaf AND drop its SDF twin, then invalidate the
    // lighting caches over its region (the ADR-0032 C2 hooks — the DDGI probes fast-track and the
    // local shadow cache re-renders). Exactly what a real destruction event stream would drive.
    void break_wall() {
        if (wall_broken)
            return;
        wall_broken = true;
        app.world().despawn(wall_visual);
        renderer.sdf_clipmap().remove_instance(kWallSdf);
        const render::WorldAabb region{kWallCenter - kWallHalf, kWallCenter + kWallHalf};
        renderer.invalidate_ddgi_region(region);
        renderer.invalidate_sdf_region(region);
        renderer.invalidate_shadow_region(region);
    }
};

// ── Camera ────────────────────────────────────────────────────────────────────────────────────
// Low and close, INSIDE the dark room looking out across the divider toward the sunlit room — the
// DDGI lattice is camera-centred, so a low eye keeps the probes over the floor (an elevated
// bird's-eye view would lift the lattice off the ground). The wall fills the middle distance; when
// it drops, the sunlit room and the relit near floor open up in one view.
core::Transform demo_camera() {
    core::Transform t{};
    // Low and close, on the shadowed side, looking forward (−z) and down across the strip toward
    // the divider — the DDGI lattice is camera-centred, so a low eye keeps the probes over the
    // floor (a bird's-eye view would lift the lattice off the ground). y = 1.75 snaps the lowest
    // probe layer clear of the floor (the gi_thesis snap lesson). The divider fills the
    // mid-distance; when it drops, the shadowed near floor floods with light and the sunlit room
    // opens up beyond.
    t.translation = {0.0f, 1.75f, 2.4f};
    t.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -0.34f); // ≈ 0.34 rad down
    return t;
}

// Spawn the demo camera entity (its lens; the transform is demo_camera()).
ecs::Entity spawn_camera(app::Application& app) {
    return app.world().spawn_with(WorldTransform{demo_camera()},
                                  render::Camera{1.05f, 0.1f, 40.0f});
}

// ── I/O helpers (the 07/10 pattern) ──────────────────────────────────────────────────────────────
std::vector<std::uint8_t>
read_rgba8(rhi::Device& device, rhi::TextureHandle tex, std::uint32_t w, std::uint32_t h) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 4;
    rhi::BufferDesc bd{};
    bd.size = bytes;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryUsage::GpuToCpu;
    const rhi::BufferHandle rb = device.create_buffer(bd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(tex, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

// Average luminance over a screen rectangle (fractions of the frame), the robust region measure a
// "the room lit up" proof wants (a single pixel is at the mercy of a reflection highlight).
double
region_luminance(const std::vector<std::uint8_t>& px, float x0, float y0, float x1, float y1) {
    const auto cx0 = static_cast<std::uint32_t>(x0 * static_cast<float>(kWidth));
    const auto cx1 = static_cast<std::uint32_t>(x1 * static_cast<float>(kWidth));
    const auto cy0 = static_cast<std::uint32_t>(y0 * static_cast<float>(kHeight));
    const auto cy1 = static_cast<std::uint32_t>(y1 * static_cast<float>(kHeight));
    double sum = 0.0;
    std::uint64_t n = 0;
    for (std::uint32_t y = cy0; y < cy1; ++y) {
        for (std::uint32_t x = cx0; x < cx1; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            sum += (px[i] + px[i + 1] + px[i + 2]) / 3.0;
            ++n;
        }
    }
    return n ? sum / static_cast<double>(n) : 0.0;
}

bool scene_is_lit(const std::vector<std::uint8_t>& px) {
    std::uint64_t lit = 0, bright = 0;
    const std::size_t n = px.size() / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const int lum = (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3;
        if (lum > 25)
            ++lit;
        if (lum > 170)
            ++bright;
    }
    return lit > n / 40 && bright > 30;
}

std::uint64_t
image_diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int threshold) {
    std::uint64_t changed = 0;
    const std::size_t n = std::min(a.size(), b.size()) / 4;
    for (std::size_t i = 0; i < n; ++i) {
        const int d = std::abs(a[i * 4] - b[i * 4]) + std::abs(a[i * 4 + 1] - b[i * 4 + 1]) +
                      std::abs(a[i * 4 + 2] - b[i * 4 + 2]);
        if (d > threshold)
            ++changed;
    }
    return changed;
}

void write_ppm(const char* path, const std::vector<std::uint8_t>& px) {
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%u %u\n255\n", kWidth, kHeight);
    for (std::size_t i = 0; i < static_cast<std::size_t>(kWidth) * kHeight; ++i)
        std::fwrite(&px[i * 4], 1, 3, f);
    std::fclose(f);
    std::printf("  wrote %s\n", path);
}

app::AppConfig gpu_config() {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {kWidth, kHeight};
    cfg.tick_hz = 60.0;
    return cfg;
}

// The dark room's near floor, in frame fractions — the patch the divider seals from the sun and the
// break relights. Tuned to the demo camera above (the lower-left third, where the near floor sits).
constexpr float kDarkX0 = 0.10f, kDarkY0 = 0.62f, kDarkX1 = 0.45f, kDarkY1 = 0.92f;

// ── --headless: the M10 done-when ────────────────────────────────────────────────────────────────
int run_headless(int converge, const char* ppm) {
    app::Application app(gpu_config());
    if (!app.device()) {
        std::fprintf(stderr, "11-lit-rooms: no Vulkan device (need a driver or lavapipe)\n");
        return std::getenv("RIME_REQUIRE_VULKAN") ? 1 : 0; // absent GPU: a skip, unless required
    }
    LitRoomsApp scene(app);
    (void)spawn_camera(app);
    std::printf("11-lit-rooms: full M10 stack on '%s' (%ux%u), converging DDGI %d frames…\n",
                app.device()->adapter().name.c_str(),
                kWidth,
                kHeight,
                converge);

    // Converge the probe field with the wall UP, then read the sealed dark room's near floor.
    for (int i = 0; i < converge; ++i)
        app.step(app.fixed_dt());
    const std::vector<std::uint8_t> before =
        read_rgba8(*app.device(), app.graph()->physical(scene.last_ldr), kWidth, kHeight);
    const double dark_before = region_luminance(before, kDarkX0, kDarkY0, kDarkX1, kDarkY1);
    const bool lit = scene_is_lit(before);
    if (ppm)
        write_ppm(ppm, before);

    // Break the divider and let the fast-tracked probes + shadow cache catch up.
    scene.break_wall();
    for (int i = 0; i < converge; ++i)
        app.step(app.fixed_dt());
    const std::vector<std::uint8_t> after =
        read_rgba8(*app.device(), app.graph()->physical(scene.last_ldr), kWidth, kHeight);
    const double dark_after = region_luminance(after, kDarkX0, kDarkY0, kDarkX1, kDarkY1);
    const std::uint64_t changed = image_diff(before, after, 24);

    std::printf("  full-stack render: scene lit=%d\n", lit);
    std::printf("  dark-room floor: before=%.2f  after=%.2f  (%.2fx)\n",
                dark_before,
                dark_after,
                dark_before > 0.01 ? dark_after / dark_before : 0.0);
    std::printf("  the break repainted %llu px\n", static_cast<unsigned long long>(changed));

    // The done-when: the full stack renders lit (all six gates compose without conflict), the dark
    // room's floor brightens materially once the divider falls (the integrated thesis — direct sun
    // through the gap, the CSM shadow lifting, and the DDGI bounce arriving), and the break visibly
    // repaints the frame.
    const bool thesis = dark_after > dark_before * 1.4 && (dark_after - dark_before) > 4.0;
    const bool ok = lit && thesis && changed > 2000;
    std::printf("11-lit-rooms: %s\n",
                ok ? "the wall falls, the dark room lights up — M10 green!" : "FAILED self-check");
    return ok ? 0 : 1;
}

// ── --serve: stream the beat live; any key drops the wall ────────────────────────────────────────
int run_serve(const std::string& host, std::uint16_t port, stream::Codec codec) {
    app::Application app(gpu_config());
    if (!app.device()) {
        std::fprintf(stderr, "11-lit-rooms server: no Vulkan device (need lavapipe/a GPU)\n");
        return 1;
    }
    LitRoomsApp scene(app);
    (void)spawn_camera(app);
    auto streamer = stream::FrameStreamer::create(*app.device(), {kWidth, kHeight});
    auto listener = platform::TcpListener::bind(port, host);
    if (!streamer || !listener) {
        std::fprintf(stderr, "11-lit-rooms server: could not create streamer/listener\n");
        return 1;
    }
    std::printf("11-lit-rooms server: listening on %s:%u — waiting for a client…\n",
                host.c_str(),
                listener->local_port());
    auto accepted = listener->accept();
    if (!accepted)
        return 1;
    stream::ProtocolConnection conn(std::move(*accepted));
    if (!conn.handshake())
        return 1;
    std::printf("11-lit-rooms server: client connected — streaming %up. Press any key to break the "
                "wall.\n",
                kHeight);

    std::atomic<bool> stop{false};
    std::atomic<bool> hit{false};
    std::thread input_thread([&] {
        stream::MessageType type{};
        std::vector<std::byte> payload;
        while (!stop.load()) {
            if (!conn.recv_message(type, payload) || type == stream::MessageType::Bye)
                break;
            if (type == stream::MessageType::Input) {
                stream::InputEvent e;
                if (e.decode(payload) && e.kind == stream::InputEvent::Kind::KeyDown)
                    hit.store(true);
            }
        }
        stop.store(true);
    });

    stream::FrameEncoder encoder;
    std::uint64_t seq = 0;
    const auto period = std::chrono::milliseconds(33);
    auto next = std::chrono::steady_clock::now();
    int frame = 0;
    while (!stop.load()) {
        // Auto-break at ~3 s if the client hasn't, so a passive viewer still sees the beat.
        if ((!scene.wall_broken && frame == 90) || hit.exchange(false))
            scene.break_wall();
        app.step(app.fixed_dt());
        const stream::FrameView fv = streamer->capture(app.graph()->physical(scene.last_ldr));
        stream::FrameMessage fm;
        fm.sequence = seq;
        fm.capture_us = platform::Clock::now_ns() / 1000;
        fm.codec = codec;
        fm.desc = {{kWidth, kHeight}, fv.format};
        if (!encoder.encode(codec, fm.desc, fv.pixels, fm.data) || !conn.send_frame(fm))
            break;
        ++seq;
        ++frame;
        next += period;
        std::this_thread::sleep_until(next);
    }
    stop.store(true);
    input_thread.join();
    std::printf("11-lit-rooms server: done (%llu frames).\n", static_cast<unsigned long long>(seq));
    return 0;
}

stream::Codec parse_codec(std::string_view s) {
    if (s == "raw")
        return stream::Codec::Raw;
    if (s == "lz4")
        return stream::Codec::LZ4;
    return stream::Codec::Jpeg;
}

} // namespace

int main(int argc, char** argv) {
    enum class Mode { Headless, Serve } mode = Mode::Headless;
    int converge = 24;
    const char* ppm = nullptr;
    std::string host = "0.0.0.0";
    std::uint16_t port = 9100;
    stream::Codec codec = stream::Codec::Jpeg;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--headless")
            mode = Mode::Headless;
        else if (a == "--serve")
            mode = Mode::Serve;
        else if (a == "--frames" && i + 1 < argc)
            converge = std::atoi(argv[++i]);
        else if (a == "--ppm" && i + 1 < argc)
            ppm = argv[++i];
        else if (a == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--codec" && i + 1 < argc)
            codec = parse_codec(argv[++i]);
    }

    if (mode == Mode::Serve)
        return run_serve(host, port, codec);
    return run_headless(converge, ppm);
}
