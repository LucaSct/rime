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
//           build/dev/bin/lit_rooms --perf [--out r.json] [--baseline b.json] [--width W]
//
// The headless self-check is the CI-gated done-when: on a host with a Vulkan device (lavapipe on
// Linux CI) it renders the full stack, breaks the wall, and asserts the dark room's floor
// brightens; with no device it is an honest skip (exit 0) unless RIME_REQUIRE_VULKAN demands one.
// The rigorous, isolated GI mechanism proofs live in tests/render/gi_thesis_test.cpp; this sample
// is the everything-on-at-once integration and the lived demo.
//
// --perf is the HARDWARE half (m12.0-perf / ADR-0035 §2b), and it is deliberately NOT a CTest
// target. CI renders on lavapipe, a CPU rasterizer, where a millisecond means nothing about a GPU;
// gating absolute time there would be gating the CI machine's mood. So the counts stay in
// --headless where CI can fail on them forever, and the clock lives here, run by hand or by
// scripts/perf.sh on a machine whose fingerprint is written into the report.

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
#include "rime/core/diagnostics/perf_report.hpp"
#include "rime/core/diagnostics/profile.hpp"
#include "rime/core/diagnostics/work_ledger.hpp"
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
// so the wall casts a hard shadow across the floor strip in front of it (toward the camera),
// sealing that strip — "the dark room" — from the direct sun; the floor beyond is sunlit. Break the
// wall and the shadow lifts: direct sun floods the strip, its CSM shadow is gone, the DDGI bounce
// updates, and the reflective floor picks it all up. Unlike gi_thesis_test.cpp's ceiling-sealed
// room (whose relight is a small GI-only rise, deliberately measured in HDR), a lifted SUN shadow
// is a large, plainly-visible LDR change — the right thing for a lived demo. All coordinates in
// metres.
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

// The perf run renders at a real display resolution rather than the 540p the CI proof uses — a
// frame budget is a claim about the resolution it was measured at, which is why the extent is part
// of the report's fingerprint and not a footnote.
app::AppConfig perf_config(std::uint32_t w, std::uint32_t h) {
    app::AppConfig cfg{};
    cfg.gpu = true;
    cfg.render_extent = {w, h};
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

    // ── The WORK LEDGER, static half (m12.0-perf / ADR-0035 §2a) ─────────────────────────────
    // Sampled on the LAST converged frame, so it describes the steady state rather than warmup.
    // These are per-frame counters (each subsystem resets its own at the top of its update), and
    // they are the numbers that make m10.2's and m10.4b's central claim — "a static scene must
    // cost nothing after warmup, and a break must recompose only the region it could have
    // affected" — into something CI can fail on rather than something a comment asserts.
    core::WorkLedger ledger;
    ledger.set("sdf.stamps_static", scene.renderer.sdf_clipmap().stats().stamps);
    ledger.set("sdf.dirty_regions_static", scene.renderer.sdf_clipmap().stats().dirty_regions);
    ledger.set("shadow.rendered_static", scene.renderer.local_shadow_stats().rendered);
    ledger.set("shadow.reused_static", scene.renderer.local_shadow_stats().reused);
    ledger.set("ddgi.probes_updated_static", scene.renderer.ddgi_stats().probes_updated);
    const std::vector<std::uint8_t> before =
        read_rgba8(*app.device(), app.graph()->physical(scene.last_ldr), kWidth, kHeight);
    const double dark_before = region_luminance(before, kDarkX0, kDarkY0, kDarkX1, kDarkY1);
    const bool lit = scene_is_lit(before);
    if (ppm)
        write_ppm(ppm, before);

    // Break the divider and let the fast-tracked probes + shadow cache catch up.
    scene.break_wall();
    // Peaks across the post-break frames rather than a guess at WHICH frame the invalidation
    // lands on: the C1/C2 seams queue work that the next render drains, and pinning the sample to
    // frame N+1 would make the proof depend on an ordering nobody promised.
    std::uint32_t sdf_stamps_break = 0, shadow_rendered_break = 0, ddgi_fast_break = 0;
    for (int i = 0; i < converge; ++i) {
        app.step(app.fixed_dt());
        sdf_stamps_break = std::max(sdf_stamps_break, scene.renderer.sdf_clipmap().stats().stamps);
        shadow_rendered_break =
            std::max(shadow_rendered_break, scene.renderer.local_shadow_stats().rendered);
        ddgi_fast_break = std::max(ddgi_fast_break, scene.renderer.ddgi_stats().fast_tracked);
    }
    ledger.set("sdf.stamps_on_break", sdf_stamps_break);
    ledger.set("shadow.rendered_on_break", shadow_rendered_break);
    ledger.set("ddgi.fast_tracked_on_break", ddgi_fast_break);
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
    std::printf("  work ledger: %s\n", ledger.to_json(-1).c_str());

    // The budget. These are the M10 caching claims turned into a CI gate, and they come in
    // STATIC/BREAK PAIRS on purpose: "a static frame re-stamps nothing" is worth nothing on its
    // own, because a clipmap that had silently stopped working would also re-stamp nothing. The
    // paired floor is what proves the zero was a decision rather than a corpse — the same reason
    // m11.7 pairs every "it agreed" with a negative control that disagrees.
    //
    // Floors and ceilings with margin, never the measured value pinned exactly: these are counts
    // of CPU-side decisions, so they are device-independent (lavapipe in CI reads the same numbers
    // this RTX 3060 does), but a proof that demands equality would break on any legitimate content
    // tweak and teach everyone to edit the expectation instead of reading it.
    core::WorkBudget budget;
    budget
        // m10.4b: a static scene costs nothing after warmup…
        .at_most("sdf.stamps_static", 0)
        .at_most("sdf.dirty_regions_static", 0)
        // …and the break recomposes, so that zero is a live decision, not a dead subsystem.
        .at_least("sdf.stamps_on_break", 1)
        // m10.2: the shadow cache serves a static frame from cache…
        .at_most("shadow.rendered_static", 0)
        .at_least("shadow.reused_static", 1)
        // …and a destruction event invalidates the slot it touched.
        .at_least("shadow.rendered_on_break", 1)
        // m10.5: probes keep round-robining, and destruction fast-tracks the ones it touched.
        .at_least("ddgi.probes_updated_static", 1)
        .at_least("ddgi.fast_tracked_on_break", 1);

    const auto violations = budget.check(ledger);
    if (!violations.empty()) {
        std::fprintf(
            stderr, "  BUDGET VIOLATIONS:\n%s", core::WorkBudget::format(violations).c_str());
    }

    // The done-when: the full stack renders lit (all six gates compose without conflict), the dark
    // room's floor brightens materially once the divider falls (the integrated thesis — direct sun
    // through the gap, the CSM shadow lifting, and the DDGI bounce arriving), and the break visibly
    // repaints the frame.
    const bool thesis = dark_after > dark_before * 1.4 && (dark_after - dark_before) > 4.0;
    const bool ok = lit && thesis && changed > 2000 && violations.empty();
    std::printf("11-lit-rooms: %s\n",
                ok ? "the wall falls, the dark room lights up — M10 green!" : "FAILED self-check");
    return ok ? 0 : 1;
}

// ── --perf: the hardware report (m12.0-perf / ADR-0035 §2b) ─────────────────────────────────────
struct PerfOptions {
    int warmup = 60;      // unmeasured: DDGI convergence is not the steady state we are gating
    int frames = 600;     // 10 s at 60 Hz — enough that p99 is the 594th frame, not max
    int break_frame = 300;// the wall falls mid-run, so the collapse sits inside the sample
    int collapse = 60;    // frames after the break that count as "the collapse window"
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    const char* out = nullptr;
    const char* baseline = nullptr;
};

int run_perf(const PerfOptions& opt) {
    app::Application app(perf_config(opt.width, opt.height));
    if (!app.device()) {
        std::fprintf(stderr, "11-lit-rooms --perf: no Vulkan device — a perf run needs real "
                             "hardware, and there is nothing here to measure\n");
        return 1;
    }
    LitRoomsApp scene(app);
    (void)spawn_camera(app);

    core::PerfReport report;
    core::MachineFingerprint fp = core::MachineFingerprint::detect();
    const rhi::AdapterInfo& adapter = app.device()->adapter();
    fp.gpu = adapter.name;
    // Both halves of the driver identity: "NVIDIA 610.43.03" rather than either alone, because a
    // Mesa version string and an NVIDIA one are not comparable without knowing which is which.
    fp.driver = adapter.driver_name + " " + adapter.driver_info;
    fp.preset = "all-lighting-gates"; // csm + local shadows + clustered + sdf + ddgi + ssr
    fp.width = opt.width;
    fp.height = opt.height;
    report.set_machine(fp);
    report.set_run(core::RunInfo::detect("11-lit-rooms"));

    // Per-pass GPU cost arrives through the one window in which it is readable: after the frame's
    // submission has completed, before the graph resets (app::Application::on_post_submit).
    std::vector<core::PassTiming> passes;
    bool timestamps_seen = false;
    app.on_post_submit([&](render::RenderGraph& graph, rhi::CommandBuffer& cmd) {
        passes.clear();
        for (const render::RenderGraph::PassTiming& t : graph.resolve_timings(cmd)) {
            passes.push_back(core::PassTiming{std::string(t.name), t.gpu_ms});
            timestamps_seen = true;
        }
    });

    std::printf("11-lit-rooms --perf: %s (%s %s), %ux%u, warmup %d, measuring %d frames…\n",
                fp.gpu.c_str(),
                adapter.driver_name.c_str(),
                adapter.driver_info.c_str(),
                opt.width,
                opt.height,
                opt.warmup,
                opt.frames);

    for (int i = 0; i < opt.warmup; ++i)
        app.step(app.fixed_dt());
    const std::uint32_t probes_static = scene.renderer.ddgi_stats().probes_updated;

    // From here the profile zones feed the report, so `sim.*` and `frame.*` land as their own
    // timelines — the per-stage CPU breakdown, with no per-sample plumbing.
    core::ZoneTimelines zones(report);
    std::uint32_t sdf_break = 0, shadow_break = 0, ddgi_fast_break = 0;
    for (int i = 0; i < opt.frames; ++i) {
        if (i == opt.break_frame)
            scene.break_wall();
        const core::Stopwatch watch;
        app.step(app.fixed_dt());
        const double ms = watch.elapsed_ms();
        report.observe_frame(static_cast<std::uint64_t>(i), ms, passes);
        if (i >= opt.break_frame && i < opt.break_frame + opt.collapse) {
            // The same frames again, on their own timeline. A collapse that hitches is invisible
            // in a 600-frame p99 (60 frames cannot move the 594th) and obvious in a 60-frame one —
            // which is the entire reason ADR-0035 asks for the window separately.
            report.observe("frame.collapse", ms);
            sdf_break = std::max(sdf_break, scene.renderer.sdf_clipmap().stats().stamps);
            shadow_break = std::max(shadow_break, scene.renderer.local_shadow_stats().rendered);
            ddgi_fast_break =
                std::max(ddgi_fast_break, scene.renderer.ddgi_stats().fast_tracked);
        }
    }
    zones.stop();

    // The ledger travels WITH the timings, so the report can never be read as "fast" without also
    // being read as "…and here is the work it did". A run that was quick because the lighting
    // silently switched itself off is not a pass (ADR-0035 §2b's vacuity guard).
    core::WorkLedger ledger;
    ledger.set("frames.measured", static_cast<std::uint64_t>(opt.frames));
    ledger.set("ddgi.probes_updated_static", probes_static);
    ledger.set("sdf.stamps_on_break", sdf_break);
    ledger.set("shadow.rendered_on_break", shadow_break);
    ledger.set("ddgi.fast_tracked_on_break", ddgi_fast_break);
    report.set_ledger(ledger);

    // The gate. Absolute ceilings are ADR-0035's proposed headline budget; the sample floors are
    // what stop a short or empty run from passing; the work budget is the vacuity guard.
    core::PerfGate gate;
    gate.at_most("frame", core::PerfStat::P99, 16.6)
        .at_most("frame", core::PerfStat::Max, 33.0)
        .at_most("frame.collapse", core::PerfStat::Max, 33.0)
        .require_samples("frame", 200)
        .require_samples("frame.collapse", 30)
        .max_regression(0.10);
    gate.work()
        .at_least("ddgi.probes_updated_static", 1) // the probe field is alive…
        .at_least("sdf.stamps_on_break", 1)        // …and the wall really came down
        .at_least("shadow.rendered_on_break", 1);
    // (draws.submitted / draws.culled join this budget at m12.7, when a frustum cull exists to
    // count — today nothing in the engine counts a draw, so claiming one here would be a lie.)

    core::PerfReport baseline;
    const core::PerfReport* baseline_ptr = nullptr;
    if (opt.baseline) {
        std::string error;
        if (core::PerfReport::load_file(opt.baseline, baseline, error)) {
            baseline_ptr = &baseline;
        } else {
            // Not fatal, and not silent: a missing baseline means this run establishes one.
            std::printf("  (no usable baseline: %s)\n", error.c_str());
        }
    }
    const core::PerfGate::Result result = gate.check(report, baseline_ptr);

    const auto frame = report.distribution("frame");
    const auto collapse = report.distribution("frame.collapse");
    const auto tick = report.distribution("sim.tick");
    if (!frame) {
        std::fprintf(stderr, "11-lit-rooms --perf: no frames were measured (--frames %d)\n",
                     opt.frames);
        return 1;
    }
    std::printf("  frame     p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  (%zu frames)\n",
                frame->p50_ms,
                frame->p95_ms,
                frame->p99_ms,
                frame->max_ms,
                frame->count);
    if (collapse) {
        std::printf("  collapse  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  (%zu frames)\n",
                    collapse->p50_ms,
                    collapse->p95_ms,
                    collapse->p99_ms,
                    collapse->max_ms,
                    collapse->count);
    }
    if (tick) {
        std::printf("  sim.tick  p50 %.3f  p99 %.3f  max %.3f ms\n",
                    tick->p50_ms,
                    tick->p99_ms,
                    tick->max_ms);
    }
    std::printf("  worst frame #%llu at %.2f ms\n",
                static_cast<unsigned long long>(report.worst_frame().index),
                report.worst_frame().ms);
    if (!timestamps_seen) {
        std::printf("  (this device reports no GPU timestamps — the per-pass table is empty)\n");
    }
    std::printf("  work ledger: %s\n", ledger.to_json(-1).c_str());

    if (opt.out) {
        FILE* f = std::fopen(opt.out, "wb");
        if (!f) {
            std::fprintf(stderr, "  could not write %s\n", opt.out);
            return 1;
        }
        const std::string json = report.to_json();
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        std::printf("  wrote %s\n", opt.out);
    }

    std::fflush(stdout); // so the gate's stderr lines land after the numbers they judge
    if (!result.ok()) {
        std::fprintf(stderr, "  PERF GATE:\n%s", core::PerfGate::format(result).c_str());
    } else {
        std::printf("  perf gate: %s", core::PerfGate::format(result).c_str());
    }
    std::printf("11-lit-rooms --perf: %s\n", result.ok() ? "within budget" : "FAILED the perf gate");
    return result.ok() ? 0 : 1;
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
    enum class Mode { Headless, Serve, Perf } mode = Mode::Headless;
    int converge = 24;
    const char* ppm = nullptr;
    std::string host = "0.0.0.0";
    std::uint16_t port = 9100;
    stream::Codec codec = stream::Codec::Jpeg;
    PerfOptions perf;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a(argv[i]);
        if (a == "--headless")
            mode = Mode::Headless;
        else if (a == "--serve")
            mode = Mode::Serve;
        else if (a == "--perf")
            mode = Mode::Perf;
        else if (a == "--frames" && i + 1 < argc) {
            converge = std::atoi(argv[++i]);
            perf.frames = converge;
            perf.break_frame = converge / 2;
        } else if (a == "--warmup" && i + 1 < argc)
            perf.warmup = std::atoi(argv[++i]);
        else if (a == "--width" && i + 1 < argc)
            perf.width = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--height" && i + 1 < argc)
            perf.height = static_cast<std::uint32_t>(std::atoi(argv[++i]));
        else if (a == "--out" && i + 1 < argc)
            perf.out = argv[++i];
        else if (a == "--baseline" && i + 1 < argc)
            perf.baseline = argv[++i];
        else if (a == "--ppm" && i + 1 < argc)
            ppm = argv[++i];
        else if (a == "--host" && i + 1 < argc)
            host = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        else if (a == "--codec" && i + 1 < argc)
            codec = parse_codec(argv[++i]);
    }

    // Keep the scripted break inside a shortened run (see the same clamp in 10-destructible-wall):
    // a smoke run whose break never fires would measure a wall that is still standing.
    if (mode == Mode::Perf) {
        if (perf.break_frame >= perf.frames)
            perf.break_frame = perf.frames / 2;
        perf.collapse = std::min(perf.collapse, perf.frames - perf.break_frame);
    }

    if (mode == Mode::Serve)
        return run_serve(host, port, codec);
    if (mode == Mode::Perf)
        return run_perf(perf);
    return run_headless(converge, ppm);
}
