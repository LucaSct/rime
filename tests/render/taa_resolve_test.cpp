// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// ADR-0078 step 1d: the TAA resolve. Every proof is STRUCTURAL -- a property the mathematics
// guarantees, checked with a stated margin on whatever Vulkan device is available -- never a
// golden image.
//
// THE RIG. Each proof renders the SAME world through two renderers in lockstep on one device:
//   * `ref`  -- jitter on, resolve OFF. Its frames are the raw jittered samples.
//   * `taa`  -- jitter on, resolve ON.
// Both start at jitter index 0 and advance once per render, so frame t of one has exactly the
// sampling offset of frame t of the other. That is what makes "the mean of the individual jittered
// frames" (the supersampled ground truth) computable, and what lets a proof compare the resolved
// pixel against "the current frame" exactly instead of approximately.
//
// All numbers are LUMINANCE of the pre-tonemap HDR target (Output::hdr, which is the resolved
// radiance when the resolve ran). Pixels are normalised to coverage ("0 = background, 1 =
// object") where a proof is about geometry, so a margin reads as a fraction of the contrast.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
#include "rime/core/math/quat.hpp"
#include "rime/ecs/transform.hpp"
#include "rime/ecs/world.hpp"
#include "rime/render/components.hpp"
#include "rime/render/mesh.hpp"
#include "rime/render/scene_renderer.hpp"

namespace {

using namespace rime;
using namespace rime::render;
using namespace rime::render::test;

constexpr std::uint32_t kSize = 128;
constexpr std::size_t kPixels = static_cast<std::size_t>(kSize) * kSize;

// One frame's luminance, plus the raw HDR bytes for the cases that need bit identity.
struct Frame {
    std::vector<float> lum; // kPixels
    std::vector<std::uint8_t> raw;

    [[nodiscard]] float at(std::uint32_t x, std::uint32_t y) const { return lum[y * kSize + x]; }
};

// One renderer with its own world, so two of them can see the same scene.
struct View {
    MeshRegistry meshes;
    MaterialRegistry materials;
    ecs::World world;
    MeshId cube = kInvalidMeshId;
    MeshId slab =
        kInvalidMeshId; // a 1 x 1 x 0.01 box facing the camera: a flat, constant-depth quad
    MaterialId mat = 0;
    ecs::Entity camera;
    core::Transform camera_tf{};
    SceneRenderer renderer;

    explicit View(rhi::Device& device) : meshes(device), renderer(device, meshes, materials) {
        cube = meshes.add(make_cube(0.5f), "taa-cube");
        slab = meshes.add(make_box({0.5f, 0.5f, 0.005f}), "taa-slab");
        mat = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.5f});
        register_render_components(world);
        camera = world.spawn_with(ecs::WorldTransform{}, Camera{});
        // Ambient only: every visible face of the white cube has the SAME radiance, so an object
        // reads as one flat level against the clear colour and "coverage" is well defined.
        renderer.set_ambient(0.5f, 0.5f, 0.5f);
        // Since ADR-0078 step 1e the defaults are ON. `ref` must be the RAW jittered stream, so the
        // resolve and the velocity buffer are switched off here; Rig turns them on for `taa` only.
        renderer.set_motion_vectors_enabled(false);
        renderer.set_taa_resolve_enabled(false);
        renderer.set_temporal_jitter_enabled(true);
    }

    void set_camera(const core::Transform& tf) {
        camera_tf = tf;
        world.despawn(camera);
        camera = world.spawn_with(ecs::WorldTransform{tf}, Camera{});
    }
};

// Two views over one device. `taa` has the resolve (and the velocity buffer it needs) on.
struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<View> ref;
    std::unique_ptr<View> taa;

    explicit Rig(std::unique_ptr<rhi::Device> d) : device(std::move(d)) {
        ref = std::make_unique<View>(*device);
        taa = std::make_unique<View>(*device);
        taa->renderer.set_motion_vectors_enabled(true);
        taa->renderer.set_taa_resolve_enabled(true);
    }

    // Spawn the same cube in both worlds; returns the pair of entities.
    struct Pair {
        ecs::Entity a;
        ecs::Entity b;
    };

    Pair spawn_cube(core::Vec3 at, float scale = 1.0f, float roll = 0.0f) {
        core::Transform tf{};
        tf.translation = at;
        tf.scale = {scale, scale, scale};
        tf.rotation = core::quat_from_axis_angle({0.0f, 0.0f, 1.0f}, roll);
        return {ref->world.spawn_with(
                    ecs::WorldTransform{tf}, MeshRef{ref->cube}, MaterialRef{ref->mat}),
                taa->world.spawn_with(
                    ecs::WorldTransform{tf}, MeshRef{taa->cube}, MaterialRef{taa->mat})};
    }

    // A flat slab at constant depth. Translating it in x/y moves its image by EXACTLY the same
    // amount at every pixel (no perspective change, no side faces), so a shift of a whole number
    // of pixels maps the previous frame onto the current one with no resampling at all.
    Pair spawn_slab(core::Vec3 at, float scale = 1.0f) {
        core::Transform tf{};
        tf.translation = at;
        tf.scale = {scale, scale, 1.0f};
        return {ref->world.spawn_with(
                    ecs::WorldTransform{tf}, MeshRef{ref->slab}, MaterialRef{ref->mat}),
                taa->world.spawn_with(
                    ecs::WorldTransform{tf}, MeshRef{taa->slab}, MaterialRef{taa->mat})};
    }

    void move_cube(const Pair& p, core::Vec3 at) {
        auto* a = ref->world.get<ecs::WorldTransform>(p.a);
        auto* b = taa->world.get<ecs::WorldTransform>(p.b);
        REQUIRE(a != nullptr);
        REQUIRE(b != nullptr);
        a->value.translation = at;
        b->value.translation = at;
    }

    void set_camera(const core::Transform& tf) {
        ref->set_camera(tf);
        taa->set_camera(tf);
    }

    // `size` other than kSize returns only the raw bytes (the luminance helpers assume kSize).
    static Frame render_view(rhi::Device& device,
                             View& v,
                             bool* resolved = nullptr,
                             std::uint32_t size = kSize) {
        Frame f;
        RenderGraph graph(device);
        graph.reset();
        const SceneRenderer::Output out = v.renderer.render(graph, v.world, {size, size}, true);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.hdr);
        auto cmd = device.begin_commands();
        graph.execute(*cmd);
        device.submit_blocking(*cmd);
        f.raw = read_texture(device, graph.physical(out.hdr), size, size, 8);
        if (resolved != nullptr)
            *resolved = v.renderer.last_frame_resolved();
        if (size != kSize)
            return f;
        const HdrImage img = decode_hdr(f.raw, kSize, kSize);
        f.lum.resize(kPixels);
        for (std::uint32_t y = 0; y < kSize; ++y)
            for (std::uint32_t x = 0; x < kSize; ++x)
                f.lum[y * kSize + x] = img.luminance(x, y);
        return f;
    }

    // Render one frame through each view, in lockstep.
    struct Step {
        Frame ref;
        Frame taa;
    };

    Step step() {
        Step s;
        s.ref = render_view(*device, *ref);
        s.taa = render_view(*device, *taa);
        return s;
    }
};

std::unique_ptr<Rig> make_rig() {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available -- skipping TAA resolve proofs");
        return nullptr;
    }
    return std::make_unique<Rig>(std::move(device));
}

// ---- measurement helpers ---------------------------------------------------------------------

// Background and object levels, read from a frame: the corner is clear colour, and `at` is a pixel
// known to be inside the object.
struct Levels {
    float bg = 0.0f;
    float fg = 0.0f;

    [[nodiscard]] float contrast() const { return fg - bg; }

    [[nodiscard]] float coverage(float l) const { return (l - bg) / contrast(); }
};

Levels levels_of(const Frame& f) {
    Levels l;
    l.bg = f.at(1, 1);
    l.fg = *std::max_element(f.lum.begin(), f.lum.end()); // the flat object level (ambient only)
    REQUIRE(l.contrast() > 0.1f); // the object is really there and really brighter than the clear
    return l;
}

// Pixels where the raw jittered frames disagree with each other by more than `frac` of the
// contrast: exactly the pixels aliasing lives on (edge pixels with partial, jitter-dependent
// coverage). Interior and background pixels never qualify.
std::vector<std::size_t>
edge_pixels(const std::vector<Frame>& frames, const Levels& lv, float frac) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < kPixels; ++i) {
        float lo = 1e30f;
        float hi = -1e30f;
        for (const Frame& f : frames) {
            lo = std::min(lo, f.lum[i]);
            hi = std::max(hi, f.lum[i]);
        }
        if ((hi - lo) > frac * lv.contrast())
            out.push_back(i);
    }
    return out;
}

Frame mean_of(const std::vector<Frame>& frames) {
    Frame m;
    m.lum.assign(kPixels, 0.0f);
    for (const Frame& f : frames)
        for (std::size_t i = 0; i < kPixels; ++i)
            m.lum[i] += f.lum[i];
    for (float& v : m.lum)
        v /= static_cast<float>(frames.size());
    return m;
}

// Temporal variance of one pixel over a set of frames.
float variance_at(const std::vector<Frame>& frames, std::size_t i) {
    double mean = 0.0;
    for (const Frame& f : frames)
        mean += f.lum[i];
    mean /= static_cast<double>(frames.size());
    double var = 0.0;
    for (const Frame& f : frames)
        var += (f.lum[i] - mean) * (f.lum[i] - mean);
    return static_cast<float>(var / static_cast<double>(frames.size()));
}

// Root-mean-square difference of two frames over a pixel set.
float rms_diff(const Frame& a, const Frame& b, const std::vector<std::size_t>& set) {
    double s = 0.0;
    for (const std::size_t i : set)
        s += static_cast<double>(a.lum[i] - b.lum[i]) * static_cast<double>(a.lum[i] - b.lum[i]);
    return static_cast<float>(
        std::sqrt(s / static_cast<double>(std::max<std::size_t>(1, set.size()))));
}

float max_abs_diff(const Frame& a, const Frame& b) {
    float m = 0.0f;
    for (std::size_t i = 0; i < kPixels; ++i)
        m = std::max(m, std::abs(a.lum[i] - b.lum[i]));
    return m;
}

// Pixels per world unit on the plane z = `plane_z`, along x, from the renderer's own matrices.
float pixels_per_unit(const SceneRenderer& r, float plane_z) {
    const Pixel p0 = project(r.view_proj_unjittered(), {0.0f, 0.0f, plane_z}, kSize);
    const Pixel p1 = project(r.view_proj_unjittered(), {1.0f, 0.0f, plane_z}, kSize);
    return p1.x - p0.x;
}

// Frames to let the exponential average settle: 0.9^64 ~ 1e-3, below half-float resolution of the
// differences being measured, and a multiple of the jitter period (8) so the phases line up.
constexpr int kSettle = 64;

// The shared static scene for the convergence / mean / edge proofs: a cube scaled up and rolled a
// few degrees, so its silhouette is made of long, SHALLOW-angle edges across the pixel grid (the
// stair-step case aliasing is worst on). Camera at the origin.
struct StaticRun {
    std::vector<Frame> ref;       // 8 raw jittered frames, phases 0..7
    std::vector<Frame> resolved;  // 8 resolved frames, phases 0..7, after kSettle frames
    std::vector<Frame> resolved2; // the 8 after that (one more period)
    Levels lv;
};

StaticRun run_static(Rig& rig) {
    (void)rig.spawn_cube({0.0f, 0.0f, -6.0f}, 3.0f, 0.12f); // 0.12 rad ~ 6.9 degrees
    StaticRun r;
    for (int t = 0; t < kSettle; ++t) {
        Rig::Step s = rig.step();
        if (t < 8)
            r.ref.push_back(std::move(s.ref)); // phases 0..7 of the raw jittered sequence
    }
    // kSettle frames have run on both views: the jitter index is back at phase 0 (64 % 8 == 0), so
    // resolved[k] and ref[k] were rendered with the SAME sub-pixel offset.
    for (int t = 0; t < 8; ++t)
        r.resolved.push_back(rig.step().taa);
    for (int t = 0; t < 8; ++t)
        r.resolved2.push_back(rig.step().taa);
    r.lv = levels_of(r.ref[0]);
    return r;
}

} // namespace

TEST_CASE("taa resolve: off allocates nothing, declares nothing, and changes no pixel") {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required())
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        MESSAGE("no Vulkan device available -- skipping TAA resolve proofs");
        return;
    }
    // Three renderers over one scene: never touched; switched on then OFF again; and switched on
    // WITHOUT the velocity buffer it needs (the resolve must skip itself, not read garbage).
    View never(*device);
    View toggled(*device);
    View starved(*device);
    for (View* v : {&never, &toggled, &starved}) {
        v->renderer.set_temporal_jitter_enabled(false);
        core::Transform tf{};
        tf.translation = {0.0f, 0.0f, -6.0f};
        (void)v->world.spawn_with(ecs::WorldTransform{tf}, MeshRef{v->cube}, MaterialRef{v->mat});
    }
    // The View ctor switches the feature off (the default is ON since step 1e), so `never` is a
    // renderer that has had it off from the start and never switched it on.
    REQUIRE_FALSE(never.renderer.taa_resolve_enabled());
    CHECK_FALSE(never.renderer.taa_history_allocated());

    toggled.renderer.set_motion_vectors_enabled(true);
    toggled.renderer.set_taa_resolve_enabled(true);
    bool resolved = false;
    (void)Rig::render_view(*device, toggled, &resolved);
    CHECK(resolved);
    CHECK(toggled.renderer.taa_history_allocated()); // on: the pair exists...
    toggled.renderer.set_taa_resolve_enabled(false);
    CHECK_FALSE(toggled.renderer.taa_history_allocated()); // ...off: freed again
    toggled.renderer.set_motion_vectors_enabled(false);

    starved.renderer.set_taa_resolve_enabled(true); // motion vectors left OFF
    const Frame f_never = Rig::render_view(*device, never);
    const Frame f_toggled = Rig::render_view(*device, toggled, &resolved);
    CHECK_FALSE(resolved);
    CHECK(f_never.raw == f_toggled.raw); // bit-identical to a renderer that never saw the feature

    const Frame f_starved = Rig::render_view(*device, starved, &resolved);
    CHECK_FALSE(resolved);
    CHECK_FALSE(starved.renderer.taa_history_allocated());
    CHECK(f_never.raw == f_starved.raw);
}

TEST_CASE(
    "taa resolve: the first frame, and the first after a resize, take the current frame whole") {
    auto rig = make_rig();
    if (!rig)
        return;
    (void)rig->spawn_cube({0.0f, 0.0f, -6.0f}, 3.0f, 0.12f);
    // Frame 0: no history exists. The resolved frame must equal the raw one BIT FOR BIT -- not
    // "close": with history_valid == 0 the shader returns `current` untouched.
    const Rig::Step s0 = rig->step();
    CHECK(s0.ref.raw == s0.taa.raw);
    // Frame 1 uses history, so (jitter differs) it is allowed to differ and must, on an edge.
    const Rig::Step s1 = rig->step();
    CHECK(max_abs_diff(s1.ref, s1.taa) > 1e-3f);
    // A RESIZE invalidates the history (it is the wrong shape), so the first frame at the new size
    // is again the current frame whole, bit for bit, and the pair has been reallocated.
    bool resolved = false;
    const Frame small_ref = Rig::render_view(*rig->device, *rig->ref, nullptr, 96);
    const Frame small_taa = Rig::render_view(*rig->device, *rig->taa, &resolved, 96);
    CHECK(resolved);
    CHECK(small_ref.raw == small_taa.raw);
    // ...and the frame after that resolves against real history again (jitter differs, so on an
    // edge the two must now disagree).
    const Frame small_ref2 = Rig::render_view(*rig->device, *rig->ref, nullptr, 96);
    const Frame small_taa2 = Rig::render_view(*rig->device, *rig->taa, nullptr, 96);
    CHECK(small_ref2.raw != small_taa2.raw);
}

TEST_CASE("taa resolve: static scene, no jitter -> the resolve is a fixed point") {
    auto rig = make_rig();
    if (!rig)
        return;
    rig->ref->renderer.set_temporal_jitter_enabled(false);
    rig->taa->renderer.set_temporal_jitter_enabled(false);
    (void)rig->spawn_cube({0.0f, 0.0f, -6.0f}, 3.0f, 0.12f);
    float worst = 0.0f;
    for (int t = 0; t < 12; ++t) {
        const Rig::Step s = rig->step();
        worst = std::max(worst, max_abs_diff(s.ref, s.taa));
    }
    std::printf("[taa] static/no-jitter worst |resolved - raw| over 12 frames: %.3g\n", worst);
    // With nothing moving and nothing jittering, history == current at every pixel, so mixing them
    // changes nothing; what remains is half-float rounding of a weighted sum of two equal values.
    CHECK(worst < 1e-3f);
}

TEST_CASE(
    "taa resolve: converges, and converges to the supersampled mean, not merely to something") {
    auto rig = make_rig();
    if (!rig)
        return;
    const StaticRun r = run_static(*rig);
    const Frame mean = mean_of(r.ref);
    const std::vector<std::size_t> edges = edge_pixels(r.ref, r.lv, 0.05f);
    REQUIRE(edges.size() > 100); // a real set of aliasing pixels, or every number below is vacuous

    // (1) CONVERGENCE. The input is periodic (period 8), so the converged output is periodic too:
    // frame t+8 must equal frame t. After kSettle frames the transient is 0.9^64 ~ 1e-3 of its
    // start, so what is left is half-float rounding.
    float period_err = 0.0f;
    for (int k = 0; k < 8; ++k)
        period_err = std::max(period_err, max_abs_diff(r.resolved[k], r.resolved2[k]));

    // (2) THE RIGHT ANSWER. For a linear exponential average over a periodic input, the average of
    // the 8 outputs of one period equals the plain mean of the 8 inputs EXACTLY (each input frame
    // contributes the same total weight across a period). The plain mean of the raw jittered
    // frames is the supersampled ground truth. So the period-mean of the resolve must match it.
    // This is what separates "averaging correctly" from "blurring": a blur would match neither.
    const Frame resolved_mean = mean_of(r.resolved);
    const float mean_err_max = max_abs_diff(resolved_mean, mean);
    const float mean_err_rms = rms_diff(resolved_mean, mean, edges);

    // The yardstick: how far a SINGLE raw frame is from that mean, and a single resolved frame.
    float single_raw = 0.0f;
    float single_res = 0.0f;
    for (int k = 0; k < 8; ++k) {
        single_raw += rms_diff(r.ref[k], mean, edges) / 8.0f;
        single_res += rms_diff(r.resolved[k], mean, edges) / 8.0f;
    }
    std::printf("[taa] contrast %.4f, edge pixels %zu\n", r.lv.contrast(), edges.size());
    std::printf("[taa] period error max|R(t+8)-R(t)|: %.3g\n", period_err);
    std::printf("[taa] period-mean(R) vs mean(F): max %.3g, rms on edges %.3g\n",
                mean_err_max,
                mean_err_rms);
    std::printf("[taa] single raw frame vs mean, rms on edges: %.3g\n", single_raw);
    std::printf("[taa] single resolved frame vs mean, rms on edges: %.3g\n", single_res);

    // THRESHOLDS, derived from measurement (RTX 3060 / Vulkan, 2026-10-07, 128x128, contrast 0.5):
    //   period error            2.44e-4  (= one half-float ulp at 0.5; the transient is gone)
    //   period-mean vs mean     max 1.65e-3, rms on edges 8.8e-4  (the exact-identity prediction)
    //   single raw vs mean      rms on edges 0.218
    //   single resolved vs mean rms on edges 0.0157  (the ripple of the exponential window)
    // Each limit is ~3-4x the measured value. They are also FAR from what a wrong pass gives: with
    // the history weight forced to 0 the "resolved" frame IS the raw frame (0.218, ratio 1.0);
    // forced to 1 the mean error is 0.17 rms / 0.31 max. So these margins cannot be met by a blur
    // or by averaging with the wrong weights, which is the point of comparing against the mean.
    CHECK(period_err < 1e-3f);
    CHECK(mean_err_rms < 3e-3f);
    CHECK(mean_err_max < 6e-3f);
    // And the resolve is genuinely closer to the truth than the single frame it started from
    // (measured ratio 0.072; limit 0.2).
    CHECK(single_res < 0.2f * single_raw);
}

TEST_CASE("taa resolve: edge pixels are far more stable over time than raw jittered frames") {
    auto rig = make_rig();
    if (!rig)
        return;
    const StaticRun r = run_static(*rig);
    const std::vector<std::size_t> edges = edge_pixels(r.ref, r.lv, 0.05f);
    REQUIRE(edges.size() > 100);
    double raw_var = 0.0;
    double res_var = 0.0;
    for (const std::size_t i : edges) {
        raw_var += variance_at(r.ref, i);
        res_var += variance_at(r.resolved, i);
    }
    raw_var /= static_cast<double>(edges.size());
    res_var /= static_cast<double>(edges.size());
    std::printf("[taa] mean temporal variance on %zu edge pixels: raw %.4g, resolved %.4g "
                "(ratio %.4g)\n",
                edges.size(),
                raw_var,
                res_var,
                res_var / raw_var);
    // Measured 2026-10-07 (RTX 3060, 284 edge pixels): raw 0.04877, resolved 0.0002545, ratio
    // 0.0052. The limit is ~10x the measured ratio; with the resolve weakened to nothing the ratio
    // is 1.0, so the margin has nothing to hide behind.
    CHECK(res_var < 0.05 * raw_var);
}

TEST_CASE("taa resolve: a disoccluded pixel shows the revealed background, not the occluder") {
    auto rig = make_rig();
    if (!rig)
        return;
    const Rig::Pair cube = rig->spawn_cube({-1.0f, 0.0f, -6.0f});
    Frame before;
    for (int t = 0; t < 40; ++t)
        before = rig->step().ref;
    const Levels lv = levels_of(before);

    // Move the cube far enough that its old footprint is entirely revealed and never overlaps the
    // new one (the cube is ~25 px wide; this moves it ~50).
    rig->move_cube(cube, {1.0f, 0.0f, -6.0f});
    const Rig::Step now = rig->step();

    // Revealed pixels: inside the old object, outside the new one, each with a 3-pixel margin. The
    // margin matters: the clamp's 3x3 box legitimately admits history at a pixel whose
    // neighbourhood still contains the occluder, so a pixel ADJACENT to the edge is allowed one
    // pixel of ghost. A pixel 3 away has nothing but background around it and must be clean.
    constexpr int kMargin = 3;
    auto is_obj = [&](const Frame& f, int x, int y) {
        if (x < 0 || y < 0 || x >= static_cast<int>(kSize) || y >= static_cast<int>(kSize))
            return false;
        return lv.coverage(f.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y))) >
               0.5f;
    };
    auto near_obj = [&](const Frame& f, int x, int y) {
        for (int dy = -kMargin; dy <= kMargin; ++dy)
            for (int dx = -kMargin; dx <= kMargin; ++dx)
                if (is_obj(f, x + dx, y + dy))
                    return true;
        return false;
    };
    int tested = 0;
    float worst_vs_current = 0.0f;
    float worst_occluder_share = 0.0f;
    for (int y = 0; y < static_cast<int>(kSize); ++y)
        for (int x = 0; x < static_cast<int>(kSize); ++x) {
            if (!is_obj(before, x, y) || near_obj(now.ref, x, y))
                continue;
            // ...and every pixel of the old object must be well inside it.
            bool interior = true;
            for (int dy = -kMargin; dy <= kMargin && interior; ++dy)
                for (int dx = -kMargin; dx <= kMargin; ++dx)
                    if (!is_obj(before, x + dx, y + dy)) {
                        interior = false;
                        break;
                    }
            if (!interior)
                continue;
            ++tested;
            const auto ux = static_cast<std::uint32_t>(x);
            const auto uy = static_cast<std::uint32_t>(y);
            worst_vs_current =
                std::max(worst_vs_current, std::abs(now.taa.at(ux, uy) - now.ref.at(ux, uy)));
            // How much of the OCCLUDER's colour is in the result: 0 = pure background.
            worst_occluder_share =
                std::max(worst_occluder_share, std::abs(lv.coverage(now.taa.at(ux, uy))));
        }
    std::printf("[taa] disocclusion: %d revealed pixels, worst |resolved - current| %.3g, "
                "worst occluder share %.3g\n",
                tested,
                worst_vs_current,
                worst_occluder_share);
    REQUIRE(tested > 50); // the revealed region really exists, or this proves nothing
    // The history at every one of these pixels IS the occluder (coverage 1.0): the stale value is
    // as far from the right answer as it can be, so a regression that reuses it cannot hide.
    CHECK(worst_occluder_share < 0.02f);
    CHECK(worst_vs_current < 0.02f * lv.contrast());
}

TEST_CASE("taa resolve: history that falls off the screen is rejected, not clamped from the edge") {
    auto rig = make_rig();
    if (!rig)
        return;
    // A camera pan of `kShift` pixels moves everything on the cube's face left by that much, so
    // a face pixel at column c had its history at column c + kShift: off the right edge when
    // c + kShift >= 128. The cube is placed so that BEFORE the pan it is entirely off-screen to
    // the right, so the clamped edge texel of the history image is BACKGROUND, while the pixel at
    // the cube's left edge has both colours in its 3x3 box -- the one place the neighbourhood
    // clamp cannot rescue a bad history by itself.
    constexpr float kShift = 10.0f;
    // Render once so the renderer reports its matrices, then size the placement from them.
    (void)rig->step();
    const float ppu = pixels_per_unit(rig->ref->renderer, -5.5f);
    REQUIRE(ppu > 5.0f);
    const float dx = kShift / ppu;
    // Want the cube's left SILHOUETTE edge near column 120 AFTER the pan. The cube sits right of
    // the camera, so its near-left side face widens the silhouette ~8 px beyond the front face
    // (measured: front face at 128.5 puts the silhouette at 112+8 -> see the REQUIREs below, which
    // are the guard if this placement ever drifts).
    const float left_edge_world = dx + (128.5f - 64.0f) / ppu;
    const float centre_x = left_edge_world + 0.5f;
    (void)rig->spawn_cube({centre_x, 0.0f, -6.0f});
    for (int t = 0; t < 12; ++t)
        (void)rig->step(); // pre-pan: cube off-screen right; history = background everywhere
    core::Transform tf{};
    tf.translation = {dx, 0.0f, 0.0f};
    rig->set_camera(tf);
    const Rig::Step now = rig->step();

    const Levels lv = [&] {
        Levels l;
        l.bg = now.ref.at(1, 1);
        l.fg = now.ref.at(kSize - 2, kSize / 2);
        REQUIRE(l.contrast() > 0.1f);
        return l;
    }();
    // The cube's left edge column in the row through its middle.
    int edge_col = -1;
    for (int x = 0; x < static_cast<int>(kSize); ++x)
        if (lv.coverage(now.ref.at(static_cast<std::uint32_t>(x), kSize / 2)) > 0.5f) {
            edge_col = x;
            break;
        }
    std::printf("[taa] off-screen: left edge column %d (shift %.1f px)\n", edge_col, kShift);
    REQUIRE(edge_col >= 0);
    // The history of the edge pixel is genuinely off-screen: its column + shift >= 128. If this
    // fails the test is not exercising rejection at all, so it must fail rather than pass.
    REQUIRE(static_cast<float>(edge_col) + kShift >= static_cast<float>(kSize));
    // And the edge pixel's 3x3 box really holds both colours (else the clamp alone rescues it).
    REQUIRE(lv.coverage(now.ref.at(static_cast<std::uint32_t>(edge_col - 1), kSize / 2)) < 0.5f);

    float worst = 0.0f;
    for (int y = static_cast<int>(kSize / 2) - 6; y <= static_cast<int>(kSize / 2) + 6; ++y)
        for (int x = edge_col; x < static_cast<int>(kSize); ++x) {
            const auto ux = static_cast<std::uint32_t>(x);
            const auto uy = static_cast<std::uint32_t>(y);
            worst = std::max(worst, std::abs(now.taa.at(ux, uy) - now.ref.at(ux, uy)));
        }
    std::printf("[taa] off-screen: worst |resolved - current| %.3g (contrast %.3g)\n",
                worst,
                lv.contrast());
    // Rejected history means "this pixel IS the current frame": equal up to half-float rounding.
    CHECK(worst < 0.01f * lv.contrast());
}

TEST_CASE("taa resolve: reprojection follows motion on both axes (a moving edge stays put)") {
    // A flat slab slides across the screen by a WHOLE number of pixels per frame (jitter off, so
    // the raster of each frame is the previous one shifted by exactly that). Reprojection that
    // finds the right history therefore reproduces the CURRENT frame at every pixel the object
    // covers, EXACTLY: the leading edge's history is the slab's interior one step back, the
    // trailing edge's is the previous trailing edge. Reprojection that looks in the wrong place --
    // an inverted y, a missing half-extent factor -- fetches background at the leading edge
    // (displaced by 2s or s/2 instead of 0), which the neighbourhood clamp ADMITS (the box there
    // holds both colours), so the leading edge turns up to 90% background. That is the failure
    // this measures, and it is a failure the clamp cannot hide.
    //
    // Only object pixels are scored. The background pixels just behind the trailing edge are the
    // disocclusion case, which the box clamp deliberately lets ghost by one pixel (see the
    // disocclusion proof for why that is the right trade).
    struct Dir {
        const char* name;
        float sx; // pixels per frame, image x
        float sy; // pixels per frame, image y (down)
    };

    for (const Dir d : {Dir{"x", 3.0f, 0.0f}, Dir{"y", 0.0f, 3.0f}, Dir{"diagonal", 2.0f, -3.0f}}) {
        auto rig = make_rig();
        if (!rig)
            return;
        rig->ref->renderer.set_temporal_jitter_enabled(false);
        rig->taa->renderer.set_temporal_jitter_enabled(false);
        (void)rig->step(); // so the renderer reports its matrices
        const float ppu = pixels_per_unit(rig->ref->renderer, -5.995f); // the slab's front face
        const float ux = d.sx / ppu;
        const float uy = -d.sy / ppu; // image y points down, world y points up
        // 24 px wide: its edges then fall on pixel BOUNDARIES (half a pixel from every centre), so
        // a 1e-4 px error in the step cannot flip which side of an edge a pixel centre lies on.
        const Rig::Pair slab = rig->spawn_slab({0.0f, 0.0f, -6.0f}, 24.0f / ppu);
        Levels lv;
        float worst = 0.0f;
        int scored = 0;
        // 64 frames standing still first: an object that APPEARS fades in over ~22 frames at its
        // silhouette (its edge pixels have background in their 3x3 box, so the clamp admits the
        // empty history), and that fade is correct, separate behaviour. Start the motion from a
        // converged image so what is measured is the reprojection and nothing else.
        constexpr int kStill = 64;
        constexpr int kMoving = 12;
        for (int t = 0; t < kStill + kMoving; ++t) {
            const float dt = static_cast<float>(std::max(0, t - kStill + 1));
            rig->move_cube(slab, {ux * dt, uy * dt, -6.0f});
            const Rig::Step s = rig->step();
            if (t == 0)
                lv = levels_of(s.ref);
            if (t < kStill)
                continue;
            for (std::size_t i = 0; i < kPixels; ++i) {
                if (lv.coverage(s.ref.lum[i]) < 0.5f)
                    continue;
                ++scored;
                worst = std::max(worst, std::abs(s.taa.lum[i] - s.ref.lum[i]) / lv.contrast());
            }
        }
        std::printf("[taa] motion %-8s: %d object pixels scored, worst |resolved - current| "
                    "= %.3g of contrast\n",
                    d.name,
                    scored,
                    worst);
        REQUIRE(scored > 1000);
        // Measured 2026-10-07 (RTX 3060): 0.0137 / 0.0142 / 0.0044 of contrast for x / y /
        // diagonal when correct (residual: the history of a settled edge is itself ~0.9^64 short
        // of converged, plus half-float rounding). Wrong lookups give 0.9 -- inverted y, inverted
        // x and a missing 0.5 all measured -- so 0.05 sits 3.5x above correct and 18x below wrong.
        CHECK(worst < 0.05f);
    }
}

TEST_CASE("taa resolve: a camera turn does not smear the background (depth reprojection)") {
    // The velocity buffer is ZERO on background pixels. Left at zero, a background pixel beside a
    // moving object would fetch its history from the same screen position, where last frame the
    // OBJECT was, and take its colour. Reprojecting those pixels from depth (the camera turn is
    // exactly recoverable from the two view-projections) finds the true history: background.
    for (const bool pitch : {false, true}) {
        auto rig = make_rig();
        if (!rig)
            return;
        // Keep the jitter off: this proof is about where history is fetched from, and a clean
        // comparison against the current frame needs the two views to differ only by the resolve.
        rig->ref->renderer.set_temporal_jitter_enabled(false);
        rig->taa->renderer.set_temporal_jitter_enabled(false);
        (void)rig->spawn_cube({0.0f, 0.0f, -6.0f}, 2.0f);
        auto pose = [&](float angle) {
            core::Transform tf{};
            tf.rotation = core::quat_from_axis_angle(
                pitch ? core::Vec3{1.0f, 0.0f, 0.0f} : core::Vec3{0.0f, 1.0f, 0.0f}, angle);
            rig->set_camera(tf);
        };
        pose(0.0f);
        for (int t = 0; t < 8; ++t)
            (void)rig->step();
        Levels lv;
        double band_err = 0.0;
        double band_cnt = 0.0;
        for (int t = 1; t <= 6; ++t) {
            pose(0.012f * static_cast<float>(t)); // ~1.7 px per frame at 128 px, 50 degrees
            const Rig::Step s = rig->step();
            if (t == 1)
                lv = levels_of(s.ref);
            // The band: background pixels within 2 px of the object, where the 3x3 box contains
            // object colour and so cannot rescue a bad history on its own.
            for (int y = 2; y < static_cast<int>(kSize) - 2; ++y)
                for (int x = 2; x < static_cast<int>(kSize) - 2; ++x) {
                    const auto ux = static_cast<std::uint32_t>(x);
                    const auto uy = static_cast<std::uint32_t>(y);
                    if (lv.coverage(s.ref.at(ux, uy)) > 0.02f)
                        continue; // only background pixels
                    bool near_obj = false;
                    for (int dy = -2; dy <= 2 && !near_obj; ++dy)
                        for (int dx = -2; dx <= 2; ++dx)
                            if (lv.coverage(s.ref.at(static_cast<std::uint32_t>(x + dx),
                                                     static_cast<std::uint32_t>(y + dy))) > 0.5f) {
                                near_obj = true;
                                break;
                            }
                    if (!near_obj)
                        continue;
                    band_err +=
                        std::abs(lv.coverage(s.taa.at(ux, uy)) - lv.coverage(s.ref.at(ux, uy)));
                    band_cnt += 1.0;
                }
        }
        REQUIRE(band_cnt > 50.0);
        const double mean_band_err = band_err / band_cnt;
        std::printf("[taa] camera %s: mean background-band error %.4g (of contrast) over %.0f px\n",
                    pitch ? "pitch" : "yaw",
                    mean_band_err,
                    band_cnt);
        // Measured 2026-10-07 (RTX 3060): 0.0339 with depth reprojection, 0.1008 with background
        // velocity left at zero. The 0.034 is not a defect: it is the clamp's deliberate one-pixel
        // ghost beside a moving edge plus bilinear history blur. 0.06 splits the two.
        CHECK(mean_band_err < 0.06);
    }
}
