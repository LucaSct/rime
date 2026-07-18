// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for the editor gizmo renderer (m9.6 Part B). Structural, never golden (the M5.6 rule):
// the scene is planted, expected pixels are COMPUTED by projecting through the very CameraLens the
// engine renders with, and colours are asserted with wide margins in small search windows (a
// 1-px line must land within rounding of its projected position, not on an exact texel).
//
//   (1) GPU-free: compute_camera_lens agrees with the renderer's formula and ships a true inverse
//       (project → unproject round-trips), and a camera-less world yields no lens.
//   (2) On lavapipe: a lit cube + a translate gizmo —
//         a. the overlay changed the frame (baseline vs gizmo bytes differ);
//         b. the +X shaft shows RED pixels at computed points, +Y GREEN, and the highlighted axis
//            turns YELLOW (GizmoState.axis at work) while the others keep their colours;
//         c. the selected mesh is TINTED: the cube's blue channel rises against the baseline at a
//            computed on-mesh, off-shaft pixel;
//         d. mode None, a dead entity, or a camera-less lens render NO overlay (bytes equal the
//            baseline) — the "hide the gizmo" contract.
//         e. rotate/scale modes draw their handle geometry (ring / cube-end) at computed pixels.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/gizmo_renderer.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

namespace {

using namespace rime;
using namespace rime::render;

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

constexpr std::uint32_t kSize = 512;
constexpr rhi::Extent2D kExtent{kSize, kSize};

// Project a world point through the lens to pixel coordinates (the engine's y-down NDC is baked
// into perspective, so no flips — the pick_test pattern).
struct PixelI {
    std::int32_t x = 0;
    std::int32_t y = 0;
};

PixelI project_px(const core::Mat4& view_proj, core::Vec3 world) {
    const core::Vec4 clip = view_proj * core::Vec4{world.x, world.y, world.z, 1.0f};
    return {
        static_cast<std::int32_t>((clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(kSize)),
        static_cast<std::int32_t>((clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(kSize))};
}

// RGBA8 access into a read-back LDR frame.
struct Rgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
};

Rgba at(const std::vector<std::uint8_t>& px, std::int32_t x, std::int32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
    return {px[i], px[i + 1], px[i + 2], px[i + 3]};
}

// True if any pixel in the (2r+1)² window around (x, y) satisfies `pred` — the rounding-proof way
// to assert "the 1-px line passes here".
template <class Pred>
bool window_any(const std::vector<std::uint8_t>& px, PixelI p, std::int32_t r, Pred pred) {
    for (std::int32_t dy = -r; dy <= r; ++dy) {
        for (std::int32_t dx = -r; dx <= r; ++dx) {
            const std::int32_t x = p.x + dx;
            const std::int32_t y = p.y + dy;
            if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(kSize) ||
                y >= static_cast<std::int32_t>(kSize)) {
                continue;
            }
            if (pred(at(px, x, y))) {
                return true;
            }
        }
    }
    return false;
}

// The colour classifiers, with margins wide enough to shrug off blending against the scene and
// tonemapping, yet strict enough that the axes are mutually exclusive.
bool reddish(Rgba c) {
    return c.r > 150 && c.r > c.g + 60 && c.r > c.b + 60;
}

bool greenish(Rgba c) {
    return c.g > 140 && c.g > c.r + 50 && c.g > c.b + 50;
}

bool yellowish(Rgba c) {
    return c.r > 160 && c.g > 130 && static_cast<int>(c.b) < static_cast<int>(c.g) - 40;
}

// Read a rendered texture back to CPU bytes (the tests/rhi pattern: copy → host buffer → read).
std::vector<std::uint8_t> read_texture(rhi::Device& device, rhi::TextureHandle texture) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(kSize) * kSize * 4;
    rhi::BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = rhi::BufferUsage::TransferDst;
    rbd.memory = rhi::MemoryUsage::GpuToCpu;
    rbd.debug_name = "gizmo-readback";
    const rhi::BufferHandle rb = device.create_buffer(rbd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(texture, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

} // namespace

TEST_CASE("gizmo: compute_camera_lens matches the render formula and inverts (m9.6b)") {
    // GPU-free: the lens is pure extraction + math. Camera at (2, 1.5, 8), identity rotation.
    using ecs::WorldTransform;
    ecs::World world;
    register_render_components(world);
    core::Transform cam_tf{};
    cam_tf.translation = {2.0f, 1.5f, 8.0f};
    (void)world.spawn_with(WorldTransform{cam_tf}, Camera{});

    const CameraLens lens = compute_camera_lens(world, kExtent);
    REQUIRE(lens.found);
    CHECK(lens.eye.x == 2.0f);
    CHECK(lens.eye.y == 1.5f);
    CHECK(lens.eye.z == 8.0f);
    CHECK(lens.fov_y == doctest::Approx(0.87266f));

    // The lens equals the SceneRenderer/ScenePicker formula: perspective * inverse(camera world).
    const core::Mat4 expect =
        core::perspective(0.87266f, 1.0f, 0.1f, 1000.0f) * core::inverse(core::to_matrix(cam_tf));
    CHECK(core::approx_eq(lens.view_proj, expect, 1e-4f));

    // inv_view_proj is a genuine inverse: vp * inv ≈ identity — what the editor's pixel→ray
    // unprojection trusts blindly.
    CHECK(core::approx_eq(lens.view_proj * lens.inv_view_proj, core::Mat4{}, 1e-4f));

    // No active camera ⇒ no lens (and the gizmo renderer declares nothing).
    ecs::World empty;
    register_render_components(empty);
    CHECK_FALSE(compute_camera_lens(empty, kExtent).found);
    CHECK_FALSE(compute_camera_lens(world, rhi::Extent2D{0, 0}).found);
}

TEST_CASE("gizmo: handles, highlight, and tint render at computed pixels (m9.6b, lavapipe)") {
    using ecs::WorldTransform;

    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping gizmo render proofs");
        return;
    }

    MeshRegistry meshes(*device);
    const MeshId cube = meshes.add(make_cube(1.0f), "gizmo-cube");
    REQUIRE(cube != kInvalidMeshId);
    MaterialRegistry materials;
    // A warm, low-blue material so the blue-ish selection tint moves the blue channel decisively.
    PbrMaterialDesc mat{};
    mat.base_color[0] = 0.8f;
    mat.base_color[1] = 0.35f;
    mat.base_color[2] = 0.08f;
    mat.metallic = 0.0f;
    mat.roughness = 0.8f;
    const MaterialId orange = materials.add(mat);

    // The planted scene: a cube at the origin, a light, and a camera OFF the cube's axis (at
    // (2, 1.5, 8), looking down −z) — so the +Z handle has a visible on-screen direction too.
    ecs::World world;
    register_render_components(world);
    const auto at_tf = [](float x, float y, float z) {
        core::Transform tf{};
        tf.translation = {x, y, z};
        return WorldTransform{tf};
    };
    const ecs::Entity cube_e = world.spawn_with(at_tf(0, 0, 0), MeshRef{cube}, MaterialRef{orange});
    (void)world.spawn_with(at_tf(3, 4, 6), PointLight{1.0f, 0.95f, 0.9f, 300.0f, 50.0f});
    (void)world.spawn_with(at_tf(2.0f, 1.5f, 8.0f), Camera{});

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.06f, 0.06f, 0.07f);
    GizmoRenderer gizmos(*device, meshes);
    RenderGraph graph(*device);

    const CameraLens lens = compute_camera_lens(world, kExtent);
    REQUIRE(lens.found);

    // Render one frame with `sel` composited and read the LDR back.
    const auto render_with = [&](const GizmoSelection& sel) {
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, kExtent, true);
        gizmos.declare(graph, world, out.ldr, lens, sel);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return read_texture(*device, graph.physical(out.ldr));
    };

    // The gizmo's world size, exactly as the renderer computes it (screen-constant scaling).
    const core::Vec3 center{0, 0, 0};
    const float dist = core::length(center - lens.eye);
    const float s = dist * std::tan(lens.fov_y * 0.5f) * kGizmoScreenFraction;

    const auto baseline = render_with({}); // mode None ⇒ no overlay

    // ── Translate: axis shafts at their projected pixels, in their colours ────────────────
    GizmoSelection sel{};
    sel.entity = cube_e;
    sel.mode = GizmoMode::Translate;
    const auto translate = render_with(sel);

    CHECK(translate != baseline); // the overlay drew SOMETHING

    // Sample mid-shaft points (past the cube's silhouette not required — always-on-top draws over
    // the mesh; the windows are ±2 px around computed positions).
    for (const float t : {0.55f, 0.75f}) {
        const PixelI px_x = project_px(lens.view_proj, center + core::Vec3{s * t, 0, 0});
        CHECK_MESSAGE(window_any(translate, px_x, 2, reddish),
                      "expected a red +X shaft pixel near (" << px_x.x << "," << px_x.y << ")");
        const PixelI px_y = project_px(lens.view_proj, center + core::Vec3{0, s * t, 0});
        CHECK_MESSAGE(window_any(translate, px_y, 2, greenish),
                      "expected a green +Y shaft pixel near (" << px_y.x << "," << px_y.y << ")");
    }

    // ── The wire's highlight axis turns yellow; the others keep their colours ─────────────
    sel.axis = GizmoAxis::X;
    const auto highlighted = render_with(sel);
    const PixelI hx = project_px(lens.view_proj, center + core::Vec3{s * 0.65f, 0, 0});
    CHECK(window_any(highlighted, hx, 2, yellowish));
    CHECK_FALSE(window_any(highlighted, hx, 2, reddish)); // it really changed, not just added
    const PixelI hy = project_px(lens.view_proj, center + core::Vec3{0, s * 0.65f, 0});
    CHECK(window_any(highlighted, hy, 2, greenish)); // Y unaffected by X's highlight
    sel.axis = GizmoAxis::None;

    // ── Tint: the selected mesh's blue channel rises against the baseline ─────────────────
    // Sampled ON the cube's front face (z = +1) but AWAY from every shaft (y < 0 keeps clear of
    // +X/+Z; x > 0 of +Y) — computed, not guessed.
    const PixelI on_mesh = project_px(lens.view_proj, {0.45f, -0.5f, 1.0f});
    const Rgba base_c = at(baseline, on_mesh.x, on_mesh.y);
    const Rgba tint_c = at(translate, on_mesh.x, on_mesh.y);
    CHECK_MESSAGE(static_cast<int>(tint_c.b) > static_cast<int>(base_c.b) + 25,
                  "expected the selection tint to lift blue at ("
                      << on_mesh.x << "," << on_mesh.y << "): " << static_cast<int>(base_c.b)
                      << " -> " << static_cast<int>(tint_c.b));

    // ── Nothing draws for mode None or a dead entity (bytes equal the baseline) ───────────
    CHECK(render_with({}) == baseline);
    GizmoSelection dead = sel;
    world.despawn(cube_e);
    const auto after_despawn_baseline = render_with({});
    CHECK(render_with(dead) == after_despawn_baseline);
    // (Respawn the scene state for the remaining checks.)
    const ecs::Entity cube2 = world.spawn_with(at_tf(0, 0, 0), MeshRef{cube}, MaterialRef{orange});

    // ── Rotate and scale modes draw their handle geometry ─────────────────────────────────
    GizmoSelection rot{};
    rot.entity = cube2;
    rot.mode = GizmoMode::Rotate;
    const auto rotate = render_with(rot);
    // The X ring lies in the YZ plane: its point at angle 0 is center + (0, s, 0)… parameterised
    // by perp1/perp2 = Y/Z. Sample two ring points; red pixels must appear there.
    const PixelI ring_a = project_px(lens.view_proj, center + core::Vec3{0, s, 0});
    const PixelI ring_b = project_px(lens.view_proj, center + core::Vec3{0, 0, s});
    CHECK(window_any(rotate, ring_a, 2, reddish));
    CHECK(window_any(rotate, ring_b, 2, reddish));

    GizmoSelection scl{};
    scl.entity = cube2;
    scl.mode = GizmoMode::Scale;
    const auto scale = render_with(scl);
    // The scale shaft ends in a cube at 0.9·s along the axis.
    const PixelI cube_x = project_px(lens.view_proj, center + core::Vec3{s * 0.9f, 0, 0});
    const PixelI cube_y = project_px(lens.view_proj, center + core::Vec3{0, s * 0.9f, 0});
    CHECK(window_any(scale, cube_x, 3, reddish));
    CHECK(window_any(scale, cube_y, 3, greenish));
}
