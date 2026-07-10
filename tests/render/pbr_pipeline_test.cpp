// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// Proofs for the forward-PBR brick (M5.6). Golden images are useless across drivers, so every
// GPU assertion here is STRUCTURAL — a property the physics guarantees, checked with margins:
//
//  (1) extraction pins the scene conventions (GPU-free): first-active camera, the −z light/camera
//      orientation rule, draw filtering, radiance = color × intensity folding.
//  (2) On a metallic×roughness sphere grid under one point light:
//        a. the limb facing the light is much brighter than the limb facing away;
//        b. the specular highlight TIGHTENS as roughness falls — smoother sphere: higher peak
//           radiance, fewer pixels above half its own peak;
//        c. no HDR pixel exceeds an analytic energy bound derived from the BRDF (and none is
//           NaN/Inf) — the "did someone drop a /π or double a light" tripwire;
//        d. the roughness-1 dielectric sphere obeys a TIGHT bound (its BRDF is nearly flat, so
//           its ceiling is computable to ~25% headroom — this one catches real energy bugs).
//  (3) Depth pre-pass ON vs OFF produces byte-identical LDR images (the invariant-gl_Position
//      contract between depth_only.vert and pbr_forward.vert, and CompareOp::Equal, hold).
//  (4) A base-color texture actually reaches the shading: a red/green checker floor shows both
//      dominant colors (sRGB decode + uv interpolation + the binding-2 path).
//
// GPU-free on lavapipe like every proof since M3.3. The scene numbers (positions, intensity,
// bounds) are derived in comments where they are used — change one, re-derive the rest.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/core/math/transform.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/render_graph.hpp"
#include "rime/render/scene_renderer.hpp"

namespace {

using namespace rime;
using namespace rime::render;

bool vulkan_required() {
    return std::getenv("RIME_REQUIRE_VULKAN") != nullptr;
}

// IEEE 754 half → float, the textbook bit dance (sign, rebiased exponent, normalized mantissa).
// The HDR target is RGBA16Float; the CPU has to decode it to assert on radiance.
float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = static_cast<std::uint32_t>(h & 0x8000u) << 16;
    std::uint32_t exp = (h >> 10) & 0x1Fu;
    std::uint32_t mant = h & 0x3FFu;
    std::uint32_t bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign; // ±0
        } else {
            // Subnormal half: renormalize into a normal float.
            exp = 127 - 15 + 1;
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13); // ±inf / NaN
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Read a rendered texture back to CPU bytes (the tests/rhi pattern: copy → host buffer → read).
std::vector<std::uint8_t> read_texture(rhi::Device& device,
                                       rhi::TextureHandle texture,
                                       std::uint32_t width,
                                       std::uint32_t height,
                                       std::uint32_t bytes_per_pixel) {
    const std::uint64_t bytes = static_cast<std::uint64_t>(width) * height * bytes_per_pixel;
    rhi::BufferDesc rbd{};
    rbd.size = bytes;
    rbd.usage = rhi::BufferUsage::TransferDst;
    rbd.memory = rhi::MemoryUsage::GpuToCpu;
    rbd.debug_name = "pbr-readback";
    const rhi::BufferHandle rb = device.create_buffer(rbd);
    auto cmd = device.begin_commands();
    cmd->copy_texture_to_buffer(texture, rb);
    device.submit_blocking(*cmd);
    std::vector<std::uint8_t> out(bytes);
    device.read_buffer(rb, out.data(), out.size(), 0);
    device.destroy(rb);
    return out;
}

// A decoded HDR image: linear radiance per channel.
struct HdrImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgb; // 3 floats per pixel

    [[nodiscard]] float luminance(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 3;
        // Rec.709 luma weights on linear values — "how bright" in one number.
        return 0.2126f * rgb[i] + 0.7152f * rgb[i + 1] + 0.0722f * rgb[i + 2];
    }
};

HdrImage
decode_hdr(const std::vector<std::uint8_t>& bytes, std::uint32_t width, std::uint32_t height) {
    HdrImage img;
    img.width = width;
    img.height = height;
    img.rgb.resize(static_cast<std::size_t>(width) * height * 3);
    const auto* half = reinterpret_cast<const std::uint16_t*>(bytes.data());
    for (std::size_t p = 0; p < static_cast<std::size_t>(width) * height; ++p) {
        img.rgb[p * 3 + 0] = half_to_float(half[p * 4 + 0]);
        img.rgb[p * 3 + 1] = half_to_float(half[p * 4 + 1]);
        img.rgb[p * 3 + 2] = half_to_float(half[p * 4 + 2]);
    }
    return img;
}

// Project a world point to pixel coordinates with the SAME math the renderer uses (perspective
// already bakes Vulkan's y-down NDC, so world +y lands at a LOW row index; no flips here).
struct Pixel {
    float x = 0.0f;
    float y = 0.0f;
};

Pixel project(const core::Mat4& view_proj, core::Vec3 world, std::uint32_t size) {
    const core::Vec4 clip = view_proj * core::Vec4{world.x, world.y, world.z, 1.0f};
    return {(clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(size),
            (clip.y / clip.w * 0.5f + 0.5f) * static_cast<float>(size)};
}

} // namespace

TEST_CASE("pbr: extraction pins the scene conventions (M5.6)") {
    using ecs::WorldTransform;
    ecs::World world;
    register_render_components(world);

    // Two cameras: the inactive one must lose, regardless of spawn order.
    core::Transform cam_tf{};
    cam_tf.translation = {1.0f, 2.0f, 8.0f};
    (void)world.spawn_with(WorldTransform{cam_tf}, Camera{0.9f, 0.5f, 200.0f, true});
    (void)world.spawn_with(WorldTransform{}, Camera{1.2f, 0.1f, 10.0f, false});

    // A directional light rotated −90° about +x: its local −z swings to... the test asserts it.
    core::Transform light_tf{};
    light_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -core::kHalfPi);
    (void)world.spawn_with(WorldTransform{light_tf}, DirectionalLight{1.0f, 0.5f, 0.25f, 2.0f});

    core::Transform point_tf{};
    point_tf.translation = {1.0f, 2.0f, 3.0f};
    (void)world.spawn_with(WorldTransform{point_tf}, PointLight{1.0f, 1.0f, 1.0f, 3.0f, 7.0f});

    // Two drawable entities and one with an invalid mesh id (must be filtered).
    core::Transform draw_tf{};
    draw_tf.translation = {5.0f, 0.0f, 0.0f};
    (void)world.spawn_with(WorldTransform{draw_tf}, MeshRef{0}, MaterialRef{0});
    (void)world.spawn_with(WorldTransform{}, MeshRef{1}, MaterialRef{1});
    (void)world.spawn_with(WorldTransform{}, MeshRef{kInvalidMeshId}, MaterialRef{0});

    const ExtractedScene scene = extract_scene(world);

    REQUIRE(scene.camera.found);
    CHECK(scene.camera.fov_y == doctest::Approx(0.9f)); // the ACTIVE camera's lens
    CHECK(scene.camera.position[2] == doctest::Approx(8.0f));
    // view = inverse(camera world matrix): the camera's own position must map to the origin.
    const core::Vec3 eye_in_view = core::transform_point(scene.camera.view, {1.0f, 2.0f, 8.0f});
    CHECK(std::fabs(eye_in_view.x) < 1e-5f);
    CHECK(std::fabs(eye_in_view.y) < 1e-5f);
    CHECK(std::fabs(eye_in_view.z) < 1e-5f);

    REQUIRE(scene.dir_lights.size() == 1);
    // Rotating (0,0,−1) by −90° about +x gives (0,−1,0): the light now shines straight DOWN —
    // aim a light exactly like you'd aim a camera.
    CHECK(scene.dir_lights[0].direction[0] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(scene.dir_lights[0].direction[1] == doctest::Approx(-1.0f).epsilon(1e-4));
    CHECK(scene.dir_lights[0].direction[2] == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(scene.dir_lights[0].radiance[1] == doctest::Approx(1.0f)); // 0.5 × intensity 2

    REQUIRE(scene.point_lights.size() == 1);
    CHECK(scene.point_lights[0].position[1] == doctest::Approx(2.0f));
    CHECK(scene.point_lights[0].position[3] == doctest::Approx(7.0f)); // w carries the radius
    CHECK(scene.point_lights[0].radiance[0] == doctest::Approx(3.0f)); // 1.0 × intensity 3

    REQUIRE(scene.draws.size() == 2); // the invalid-mesh entity was filtered
    const bool has_translated_draw = std::fabs(scene.draws[0].model.at(0, 3) - 5.0f) < 1e-5f ||
                                     std::fabs(scene.draws[1].model.at(0, 3) - 5.0f) < 1e-5f;
    CHECK(has_translated_draw);
}

TEST_CASE("pbr: sphere-grid structural proofs (M5.6)") {
    using ecs::WorldTransform;

    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping PBR render proofs");
        return;
    }
    constexpr std::uint32_t kSize = 256;

    // ── The scene ─────────────────────────────────────────────────────────────────────────
    // Six unit spheres, two rows of three. Top row (world y = +1.2) metals, bottom row
    // dielectrics; roughness rises left → right. One white point light up-right-front at
    // (6, 5, 2) — far enough right that each sphere's −x limb faces AWAY from it (that is what
    // makes the lit/unlit test meaningful; a frontal light would graze-light everything).
    MeshRegistry meshes(*device);
    const MeshId sphere = meshes.add(make_uv_sphere(1.0f, 32, 64), "proof-sphere");
    REQUIRE(sphere != kInvalidMeshId);

    constexpr float kMetalRough[3] = {0.15f, 0.45f, 0.85f};
    constexpr float kDielecRough[3] = {0.3f, 0.65f, 1.0f};
    constexpr float kX[3] = {-2.2f, 0.0f, 2.2f};
    constexpr float kLightPos[3] = {6.0f, 5.0f, 2.0f};
    constexpr float kIntensity = 60.0f;
    constexpr float kAmbient = 0.02f;

    MaterialRegistry materials;
    ecs::World world;
    register_render_components(world);
    for (int i = 0; i < 3; ++i) {
        // No base-color texture: the untextured path, so the renderer's own 1x1 white fallback
        // is what these draws sample (a separate proof, "a base-color texture reaches the
        // shading", covers the textured path).
        const MaterialId metal = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, kMetalRough[i]});
        core::Transform tf{};
        tf.translation = {kX[i], 1.2f, 0.0f};
        (void)world.spawn_with(WorldTransform{tf}, MeshRef{sphere}, MaterialRef{metal});

        const MaterialId dielec = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, kDielecRough[i]});
        tf.translation = {kX[i], -1.2f, 0.0f};
        (void)world.spawn_with(WorldTransform{tf}, MeshRef{sphere}, MaterialRef{dielec});
    }

    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 0.0f, 8.0f}; // identity rotation looks down −z, at the grid
    (void)world.spawn_with(WorldTransform{cam_tf}, Camera{});

    core::Transform light_tf{};
    light_tf.translation = {kLightPos[0], kLightPos[1], kLightPos[2]};
    (void)world.spawn_with(WorldTransform{light_tf},
                           PointLight{1.0f, 1.0f, 1.0f, kIntensity, 30.0f});

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(kAmbient, kAmbient, kAmbient);

    // ── Frame 1: with the depth pre-pass ──────────────────────────────────────────────────
    RenderGraph graph(*device);
    graph.reset();
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
    REQUIRE(out.ldr.is_valid());
    graph.export_texture(out.hdr); // the test wants the raw radiance too

    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);

    // The graph really composed the three-pass frame, in dependency order.
    {
        const auto order = graph.execution_order();
        REQUIRE(order.size() == 3);
        CHECK(graph.pass_name(order[0]) == "depth-prepass");
        CHECK(graph.pass_name(order[1]) == "forward-pbr");
        CHECK(graph.pass_name(order[2]) == "tonemap");
    }

    const HdrImage hdr =
        decode_hdr(read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
    const std::vector<std::uint8_t> ldr_prepass =
        read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);

    if (const char* dump = std::getenv("RIME_PBR_DUMP")) { // debugging aid: write the LDR as PPM
        FILE* f = std::fopen(dump, "wb");
        if (f) {
            std::fprintf(f, "P6\n%u %u\n255\n", kSize, kSize);
            for (std::size_t p = 0; p < ldr_prepass.size(); p += 4)
                std::fwrite(&ldr_prepass[p], 1, 3, f);
            std::fclose(f);
        }
    }

    // The camera math, replicated exactly (perspective already carries Vulkan's y-down NDC).
    const core::Mat4 view_proj =
        core::perspective(Camera{}.fov_y, 1.0f, Camera{}.z_near, Camera{}.z_far) *
        core::inverse(core::to_matrix(cam_tf));
    // A unit sphere at z=0 seen from z=8 spans ~34 px of the 256: measure it off the projection
    // so the numbers below track any change of lens/layout.
    const float radius_px = project(view_proj, {1.0f, 0.0f, 0.0f}, kSize).x -
                            project(view_proj, {0.0f, 0.0f, 0.0f}, kSize).x;
    REQUIRE(radius_px > 10.0f);

    // ── (a) lit limb ≫ unlit limb ─────────────────────────────────────────────────────────
    // Center dielectric sphere (0, −1.2, 0): sample on its horizontal diameter, 55% of the
    // radius toward (+x, lit) and away from (−x, unlit) the light. The unlit sample's normal
    // has n·l < 0 for this light position (verified by the arithmetic in the scene comment), so
    // it sees only ambient ≈ 0.02.
    {
        const Pixel c = project(view_proj, {0.0f, -1.2f, 0.0f}, kSize);
        const auto cx = static_cast<std::uint32_t>(c.x);
        const auto cy = static_cast<std::uint32_t>(c.y);
        const auto off = static_cast<std::uint32_t>(0.55f * radius_px);
        const float lit = hdr.luminance(cx + off, cy);
        const float unlit = hdr.luminance(cx - off, cy);
        CHECK(lit > 3.0f * unlit);
        CHECK(lit > 0.1f);    // really lit (E ≈ 0.77 here; diffuse alone gives ≳ 0.15)
        CHECK(unlit < 0.06f); // ambient-only, with slack for the sphere's own curvature
    }

    // ── (b) the highlight tightens as roughness falls ─────────────────────────────────────
    // Metal row: within each sphere's projected disc, find the peak radiance and count pixels
    // above HALF THAT SPHERE'S OWN peak (a scale-free lobe-width proxy). GGX's peak D is
    // 1/(π·α²): roughness 0.15 → D≈629, roughness 0.85 → D≈0.61 — three orders of magnitude, so
    // "smoother ⇒ higher peak" survives any E difference across the row. And a tight lobe means
    // FEWER pixels above half its own (huge) peak.
    {
        float peak[3] = {0.0f, 0.0f, 0.0f};
        std::uint32_t half_peak_area[3] = {0, 0, 0};
        for (int s = 0; s < 3; ++s) {
            const Pixel c = project(view_proj, {kX[s], 1.2f, 0.0f}, kSize);
            const auto r = static_cast<std::int32_t>(0.9f * radius_px);
            for (std::int32_t dy = -r; dy <= r; ++dy) {
                for (std::int32_t dx = -r; dx <= r; ++dx) {
                    if (dx * dx + dy * dy > r * r)
                        continue;
                    const float lum =
                        hdr.luminance(static_cast<std::uint32_t>(c.x + static_cast<float>(dx)),
                                      static_cast<std::uint32_t>(c.y + static_cast<float>(dy)));
                    peak[s] = std::max(peak[s], lum);
                }
            }
            const float half = 0.5f * peak[s];
            for (std::int32_t dy = -r; dy <= r; ++dy) {
                for (std::int32_t dx = -r; dx <= r; ++dx) {
                    if (dx * dx + dy * dy > r * r)
                        continue;
                    const float lum =
                        hdr.luminance(static_cast<std::uint32_t>(c.x + static_cast<float>(dx)),
                                      static_cast<std::uint32_t>(c.y + static_cast<float>(dy)));
                    if (lum > half)
                        ++half_peak_area[s];
                }
            }
        }
        CHECK(peak[0] > 10.0f * peak[2]); // smooth peak dwarfs rough peak
        CHECK(peak[0] > peak[1]);         // and the middle sits between
        CHECK(peak[1] > peak[2]);
        CHECK(half_peak_area[0] * 4 < half_peak_area[2]); // tight lobe ≪ broad lobe
    }

    // ── (c) global energy bound (and no NaN/Inf) ──────────────────────────────────────────
    // For a punctual light, L_o = f·E·(n·l). Bounding each BRDF factor at the grid's smoothest
    // roughness (α_min = 0.15²): D ≤ 1/(π·α_min²); the height-correlated V times (n·l) ≤
    // 0.5/α_min (its denominator keeps a μ·α term as μ_v → 0); F ≤ 1; diffuse·(n·l) ≤ 1/π.
    // E ≤ I/d_min² with d_min = closest light-to-surface distance over all spheres. Loose by
    // design — its job is catching sign/unit/π disasters and infinities, not grading shading.
    {
        float d_min = 1e9f;
        for (const float x : kX) {
            for (const float y : {1.2f, -1.2f}) {
                const float dx = kLightPos[0] - x;
                const float dy = kLightPos[1] - y;
                const float dz = kLightPos[2];
                d_min = std::min(d_min, std::sqrt(dx * dx + dy * dy + dz * dz) - 1.0f);
            }
        }
        constexpr float kPi = 3.14159265f;
        const float alpha_min = kMetalRough[0] * kMetalRough[0];
        const float e_max = kIntensity / (d_min * d_min);
        const float bound =
            e_max * (1.0f / (kPi * alpha_min * alpha_min) * (0.5f / alpha_min) + 1.0f / kPi) +
            kAmbient;
        float img_max = 0.0f;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const float lum = hdr.luminance(x, y);
                REQUIRE(std::isfinite(lum));
                img_max = std::max(img_max, lum);
            }
        }
        CHECK(img_max < bound);
        CHECK(img_max > 1.0f); // and the scene genuinely is HDR — the tonemap has work to do
    }

    // ── (d) tight bound on the roughness-1 dielectric ─────────────────────────────────────
    // At α = 1, GGX flattens: D = 1/π everywhere, V·(n·l) = 0.5·μl/(μl+μv) ≤ 0.5, F ≤ 1 —
    // so spec·(n·l) ≤ 1/(2π). Diffuse·(n·l) ≤ albedo/π ≤ 1/π. With E ≤ I/d², d = light-to-
    // surface distance of THIS sphere, the ceiling is E·(3/(2π)) + ambient — only ~35% above
    // what the sphere actually reaches, so a doubled intensity or a lost /π fails loudly.
    {
        const float dx = kLightPos[0] - kX[2];
        const float dy = kLightPos[1] - (-1.2f);
        const float dz = kLightPos[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - 1.0f;
        constexpr float kPi = 3.14159265f;
        const float bound = (kIntensity / (d * d)) * (3.0f / (2.0f * kPi)) + kAmbient;

        const Pixel c = project(view_proj, {kX[2], -1.2f, 0.0f}, kSize);
        const auto r = static_cast<std::int32_t>(0.9f * radius_px);
        float sphere_max = 0.0f;
        for (std::int32_t oy = -r; oy <= r; ++oy) {
            for (std::int32_t ox = -r; ox <= r; ++ox) {
                if (ox * ox + oy * oy > r * r)
                    continue;
                sphere_max = std::max(
                    sphere_max,
                    hdr.luminance(static_cast<std::uint32_t>(c.x + static_cast<float>(ox)),
                                  static_cast<std::uint32_t>(c.y + static_cast<float>(oy))));
            }
        }
        CHECK(sphere_max < bound);
        CHECK(sphere_max > 0.1f); // and it IS lit — an all-black sphere would "pass" vacuously
    }

    // ── (3) depth pre-pass on/off: byte-identical LDR ─────────────────────────────────────
    // Frame 2 renders the same scene without the pre-pass. If the invariance contract holds
    // (same gl_Position bits from both vertex shaders, Equal test) the visible surfaces — and
    // therefore every shaded pixel — are identical down to the byte.
    {
        graph.reset();
        const SceneRenderer::Output out2 = renderer.render(graph, world, {kSize, kSize}, false);
        REQUIRE(out2.ldr.is_valid());
        auto cmd2 = device->begin_commands();
        graph.execute(*cmd2);
        device->submit_blocking(*cmd2);
        CHECK(graph.execution_order().size() == 2); // no prepass declared this time

        const std::vector<std::uint8_t> ldr_direct =
            read_texture(*device, graph.physical(out2.ldr), kSize, kSize, 4);
        REQUIRE(ldr_direct.size() == ldr_prepass.size());
        CHECK(ldr_direct == ldr_prepass);
    }
}

TEST_CASE("pbr: a base-color texture reaches the shading (M5.6)") {
    using ecs::WorldTransform;

    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available — skipping texture proof");
        return;
    }
    constexpr std::uint32_t kSize = 128;

    // A red/green 2×2 checker, sRGB like every base-color map. The floor plane tiles it twice,
    // so the rendered floor must show BOTH dominant colors — which fails if the texture never
    // binds (all white), if uvs collapse (one color), or if sRGB decode is skipped (washed out
    // enough to break the dominance ratio below).
    rhi::TextureDesc td{};
    td.extent = {2, 2};
    td.format = rhi::Format::RGBA8Srgb;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "proof-checker";
    const rhi::TextureHandle checker = device->create_texture(td);
    const std::uint8_t texels[16] = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255, 255, 0, 0, 255};
    device->write_texture(checker, texels, sizeof(texels));

    MeshRegistry meshes(*device);
    const MeshId plane = meshes.add(make_plane(2.0f, 2.0f), "proof-floor");

    MaterialRegistry materials;
    PbrMaterialDesc floor_mat{};
    floor_mat.metallic = 0.0f;
    floor_mat.roughness = 1.0f;
    floor_mat.base_color_texture = checker;
    const MaterialId floor = materials.add(floor_mat);

    ecs::World world;
    register_render_components(world);
    (void)world.spawn_with(WorldTransform{}, MeshRef{plane}, MaterialRef{floor});

    // Camera 6 up, pitched −90° so its −z looks straight down at the plane.
    core::Transform cam_tf{};
    cam_tf.translation = {0.0f, 6.0f, 0.0f};
    cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -core::kHalfPi);
    (void)world.spawn_with(WorldTransform{cam_tf}, Camera{});

    core::Transform light_tf{};
    light_tf.translation = {0.0f, 5.0f, 0.0f};
    (void)world.spawn_with(WorldTransform{light_tf}, PointLight{1.0f, 1.0f, 1.0f, 30.0f, 50.0f});

    SceneRenderer renderer(*device, meshes, materials);
    renderer.set_ambient(0.1f, 0.1f, 0.1f);

    RenderGraph graph(*device);
    graph.reset();
    // No depth pre-pass here — this also exercises the standalone (Clear + Less) forward path.
    const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, false);
    REQUIRE(out.ldr.is_valid());
    auto cmd = device->begin_commands();
    graph.execute(*cmd);
    device->submit_blocking(*cmd);

    const std::vector<std::uint8_t> ldr =
        read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
    std::uint32_t red_dominant = 0;
    std::uint32_t green_dominant = 0;
    for (std::size_t p = 0; p < ldr.size(); p += 4) {
        const std::uint8_t r = ldr[p + 0];
        const std::uint8_t g = ldr[p + 1];
        if (r > 80 && r > g + 60)
            ++red_dominant;
        if (g > 80 && g > r + 60)
            ++green_dominant;
    }
    // The plane covers ~90×90 of the 128² frame, half red / half green — hundreds of clean
    // pixels of each even after filtering blur at the checker seams.
    CHECK(red_dominant > 500);
    CHECK(green_dominant > 500);
}

// ── M6.4: material maps ─────────────────────────────────────────────────────────────────────────
// The forward pass now consumes normal / metallic-roughness / emissive / occlusion maps and cooked
// tangents. Same rule as above: structural properties the physics guarantees, never golden pixels.

// Encode a tangent-space normal in [-1,1] to the RGBA8 texel the shader decodes ((n+1)/2 · 255).
std::array<std::uint8_t, 4> encode_normal(float x, float y, float z) {
    const auto b = [](float c) {
        return static_cast<std::uint8_t>(std::lround((c * 0.5f + 0.5f) * 255.0f));
    };
    return {b(x), b(y), b(z), 255};
}

TEST_CASE("pbr: a normal map perturbs shading under grazing light (M6.4)") {
    using ecs::WorldTransform;
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available — skipping normal-map proof");
        return;
    }
    constexpr std::uint32_t kSize = 128;

    // An 8×8 checker normal map: texels alternate a strong +u tilt and −u tilt. On make_plane the
    // tangent is +x, so +u tilts the world normal toward +x, −u toward −x.
    constexpr std::uint32_t kN = 8;
    std::vector<std::uint8_t> npx(static_cast<std::size_t>(kN) * kN * 4);
    for (std::uint32_t y = 0; y < kN; ++y) {
        for (std::uint32_t x = 0; x < kN; ++x) {
            const bool up = ((x + y) & 1u) == 0u;
            const std::array<std::uint8_t, 4> t =
                up ? encode_normal(0.6f, 0.0f, 0.8f) : encode_normal(-0.6f, 0.0f, 0.8f);
            std::memcpy(&npx[(static_cast<std::size_t>(y) * kN + x) * 4], t.data(), 4);
        }
    }
    rhi::TextureDesc nd{};
    nd.extent = {kN, kN};
    nd.format = rhi::Format::RGBA8Unorm; // a normal map is linear DATA, never sRGB
    nd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    nd.debug_name = "proof-normal-map";
    const rhi::TextureHandle normal_map = device->create_texture(nd);
    device->write_texture(normal_map, npx.data(), npx.size());

    // Render the floor once with the map, once flat (invalid handle → the renderer's flat-normal
    // fallback). A horizontal directional light travelling −x grazes the +y floor: a FLAT floor
    // gets n·l ≈ 0 (only ambient, uniform); the mapped floor's +x-tilted texels catch the light
    // while −x ones stay dark — a high-contrast checker. Variance ratio proves the perturbation
    // reached shading.
    const auto render_floor = [&](rhi::TextureHandle normal_tex) -> HdrImage {
        MeshRegistry meshes(*device);
        const MeshId plane = meshes.add(make_plane(2.0f, 1.0f), "np-floor");
        MaterialRegistry materials;
        PbrMaterialDesc m{};
        m.metallic = 0.0f;
        m.roughness = 0.6f;
        m.normal_texture = normal_tex;
        const MaterialId id = materials.add(m);

        ecs::World world;
        register_render_components(world);
        (void)world.spawn_with(WorldTransform{}, MeshRef{plane}, MaterialRef{id});
        core::Transform cam_tf{};
        cam_tf.translation = {0.0f, 6.0f, 0.0f};
        cam_tf.rotation = core::quat_from_axis_angle({1.0f, 0.0f, 0.0f}, -core::kHalfPi);
        (void)world.spawn_with(WorldTransform{cam_tf}, Camera{});
        core::Transform light_tf{};
        light_tf.rotation =
            core::quat_from_axis_angle({0.0f, 1.0f, 0.0f}, core::kHalfPi); // −z → −x
        (void)world.spawn_with(WorldTransform{light_tf}, DirectionalLight{1.0f, 1.0f, 1.0f, 3.0f});

        SceneRenderer renderer(*device, meshes, materials);
        renderer.set_ambient(0.02f, 0.02f, 0.02f);
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        return decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
    };

    const HdrImage bumpy = render_floor(normal_map);
    const HdrImage flat = render_floor(rhi::TextureHandle{});
    device->destroy(normal_map);

    // Luminance variance over the central region (all floor — the plane fills the frame from
    // above).
    const auto variance = [](const HdrImage& img) {
        double sum = 0.0, sum2 = 0.0;
        std::size_t n = 0;
        for (std::uint32_t y = 32; y < 96; ++y) {
            for (std::uint32_t x = 32; x < 96; ++x) {
                const double l = img.luminance(x, y);
                sum += l;
                sum2 += l * l;
                ++n;
            }
        }
        const double mean = sum / static_cast<double>(n);
        return sum2 / static_cast<double>(n) - mean * mean;
    };
    const double var_bumpy = variance(bumpy);
    const double var_flat = variance(flat);
    MESSAGE("normal-map variance: bumpy=" << var_bumpy << " flat=" << var_flat);
    CHECK(var_bumpy > 0.002);           // the bumps really do vary the shading
    CHECK(var_bumpy > 25.0 * var_flat); // ≫ the near-uniform flat control
    CHECK(var_flat < 0.0005);           // and the flat floor is essentially uniform (ambient)
}

TEST_CASE("pbr: a metallic-roughness texture drives the specular lobe (M6.4)") {
    using ecs::WorldTransform;
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available — skipping metallic-roughness proof");
        return;
    }
    constexpr std::uint32_t kSize = 160;

    // Render the SAME metal sphere under the SAME point light twice, changing only the roughness
    // delivered by a metallic-roughness TEXTURE (G = roughness, B = metallic). Everything geometric
    // is held fixed, so any difference in the specular peak is the map's roughness channel reaching
    // shading through binding 3. A smooth map must give a far brighter, tighter highlight than a
    // rough one (peak GGX D ∝ 1/α²) — the same physics as the M5.6 factor proof, now
    // texture-driven.
    const auto sphere_peak = [&](std::uint8_t rough_g) -> float {
        const std::uint8_t texel[4] = {0, rough_g, 255, 255}; // G roughness, B = 1 (metal)
        rhi::TextureDesc td{};
        td.extent = {1, 1};
        td.format = rhi::Format::RGBA8Unorm; // metallic-roughness is linear DATA
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        td.debug_name = "proof-mr";
        const rhi::TextureHandle mr = device->create_texture(td);
        device->write_texture(mr, texel, sizeof(texel));

        MeshRegistry meshes(*device);
        const MeshId sphere = meshes.add(make_uv_sphere(1.0f, 32, 64), "mr-sphere");
        MaterialRegistry materials;
        PbrMaterialDesc m{};
        m.base_color[0] = m.base_color[1] = m.base_color[2] = 1.0f;
        m.metallic = 1.0f;  // × MR.b
        m.roughness = 1.0f; // × MR.g, so effective roughness = the texel's G
        m.metallic_roughness_texture = mr;
        const MaterialId id = materials.add(m);

        ecs::World world;
        register_render_components(world);
        (void)world.spawn_with(WorldTransform{}, MeshRef{sphere}, MaterialRef{id});
        core::Transform cam_tf{};
        cam_tf.translation = {0.0f, 0.0f, 4.0f};
        (void)world.spawn_with(WorldTransform{cam_tf}, Camera{});
        core::Transform light_tf{};
        light_tf.translation = {2.5f, 3.0f, 4.0f}; // front-up-right: a clear specular highlight
        (void)world.spawn_with(WorldTransform{light_tf},
                               PointLight{1.0f, 1.0f, 1.0f, 60.0f, 40.0f});

        SceneRenderer renderer(*device, meshes, materials);
        renderer.set_ambient(0.0f, 0.0f, 0.0f); // isolate specular — a metal has no diffuse anyway
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, true);
        graph.export_texture(out.hdr);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        const HdrImage hdr = decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
        device->destroy(mr);

        float peak = 0.0f;
        for (std::uint32_t y = 0; y < kSize; ++y) {
            for (std::uint32_t x = 0; x < kSize; ++x) {
                const float l = hdr.luminance(x, y);
                REQUIRE(std::isfinite(l));
                peak = std::max(peak, l);
            }
        }
        return peak;
    };

    const float smooth = sphere_peak(26); // ≈ 0.10 roughness
    const float rough = sphere_peak(230); // ≈ 0.90 roughness
    MESSAGE("MR-texture specular peak: smooth=" << smooth << " rough=" << rough);
    CHECK(smooth > 8.0f * rough); // the smooth map's mirror peak dwarfs the rough map's broad lobe
    CHECK(smooth > 1.0f);         // and it is genuinely HDR-bright (the map really reached shading)
    CHECK(rough > 0.01f); // the rough sphere is still lit — not a black-passes-vacuously test
}

TEST_CASE("m6.4: tangent handedness flips under mirrored UVs, bitangent stays put") {
    // The MikkTSpace trap (docs/math/tangent-space.md §4): a mirrored UV chart must flip the stored
    // handedness sign so the shader's B = w·cross(N,T) still reproduces the geometric bitangent
    // ∂p/∂v. A +z-facing quad; build it once with a standard chart and once with U mirrored, and
    // check both.
    const auto quad = [](bool mirror_u) {
        CpuMesh m;
        const float u0 = mirror_u ? 1.0f : 0.0f;
        const float u1 = mirror_u ? 0.0f : 1.0f;
        // Positions in the xy-plane, normal +z; u grows with +x (or −x if mirrored), v grows with
        // +y.
        m.vertices = {
            {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, u0, 0.0f},
            {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, u1, 0.0f},
            {1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, u1, 1.0f},
            {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, u0, 1.0f},
        };
        m.indices = {0, 1, 2, 0, 2, 3};
        compute_tangents(m);
        return m;
    };

    const CpuMesh normal = quad(false);
    const CpuMesh mirrored = quad(true);

    // The handedness sign is opposite between the two charts (the whole point of storing w).
    CHECK(normal.vertices[0].tw * mirrored.vertices[0].tw < 0.0f);

    // Yet in BOTH, the reconstructed bitangent B = w·cross(N,T) points +y — the geometric ∂p/∂v —
    // because w compensates for the mirrored tangent. That invariance is what keeps a mirror seam
    // from lighting inside-out.
    for (const CpuMesh* mesh : {&normal, &mirrored}) {
        for (const MeshVertex& v : mesh->vertices) {
            const core::Vec3 N{v.nx, v.ny, v.nz};
            const core::Vec3 T{v.tx, v.ty, v.tz};
            const core::Vec3 B = core::cross(N, T) * v.tw;
            CHECK(B.y > 0.9f); // bitangent reproduces +y (∂p/∂v)
            CHECK(std::fabs(B.x) < 0.1f);
            CHECK(std::fabs(B.z) < 0.1f);
            CHECK(std::fabs(std::fabs(v.tw) - 1.0f) < 1e-5f); // and w is a clean ±1
        }
    }
}
