// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 The Rime Engine Authors.
//
// ADR-0078 step 1c: the motion-vector buffer. Every proof here is STRUCTURAL -- a property the
// geometry guarantees, checked with a stated margin on whatever Vulkan device is available -- and
// never a golden image. The expected velocities are computed in C++ from the renderer's own
// unjittered matrices, NOT read back from the shader and compared with itself.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "render_test_support.hpp"
#include "rime/core/math/mat.hpp"
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

// The velocity target decoded to floats: two NDC-unit components per pixel, plus the raw 16-bit
// words so "exactly zero" can be asserted on BITS rather than within an epsilon.
//
// Two strengths of zero, deliberately separated, because they fail for different reasons.
// MAGNITUDE zero (0x0000 or 0x8000) is the property that matters: a static pixel reprojects to
// where it already was, so the two clip positions are equal and their difference is zero. One ULP
// of drift would fail it, which is the drift this file exists to catch. STRICT +0.0 (0x0000 only)
// is a sharper probe of one cause: -0.0 appears when the compiler contracts the two syntactically
// identical reprojections DIFFERENTLY, which `precise` in velocity.vert forbids. Measured on an
// RTX 3060, 568 words came out -0.0 without it. That cause is vendor- and driver-specific, so it
// gets its own test case: if a driver disagrees, the sharp probe fails and says so while the
// load-bearing property still holds.
struct Velocity {
    std::vector<std::uint16_t> raw; // 2 words per pixel
    std::vector<float> xy;          // 2 floats per pixel

    [[nodiscard]] core::Vec2 at(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 2;
        return {xy[i], xy[i + 1]};
    }

    // Magnitude zero: +0.0 or -0.0. Any real value, however small, fails.
    [[nodiscard]] static bool is_zero_word(std::uint16_t w) noexcept {
        return w == 0x0000 || w == 0x8000;
    }

    [[nodiscard]] bool raw_zero_at(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 2;
        return is_zero_word(raw[i]) && is_zero_word(raw[i + 1]);
    }

    [[nodiscard]] std::size_t nonzero_words() const {
        std::size_t n = 0;
        for (const std::uint16_t w : raw)
            n += is_zero_word(w) ? 0 : 1;
        return n;
    }

    // The sharper probe: strictly +0.0, no negative zero. See the note above.
    [[nodiscard]] std::size_t negative_zero_words() const {
        std::size_t n = 0;
        for (const std::uint16_t w : raw)
            n += w == 0x8000 ? 1 : 0;
        return n;
    }
};

struct Frame {
    bool has_velocity = false;
    Velocity velocity;
    std::vector<std::uint8_t> ldr;
    HdrImage hdr;
};

// A world with a camera at the origin looking down -z, a unit cube (half extent 0.5) at
// (0, 0, -6), and ambient light so the cube is visible in the HDR target. Held in a struct so each
// test mutates the same pieces.
struct Scene {
    std::unique_ptr<rhi::Device> device;
    MeshRegistry meshes;
    MaterialRegistry materials;
    ecs::World world;
    MeshId cube = kInvalidMeshId;
    MaterialId mat = 0;
    ecs::Entity camera;
    SceneRenderer renderer;

    explicit Scene(std::unique_ptr<rhi::Device> d)
        : device(std::move(d)), meshes(*device), renderer(*device, meshes, materials) {
        cube = meshes.add(make_cube(0.5f), "mv-cube");
        mat = materials.add({{1.0f, 1.0f, 1.0f, 1.0f}, 0.0f, 0.5f});
        register_render_components(world);
        camera = world.spawn_with(ecs::WorldTransform{}, Camera{});
        renderer.set_ambient(0.5f, 0.5f, 0.5f);
    }

    ecs::Entity spawn_cube(core::Vec3 at) {
        core::Transform tf{};
        tf.translation = at;
        return world.spawn_with(ecs::WorldTransform{tf}, MeshRef{cube}, MaterialRef{mat});
    }

    void move_camera_to(core::Vec3 at) {
        core::Transform tf{};
        tf.translation = at;
        world.despawn(camera);
        camera = world.spawn_with(ecs::WorldTransform{tf}, Camera{});
    }

    void move_cube_to(ecs::Entity e, core::Vec3 at) {
        auto* wt = world.get<ecs::WorldTransform>(e);
        REQUIRE(wt != nullptr);
        wt->value.translation = at;
    }

    // Render one frame and read back velocity (when the renderer produced one), LDR and HDR.
    Frame render(bool prepass = true) {
        Frame f;
        RenderGraph graph(*device);
        graph.reset();
        const SceneRenderer::Output out = renderer.render(graph, world, {kSize, kSize}, prepass);
        REQUIRE(out.ldr.is_valid());
        graph.export_texture(out.hdr);
        if (out.velocity.is_valid())
            graph.export_texture(out.velocity);
        auto cmd = device->begin_commands();
        graph.execute(*cmd);
        device->submit_blocking(*cmd);
        f.ldr = read_texture(*device, graph.physical(out.ldr), kSize, kSize, 4);
        f.hdr = decode_hdr(
            read_texture(*device, graph.physical(out.hdr), kSize, kSize, 8), kSize, kSize);
        f.has_velocity = out.velocity.is_valid();
        if (f.has_velocity) {
            const auto bytes = read_texture(*device, graph.physical(out.velocity), kSize, kSize, 4);
            const std::size_t words = static_cast<std::size_t>(kSize) * kSize * 2;
            f.velocity.raw.resize(words);
            std::memcpy(f.velocity.raw.data(), bytes.data(), words * sizeof(std::uint16_t));
            f.velocity.xy.resize(words);
            for (std::size_t i = 0; i < words; ++i)
                f.velocity.xy[i] = half_to_float(f.velocity.raw[i]);
        }
        return f;
    }
};

std::unique_ptr<Scene> make_scene() {
    auto device = rhi::create_device({});
    if (!device) {
        if (vulkan_required()) {
            FAIL("RIME_REQUIRE_VULKAN is set but no Vulkan device could be created");
        }
        MESSAGE("no Vulkan device available -- skipping motion-vector proofs");
        return nullptr;
    }
    return std::make_unique<Scene>(std::move(device));
}

// NDC of a world point through a clip-from-world matrix.
core::Vec2 ndc_of(const core::Mat4& vp, core::Vec3 p) {
    const core::Vec4 c = vp * core::Vec4{p.x, p.y, p.z, 1.0f};
    return {c.x / c.w, c.y / c.w};
}

// The pixel a world point lands on under `vp` (the pixel CENTRE convention: ndc 0 is between
// pixels 63 and 64 of 128, so floor of the continuous coordinate).
void pixel_of(const core::Mat4& vp, core::Vec3 p, std::uint32_t& px, std::uint32_t& py) {
    const core::Vec2 n = ndc_of(vp, p);
    px = static_cast<std::uint32_t>(std::floor((n.x * 0.5f + 0.5f) * static_cast<float>(kSize)));
    py = static_cast<std::uint32_t>(std::floor((n.y * 0.5f + 0.5f) * static_cast<float>(kSize)));
}

// Margin on the analytic comparison, in NDC units. Half floats keep ~11 significant bits: for the
// largest velocity any test produces (~0.12) the storage step is ~6e-5, and perspective-correct
// interpolation adds float noise of ~1e-6. 5e-4 is ~8x the storage step and ~0.03 of a pixel at
// this resolution (one pixel is 2/128 = 0.0156), far below any real bug (a sign flip, a missing
// prev_model, or jitter -- up to 0.5 px = 0.0078 -- each move the answer by >= 0.0078).
constexpr float kMargin = 5e-4f;

// The face of the cube nearest the camera sits at z = -5.5; a planar face parallel to the image
// plane under a pure translation has the SAME NDC shift everywhere on it, so any pixel on the face
// can be checked against the one analytic number.
constexpr float kFaceZ = -5.5f;

} // namespace

TEST_CASE("motion vectors: static camera, static object -> exactly zero, with and without jitter") {
    auto s = make_scene();
    if (!s)
        return;
    (void)s->spawn_cube({0.0f, 0.0f, -6.0f});
    s->renderer.set_motion_vectors_enabled(true);

    for (const bool jitter : {false, true}) {
        s->renderer.set_temporal_jitter_enabled(jitter);
        (void)s->render(); // first frame of this configuration: no history
        for (int i = 0; i < 4; ++i) {
            const Frame f = s->render();
            REQUIRE(f.has_velocity);
            // The cube must actually be on screen, or "all zero" would prove nothing.
            CHECK(f.hdr.luminance(kSize / 2, kSize / 2) > 0.05f);
            // BIT-exact: every word of every pixel is +0.0. A jittered matrix leaking into either
            // end, a one-frame-stale matrix, or a transform rebuilt by a different code path would
            // each leave some pixel nonzero. With jitter on, the offset changes EVERY frame, so
            // that case is the one that catches jitter reaching the velocity.
            CHECK(f.velocity.nonzero_words() == 0);
        }
    }
}

// The sharper probe of the same frame, kept separate because it can fail for a reason that is
// not a bug in this brick. `velocity.vert` qualifies both clip positions `precise`, which forbids
// the compiler from contracting two syntactically identical reprojections differently. Without it,
// 568 words of a static scene came back -0.0 on an RTX 3060 -- numerically zero, but evidence that
// the two expressions were NOT compiled the same way, and a sign that a future driver could differ
// by a real ULP rather than only by a sign bit.
//
// So this asserts the stronger thing, and its failure means "`precise` did not hold on this
// driver", not "motion vectors are wrong". If it ever goes red while the case above stays green,
// that is what it is telling you, and the fix is a portable formulation in the shader rather than
// a weaker assertion here.
TEST_CASE("motion vectors: `precise` holds -- a static scene is +0.0, never -0.0 (driver probe)") {
    auto s = make_scene();
    if (!s)
        return;
    (void)s->spawn_cube({0.0f, 0.0f, -6.0f});
    s->renderer.set_motion_vectors_enabled(true);
    s->renderer.set_temporal_jitter_enabled(true); // the harder case: jitter must not leak in
    (void)s->render();                             // first frame: no history
    const Frame f = s->render();
    REQUIRE(f.has_velocity);
    CHECK(f.hdr.luminance(kSize / 2, kSize / 2) > 0.05f); // the cube is really on screen
    CHECK(f.velocity.negative_zero_words() == 0);
}

TEST_CASE("motion vectors: camera translates, object static -> the analytic NDC reprojection") {
    auto s = make_scene();
    if (!s)
        return;
    (void)s->spawn_cube({0.0f, 0.0f, -6.0f});
    s->renderer.set_motion_vectors_enabled(true);
    // Jitter ON: the velocity must still be pure geometric motion, so the expectation below (from
    // the UNJITTERED matrices) is only right if both ends of the shader's subtraction are
    // unjittered.
    s->renderer.set_temporal_jitter_enabled(true);

    (void)s->render();
    s->move_camera_to({0.3f, 0.1f, 0.0f});
    const Frame f = s->render();
    REQUIRE(f.has_velocity);

    // Expectation from the SAME two matrices the renderer reports -- computed here in C++.
    const core::Vec3 p{0.0f, 0.0f, kFaceZ};
    const core::Vec2 cur = ndc_of(s->renderer.view_proj_unjittered(), p);
    const core::Vec2 prev = ndc_of(s->renderer.previous_view_proj_unjittered(), p);
    const core::Vec2 expect{cur.x - prev.x, cur.y - prev.y};
    REQUIRE(std::abs(expect.x) > 0.05f); // a real motion, not a degenerate zero-vs-zero pass

    std::uint32_t px = 0;
    std::uint32_t py = 0;
    pixel_of(s->renderer.view_proj_unjittered(), p, px, py);
    const core::Vec2 got = f.velocity.at(px, py);
    CHECK(std::abs(got.x - expect.x) < kMargin);
    CHECK(std::abs(got.y - expect.y) < kMargin);
}

TEST_CASE("motion vectors: object translates, camera static -> the analytic NDC reprojection") {
    auto s = make_scene();
    if (!s)
        return;
    const ecs::Entity cube = s->spawn_cube({0.0f, 0.0f, -6.0f});
    s->renderer.set_motion_vectors_enabled(true);

    (void)s->render();
    s->move_cube_to(cube, {0.4f, -0.2f, -6.0f});
    const Frame f = s->render();
    REQUIRE(f.has_velocity);

    // The camera did not move, so the two matrices are equal and ALL the motion is the object's:
    // this is the test that fails if prev_model is just `model` twice.
    REQUIRE(std::memcmp(&s->renderer.view_proj_unjittered(),
                        &s->renderer.previous_view_proj_unjittered(),
                        sizeof(core::Mat4)) == 0);
    const core::Vec3 p_now{0.4f, -0.2f, kFaceZ};
    const core::Vec3 p_before{0.0f, 0.0f, kFaceZ};
    const core::Vec2 cur = ndc_of(s->renderer.view_proj_unjittered(), p_now);
    const core::Vec2 prev = ndc_of(s->renderer.previous_view_proj_unjittered(), p_before);
    const core::Vec2 expect{cur.x - prev.x, cur.y - prev.y};
    REQUIRE(std::abs(expect.x) > 0.05f);

    std::uint32_t px = 0;
    std::uint32_t py = 0;
    pixel_of(s->renderer.view_proj_unjittered(), p_now, px, py);
    const core::Vec2 got = f.velocity.at(px, py);
    CHECK(std::abs(got.x - expect.x) < kMargin);
    CHECK(std::abs(got.y - expect.y) < kMargin);
}

TEST_CASE("motion vectors: a newly spawned entity's first frame has exactly zero velocity") {
    auto s = make_scene();
    if (!s)
        return;
    const ecs::Entity mover = s->spawn_cube({-1.5f, 0.0f, -6.0f});
    s->renderer.set_motion_vectors_enabled(true);
    (void)s->render();

    // Frame 2: the first cube moves, a second cube APPEARS. The camera is static.
    s->move_cube_to(mover, {-1.1f, 0.0f, -6.0f});
    (void)s->spawn_cube({1.5f, 0.0f, -6.0f});
    const Frame f = s->render();
    REQUIRE(f.has_velocity);

    std::uint32_t mx = 0;
    std::uint32_t my = 0;
    std::uint32_t nx = 0;
    std::uint32_t ny = 0;
    pixel_of(s->renderer.view_proj_unjittered(), {-1.1f, 0.0f, kFaceZ}, mx, my);
    pixel_of(s->renderer.view_proj_unjittered(), {1.5f, 0.0f, kFaceZ}, nx, ny);
    // Both on screen, so the zero below is a measurement of a drawn surface rather than of the
    // clear.
    CHECK(f.hdr.luminance(mx, my) > 0.05f);
    CHECK(f.hdr.luminance(nx, ny) > 0.05f);
    CHECK(std::abs(f.velocity.at(mx, my).x) > 0.02f); // the mover moved...
    CHECK(f.velocity.raw_zero_at(nx, ny));            // ...the newcomer did not, bit-exact
}

TEST_CASE("motion vectors: an entity absent from a frame is dropped from the cache") {
    auto s = make_scene();
    if (!s)
        return;
    const ecs::Entity a = s->spawn_cube({-1.5f, 0.0f, -6.0f});
    const ecs::Entity b = s->spawn_cube({0.0f, 0.0f, -6.0f});
    (void)s->spawn_cube({1.5f, 0.0f, -6.0f});
    CHECK(s->renderer.motion_vector_cache_size() == 0); // nothing before the first render
    s->renderer.set_motion_vectors_enabled(true);

    (void)s->render();
    CHECK(s->renderer.motion_vector_cache_size() == 3);
    s->world.despawn(a);
    (void)s->render();
    CHECK(s->renderer.motion_vector_cache_size() == 2);
    s->world.despawn(b);
    (void)s->render();
    CHECK(s->renderer.motion_vector_cache_size() == 1);

    // Churn: spawn and despawn one entity per frame for a while. The cache must track the live set
    // (1 resident + at most the one currently alive), not the number ever spawned.
    for (int i = 0; i < 20; ++i) {
        const ecs::Entity debris = s->spawn_cube({0.0f, 1.0f, -6.0f});
        (void)s->render();
        CHECK(s->renderer.motion_vector_cache_size() == 2);
        s->world.despawn(debris);
    }
    (void)s->render();
    CHECK(s->renderer.motion_vector_cache_size() == 1);

    s->renderer.set_motion_vectors_enabled(false);
    CHECK(s->renderer.motion_vector_cache_size() == 0);
}

TEST_CASE("motion vectors: off creates no target, keeps no cache, and changes no pixel") {
    auto s = make_scene();
    if (!s)
        return;
    const ecs::Entity cube = s->spawn_cube({0.0f, 0.0f, -6.0f});
    // Since ADR-0078 step 1e the default is ON; this proof is about the OFF state, so switch it off
    // before the first frame (the cache must then stay empty from the start).
    REQUIRE(s->renderer.motion_vectors_enabled());
    s->renderer.set_motion_vectors_enabled(false);
    // Jitter and the resolve are ON by default too, and both change the colour of consecutive
    // frames (the sample position moves; the resolve blends history). This proof is about the
    // velocity pass alone, so the other two are switched off to leave it the only variable.
    s->renderer.set_temporal_jitter_enabled(false);
    s->renderer.set_taa_resolve_enabled(false);

    s->move_cube_to(cube, {0.2f, 0.0f, -6.0f});
    const Frame off = s->render();
    CHECK_FALSE(off.has_velocity);
    CHECK(s->renderer.motion_vector_cache_size() == 0);

    // Same scene, feature switched on: the colour output must be identical, byte for byte, because
    // the velocity pass only ADDS a target -- it must not perturb the frame it rides beside.
    s->renderer.set_motion_vectors_enabled(true);
    const Frame on = s->render();
    CHECK(on.has_velocity);
    CHECK(off.ldr == on.ldr);

    // No depth pre-pass: the velocity pass has nothing to be Equal to, so it declares nothing.
    const Frame no_prepass = s->render(/*prepass=*/false);
    CHECK_FALSE(no_prepass.has_velocity);
}
